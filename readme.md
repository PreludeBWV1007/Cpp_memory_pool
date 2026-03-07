## MemoryPool：从 v1 到 v2 的内存池实现

这个仓库包含两个可独立编译运行的 C++ 内存池版本：

- **v1**：固定大小的小对象池（8~512B，步长 8B），用 **64 个 `MemoryPool`** 管理不同槽大小；大于 512B 直接走系统分配。
- **v2**：在 v1 思路上升级为 **ThreadCache / CentralCache / PageCache** 三层缓存架构，覆盖 **8B 对齐 ~ 256KB** 的小/中对象；更贴近生产级分配器的路径。

> 目标是：把“内存池为什么快、怎么做出来、怎么验证”这三件事讲清楚，并提供可直接运行的单测与性能测试。

---

## 目录结构

```text
.
├── v1
│   ├── include/MemoryPool.h
│   ├── src/MemoryPool.cpp
│   ├── tests/UnitTest.cpp
│   └── CMakeLists.txt
└── v2
    ├── include/
    │   ├── Common.h
    │   ├── MemoryPool.h
    │   ├── ThreadCache.h
    │   ├── CentralCache.h
    │   └── PageCache.h
    ├── src/
    │   ├── ThreadCache.cpp
    │   ├── CentralCache.cpp
    │   └── PageCache.cpp
    ├── tests/
    │   ├── UnitTest.cpp
    │   └── PerformanceTest.cpp
    └── CMakeLists.txt
```

---

## 版本对比

| 项目 | v1 | v2 |
|---|---|---|
| **对外接口** | `HashBucket::useMemory/freeMemory` + `newElement/deleteElement` | `MemoryPool::allocate/deallocate` |
| **大小范围** | 8B~512B（64 桶，8B 步进）；>512B 走 `operator new/delete` | 8B 对齐，≤256KB 走内存池；>256KB 走 `malloc/free` |
| **并发策略** | 空闲链表 CAS；“切新块”用互斥锁 | 每线程 `ThreadCache`（thread_local）；中心层按大小类自旋锁；页缓存互斥锁 |
| **上游供给** | 每个桶按需申请 block（默认 4KB）并切槽 | `PageCache` 以 span（连续多页）向系统申请（`mmap`），供 `CentralCache` 切块 |
| **回收策略** | 释放回本桶空闲链表 | 线程缓存阈值回收 → 中心缓存延迟归还 → span 全空闲归还页缓存 |

---

## v1 架构（固定 64 桶小对象池）

v1 的核心是 `HashBucket` 管理的 **64 个 `MemoryPool`**：第 \(i\) 个池负责槽大小 \((i+1)\times 8\) 字节（8、16、…、512）。每个 `MemoryPool`：

- 维护一个 **空闲链表**（`std::atomic<Slot*> freeList_`），回收的槽头插进去，分配时优先弹出；
- 当空闲链表为空时，从当前 block 的 `curSlot_` 开始按槽大小切分，切到边界 `lastSlot_` 就申请新 block；
- 申请新 block 与推进 `curSlot_` 需要互斥锁保护；空闲链表使用 CAS 实现无锁 push/pop。

```mermaid
flowchart LR
  A["用户：newElement<T>() / deleteElement<T>()"] --> B["HashBucket（64 个桶）"]
  B --> C1["MemoryPool[0] 槽=8B"]
  B --> C2["MemoryPool[1] 槽=16B"]
  B --> C3["..."]
  B --> Cn["MemoryPool[63] 槽=512B"]
  C1 --> D1["Block 链表（按需申请）"]
  C1 --> E1["freeList_（CAS 空闲栈）"]
```

### v1 架构图
![v1 内存池结构](v1/v1_structure.jpg)

---

## v2 架构（ThreadCache / CentralCache / PageCache）

v2 把热点路径拆成三层：

- **`ThreadCache`**：每个线程一个实例（`thread_local`），按大小类维护 `freeList_[]`，小对象分配/释放走纯本地链表，避免全局锁争用。
- **`CentralCache`**：全局单例，作为线程缓存的“补给站/回收站”。按大小类维护中心链表并用 **per-index 自旋锁**保护；从 `PageCache` 要 span 后切成小块供线程批量获取；线程归还时做 **延迟归还**，当某个 span 的块全部空闲时把整段 span 还给 `PageCache`。
- **`PageCache`**：全局单例，按页（4KB）管理 span（连续多页），用 `mmap` 向系统要内存，支持从更大的空闲 span 里切分以及归还时合并相邻 span。

```mermaid
flowchart TB
  U["用户：MemoryPool::allocate(size)"] --> TC["ThreadCache（thread_local）"]
  TC -->|本地命中| R["返回指针"]
  TC -->|本地缺货| CC["CentralCache（全局）"]
  CC -->|中心命中| TC
  CC -->|中心缺货| PC["PageCache（全局，span/页）"]
  PC -->|mmap / 复用空闲span| CC
  TC -->|阈值回收| CC
  CC -->|span 全空闲：归还| PC
```

### v2 架构图
![v2 内存池结构](v2/v2_structure.png)

---

## 快速开始（构建 & 运行）

### v1

```bash
cd v1
cmake -S . -B build
cmake --build build -j
./build/MemoryPoolProject
```

`v1/tests/UnitTest.cpp` 里对比了内存池与系统 `new/delete` 的简单基准（注意：这不是严谨 benchmark，但足够说明趋势）。

### v2

```bash
cd v2
cmake -S . -B build
cmake --build build -j
cmake --build build --target test
cmake --build build --target perf
```

---

## 使用方式（最小示例）

### v1：对象级接口（推荐学习用）

```cpp
#include "MemoryPool.h"
using namespace Kama_memoryPool;

int main() {
  HashBucket::initMemoryPool(); // 只需初始化一次

  auto* p = newElement<int>(42);
  deleteElement(p);
}
```

### v2：字节级接口（更像通用分配器）

```cpp
#include "MemoryPool.h"
using namespace Kama_memoryPool;

int main() {
  void* p = MemoryPool::allocate(128);
  // ... use p ...
  MemoryPool::deallocate(p, 128); // 需要传入 size（用于定位大小类）
}
```

---

## 设计要点

- **大小类与对齐**：
  - v1：8~512B 固定 64 桶，向上取整到 8 的倍数。
  - v2：`ALIGNMENT=8`，`MAX_BYTES=256KB`，大小类索引为 `SizeClass::getIndex(size)`。
- **为什么 v2 释放要带 size**：该实现没有维护“指针→大小类”的元信息（例如 header / radix map），因此释放时需要 size 才能把块放回正确的链表。
- **平台相关**：v2 的 `PageCache` 使用 `mmap`（Linux/WSL/macOS 兼容更好）；如果要支持纯 Windows，需要替换为 `VirtualAlloc/VirtualFree` 或其它后端。

---

## 性能测试与结果

本节说明 v1 / v2 的测试方式、参数含义以及在本仓库中跑出的性能结论。测试环境为常规 Linux/WSL，未固定 CPU 频率，计时仅作相对对比参考。

### v1 基准测试

**测试代码**：`v1/tests/UnitTest.cpp`。对 4 种小对象（P1~P4，约 4/20/40/80 字节）反复做「分配 + 立即释放」，分别用内存池接口（`newElement`/`deleteElement`）和系统 `new`/`delete` 跑相同次数，用 `clock()` 统计总耗时。

**参数含义**：

- **ntimes**：每轮中，每种类型（P1~P4）分配并释放的次数，即每轮共约 `ntimes × 4` 次分配/释放。
- **nworks**：并发线程数。
- **rounds**：每个线程执行的轮次数；总操作量约为 `nworks × rounds × ntimes × 4` 次分配/释放。

**说明**：v1 在多线程（如 4 线程）下曾出现段错误，当前用例仅做单线程多规模对比。

**运行方式**：

```bash
cd v1 && cmake -S . -B build && cmake --build build -j && ./build/MemoryPoolProject
```

**v1 测试配置与结果**（单线程，内存池 vs 系统 new/delete）：

| 场景说明           | ntimes | nworks | rounds | 内存池耗时 | 系统 new/delete 耗时 | 相对关系   |
|--------------------|--------|--------|--------|------------|----------------------|------------|
| 轻量               | 100    | 1      | 10     | ~175 ms    | ~42 ms               | 池更慢     |
| 中量               | 1000   | 1      | 10     | ~1468 ms   | ~409 ms              | 池更慢     |
| 大量               | 10000  | 1      | 5      | ~7665 ms   | ~1880 ms             | 池更慢     |
| 超大量             | 50000  | 1      | 3      | ~22687 ms  | ~5833 ms             | 池更慢     |

在单线程、立即分配立即释放的负载下，v1 内存池在本环境中均慢于系统默认分配器（约 3.6~4.2 倍），可能与实现开销、`clock()` 精度及小规模下系统分配器高度优化有关。v1 更适合作为理解「固定桶 + CAS 空闲链表」的示例，而非强调性能优势。

---

### v2 单元测试与性能测试

**单元测试**：`v2/tests/UnitTest.cpp`。覆盖基础分配、内存写入、多线程分配/释放、边界情况与压力测试，当前全部通过。

**性能测试**：`v2/tests/PerformanceTest.cpp`。先做内存占用对比（见下），再预热，然后在相同负载下对比内存池 `allocate/deallocate` 与系统 `new`/`delete` 的耗时（基于 `std::chrono`）。

**内存占用对比**：在相同分配模式（8 万块、混合 8~1024 B）下，统计内存池从系统（mmap）申请的总字节数（`PageCache::getTotalBytesFromOS()`），与系统分配器堆占用（Linux glibc `mallinfo2().arena`）对比，得到「内存占用降低」比例。本实现不向系统归还 span（无 munmap），故统计的是池在承担该负载时从 OS 拿到的总量，与 glibc arena 可比。

**运行方式**：

```bash
cd v2 && cmake -S . -B build && cmake --build build -j
./build/unit_test    # 单元测试
./build/perf_test    # 性能测试
```

**v2 性能测试场景与结果**：

| 场景说明                     | 主要参数与负载说明                          | 内存池     | 系统分配器   | 相对关系   |
|------------------------------|---------------------------------------------|------------|--------------|------------|
| **内存占用**                 | 8 万块混合 8~1024 B，峰值从 OS 申请量       | ~20064 KB  | ~22636 KB    | **池约低 11%** |
| 小对象单线程                 | 50000 次分配，固定小尺寸 8/16/32/64/128/256 B | ~4~6 ms    | ~2.7~3.4 ms  | 池略慢     |
| 多线程分配                   | 4 线程，每线程 25000 次分配/释放            | ~10~12 ms  | ~12~13 ms    | 池约快 14% |
| 混合大小单线程               | 100000 次分配，多种固定小尺寸               | ~3.7~4.6 ms| ~6.2 ms     | 池约快 41% |

v2 在**多线程**与**混合大小、单线程**场景下均表现出优于系统默认分配器的趋势，与 ThreadCache 减少锁争用、按大小类批量管理等设计相符；在相同负载下**从系统申请的内存总量较 glibc 堆占用约低 11%**，有利于降低碎片与占用。单线程纯小对象场景下略慢，可能与测试规模与本地缓存行为有关。

---

### 性能小结

- **v1**：单线程多规模基准表明，在当前测试负载下内存池慢于系统 new/delete；多线程下存在稳定性问题，未作为性能结论。
- **v2**：单测全部通过；在多线程与混合大小分配场景下，相对系统 new/delete 有可见优势（约 14%、41%）；在 8 万块混合小对象负载下，**内存占用较系统分配器（glibc arena）约低 11%**。适合在简历或文档中概括为「多线程与混合大小场景下性能优于系统分配器，内存占用约低 11%」。

---

## 未来研究方向

- [ ] 为 v2 增加“指针→span/size class”映射，支持 `deallocate(ptr)` 无需 size
- [ ] 把 `PageCache` 的 `std::map` 改为更低开销的数据结构（例如分桶数组/伙伴系统）
- [ ] 更严格的 benchmark（固定 CPU、禁用 Turbo、使用 `google/benchmark` 等）

---


#include "PageCache.h"
#include <sys/mman.h>
#include <cstring>

namespace Kama_memoryPool
{

// 分配一段连续 numPages 页，返回起始地址；优先用已有空闲 span，不够再向系统要。
// 核心动作：锁 → lower_bound 找 ≥ numPages 的桶 → 取链表头 → 若过大则切分后半段挂回 freeSpans_ → spanMap_ 登记 → 返回 pageAddr；若无则 systemAlloc → 建 Span → 登记 → 返回。
void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_); // 保证对 freeSpans_、spanMap_ 的修改是原子的

    // 查找合适的空闲span
    // lower_bound函数返回第一个大于等于numPages的元素的迭代器
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span* span = it->second;

        // 将取出的span从原有的空闲链表freeSpans_[it->first]中移除
        if (span->next)
        {
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it);
        }

        // 如果span大于需要的numPages则进行分割
        if (span->numPages > numPages) 
        {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + 
                                numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // 将超出部分放回空闲Span*列表头部
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            span->numPages = numPages;
        }

        // 记录span信息用于回收
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // 没有合适的span，向系统申请
    void* memory = systemAlloc(numPages);
    if (!memory) return nullptr;

    // 创建新的span
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    // 记录span信息用于回收
    spanMap_[memory] = span;
    return memory;
}

// 把从 ptr 开始的 numPages 页还回 PageCache，并尝试与后面相邻的空闲 span 合并，再挂回空闲链表。
// 核心动作：锁 → spanMap_.find(ptr) 得 Span → 算 nextAddr，若在 spanMap_ 则从 freeSpans_ 摘掉 nextSpan → 合并到当前 span，erase + delete nextSpan → 按 span->numPages 头插回 freeSpans_。
void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找对应的span，没找到代表不是PageCache分配的内存，直接返回
    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) return;

    Span* span = it->second;

    // 尝试合并相邻的span
    void* nextAddr = static_cast<char*>(ptr) + numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);
    
    if (nextIt != spanMap_.end())
    {
        Span* nextSpan = nextIt->second;
        
        // 1. 首先检查nextSpan是否在空闲链表中
        bool found = false;
        auto& nextList = freeSpans_[nextSpan->numPages];
        
        // 检查是否是头节点
        if (nextList == nextSpan)
        {
            nextList = nextSpan->next;
            found = true;
        }
        else if (nextList) // 只有在链表非空时才遍历
        {
            Span* prev = nextList;
            while (prev->next)
            {
                if (prev->next == nextSpan)
                {   
                    // 将nextSpan从空闲链表中移除
                    prev->next = nextSpan->next;
                    found = true;
                    break;
                }
                prev = prev->next;
            }
        }

        // 2. 只有在找到nextSpan的情况下才进行合并
        if (found)
        {
            // 合并span
            span->numPages += nextSpan->numPages;
            spanMap_.erase(nextAddr);
            delete nextSpan;
        }
    }

    // 将合并后的span通过头插法插入空闲列表
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
}

// 向系统申请 numPages 页的连续内存。
// 核心动作：numPages * PAGE_SIZE → mmap → 失败返回 nullptr → memset 清零 → 返回 ptr。
void* PageCache::systemAlloc(size_t numPages)
{
    size_t size = numPages * PAGE_SIZE;

    // 使用mmap分配内存
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); // mmap：匿名、私有、可读写，不依赖文件；失败返回 MAP_FAILED，这里转为 nullptr。
    if (ptr == MAP_FAILED) return nullptr;

    totalBytesFromOS_ += size;

    // 清零内存
    memset(ptr, 0, size); // memset：整块清零后再返回，避免未初始化内存。
    return ptr;
}

} // namespace memoryPool
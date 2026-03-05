#include "../include/ThreadCache.h"
#include "../include/CentralCache.h"

namespace Kama_memoryPool
{

void* ThreadCache::allocate(size_t size)
{
    // 处理0大小的分配请求
    if (size == 0)
    {
        size = ALIGNMENT; // 至少分配一个对齐大小
    }
    
    if (size > MAX_BYTES)
    {
        // 大对象直接从系统分配
        return malloc(size);
    }

    size_t index = SizeClass::getIndex(size);
    
    // 更新对应自由链表的长度计数
    freeListSize_[index]--; // 这个更新计数放在后面也合适，因为，无论是空闲链表有块直接取，还是从Central取一批内存之后再取块，都会使数量少一块。
    
    // 检查线程本地自由链表

    // 结构：
//     freeList_[index]  ──→  块A  ──→  块B  ──→  块C  ──→  nullptr
//          (头指针)           ↑         ↑         ↑
//                          块A的前8字节 块B的前8字节 块C的前8字节
//                          存的是块B的地址 存的是块C的地址 存的是nullptr
// freeList_[index] 存的是链表头，即第一个空闲块的地址，每个空闲块自己的前 8 字节里存的才是 next（下一个空闲块的地址）。

    // 如果 freeList_[index] 不为空，表示该链表中有可用内存块。
    if (void* ptr = freeList_[index]) // 取出的ptr是目标块的链表头，也就是前8字节，存的是下一个块的地址
    {
        freeList_[index] = *reinterpret_cast<void**>(ptr); // 【重要】C++ 不允许对 void* 做解引用，写 *ptr 会报错。所以需要用 reinterpret_cast 转换为 void** 类型，然后再解引用。
        // 此时原index位置的块已经变为了目标块的下一个块了，freeList_这个桶数组也已经更新了。
        return ptr;
    }
    
    // 如果线程本地自由链表为空，则从中心缓存获取一批内存，再从中返回一块。
    return fetchFromCentralCache(index);
}

void ThreadCache::deallocate(void* ptr, size_t size)
{
    if (size > MAX_BYTES)
    {
        free(ptr); // 大对象直接free
        return;
    }

    size_t index = SizeClass::getIndex(size);

    // 插入到线程本地自由链表
    // 执行前：freeList_[index] → 块A → 块B → nullptr
    // 执行后：freeList_[index] → ptr → 块A → 块B → nullptr
    // 对于传入的ptr，它是一整块的起始地址，*reinterpret_cast<void**>(ptr) 只是在读写这块里的前 8 字节，而我们约定好了ptr的结构，故 *reinterpret_cast<void**>(ptr)相当于 ptr->next
    *reinterpret_cast<void**>(ptr) = freeList_[index]; // ptr 的前 8 字节里存了“块A 的地址”（原来的头），相当于 ptr->next = 块A
    freeList_[index] = ptr; // 把这条链表的头改成 ptr，即新头是刚还回来的这块。

    // 更新对应自由链表的长度计数
    freeListSize_[index]++; 

    // 判断是否需要将部分内存回收给中心缓存
    if (shouldReturnToCentralCache(index)) // 若为true，例如该链表块数 > 256，则把多出来的部分还回 CentralCache。
    {
        returnToCentralCache(freeList_[index], size);
    }
}

// 判断是否需要将内存回收给中心缓存
bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    // 设定阈值，例如：当自由链表的大小超过一定数量时
    size_t threshold = 256; 
    return (freeListSize_[index] > threshold);
}

void* ThreadCache::fetchFromCentralCache(size_t index)
{
    // 从中心缓存批量获取内存
    void* start = CentralCache::getInstance().fetchRange(index);
    if (!start) return nullptr;

    // 从这串里拿出第一块作为本次 allocate 的返回值；再接到自由链表 freeList_[index] 上（即 freeList_[index] = *(void**)start）。
    void* result = start;
    freeList_[index] = *reinterpret_cast<void**>(start);
    
    // 更新自由链表大小
    size_t batchNum = 0;
    void* current = start; // 从start开始遍历

    // 计算从中心缓存获取的内存块数量
    while (current != nullptr)
    {
        batchNum++;
        current = *reinterpret_cast<void**>(current); // 遍历下一个内存块
    }

    // 更新freeListSize_，增加获取的内存块数量
    freeListSize_[index] += batchNum;
    
    return result;
}

// ThreadCache 只把「多出来的」块还给 CentralCache，不会把本线程常用的块都还掉
void ThreadCache::returnToCentralCache(void* start, size_t size)
{
    // 根据大小计算对应的索引
    size_t index = SizeClass::getIndex(size);

    // 获取对齐后的实际块大小
    size_t alignedSize = SizeClass::roundUp(size);

    // 计算要归还内存块数量
    size_t batchNum = freeListSize_[index];
    if (batchNum <= 1) return; // 如果只有一个块，则不归还

    // 保留策略：保留约 1/4（至少 1 块）在ThreadCache中
    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t returnNum = batchNum - keepNum;

    // 将内存块串成链表
    char* current = static_cast<char*>(start);

    // 使用对齐后的大小计算分割点
    char* splitNode = current;
    for (size_t i = 0; i < keepNum - 1; ++i) 
    {
        splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
        if (splitNode == nullptr) 
        {
            // 如果链表提前结束，更新实际的返回数量
            returnNum = batchNum - (i + 1);
            break;
        }
    }

    if (splitNode != nullptr) 
    {
        // 从 start 出发沿链表走 keepNum - 1 步，找到「保留部分」的最后一块 splitNode，把链表从这里断开。
        void* nextNode = *reinterpret_cast<void**>(splitNode);
        *reinterpret_cast<void**>(splitNode) = nullptr; // 断开连接

        // 更新ThreadCache的空闲链表
        freeList_[index] = start;

        // 更新自由链表大小
        freeListSize_[index] = keepNum;

        // 将剩余部分返回给CentralCache
        if (returnNum > 0 && nextNode != nullptr)
        {
            CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
        }
    }
}

} // namespace memoryPool
#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <thread>
#include <chrono>

namespace Kama_memoryPool
{

const std::chrono::milliseconds CentralCache::DELAY_INTERVAL{1000};

// 每次从PageCache获取span大小（以页为单位）
static const size_t SPAN_PAGES = 8;

CentralCache::CentralCache()
{
    for (auto& ptr : centralFreeList_)
    {
        ptr.store(nullptr, std::memory_order_relaxed);
    }
    for (auto& lock : locks_)
    {
        lock.clear();
    }
    // 初始化延迟归还相关的成员变量
    for (auto& count : delayCounts_)
    {
        count.store(0, std::memory_order_relaxed);
    }
    for (auto& time : lastReturnTimes_)
    {
        time = std::chrono::steady_clock::now();
    }
    spanCount_.store(0, std::memory_order_relaxed);
}

// 给 ThreadCache 提供一块内存，返回的只是一块；这一块可能来自「原来就有的链表」，也可能来自「刚从 PageCache 要来的新 span 并切好的链表」。
void* CentralCache::fetchRange(size_t index)
{
    // 索引检查，当索引大于等于FREE_LIST_SIZE时，说明申请内存过大应直接向系统申请
    if (index >= FREE_LIST_SIZE) 
        return nullptr;

    // 自旋锁保护
    while (locks_[index].test_and_set(std::memory_order_acquire)) // 对 locks_[index] 自旋 test_and_set，拿不到就 yield
    {
        std::this_thread::yield(); // 添加线程让步，避免忙等待，避免过度消耗CPU
    }

    void* result = nullptr;
    try 
    {
        // 尝试从中心缓存获取内存块
        result = centralFreeList_[index].load(std::memory_order_relaxed);

        if (!result)
        {
            // 如果中心缓存为空，从页缓存获取新的内存块
            size_t size = (index + 1) * ALIGNMENT;
            result = fetchFromPageCache(size);

            if (!result)
            {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            // 将获取的内存块切分成小块
            char* start = static_cast<char*>(result);

            // 计算实际分配的页数
            size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE) ? 
                             SPAN_PAGES : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
            // 使用实际页数计算块数
            size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;
            
            if (blockNum > 1) 
            {  // 确保至少有两个块才构建链表
                for (size_t i = 1; i < blockNum; ++i) 
                {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;
                
                // 保存result的下一个节点
                void* next = *reinterpret_cast<void**>(result);
                // 将result与链表断开
                *reinterpret_cast<void**>(result) = nullptr;
                // 更新中心缓存
                centralFreeList_[index].store(
                    next, 
                    std::memory_order_release
                );
                
                // 使用无锁方式记录span信息
                // 做记录是为了将中心缓存多余内存块归还给页缓存做准备。考虑点：
                // 1.CentralCache 管理的是小块内存，这些内存可能不连续
                // 2.PageCache 的 deallocateSpan 要求归还连续的内存
                size_t trackerIndex = spanCount_++;
                if (trackerIndex < spanTrackers_.size())
                {
                    spanTrackers_[trackerIndex].spanAddr.store(start, std::memory_order_release);
                    spanTrackers_[trackerIndex].numPages.store(numPages, std::memory_order_release);
                    spanTrackers_[trackerIndex].blockCount.store(blockNum, std::memory_order_release); // 共分配了blockNum个内存块
                    spanTrackers_[trackerIndex].freeCount.store(blockNum - 1, std::memory_order_release); // 第一个块result已被分配出去，所以初始空闲块数为blockNum - 1
                }
            }
        } 
        else 
        {
            // 保存result的下一个节点
            void* next = *reinterpret_cast<void**>(result);
            // 将result与链表断开
            *reinterpret_cast<void**>(result) = nullptr;
            
            // 更新中心缓存
            centralFreeList_[index].store(next, std::memory_order_release);

             // 更新span的空闲计数
            SpanTracker* tracker = getSpanTracker(result);
            if (tracker)
            {
                // 减少一个空闲块
                tracker->freeCount.fetch_sub(1, std::memory_order_release);
            }
        }
    }
    catch (...) 
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    // 释放锁
    locks_[index].clear(std::memory_order_release);
    return result;
}

// ThreadCache 还回一串块（链表从 start 开始，总字节数 size，块大小由 index 决定），把还回来的链表接到中心链表前面。
void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE) 
        return;

    size_t blockSize = (index + 1) * ALIGNMENT;
    size_t blockCount = size / blockSize;    

    while (locks_[index].test_and_set(std::memory_order_acquire)) 
    {
        std::this_thread::yield();
    }

    try 
    {
        // 1. 将归还的链表连接到中心缓存
        void* end = start;
        size_t count = 1;
        while (*reinterpret_cast<void**>(end) != nullptr && count < blockCount) {
            end = *reinterpret_cast<void**>(end);
            count++;
        }
        void* current = centralFreeList_[index].load(std::memory_order_relaxed);
        *reinterpret_cast<void**>(end) = current; // 头插法（将原有链表接在归还链表后边）
        centralFreeList_[index].store(start, std::memory_order_release);
        
        // 2. 更新延迟计数
        size_t currentCount = delayCounts_[index].fetch_add(1, std::memory_order_relaxed) + 1;
        auto currentTime = std::chrono::steady_clock::now();
        
        // 3. 检查是否需要执行延迟归还
        if (shouldPerformDelayedReturn(index, currentCount, currentTime))
        {
            performDelayedReturn(index);
        }
    }
    catch (...) 
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
}

// 检查是否需要执行延迟归还
// 何时执行：该 index 的 delayCounts_ 达到 MAX_DELAY_COUNT（48），或 距离 lastReturnTimes_[index] 已经超过 DELAY_INTERVAL（1 秒）。
bool CentralCache::shouldPerformDelayedReturn(size_t index, size_t currentCount, 
    std::chrono::steady_clock::time_point currentTime)
{
    // 基于计数和时间的双重检查
    if (currentCount >= MAX_DELAY_COUNT)
    {
        return true;
    }
    
    auto lastTime = lastReturnTimes_[index];
    return (currentTime - lastTime) >= DELAY_INTERVAL;
}

// 执行延迟归还
void CentralCache::performDelayedReturn(size_t index)
{
    // 重置延迟计数
    delayCounts_[index].store(0, std::memory_order_relaxed);
    // 更新最后归还时间
    lastReturnTimes_[index] = std::chrono::steady_clock::now();
    
    // 统计每个span的空闲块数
    std::unordered_map<SpanTracker*, size_t> spanFreeCounts;
    void* currentBlock = centralFreeList_[index].load(std::memory_order_relaxed);
    
    while (currentBlock)
    {
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if (tracker)
        {
            spanFreeCounts[tracker]++;
        }
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }
    
    // 更新每个span的空闲计数并检查是否可以归还
    for (const auto& [tracker, newFreeBlocks] : spanFreeCounts)
    {
        updateSpanFreeCount(tracker, newFreeBlocks, index);
    }
}

// 1. 把该 span 的 freeCount 加上 newFreeBlocks（这次延迟归还扫描时，该 span 在中心链表里的块数）。
// 2. 若更新后 freeCount == blockCount，说明这个 span 的所有块都回到中心且空闲，可以整段还给 PageCache，这样 CentralCache 不会长期攥着「已经完全空闲」的 span，可以还给 PageCache 复用。
void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index)
{
    size_t oldFreeCount = tracker->freeCount.load(std::memory_order_relaxed);
    size_t newFreeCount = oldFreeCount + newFreeBlocks;
    tracker->freeCount.store(newFreeCount, std::memory_order_release);
    
    // 如果所有块都空闲，归还span
    if (newFreeCount == tracker->blockCount.load(std::memory_order_relaxed))
    {
        void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
        size_t numPages = tracker->numPages.load(std::memory_order_relaxed);
        
        // 从自由链表中移除这些块
        void* head = centralFreeList_[index].load(std::memory_order_relaxed);
        void* newHead = nullptr;
        void* prev = nullptr;
        void* current = head;
        
        while (current)
        {
            void* next = *reinterpret_cast<void**>(current);
            if (current >= spanAddr && 
                current < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE)
            {
                if (prev)
                {
                    *reinterpret_cast<void**>(prev) = next;
                }
                else
                {
                    newHead = next;
                }
            }
            else
            {
                prev = current;
            }
            current = next;
        }
        
        centralFreeList_[index].store(newHead, std::memory_order_release);
        PageCache::getInstance().deallocateSpan(spanAddr, numPages);
    }
}

// 小对象：size <= SPAN_PAGES * PAGE_SIZE（例如 8 页 × 4KB = 32KB）时，固定向 PageCache 要 SPAN_PAGES（8）页。
// 大对象：按 size 向上取整到页数，要 numPages 页。
// 返回值是 span 的起始地址（即第一块的地址）；在 fetchRange 里会按块大小把整段切成链表。
void* CentralCache::fetchFromPageCache(size_t size)
{   
    // 1. 计算实际需要的页数
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    // 2. 根据大小决定分配策略
    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE) 
    {
        // 小于等于32KB的请求，使用固定8页
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    } 
    else 
    {
        // 大于32KB的请求，按实际需求分配
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

// 用于：取走一块时减少该 span 的 freeCount；延迟归还时统计每个 span 在链上有多少块、并决定是否还 span。
// 做法：遍历 spanTrackers_[0 .. spanCount_)，看 blockAddr 是否落在某个 span 的 [spanAddr, spanAddr + numPages*PAGE_SIZE) 区间内。
SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    // 遍历spanTrackers_数组，找到blockAddr所属的span
    for (size_t i = 0; i < spanCount_.load(std::memory_order_relaxed); ++i)
    {
        void* spanAddr = spanTrackers_[i].spanAddr.load(std::memory_order_relaxed);
        size_t numPages = spanTrackers_[i].numPages.load(std::memory_order_relaxed);
        
        if (blockAddr >= spanAddr && 
            blockAddr < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE)
        {
            return &spanTrackers_[i];
        }
    }
    return nullptr;
}

// 和 ThreadCache 的衔接：
// ThreadCache::allocate 本地链表空时，调 CentralCache::getInstance().fetchRange(index) 拿一块（或触发从 PageCache 要 span 并切块）。
// ThreadCache::deallocate 在满足条件时调 CentralCache::getInstance().returnRange(...) 把一批块还回中心；中心再通过延迟归还把「整 span 都空闲」的 span 还给 PageCache。

} // namespace memoryPool
#pragma once
#include "Common.h"
#include <mutex>
#include <unordered_map>
#include <array>
#include <atomic>
#include <chrono>

namespace Kama_memoryPool
{

// 使用无锁的span信息存储
struct SpanTracker {
    std::atomic<void*> spanAddr{nullptr}; // 这段span的起始地址
    std::atomic<size_t> numPages{0}; // 这段span有多少页
    std::atomic<size_t> blockCount{0}; // 从该 span 切出来的块总数
    std::atomic<size_t> freeCount{0}; // 当前还有多少块在 CentralCache 的自由链表里是空闲的；当 freeCount == blockCount 时，整段 span 可以归还给 PageCache
};

// CentralCache
// 1. 全局单例（所有线程共享），介于 ThreadCache 和 PageCache 之间。
// 2. 从 PageCache 按 span（多页） 要内存，切成固定大小的小块挂到对应 size class 的自由链表上。
// 3. ThreadCache 缺块时向 CentralCache 要一批；ThreadCache 多出来的块可以还回 CentralCache。
// 4. 用 per-index 自旋锁 保护每条自由链表，避免多线程竞争。
class CentralCache
{
public:
    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    void* fetchRange(size_t index);
    void returnRange(void* start, size_t size, size_t index);

private:
    // 相互是还所有原子指针为nullptr
    CentralCache();
    // 从页缓存获取内存
    void* fetchFromPageCache(size_t size);

    // 获取span信息
    SpanTracker* getSpanTracker(void* blockAddr);

    // 更新span的空闲计数并检查是否可以归还
    void updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index);

private:
    // 中心缓存的自由链表
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

    // 用于同步的自旋锁
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
    
    // 使用数组存储span信息，避免map的开销
    std::array<SpanTracker, 1024> spanTrackers_;
    std::atomic<size_t> spanCount_{0};

    // 延迟归还相关的成员变量
    static const size_t MAX_DELAY_COUNT = 48;  // 最大延迟计数
    std::array<std::atomic<size_t>, FREE_LIST_SIZE> delayCounts_;  // 每个大小类的延迟计数
    std::array<std::chrono::steady_clock::time_point, FREE_LIST_SIZE> lastReturnTimes_;  // 上次归还时间
    static const std::chrono::milliseconds DELAY_INTERVAL;  // 延迟间隔

    bool shouldPerformDelayedReturn(size_t index, size_t currentCount, std::chrono::steady_clock::time_point currentTime);
    void performDelayedReturn(size_t index);
};

} // namespace memoryPool
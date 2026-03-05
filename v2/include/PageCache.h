#pragma once
#include "Common.h"
#include <map>
#include <mutex>

namespace Kama_memoryPool
{

// 1. 全局单例，整个内存池里唯一直接向系统要/还内存的一层。
// 2. 以 页（4KB） 为单位管理，分配/释放的单位是 span（连续多页）。
// 3. CentralCache 缺内存时向 PageCache 要 span；CentralCache 整 span 空闲时通过 deallocateSpan 还回来。
// 4. 用一把互斥锁保护所有操作（只有 CentralCache 会调，锁竞争相对可控）。
class PageCache
{
public:
    static const size_t PAGE_SIZE = 4096; // 4K页大小

    static PageCache& getInstance()
    {
        static PageCache instance;
        return instance;
    }

    // 分配指定页数的span
    void* allocateSpan(size_t numPages);

    // 释放span
    void deallocateSpan(void* ptr, size_t numPages);

private:
    PageCache() = default;

    // 向系统申请内存
    void* systemAlloc(size_t numPages); 
private:
    // Span 本身是堆上对象，用 pageAddr 和 numPages 描述「一段连续 numPages 页」的元信息
    struct Span
    {
        void*  pageAddr; // 页起始地址
        size_t numPages; // 页数
        Span*  next;     // 链表 next，用于挂在 freeSpans_[n] 里
    };

    // 按页数管理空闲span，不同页数对应不同Span链表
    std::map<size_t, Span*> freeSpans_;
    // 页号到span的映射，用于回收
    std::map<void*, Span*> spanMap_;
    std::mutex mutex_;
};

} // namespace memoryPool
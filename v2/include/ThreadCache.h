#pragma once
#include "Common.h"

namespace Kama_memoryPool 
{

// 线程本地缓存
// 每个线程一个 ThreadCache，内部按「大小类」用 freeList_ + freeListSize_ 管理一批小对象块。
class ThreadCache
{
public:
    static ThreadCache* getInstance()
    {
        static thread_local ThreadCache instance; // 每个线程一个实例
        return &instance; // 返回该实例的地址
    }

    void* allocate(size_t size); // 分配内存
    void deallocate(void* ptr, size_t size); // 释放内存
private:
    ThreadCache() 
    {
        // 初始化自由链表和大小统计
        freeList_.fill(nullptr); // 初始化自由链表为空
        freeListSize_.fill(0); // 初始化自由链表大小统计为0
    }
    
    // 从中心缓存获取内存
    void* fetchFromCentralCache(size_t index); // 从中心缓存获取内存
    // 归还内存到中心缓存
    void returnToCentralCache(void* start, size_t size); // 归还内存到中心缓存

    bool shouldReturnToCentralCache(size_t index); // 判断是否需要归还内存到中心缓存
private:
    // 每个线程的自由链表数组
    std::array<void*, FREE_LIST_SIZE>  freeList_; // 自由链表数组
    std::array<size_t, FREE_LIST_SIZE> freeListSize_; // 自由链表大小统计数组   
};

} // namespace memoryPool
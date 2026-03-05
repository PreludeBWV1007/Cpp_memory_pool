#pragma once 

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>

namespace Kama_memoryPool
{
#define MEMORY_POOL_NUM 64 // 64个内存池，池的内存槽大小是8、16、24...512（64*8），申请分配时向上取整
#define SLOT_BASE_SIZE 8
#define MAX_SLOT_SIZE 512


/* 具体内存池的槽大小没法确定，因为每个内存池的槽大小不同(8的倍数)
   所以这个槽结构体的sizeof 不是实际的槽大小 */
// 这是最基础的内存分配单元，槽，里面只放一个指针，空闲时用 next 串成链表
struct Slot
{
    std::atomic<Slot*> next; // 原子指针
    // next：当这块内存空闲、待在空闲链表里时，next 指向下一个空闲槽；分配出去后，这块内存给用户用，不再用 next 串联（但内存布局没变，回收时又会当链表节点用）。
    // atomic：多线程会同时往空闲链表里塞/取节点，用原子指针才能做无锁的入队/出队。
};

// Kama_memoryPool提供了64个内存池，即64个不同的MemoryPool对象，每个池负责一种槽大小，每个池内部，可以有多个Block，按分配需求增加
class MemoryPool
{
public:
    MemoryPool(size_t BlockSize = 4096);
    ~MemoryPool();
    
    void init(size_t);

    void* allocate();
    void deallocate(void*);
private:
    void allocateNewBlock();
    size_t padPointer(char* p, size_t align);

    // 使用CAS操作进行无锁入队和出队
    bool pushFreeList(Slot* slot);
    Slot* popFreeList();
private:
    int                 BlockSize_; // 内存块大小
    int                 SlotSize_; // 槽大小
    Slot*               firstBlock_; // 指向内存池管理的首个实际内存块
    Slot*               curSlot_; // 当前块里“下一个可以切给用户的槽”的起始位置
    std::atomic<Slot*>  freeList_; // 空闲链表头指针；已经还回来的槽串在这条链表上
    Slot*               lastSlot_; // 当前块里“最后一个合法槽”的边界，超过就要再申请新块
    //std::mutex          mutexForFreeList_; // 保证freeList_在多线程中操作的原子性
    std::mutex          mutexForBlock_; // 保护“申请新块、推进 curSlot_”的互斥锁
// 可以想象：
// 一条链表：Block1 → Block2 → …，每块是一大块连续内存。
// 当前块：正在从某一块里用 curSlot_ 往后切槽，切到 lastSlot_ 就换下一块。
// 另一条链：freeList_ → 槽A → 槽B → …，全是已经用过又还回来的槽。
};

class HashBucket
{
public:
    static void initMemoryPool();
    static MemoryPool& getMemoryPool(int index);

    static void* useMemory(size_t size)
    {
        if (size <= 0)
            return nullptr;
        if (size > MAX_SLOT_SIZE) // 大于512字节的内存，则使用new
            return operator new(size);

        // 相当于size / 8 向上取整（因为分配内存只能大不能小
        return getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).allocate();
    }

    static void freeMemory(void* ptr, size_t size)
    {
        if (!ptr)
            return;
        if (size > MAX_SLOT_SIZE)
        {
            operator delete(ptr);
            return;
        }

        getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).deallocate(ptr);
    }

    template<typename T, typename... Args> 
    friend T* newElement(Args&&... args);
    
    template<typename T>
    friend void deleteElement(T* p);
};

template<typename T, typename... Args>
T* newElement(Args&&... args)
{
    T* p = nullptr;
    // 根据元素大小选取合适的内存池分配内存
    if ((p = reinterpret_cast<T*>(HashBucket::useMemory(sizeof(T)))) != nullptr)
        // 在分配的内存上构造对象
        new(p) T(std::forward<Args>(args)...);

    return p;
}

template<typename T>
void deleteElement(T* p)
{
    // 对象析构
    if (p)
    {
        p->~T();
         // 内存回收
        HashBucket::freeMemory(reinterpret_cast<void*>(p), sizeof(T));
    }
}

} // namespace memoryPool

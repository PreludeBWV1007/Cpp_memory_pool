#include "../include/MemoryPool.h"

// 命名空间的使用：例如标准库是 std::，Boost 是 boost::，你这个内存池如果给别人用，用 Kama_memoryPool:: 很合理。
namespace Kama_memoryPool 
{
MemoryPool::MemoryPool(size_t BlockSize)
    : BlockSize_ (BlockSize)
    , SlotSize_ (0)
    , firstBlock_ (nullptr)
    , curSlot_ (nullptr)
    , freeList_ (nullptr)
    , lastSlot_ (nullptr)
{}

MemoryPool::~MemoryPool()
{
    // 把连续的block删除
    Slot* cur = firstBlock_;
    while (cur)
    {
        Slot* next = cur->next;
        // 等同于 free(reinterpret_cast<void*>(firstBlock_));
        // 转化为 void 指针，因为 void 类型不需要调用析构函数，只释放空间
        operator delete(reinterpret_cast<void*>(cur));
        cur = next;
    }
}

void MemoryPool::init(size_t size)
{
    assert(size > 0);
    SlotSize_ = size;
    firstBlock_ = nullptr;
    curSlot_ = nullptr;
    freeList_ = nullptr;
    lastSlot_ = nullptr;
}

void* MemoryPool::allocate()
{
    // 1. 优先使用空闲链表中的内存槽
    Slot* slot = popFreeList(); // 从空闲链表里弹出一个槽（如果有）。有的话直接返回，不碰 curSlot_、也不申请新块。
    if (slot != nullptr)
        return slot;

    // 2. 从当前块中切，由于设计并发分配内存，所以需要加锁
    Slot* temp;
    {   
        std::lock_guard<std::mutex> lock(mutexForBlock_);
        // A. 当前块已经切完了，调用 allocateNewBlock() 申请一块新的大块，并把 curSlot_ / lastSlot_ 指到新块上。
        if (curSlot_ >= lastSlot_)
        {
            // 当前内存块已无内存槽可用，开辟一块新的内存
            allocateNewBlock();
        }
    
        // B. 当前可用的槽就是 curSlot_ 指向的那块内存。
        temp = curSlot_;
        // 需要让curSlot往后移动一个槽的距离，一个槽占SlotSize_字节，所以槽的个数是SlotSize_ / sizeof(Slot)？？？不懂
        curSlot_ += SlotSize_ / sizeof(Slot);
    }
    
    return temp; 
}

// 释放就是“把这块内存还回池里”，并且希望下次分配时能优先用到它，只做一件事：把它挂回空闲链表
void MemoryPool::deallocate(void* ptr) // 把用户传来的 void* 当成一块“槽”的起始地址，转成 Slot*
{
    if (!ptr) return;

    Slot* slot = reinterpret_cast<Slot*>(ptr);
    pushFreeList(slot); // 把这块槽插回空闲链表（头插）
}

// 申请新块：
// 每次申请新块都会，多一块 Block；同时，curSlot_ 指向这块里第一个可用槽，lastSlot_ 指向这块里“能用的最后一个槽”的界限。
void MemoryPool::allocateNewBlock()
{   
    //std::cout << "申请一块内存块，SlotSize: " << SlotSize_ << std::endl;
    // 头插法插入新的内存块
    void* newBlock = operator new(BlockSize_); // 向系统要一块大小为 BlockSize_ 的连续内存
    reinterpret_cast<Slot*>(newBlock)->next = firstBlock_; // 头插法
    firstBlock_ = reinterpret_cast<Slot*>(newBlock);

    char* body = reinterpret_cast<char*>(newBlock) + sizeof(Slot*); // 新块里真正用来切槽的区域从“块头 + sizeof(Slot)”开始（跳过块头存的 next）。
    // ？？？用 char* 是为了按字节算对齐。
    size_t paddingSize = padPointer(body, SlotSize_); // 希望每个槽的起始地址按 SlotSize_ 对齐，所以要计算需要跳过多少字节才能对齐
    curSlot_ = reinterpret_cast<Slot*>(body + paddingSize); // 第一个可用的槽就从 body + paddingSize 开始，赋给 curSlot_。

    // 超过该标记位置，则说明该内存块已无内存槽可用，需向系统申请新的内存块
    lastSlot_ = reinterpret_cast<Slot*>(reinterpret_cast<size_t>(newBlock) + BlockSize_ - SlotSize_ + 1); // 块内最后一个能放下一整块槽的起始位置
}

// 池在“切槽”时，让每个槽的起始地址都按槽大小的倍数位置对齐
size_t MemoryPool::padPointer(char* p, size_t align) // align：这里就是槽大小 SlotSize_（例如 32），p为某块内存的起始地址
{
    // align 是槽大小
    size_t rem = (reinterpret_cast<size_t>(p) % align);
    return rem == 0 ? 0 : (align - rem);
}

// 【无锁空闲链表：pushFreeList / popFreeList】
// 空闲链表会被多个线程同时“还槽”和“取槽”，用互斥锁会容易成为瓶颈。
// 这个项目里用 CAS（Compare-And-Swap） 实现无锁的栈式链表（头插、头删）。

// CAS无锁头插入队
bool MemoryPool::pushFreeList(Slot* slot)
{
    while (true)
    {
        // 获取当前头节点
        Slot* oldHead = freeList_.load(std::memory_order_relaxed);
        // ？？？memory_order_relaxed是什么，不是自定义的吧？
        // 将新节点的 next 指向当前头节点
        slot->next.store(oldHead, std::memory_order_relaxed);

        // CAS尝试将新节点设置为头节点
        // 若此时 freeList_ 没被别的线程改过，就改成功，返回；否则，继续while-true，用新的 oldHead 重试。
        if (freeList_.compare_exchange_weak(oldHead, slot,
         std::memory_order_release, std::memory_order_relaxed))
        {
            return true;
        }
        // 失败：说明另一个线程可能已经修改了 freeList_
        // CAS 失败则重试
    }
}

// CAS无锁头删出队
Slot* MemoryPool::popFreeList()
{
    while (true)
    {
        Slot* oldHead = freeList_.load(std::memory_order_acquire);
        if (oldHead == nullptr)
            return nullptr; // 队列为空

        // 在访问 newHead 之前再次验证 oldHead 的有效性
        Slot* newHead = nullptr;
        try
        {
            newHead = oldHead->next.load(std::memory_order_relaxed);
        }
        catch(...)
        {
            // 如果返回失败，则continue重新尝试申请内存
            continue;
        }
        
        // 尝试更新头结点
        // 原子性地尝试将 freeList_ 从 oldHead 更新为 newHead
        if (freeList_.compare_exchange_weak(oldHead, newHead,
         std::memory_order_acquire, std::memory_order_relaxed))
        {
            return oldHead;
        }
        // 失败：说明另一个线程可能已经修改了 freeList_
        // CAS 失败则重试
    }
}


void HashBucket::initMemoryPool()
{
    for (int i = 0; i < MEMORY_POOL_NUM; i++)
    {
        getMemoryPool(i).init((i + 1) * SLOT_BASE_SIZE); // 依次是8、16、24...512
    }
}   

// 单例模式：“单例”指的是由 HashBucket 管理的那 64 个 MemoryPool 静态实例，而不是 MemoryPool 类自己提供的单例接口。
// MemoryPool：普通类，负责“一个池子”的 allocate/deallocate，没有 getInstance() 之类的单例接口。
// HashBucket：提供“全局唯一一份”的 64 个 MemoryPool（static MemoryPool memoryPool[MEMORY_POOL_NUM]），通过 getMemoryPool(index) 按索引访问。
// 相当于，如果按照传统的单例模式，那么一个类做单例，那么整个项目中只会有一个单例，而我们现在需要的是64个类似但不同的单例，所以就需要在外部来控制单例。
MemoryPool& HashBucket::getMemoryPool(int index)
{
    static MemoryPool memoryPool[MEMORY_POOL_NUM];
    return memoryPool[index];
}

} // namespace memoryPool


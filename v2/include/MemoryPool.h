#pragma once
#include "ThreadCache.h"

namespace Kama_memoryPool
{

class MemoryPool
{
public:
    static void* allocate(size_t size)
    {
        return ThreadCache::getInstance()->allocate(size); // 静态的getInstance()单例模式返回一个ThreadCache*，然后用它来调它的成员函数allocate(size)
    }

    static void deallocate(void* ptr, size_t size) // ptr是用户传来的内存块起始地址，size是用户传来的内存块大小
    {
        ThreadCache::getInstance()->deallocate(ptr, size);
    }
};

} // namespace memoryPool
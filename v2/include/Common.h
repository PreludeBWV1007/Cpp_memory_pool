#pragma once // 防止头文件被重复包含
#include <cstddef>
#include <atomic>
#include <array>

namespace Kama_memoryPool 
{
// 对齐数和大小定义
constexpr size_t ALIGNMENT = 8; // 内存对齐单位（字节），通常与指针大小一致。
constexpr size_t MAX_BYTES = 256 * 1024; // 256KB，内存池管理的最大单次分配大小，超过则走系统分配。
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // ALIGNMENT等于指针void*的大小，自由链表数组长度，即“有多少种大小类”。
// 从 8 字节起，每 8 字节一档，一直到 256KB，一共 FREE_LIST_SIZE 档。

// 内存块头部信息
// 每个内存块前面会带一个这样的“头”，用来知道块大小、是否在用、在自由链表中时指向下一块，这是一个单链表。
struct BlockHeader
{
    size_t size; // 内存块大小
    bool   inUse; // 使用标志
    BlockHeader* next; // 指向下一个内存块
};

// 大小类管理
class SizeClass 
{
public:
    static size_t roundUp(size_t bytes)
    {
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1); // 7是111，将bytes+7，再将低三位清0
        // 比如，bytes=10，+7后为17，为10001，低三位清除，为10000，16
    }

    static size_t getIndex(size_t bytes)
    {   
        // 确保bytes至少为ALIGNMENT
        bytes = std::max(bytes, ALIGNMENT);
        // 向上取整后-1
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1; // 比如bytes=9，结果为 (9+7)/8 - 1 = 1，index为1，表示对应于自由链表的第1位
    }
};

} // namespace memoryPool
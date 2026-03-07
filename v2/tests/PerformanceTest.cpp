#include "../include/MemoryPool.h"
#include "../include/PageCache.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <iomanip>
#include <thread>
#include <array>
#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

using namespace Kama_memoryPool;
using namespace std::chrono;

// 计时器类
class Timer 
{
    high_resolution_clock::time_point start;
public:
    Timer() : start(high_resolution_clock::now()) {}
    
    double elapsed() 
    {
        auto end = high_resolution_clock::now();
        return duration_cast<microseconds>(end - start).count() / 1000.0; // 转换为毫秒
    }
};

// 性能测试类
class PerformanceTest 
{
private:
    // 测试统计信息
    struct TestStats 
    {
        double memPoolTime{0.0};
        double systemTime{0.0};
        size_t totalAllocs{0};
        size_t totalBytes{0};
    };

public:
    // 1. 系统预热
    static void warmup() 
    {
        std::cout << "Warming up memory systems...\n";
        // 使用 pair 来存储指针和对应的大小
        std::vector<std::pair<void*, size_t>> warmupPtrs;
        
        // 预热内存池
        for (int i = 0; i < 1000; ++i) 
        {
            for (size_t size : {8, 16, 32, 64, 128, 256, 512, 1024}) {
                void* p = MemoryPool::allocate(size);
                warmupPtrs.emplace_back(p, size);  // 存储指针和对应的大小
            }
        }
        
        // 释放预热内存
        for (const auto& [ptr, size] : warmupPtrs) 
        {
            MemoryPool::deallocate(ptr, size);  // 使用实际分配的大小进行释放
        }
        
        std::cout << "Warmup complete.\n\n";
    }

    // 2. 小对象分配测试
    static void testSmallAllocation() 
    {
        constexpr size_t NUM_ALLOCS = 50000;
        // 使用固定的几个小对象大小，这些大小都是内存池最优化的大小
        const size_t SIZES[] = {8, 16, 32, 64, 128, 256};
        const size_t NUM_SIZES = sizeof(SIZES) / sizeof(SIZES[0]);
        
        std::cout << "\nTesting small allocations (" << NUM_ALLOCS 
                  << " allocations of fixed sizes):" << std::endl;
        
        // 测试内存池
        {
            Timer t;
            // 按大小分类存储内存块
            std::array<std::vector<std::pair<void*, size_t>>, NUM_SIZES> sizePtrs;
            for (auto& ptrs : sizePtrs) {
                ptrs.reserve(NUM_ALLOCS / NUM_SIZES);
            }
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                // 循环使用不同大小
                size_t sizeIndex = i % NUM_SIZES;
                size_t size = SIZES[sizeIndex];
                void* ptr = MemoryPool::allocate(size);
                sizePtrs[sizeIndex].push_back({ptr, size});
                
                // 模拟真实使用：部分立即释放
                if (i % 4 == 0) 
                {
                    // 随机选择一个大小类别进行释放
                    size_t releaseIndex = rand() % NUM_SIZES;
                    auto& ptrs = sizePtrs[releaseIndex];
                    
                    if (!ptrs.empty()) 
                    {
                        MemoryPool::deallocate(ptrs.back().first, ptrs.back().second);
                        ptrs.pop_back();
                    }
                }
            }
            
            // 清理剩余内存
            for (auto& ptrs : sizePtrs) 
            {
                for (const auto& [ptr, size] : ptrs) 
                {
                    MemoryPool::deallocate(ptr, size);
                }
            }
            
            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
        
        // 测试new/delete
        {
            Timer t;
            std::array<std::vector<std::pair<void*, size_t>>, NUM_SIZES> sizePtrs;
            for (auto& ptrs : sizePtrs) {
                ptrs.reserve(NUM_ALLOCS / NUM_SIZES);
            }
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                size_t sizeIndex = i % NUM_SIZES;
                size_t size = SIZES[sizeIndex];
                void* ptr = new char[size];
                sizePtrs[sizeIndex].push_back({ptr, size});
                
                if (i % 4 == 0) 
                {
                    size_t releaseIndex = rand() % NUM_SIZES;
                    auto& ptrs = sizePtrs[releaseIndex];
                    
                    if (!ptrs.empty()) 
                    {
                        delete[] static_cast<char*>(ptrs.back().first);
                        ptrs.pop_back();
                    }
                }
            }
            
            for (auto& ptrs : sizePtrs) 
            {
                for (const auto& [ptr, size] : ptrs) 
                {
                    delete[] static_cast<char*>(ptr);
                }
            }
            
            std::cout << "New/Delete: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
    }
    
    // 3. 多线程测试
    static void testMultiThreaded() 
    {
        constexpr size_t NUM_THREADS = 4;
        constexpr size_t ALLOCS_PER_THREAD = 25000;
        
        std::cout << "\nTesting multi-threaded allocations (" << NUM_THREADS 
                  << " threads, " << ALLOCS_PER_THREAD << " allocations each):" 
                  << std::endl;
        
        auto threadFunc = [](bool useMemPool) 
        {
            std::random_device rd;
            std::mt19937 gen(rd());
            
            // 使用固定的几个大小，这样可以更好地测试内存池的复用能力
            const size_t SIZES[] = {8, 16, 32, 64, 128, 256};
            const size_t NUM_SIZES = sizeof(SIZES) / sizeof(SIZES[0]);
            
            // 每个线程维护自己的内存块列表，按大小分类存储
            std::array<std::vector<std::pair<void*, size_t>>, NUM_SIZES> sizePtrs;
            for (auto& ptrs : sizePtrs) {
                ptrs.reserve(ALLOCS_PER_THREAD / NUM_SIZES);
            }
            
            // 模拟真实应用中的内存使用模式
            for (size_t i = 0; i < ALLOCS_PER_THREAD; ++i) 
            {
                // 1. 分配阶段：优先使用ThreadCache
                size_t sizeIndex = i % NUM_SIZES;  // 循环使用不同大小
                size_t size = SIZES[sizeIndex];
                void* ptr = useMemPool ? MemoryPool::allocate(size) 
                                     : new char[size];
                sizePtrs[sizeIndex].push_back({ptr, size});
                
                // 2. 释放阶段：测试内存复用
                if (i % 100 == 0)  // 每100次分配
                {
                    // 随机选择一个大小类别进行批量释放
                    size_t releaseIndex = rand() % NUM_SIZES;
                    auto& ptrs = sizePtrs[releaseIndex];
                    
                    if (!ptrs.empty()) 
                    {
                        // 释放该大小类别中20%-30%的内存块
                        size_t releaseCount = ptrs.size() * (20 + (rand() % 11)) / 100;
                        releaseCount = std::min(releaseCount, ptrs.size());
                        
                        for (size_t j = 0; j < releaseCount; ++j) 
                        {
                            size_t index = rand() % ptrs.size();
                            if (useMemPool) 
                            {
                                MemoryPool::deallocate(ptrs[index].first, ptrs[index].second);
                            } 
                            else 
                            {
                                delete[] static_cast<char*>(ptrs[index].first);
                            }
                            ptrs[index] = ptrs.back();
                            ptrs.pop_back();
                        }
                    }
                }
                
                // 3. 内存压力测试：测试CentralCache的竞争
                if (i % 1000 == 0)  // 每1000次分配
                {
                    // 短暂地分配大量内存，触发CentralCache的竞争
                    std::vector<std::pair<void*, size_t>> pressurePtrs;
                    for (int j = 0; j < 50; ++j) 
                    {
                        size_t size = SIZES[rand() % NUM_SIZES];
                        void* ptr = useMemPool ? MemoryPool::allocate(size)
                                             : new char[size];
                        pressurePtrs.push_back({ptr, size});
                    }
                    
                    // 立即释放这些内存，测试内存池的回收效率
                    for (const auto& [ptr, size] : pressurePtrs) 
                    {
                        if (useMemPool) 
                        {
                            MemoryPool::deallocate(ptr, size);
                        } 
                        else 
                        {
                            delete[] static_cast<char*>(ptr);
                        }
                    }
                }
            }
            
            // 清理所有剩余内存
            for (auto& ptrs : sizePtrs) 
            {
                for (const auto& [ptr, size] : ptrs) 
                {
                    if (useMemPool) 
                    {
                        MemoryPool::deallocate(ptr, size);
                    } 
                    else 
                    {
                        delete[] static_cast<char*>(ptr);
                    }
                }
            }
        };
        
        // 测试内存池
        {
            Timer t;
            std::vector<std::thread> threads;
            
            for (size_t i = 0; i < NUM_THREADS; ++i) 
            {
                threads.emplace_back(threadFunc, true);
            }
            
            for (auto& thread : threads) 
            {
                thread.join();
            }
            
            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
        
        // 测试new/delete
        {
            Timer t;
            std::vector<std::thread> threads;
            
            for (size_t i = 0; i < NUM_THREADS; ++i) 
            {
                threads.emplace_back(threadFunc, false);
            }
            
            for (auto& thread : threads) 
            {
                thread.join();
            }
            
            std::cout << "New/Delete: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
    }
    
    // 4. 混合大小测试
    static void testMixedSizes() 
    {
        constexpr size_t NUM_ALLOCS = 100000;
        // 按照内存池的设计特点，将大小分为三类：
        // 1. 小对象: 适合ThreadCache
        // 2. 中等对象: 适合CentralCache
        // 3. 大对象: 适合PageCache
        
        // 使用固定的测试数据
        const size_t SMALL_SIZES[] = {8, 16, 32, 64, 128};
        const size_t MEDIUM_SIZES[] = {256, 384, 512};
        const size_t LARGE_SIZES[] = {1024, 2048, 4096};
        
        const size_t NUM_SMALL = sizeof(SMALL_SIZES) / sizeof(SMALL_SIZES[0]);
        const size_t NUM_MEDIUM = sizeof(MEDIUM_SIZES) / sizeof(MEDIUM_SIZES[0]);
        const size_t NUM_LARGE = sizeof(LARGE_SIZES) / sizeof(LARGE_SIZES[0]);
        
        std::cout << "\nTesting mixed size allocations (" << NUM_ALLOCS 
                  << " allocations with fixed sizes):" << std::endl;
        
        // 测试内存池
        {
            Timer t;
            // 按大小分类存储内存块
            std::array<std::vector<std::pair<void*, size_t>>, NUM_SMALL + NUM_MEDIUM + NUM_LARGE> sizePtrs;
            for (auto& ptrs : sizePtrs) {
                ptrs.reserve(NUM_ALLOCS / (NUM_SMALL + NUM_MEDIUM + NUM_LARGE));
            }
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                size_t size;
                int category = i % 100;  // 使用循环而不是随机数
                
                if (category < 60) {
                    // 小对象：循环使用固定大小
                    size_t index = (i / 60) % NUM_SMALL;
                    size = SMALL_SIZES[index];
                } else if (category < 90) {
                    // 中等对象：循环使用固定大小
                    size_t index = (i / 30) % NUM_MEDIUM;
                    size = MEDIUM_SIZES[index];
                } else {
                    // 大对象：循环使用固定大小
                    size_t index = (i / 10) % NUM_LARGE;
                    size = LARGE_SIZES[index];
                }
                
                void* ptr = MemoryPool::allocate(size);
                // 计算在sizePtrs中的索引
                size_t ptrIndex = (category < 60) ? (i / 60) % NUM_SMALL :
                                (category < 90) ? NUM_SMALL + (i / 30) % NUM_MEDIUM :
                                NUM_SMALL + NUM_MEDIUM + (i / 10) % NUM_LARGE;
                sizePtrs[ptrIndex].push_back({ptr, size});
                
                // 模拟真实场景：随机释放一些内存
                if (i % 50 == 0) 
                {
                    // 随机选择一个大小类别进行批量释放
                    size_t releaseIndex = rand() % sizePtrs.size();
                    auto& ptrs = sizePtrs[releaseIndex];
                    
                    if (!ptrs.empty()) 
                    {
                        // 释放该大小类别中20%-30%的内存块
                        size_t releaseCount = ptrs.size() * (20 + (rand() % 11)) / 100;
                        releaseCount = std::min(releaseCount, ptrs.size());
                        
                        for (size_t j = 0; j < releaseCount; ++j) 
                        {
                            size_t index = rand() % ptrs.size();
                            MemoryPool::deallocate(ptrs[index].first, ptrs[index].second);
                            ptrs[index] = ptrs.back();
                            ptrs.pop_back();
                        }
                    }
                }
            }
            
            // 清理所有剩余内存
            for (auto& ptrs : sizePtrs) 
            {
                for (const auto& [ptr, size] : ptrs) 
                {
                    MemoryPool::deallocate(ptr, size);
                }
            }
            
            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
        
        // 测试new/delete
        {
            Timer t;
            std::array<std::vector<std::pair<void*, size_t>>, NUM_SMALL + NUM_MEDIUM + NUM_LARGE> sizePtrs;
            for (auto& ptrs : sizePtrs) {
                ptrs.reserve(NUM_ALLOCS / (NUM_SMALL + NUM_MEDIUM + NUM_LARGE));
            }
            
            for (size_t i = 0; i < NUM_ALLOCS; ++i) 
            {
                size_t size;
                int category = i % 100;
                
                if (category < 60) {
                    size_t index = (i / 60) % NUM_SMALL;
                    size = SMALL_SIZES[index];
                } else if (category < 90) {
                    size_t index = (i / 30) % NUM_MEDIUM;
                    size = MEDIUM_SIZES[index];
                } else {
                    size_t index = (i / 10) % NUM_LARGE;
                    size = LARGE_SIZES[index];
                }
                
                void* ptr = new char[size];
                size_t ptrIndex = (category < 60) ? (i / 60) % NUM_SMALL :
                                (category < 90) ? NUM_SMALL + (i / 30) % NUM_MEDIUM :
                                NUM_SMALL + NUM_MEDIUM + (i / 10) % NUM_LARGE;
                sizePtrs[ptrIndex].push_back({ptr, size});
                
                if (i % 50 == 0) 
                {
                    size_t releaseIndex = rand() % sizePtrs.size();
                    auto& ptrs = sizePtrs[releaseIndex];
                    
                    if (!ptrs.empty()) 
                    {
                        size_t releaseCount = ptrs.size() * (20 + (rand() % 11)) / 100;
                        releaseCount = std::min(releaseCount, ptrs.size());
                        
                        for (size_t j = 0; j < releaseCount; ++j) 
                        {
                            size_t index = rand() % ptrs.size();
                            delete[] static_cast<char*>(ptrs[index].first);
                            ptrs[index] = ptrs.back();
                            ptrs.pop_back();
                        }
                    }
                }
            }
            
            for (auto& ptrs : sizePtrs) 
            {
                for (const auto& [ptr, size] : ptrs) 
                {
                    delete[] static_cast<char*>(ptr);
                }
            }
            
            std::cout << "New/Delete: " << std::fixed << std::setprecision(3) 
                      << t.elapsed() << " ms" << std::endl;
        }
    }

    // 5. 内存占用对比：相同分配模式下，内存池从系统申请总量 vs 系统分配器堆占用
    static void testMemoryUsage()
    {
        constexpr size_t NUM_ALLOCS = 80000;
        const size_t SIZES[] = {8, 16, 32, 64, 128, 256, 512, 1024};
        const size_t NUM_SIZES = sizeof(SIZES) / sizeof(SIZES[0]);

        std::cout << "\nMemory usage comparison (peak live: " << NUM_ALLOCS
                  << " blocks, mixed sizes 8~1024 B):" << std::endl;

        size_t poolBytesFromOS = 0;
        {
            std::vector<std::pair<void*, size_t>> ptrs;
            ptrs.reserve(NUM_ALLOCS);
            for (size_t i = 0; i < NUM_ALLOCS; ++i)
            {
                size_t size = SIZES[i % NUM_SIZES];
                void* p = MemoryPool::allocate(size);
                ptrs.emplace_back(p, size);
            }
            poolBytesFromOS = PageCache::getInstance().getTotalBytesFromOS();
            for (const auto& [ptr, size] : ptrs)
                MemoryPool::deallocate(ptr, size);
        }

#if defined(__linux__) && defined(__GLIBC__)
        size_t systemArena = 0;
        {
            std::vector<std::pair<void*, size_t>> ptrs;
            ptrs.reserve(NUM_ALLOCS);
            for (size_t i = 0; i < NUM_ALLOCS; ++i)
            {
                size_t size = SIZES[i % NUM_SIZES];
                void* p = ::malloc(size);
                ptrs.emplace_back(p, size);
            }
            struct mallinfo2 mi = mallinfo2();
            systemArena = mi.arena;  // 系统分配器从 OS 获得的堆总字节数
            for (const auto& [ptr, size] : ptrs)
                ::free(ptr);
        }

        std::cout << "  Memory Pool (from OS): " << (poolBytesFromOS / 1024) << " KB" << std::endl;
        std::cout << "  System allocator (arena): " << (systemArena / 1024) << " KB" << std::endl;
        if (systemArena > 0 && poolBytesFromOS <= systemArena)
        {
            double pct = (1.0 - (double)poolBytesFromOS / (double)systemArena) * 100.0;
            std::cout << "  Memory footprint reduction: " << std::fixed << std::setprecision(1)
                      << pct << "%" << std::endl;
        }
        else if (systemArena > 0)
        {
            double pct = ((double)poolBytesFromOS / (double)systemArena - 1.0) * 100.0;
            std::cout << "  Memory Pool uses " << std::fixed << std::setprecision(1)
                      << pct << "% more than system (same workload)" << std::endl;
        }
#else
        std::cout << "  Memory Pool (from OS): " << (poolBytesFromOS / 1024) << " KB" << std::endl;
        std::cout << "  System comparison skipped (mallinfo2 not available on this platform)" << std::endl;
#endif
    }
};

int main() 
{
    std::cout << "Starting performance tests..." << std::endl;

    // 内存占用对比放在最前执行，保证池从零统计、与系统分配器同一进程内可比
    PerformanceTest::testMemoryUsage();
    
    // 预热系统
    PerformanceTest::warmup();
    
    // 运行测试
    PerformanceTest::testSmallAllocation();
    PerformanceTest::testMultiThreaded();
    PerformanceTest::testMixedSizes();

    return 0;
}
#include <iostream>
#include <thread>
#include <vector>

#include "../include/MemoryPool.h"

using namespace Kama_memoryPool;

// 测试用例：选定了4种大小不同的对象，分别是4、20、40、80字节
class P1 
{
    int id_;
};

class P2 
{
    int id_[5];
};

class P3
{
    int id_[10];
};

class P4
{
    int id_[20];
};

// 单轮次申请释放次数 线程数 轮次
void BenchmarkMemoryPool(size_t ntimes, size_t nworks, size_t rounds)
{
	std::vector<std::thread> vthread(nworks); // 线程池
	size_t total_costtime = 0;
	for (size_t k = 0; k < nworks; ++k) // 创建 nworks 个线程
	{
		vthread[k] = std::thread([&]() { // & 是捕获引用，这样闭包里可以直接用 ntimes、rounds 这些外部变量
			for (size_t j = 0; j < rounds; ++j) // 每个线程跑多少轮，每轮都做ntimes次
			{
				size_t begin1 = clock();
				for (size_t i = 0; i < ntimes; i++) // 每一轮里，P1-P4各分配并释放多少次
				{
                    P1* p1 = newElement<P1>(); // 内存池对外接口
                    deleteElement<P1>(p1);
                    P2* p2 = newElement<P2>();
                    deleteElement<P2>(p2);
                    P3* p3 = newElement<P3>();
                    deleteElement<P3>(p3);
                    P4* p4 = newElement<P4>();
                    deleteElement<P4>(p4);
				}
				size_t end1 = clock();

				total_costtime += end1 - begin1;
			}
		});
	}
	for (auto& t : vthread)
	{
		t.join();
	}
	printf("%lu个线程并发执行%lu轮次，每轮次newElement&deleteElement %lu次，总计花费：%lu ms\n", nworks, rounds, ntimes, total_costtime);
}

// 测试传统的new/delete性能，对比「同一批类型、同一批次数」下，内存池 和 系统默认分配器 的耗时差异。
void BenchmarkNew(size_t ntimes, size_t nworks, size_t rounds)
{
	std::vector<std::thread> vthread(nworks);
	size_t total_costtime = 0;
	for (size_t k = 0; k < nworks; ++k)
	{
		vthread[k] = std::thread([&]() {
			for (size_t j = 0; j < rounds; ++j)
			{
				size_t begin1 = clock();
				for (size_t i = 0; i < ntimes; i++)
				{
                    P1* p1 = new P1;
                    delete p1;
                    P2* p2 = new P2;
                    delete p2;
                    P3* p3 = new P3;
                    delete p3;
                    P4* p4 = new P4;
                    delete p4;
				}
				size_t end1 = clock();
				
				total_costtime += end1 - begin1;
			}
		});
	}
	for (auto& t : vthread)
	{
		t.join();
	}
	printf("%lu个线程并发执行%lu轮次，每轮次malloc&free %lu次，总计花费：%lu ms\n", nworks, rounds, ntimes, total_costtime);
}

int main()
{
    HashBucket::initMemoryPool(); // 使用内存池接口前一定要先调用该函数

	// 多组测试：不同规模 (每轮次数, 线程数, 轮次)。注：v1 多线程下可能存在问题，此处仅做单线程多规模对比
	struct Config { size_t ntimes, nworks, rounds; const char* name; };
	Config configs[] = {
		{ 100, 1, 10, "轻量 1线程 10轮 每轮100次" },
		{ 1000, 1, 10, "中量 1线程 10轮 每轮1000次" },
		{ 10000, 1, 5, "大量 1线程 5轮 每轮10000次" },
		{ 50000, 1, 3, "超大量 1线程 3轮 每轮50000次" },
	};

	for (const auto& c : configs) {
		std::cout << "\n========== " << c.name << " ==========" << std::endl;
		BenchmarkMemoryPool(c.ntimes, c.nworks, c.rounds);
		std::cout << "-------------------------------------------" << std::endl;
		BenchmarkNew(c.ntimes, c.nworks, c.rounds);
	}

	return 0;
}
#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "common.h"
#include "PageCache.h"

class CentralCache {
public:
    static CentralCache& getInstance() {
        static CentralCache instance;
        return instance;
    }

    void* fetchRange(size_t index, size_t batchNum);
    // 从中心缓存获取内存块
    void returnRange(void* start, size_t size, size_t index);  // 归还内存块到中心缓存

private:
    CentralCache()
    {
        for (auto& ptr : centralFreeList_) {
            ptr.store(nullptr);
        }
    }

    void* fetchFromPageCache(size_t size);  // 从 PageCache 获取新的 Span

private:
private:
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;
    std::array<std::mutex, FREE_LIST_SIZE> locks_;//每个链表一个互斥锁
    std::array<std::condition_variable, FREE_LIST_SIZE> cond_vars_;
};




// 每次从PageCache取一批内存块，这一批内存块是8页，那至于每一页多大呢，就是由CentralCache自己决定的
static const size_t SPAN_PAGES = 8;


void* CentralCache::fetchRange(size_t index, size_t batchNum)
{
    // 索引检查，当索引大于等于FREE_LIST_SIZE时，说明申请内存过大应直接向系统申请
    if (index >= FREE_LIST_SIZE || batchNum == 0)
        return nullptr;

    std::unique_lock<std::mutex> lock(locks_[index]);
    size_t size = (index + 1) * ALIGNMENT;

    //循环尝试获取内存块，直到成功或失败
    //第一重检查：CentralCache的自由链表，第二重检查：看能否从PageCache中获取
    //如果都不行，那么我们就用上了条件变量：解锁+等待其它线程释放资源并通知，然后我们再重新进入循环并操作
    while (true)
    {
        void* result = centralFreeList_[index].load(std::memory_order_relaxed);

        // 情况1：中心缓存有足够的内存块
        if (result)
        {
            // 从现有链表中获取指定数量的块
            void* current = result;
            void* prev = nullptr;
            size_t count = 0;

            // 遍历链表，获取batchNum个内存块
            while (current && count < batchNum)
            {
                prev = current;
                current = *reinterpret_cast<void**>(current);
                count++;
            }

            // 如果获取到了足够的内存块
            if (count == batchNum)
            {
                if (prev) // 当前centralFreeList_[index]链表上的内存块大于batchNum时需要用到 
                {
                    *reinterpret_cast<void**>(prev) = nullptr; // 断开链表，让其前那一坨逻辑上独属于向其申请的threadCache
                }

                // 更新中心缓存的链表头
                centralFreeList_[index].store(current, std::memory_order_release);
                return result; // 返回获取的内存块链表
            }

            // 如果中心缓存的内存块不足batchNum个，继续尝试从PageCache获取
        }

        // 情况2：中心缓存为空或内存块不足batchNum，我们就要补充内存块，也就是尝试从PageCache获取新的内存块
        void* newBlocks = fetchFromPageCache(size);
        if (newBlocks)
        {
            // 将从PageCache获取的内存块切分成小块
            char* start = static_cast<char*>(newBlocks);
            size_t totalBlocks = (SPAN_PAGES * PageCache::PAGE_SIZE) / size;//计算从 PageCache 获取的大块内存能被切割成多少个小内存块。

            //确定实际要分配给 ThreadCache 的内存块数量。
            //比如我们之前计算的batchNum是10，这里算出来的totalBlocks是16，那么我们只拿10个内存块供分配。
            //剩下来的6个内存块我们保留在CentralCache的FreeList里面，供接下来别的threadCache使用
            size_t allocBlocks = std::min(batchNum, totalBlocks);

            //从PageCache拿过来的内存块，我们先取前面的allocBlocks个内存块，去构建返回给ThreadCache的内存块链表
            if (allocBlocks > 1)
            {
                // 确保至少有两个块才构建链表
                for (size_t i = 1; i < allocBlocks; ++i)
                {
                    void* current = start + (i - 1) * size;//当前小块的地址
                    void* next = start + i * size;// 下一个小块的地址
                    *reinterpret_cast<void**>(current) = next;// 将当前块的第一个字节位置指向下一块

                }
                *reinterpret_cast<void**>(start + (allocBlocks - 1) * size) = nullptr;//将最后一个块的指针域设为 nullptr，表示链表结束。
            }

            //将从PageCache中申请来的大块内存中，未被当前的ThreadCache取走的那部分剩余内存块，重新组织成一个新的链表
            //并挂载到CentralCache的自由链表中，供其他threadCache后续使用
            if (totalBlocks > allocBlocks)
            {
                void* remainStart = start + allocBlocks * size;
                for (size_t i = allocBlocks + 1; i < totalBlocks; ++i)
                {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (totalBlocks - 1) * size) = nullptr;

                // 将剩余块存入中心缓存
                centralFreeList_[index].store(remainStart, std::memory_order_release);
            }

            // 返回新分配的内存块
            return start;
        }

        // 情况3：PageCache也无法分配，使用条件变量，先解锁这里的资源，然后等待，直到有内存被归还，再重新上锁进行刚才的循环
        cond_vars_[index].wait(lock, [this, index] {
            return centralFreeList_[index].load(std::memory_order_relaxed) != nullptr;
            });
    }
}

void* CentralCache::fetchFromPageCache(size_t size)
{
    // 1. 计算实际需要的页数
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    // 2. 根据大小决定分配策略
    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
    {
        // 小于等于32KB的请求，使用固定8页 （32KB：我们设定的SPAN_PAGES=8，页大小 PAGE_SIZE=4KB，则阈值为 8*4KB=32KB）
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }
    else
    {
        // 大于32KB的请求，按实际需求分配
        return PageCache::getInstance().allocateSpan(numPages);
    }
}


void CentralCache::returnRange(void* start, size_t size, size_t index)
//将归还的内存块（其地址为start），头插至 centralFreeList_[index]这个链表 
{
    if (!start || index >= FREE_LIST_SIZE)
        return;

    std::lock_guard<std::mutex> lock(locks_[index]);

    void* current = centralFreeList_[index].load(std::memory_order_relaxed);
    *reinterpret_cast<void**>(start) = current;
    centralFreeList_[index].store(start, std::memory_order_release);

    cond_vars_[index].notify_one();  // 通知等待的线程
}



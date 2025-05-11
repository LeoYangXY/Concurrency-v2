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

    void* fetchRange(size_t index);      // 从中心缓存获取内存块
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
    std::array<std::mutex, FREE_LIST_SIZE> locks_;
    std::array<std::condition_variable, FREE_LIST_SIZE> cond_vars_;
};




// 每次从PageCache获取span大小（以页为单位）
static const size_t SPAN_PAGES = 8;

void* CentralCache::fetchRange(size_t index)
{
    if (index >= FREE_LIST_SIZE)
        return nullptr;

    std::unique_lock<std::mutex> lock(locks_[index]);
    size_t size = (index + 1) * ALIGNMENT;

    while (true)
    {
        void* result = centralFreeList_[index].load(std::memory_order_relaxed);
        if (result)
        {
            // 有内存，取出并更新链表头
            centralFreeList_[index].store(
                *reinterpret_cast<void**>(result),
                std::memory_order_release
            );
            return result;
        }

        // 链表为空，尝试从PageCache获取
        result = fetchFromPageCache(size);

        if (result)
        {
            // 分割内存块并构建链表
            char* start = static_cast<char*>(result);
            size_t blockNum = (SPAN_PAGES * PageCache::PAGE_SIZE) / size;

            if (blockNum > 1)
            {
                for (size_t i = 1; i < blockNum; ++i)
                {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

                centralFreeList_[index].store(
                    *reinterpret_cast<void**>(result),
                    std::memory_order_release
                );
            }
            else
            {
                centralFreeList_[index].store(nullptr, std::memory_order_release);
            }

            return result;
        }

        // PageCache无法分配，等待直到有内存被归还
        cond_vars_[index].wait(lock, [this, index] {
            return centralFreeList_[index].load(std::memory_order_relaxed) != nullptr;
            });
    }
}
void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE)
        return;

    std::lock_guard<std::mutex> lock(locks_[index]);

    void* current = centralFreeList_[index].load(std::memory_order_relaxed);
    *reinterpret_cast<void**>(start) = current;
    centralFreeList_[index].store(start, std::memory_order_release);

    cond_vars_[index].notify_one();  // 通知等待的线程
}


void* CentralCache::fetchFromPageCache(size_t size)
{
    // 1. 计算实际需要的页数
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    // 2. 根据大小决定分配策略
    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
    {
        // 小于等于32KB的请求，使用固定8页
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }
    else
    {
        // 大于32KB的请求，按实际需求分配
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

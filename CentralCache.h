#pragma once

#include <array>
#include <atomic>
#include "common.h"
#include "PageCache.h"

//可以考虑使用条件变量

class CentralCache {


public:

    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    void* fetchRange(size_t index);
    void returnRange(void* start, size_t size, size_t bytes);

private:
    // 相互是还所有原子指针为nullptr
    CentralCache()
    {
        for (auto& ptr : centralFreeList_)
        {
            ptr.store(nullptr);
        }
        
        for (auto& lock : locks_)
        {
            lock.clear();
        }
    }

    
    void* fetchFromPageCache(size_t size);// 从页缓存获取内存

private:  
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;// 中心缓存的自由链表
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;// 用于同步的自旋锁
};








// 每次从PageCache获取span大小（以页为单位）
static const size_t SPAN_PAGES = 8;

void* CentralCache::fetchRange(size_t index)
{
    // 索引检查，当索引大于等于FREE_LIST_SIZE时，说明申请内存过大应直接向系统申请
    if (index >= FREE_LIST_SIZE)
        return nullptr;

    // 自旋锁保护
    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield(); // 添加线程让步，避免忙等待，避免过度消耗CPU
    }

    void* result = nullptr;
    try
    {
        // 尝试从中心缓存获取内存块
        result = centralFreeList_[index].load(std::memory_order_relaxed);

        if (!result)
        {
            // 如果中心缓存为空，从页缓存获取新的内存块
            size_t size = (index + 1) * ALIGNMENT;
            result = fetchFromPageCache(size);

            if (!result)
            {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            // 将获取的内存块切分成小块
            char* start = static_cast<char*>(result);
            size_t blockNum = (SPAN_PAGES * PageCache::PAGE_SIZE) / size;

            if (blockNum > 1)
            {  // 确保至少有两个块才构建链表
                for (size_t i = 1; i < blockNum; ++i)
                {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

                // 更新中心缓存
                centralFreeList_[index].store(
                    *reinterpret_cast<void**>(result),
                    std::memory_order_release
                );
            }
        }
        else
        {
            // 6. 更新链表头
            centralFreeList_[index].store(
                *reinterpret_cast<void**>(result),
                std::memory_order_release
            );
        }
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    // 7. 释放锁
    locks_[index].clear(std::memory_order_release);
    return result;
}

void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    // 当索引大于等于FREE_LIST_SIZE时，说明内存过大应直接向系统归还
    if (!start || index >= FREE_LIST_SIZE)
        return;

    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    try
    {
        // 将归还的内存块插入到中心缓存的链表头部
        void* current = centralFreeList_[index].load(std::memory_order_relaxed);
        *reinterpret_cast<void**>(start) = current;
        centralFreeList_[index].store(start, std::memory_order_release);
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
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
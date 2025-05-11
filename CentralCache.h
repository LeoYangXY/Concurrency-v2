#pragma once

#include <array>
#include <atomic>
#include "common.h"
#include "PageCache.h"

//���Կ���ʹ����������

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
    // �໥�ǻ�����ԭ��ָ��Ϊnullptr
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

    
    void* fetchFromPageCache(size_t size);// ��ҳ�����ȡ�ڴ�

private:  
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;// ���Ļ������������
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;// ����ͬ����������
};








// ÿ�δ�PageCache��ȡspan��С����ҳΪ��λ��
static const size_t SPAN_PAGES = 8;

void* CentralCache::fetchRange(size_t index)
{
    // ������飬���������ڵ���FREE_LIST_SIZEʱ��˵�������ڴ����Ӧֱ����ϵͳ����
    if (index >= FREE_LIST_SIZE)
        return nullptr;

    // ����������
    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield(); // ����߳��ò�������æ�ȴ��������������CPU
    }

    void* result = nullptr;
    try
    {
        // ���Դ����Ļ����ȡ�ڴ��
        result = centralFreeList_[index].load(std::memory_order_relaxed);

        if (!result)
        {
            // ������Ļ���Ϊ�գ���ҳ�����ȡ�µ��ڴ��
            size_t size = (index + 1) * ALIGNMENT;
            result = fetchFromPageCache(size);

            if (!result)
            {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            // ����ȡ���ڴ���зֳ�С��
            char* start = static_cast<char*>(result);
            size_t blockNum = (SPAN_PAGES * PageCache::PAGE_SIZE) / size;

            if (blockNum > 1)
            {  // ȷ��������������Ź�������
                for (size_t i = 1; i < blockNum; ++i)
                {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

                // �������Ļ���
                centralFreeList_[index].store(
                    *reinterpret_cast<void**>(result),
                    std::memory_order_release
                );
            }
        }
        else
        {
            // 6. ��������ͷ
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

    // 7. �ͷ���
    locks_[index].clear(std::memory_order_release);
    return result;
}

void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    // ���������ڵ���FREE_LIST_SIZEʱ��˵���ڴ����Ӧֱ����ϵͳ�黹
    if (!start || index >= FREE_LIST_SIZE)
        return;

    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    try
    {
        // ���黹���ڴ����뵽���Ļ��������ͷ��
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
    // 1. ����ʵ����Ҫ��ҳ��
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    // 2. ���ݴ�С�����������
    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
    {
        // С�ڵ���32KB������ʹ�ù̶�8ҳ
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }
    else
    {
        // ����32KB�����󣬰�ʵ���������
        return PageCache::getInstance().allocateSpan(numPages);
    }
}
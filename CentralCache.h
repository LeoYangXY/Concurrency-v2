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

    void* fetchRange(size_t index);      // �����Ļ����ȡ�ڴ��
    void returnRange(void* start, size_t size, size_t index);  // �黹�ڴ�鵽���Ļ���

private:
    CentralCache() 
    {
        for (auto& ptr : centralFreeList_) {
            ptr.store(nullptr);
        }
    }

    void* fetchFromPageCache(size_t size);  // �� PageCache ��ȡ�µ� Span

private:
private:
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;
    std::array<std::mutex, FREE_LIST_SIZE> locks_;
    std::array<std::condition_variable, FREE_LIST_SIZE> cond_vars_;
};




// ÿ�δ�PageCache��ȡspan��С����ҳΪ��λ��
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
            // ���ڴ棬ȡ������������ͷ
            centralFreeList_[index].store(
                *reinterpret_cast<void**>(result),
                std::memory_order_release
            );
            return result;
        }

        // ����Ϊ�գ����Դ�PageCache��ȡ
        result = fetchFromPageCache(size);

        if (result)
        {
            // �ָ��ڴ�鲢��������
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

        // PageCache�޷����䣬�ȴ�ֱ�����ڴ汻�黹
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

    cond_vars_[index].notify_one();  // ֪ͨ�ȴ����߳�
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
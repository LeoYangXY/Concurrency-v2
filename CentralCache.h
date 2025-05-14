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
    // �����Ļ����ȡ�ڴ��
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
    std::array<std::mutex, FREE_LIST_SIZE> locks_;//ÿ������һ��������
    std::array<std::condition_variable, FREE_LIST_SIZE> cond_vars_;
};




// ÿ�δ�PageCacheȡһ���ڴ�飬��һ���ڴ����8ҳ��������ÿһҳ����أ�������CentralCache�Լ�������
static const size_t SPAN_PAGES = 8;


void* CentralCache::fetchRange(size_t index, size_t batchNum)
{
    // ������飬���������ڵ���FREE_LIST_SIZEʱ��˵�������ڴ����Ӧֱ����ϵͳ����
    if (index >= FREE_LIST_SIZE || batchNum == 0)
        return nullptr;

    std::unique_lock<std::mutex> lock(locks_[index]);
    size_t size = (index + 1) * ALIGNMENT;

    //ѭ�����Ի�ȡ�ڴ�飬ֱ���ɹ���ʧ��
    //��һ�ؼ�飺CentralCache�����������ڶ��ؼ�飺���ܷ��PageCache�л�ȡ
    //��������У���ô���Ǿ���������������������+�ȴ������߳��ͷ���Դ��֪ͨ��Ȼ�����������½���ѭ��������
    while (true)
    {
        void* result = centralFreeList_[index].load(std::memory_order_relaxed);

        // ���1�����Ļ������㹻���ڴ��
        if (result)
        {
            // �����������л�ȡָ�������Ŀ�
            void* current = result;
            void* prev = nullptr;
            size_t count = 0;

            // ����������ȡbatchNum���ڴ��
            while (current && count < batchNum)
            {
                prev = current;
                current = *reinterpret_cast<void**>(current);
                count++;
            }

            // �����ȡ�����㹻���ڴ��
            if (count == batchNum)
            {
                if (prev) // ��ǰcentralFreeList_[index]�����ϵ��ڴ�����batchNumʱ��Ҫ�õ� 
                {
                    *reinterpret_cast<void**>(prev) = nullptr; // �Ͽ���������ǰ��һ���߼��϶��������������threadCache
                }

                // �������Ļ��������ͷ
                centralFreeList_[index].store(current, std::memory_order_release);
                return result; // ���ػ�ȡ���ڴ������
            }

            // ������Ļ�����ڴ�鲻��batchNum�����������Դ�PageCache��ȡ
        }

        // ���2�����Ļ���Ϊ�ջ��ڴ�鲻��batchNum�����Ǿ�Ҫ�����ڴ�飬Ҳ���ǳ��Դ�PageCache��ȡ�µ��ڴ��
        void* newBlocks = fetchFromPageCache(size);
        if (newBlocks)
        {
            // ����PageCache��ȡ���ڴ���зֳ�С��
            char* start = static_cast<char*>(newBlocks);
            size_t totalBlocks = (SPAN_PAGES * PageCache::PAGE_SIZE) / size;//����� PageCache ��ȡ�Ĵ���ڴ��ܱ��и�ɶ��ٸ�С�ڴ�顣

            //ȷ��ʵ��Ҫ����� ThreadCache ���ڴ��������
            //��������֮ǰ�����batchNum��10�������������totalBlocks��16����ô����ֻ��10���ڴ�鹩���䡣
            //ʣ������6���ڴ�����Ǳ�����CentralCache��FreeList���棬�����������threadCacheʹ��
            size_t allocBlocks = std::min(batchNum, totalBlocks);

            //��PageCache�ù������ڴ�飬������ȡǰ���allocBlocks���ڴ�飬ȥ�������ظ�ThreadCache���ڴ������
            if (allocBlocks > 1)
            {
                // ȷ��������������Ź�������
                for (size_t i = 1; i < allocBlocks; ++i)
                {
                    void* current = start + (i - 1) * size;//��ǰС��ĵ�ַ
                    void* next = start + i * size;// ��һ��С��ĵ�ַ
                    *reinterpret_cast<void**>(current) = next;// ����ǰ��ĵ�һ���ֽ�λ��ָ����һ��

                }
                *reinterpret_cast<void**>(start + (allocBlocks - 1) * size) = nullptr;//�����һ�����ָ������Ϊ nullptr����ʾ���������
            }

            //����PageCache���������Ĵ���ڴ��У�δ����ǰ��ThreadCacheȡ�ߵ��ǲ���ʣ���ڴ�飬������֯��һ���µ�����
            //�����ص�CentralCache�����������У�������threadCache����ʹ��
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

                // ��ʣ���������Ļ���
                centralFreeList_[index].store(remainStart, std::memory_order_release);
            }

            // �����·�����ڴ��
            return start;
        }

        // ���3��PageCacheҲ�޷����䣬ʹ�������������Ƚ����������Դ��Ȼ��ȴ���ֱ�����ڴ汻�黹���������������иղŵ�ѭ��
        cond_vars_[index].wait(lock, [this, index] {
            return centralFreeList_[index].load(std::memory_order_relaxed) != nullptr;
            });
    }
}

void* CentralCache::fetchFromPageCache(size_t size)
{
    // 1. ����ʵ����Ҫ��ҳ��
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    // 2. ���ݴ�С�����������
    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
    {
        // С�ڵ���32KB������ʹ�ù̶�8ҳ ��32KB�������趨��SPAN_PAGES=8��ҳ��С PAGE_SIZE=4KB������ֵΪ 8*4KB=32KB��
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }
    else
    {
        // ����32KB�����󣬰�ʵ���������
        return PageCache::getInstance().allocateSpan(numPages);
    }
}


void CentralCache::returnRange(void* start, size_t size, size_t index)
//���黹���ڴ�飨���ַΪstart����ͷ���� centralFreeList_[index]������� 
{
    if (!start || index >= FREE_LIST_SIZE)
        return;

    std::lock_guard<std::mutex> lock(locks_[index]);

    void* current = centralFreeList_[index].load(std::memory_order_relaxed);
    *reinterpret_cast<void**>(start) = current;
    centralFreeList_[index].store(start, std::memory_order_release);

    cond_vars_[index].notify_one();  // ֪ͨ�ȴ����߳�
}



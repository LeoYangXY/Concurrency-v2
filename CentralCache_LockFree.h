#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include "common.h"
#include "PageCache.h"

// ֧�� ABA ��������Ĵ���ǩָ��
struct TaggedPtr {
    void* ptr;
    unsigned int tag;

    TaggedPtr() : ptr(nullptr), tag(0) {}
    TaggedPtr(void* p, unsigned int t) : ptr(p), tag(t) {}

    bool operator==(const TaggedPtr& other) const {
        return ptr == other.ptr && tag == other.tag;
    }
};

class CentralCache {
public:
    static CentralCache& getInstance() {
        static CentralCache instance;
        return instance;
    }

    void* fetchRange(size_t index, size_t batchNum);
    void returnRange(void* start, size_t size, size_t index);

private:
    CentralCache()
    {
        for (auto& list : centralFreeList_) {
            list.head.store(TaggedPtr(nullptr, 0));
        }
    }

    void* fetchFromPageCache(size_t size);

    struct LockFreeList {
        std::atomic<TaggedPtr> head;  // ʹ�ô���ǩ��ԭ��ָ��
    };

    std::array<LockFreeList, FREE_LIST_SIZE> centralFreeList_;
};

// ÿ�δ�PageCacheȡ8ҳ�ڴ�
static const size_t SPAN_PAGES = 8;



//������̵ı����ǣ�
//�Թ������ݵ��޸�ֻ��ͨ��ԭ�Ӳ����޸ģ��� CAS�����Թ������ݵĶ�ȡ�͵��߳�һ����������ȡ����
//��ԭ�Ӳ���ֻ����"��ռ"״̬�½��У��� CAS �ɹ���  
//���繲������A->B->C->D  ��ǰ�̰߳ѹ��������ͷ����Ϊ��C֮�����ǵ�ǰ�߳��൱�ڶ��������������Ƕ�ռ�ģ�����߳̿�����A��B�ˣ������Ժ͵��߳�һģһ���ش���A��B

void* CentralCache::fetchRange(size_t index, size_t batchNum) {
    // 1. �����Ϸ��Լ��
    if (index >= FREE_LIST_SIZE || batchNum == 0) {
        return nullptr;  // ����Խ�����������Ϊ0ʱֱ�ӷ���
    }

    // 2. ���㵱ǰ������Ӧ���ڴ���С
    size_t size = (index + 1) * ALIGNMENT;  // ALIGNMENT ���ڴ����ֵ���� 8/16��

    // 3. ����ѭ�����ԣ�������̵ĺ���ģʽ��
    while (true) {
        // 4. ��ȡ��ǰ�����ͷ����Ϣ��ԭ�Ӷ���
        LockFreeList& list = centralFreeList_[index];
        TaggedPtr old_head = list.head.load(std::memory_order_acquire);
        void* current = old_head.ptr;  // ��ǰ����ͷָ��
        void* batch_head = current;    // Ҫ���������ͷ
        void* batch_tail = nullptr;    // Ҫ���������β
        size_t count = 0;              // ͳ�ƿ����ڴ������

        // 5. ��������ͳ�ƿ����ڴ�飨���߳��߼���
        while (current && count < batchNum) {
            batch_tail = current;
            current = *reinterpret_cast<void**>(current);  // ͨ���ڴ��ǰ8�ֽڶ�ȡnext
            count++;
        }

        // 6. ���A: �����ڴ��㹻������ batchNum ���ڴ�飩
        if (count == batchNum) {
            // �����µ�����ͷ��ָ��ʣ������
            TaggedPtr new_head{ current, old_head.tag + 1 };

            // 7. ԭ�Ӹ�������ͷ��CAS �������̰߳�ȫ�Ĺؼ���
            if (list.head.compare_exchange_weak(
                old_head, new_head,           // ԭֵ����ֵ
                std::memory_order_release,     // д�����������߳̿ɼ�
                std::memory_order_acquire)) {  // ������ȷ�����������̵߳�д��

                // 8. �ض�������ԭ�Ӳ���������ʱ�Ѷ�ռ�����ڴ�飩!!!!!!!!!!!!!!!!!!!!!!!
                //=======================================================
                //=======================================================
                //���Ｋ�õ�������������̵ĺ���˼�룬��Ȼ��������ݻ��ǹ��������ϵ�
                //���Ǳ���߳��Ѿ��޷���ȡ����һ���ֵ������ˣ���Ϊ���������head���ƶ�������ȥ�ˣ������������Ϊ�ǵ�ǰ�̶߳�ռ��
                if (batch_tail) {
                    *reinterpret_cast<void**>(batch_tail) = nullptr;
                }

                // 9. ���ط�����ڴ������
                return batch_head;
            }
            else {
                // 10. CASʧ�ܣ����������߳��޸ģ���������������
                continue;
            }
        }

        // 11. ���B: �����ڴ治�㣨��Ҫ�� PageCache ��ȡ���ڴ棩
        void* newBlocks = fetchFromPageCache(size);
        if (!newBlocks) {
            continue;  // ��ȡʧ�ܣ�����
        }


        ///////////////����ĺܺõ�������������̵ĺ���˼�룺
        //�̶߳�ռ�����ݣ����·�����ڴ�飩����ֱ�Ӳ���������ԭ���ԣ���
        //�������ݣ�������ͷ������ͨ��CAS ��֤ԭ���ԣ���ֹ���̳߳�ͻ��

        //���磬���������ڴ��ǵ�ǰ�̸߳մ� PageCache ����ģ���δ���뵽��������
        //��ʱû���κ������߳��ܷ�������ڴ棨δ�������������������������ǵ�ǰ�̶߳�ռ�ģ������������д����
        //Ȼ������Ҫ��ʣ������һ����ͷ����CentralCache������
        //������������ǹ������ݣ�����߳̿���ͬʱ������޸�����ͷ��������ǵ�ʹ��CAS������
        //����������ݺ��Լ�Ԥ�ڵ�һ�£��������̻߳�û�д����������������ô��˲��ͷ��ɹ���
        //����������ݺ��Լ�Ԥ�ڵĲ�һ�£���ô�������߳��Ѿ�������������������ˣ���ô�����³��Բ���


        // 12. �з����ڴ�飨���߳��߼���
        char* start = static_cast<char*>(newBlocks);
        size_t totalBlocks = (SPAN_PAGES * PageCache::PAGE_SIZE) / size;
        size_t allocBlocks = std::min(batchNum, totalBlocks);

        // 13. �������� ThreadCache ��������ԭ�Ӳ�����
        if (allocBlocks > 1) {
            for (size_t i = 1; i < allocBlocks; ++i) {
                void* current = start + (i - 1) * size;
                void* next = start + i * size;
                *reinterpret_cast<void**>(current) = next;
            }
            *reinterpret_cast<void**>(start + (allocBlocks - 1) * size) = nullptr;
        }

        // 14. ����ʣ���ڴ�鲢����������Ҫ CAS ԭ�Ӳ�����
        if (totalBlocks > allocBlocks) {
            void* remainStart = start + allocBlocks * size;
            void* remainTail = start + (totalBlocks - 1) * size;
            *reinterpret_cast<void**>(remainTail) = nullptr;

            TaggedPtr remain_old_head;
            TaggedPtr remain_new_head;
            do {
                // 15. ��ȡ��ǰ����ͷ��ԭ�Ӷ���
                remain_old_head = list.head.load(std::memory_order_acquire);

                // 16. ��ʣ������β���ӵ���ǰ����ͷ
                *reinterpret_cast<void**>(remainTail) = remain_old_head.ptr;

                // 17. ����������ͷ�����汾�ţ�
                remain_new_head = { remainStart, remain_old_head.tag + 1 };

                // 18. ԭ�Ӳ���ʣ������CAS ѭ�����ԣ�
            } while (!list.head.compare_exchange_weak(
                remain_old_head, remain_new_head,
                std::memory_order_release, std::memory_order_acquire));
        }

        // 19. �����·�����ڴ������
        return start;
    }
};

void* CentralCache::fetchFromPageCache(size_t size)
{
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE) {
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }
    else {
        return PageCache::getInstance().allocateSpan(numPages);
    }
};

void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE)
        return;

    LockFreeList& list = centralFreeList_[index];
    TaggedPtr old_head = list.head.load(std::memory_order_relaxed);
    TaggedPtr new_head;

    do {
        // ͷ�巨�������ڴ��
        *reinterpret_cast<void**>(start) = old_head.ptr;
        new_head = { start, old_head.tag + 1 };

    } while (!list.head.compare_exchange_weak(
        old_head, new_head,
        std::memory_order_release, std::memory_order_relaxed));
}
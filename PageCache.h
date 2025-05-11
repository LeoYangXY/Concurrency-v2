#pragma once
#include <map>
#include <mutex>
#include "common.h"
#include <cstring>


class PageCache {

public:

    static const size_t PAGE_SIZE = 4096; // 4Kҳ��С

    static PageCache& getInstance()
    {
        static PageCache instance;
        return instance;
    }

    
    void* allocateSpan(size_t numPages);// ����ָ��ҳ����span

    
    void deallocateSpan(void* ptr, size_t numPages);// �ͷ�span


private:
    PageCache() = default;
   
    void* systemAlloc(size_t numPages); // ��ϵͳ�����ڴ�


private:
    struct Span
    {
        void* pageAddr; // ҳ��ʼ��ַ
        size_t numPages; // ҳ��
        Span* next;     // ����ָ��
    };

    
    std::map<size_t, Span*> freeSpans_;// ��ҳ���������span����ͬҳ����Ӧ��ͬSpan����
    std::map<void*, Span*> spanMap_;// ҳ�ŵ�span��ӳ�䣬���ڻ���. ͨ���ڴ�ҳ����ʼ��ַ�����ҵ���Ӧ�� Span ����
    std::mutex mutex_;

	

};



void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // ���Һ��ʵĿ���span
    // lower_bound�������ص�һ�����ڵ���numPages��Ԫ�صĵ�����
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span* span = it->second;

        // ��ȡ����span��ԭ�еĿ�������freeSpans_[it->first]���Ƴ�
        if (span->next)
        {
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it);
        }

        // ���span������Ҫ��numPages����зָ�
        if (span->numPages > numPages)
        {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) +
                numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // ���������ַŻؿ���Span*�б�ͷ��
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            span->numPages = numPages;
        }

        // ��¼span��Ϣ���ڻ���
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // û�к��ʵ�span����ϵͳ����
    void* memory = systemAlloc(numPages);
    if (!memory) return nullptr;

    // �����µ�span
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    // ��¼span��Ϣ���ڻ���
    spanMap_[memory] = span;
    return memory;
}




void* PageCache::systemAlloc(size_t numPages) {
    const size_t size = numPages * PAGE_SIZE;

    // ʹ�ÿ�ƽ̨�� aligned_alloc��C++17��׼��
#if defined(_ISOC11_SOURCE) || (__cplusplus >= 201703L)
    void* ptr = aligned_alloc(PAGE_SIZE, size);
#else
// ���ݾɱ�׼��ʵ��
    void* ptr = nullptr;
#ifdef _WIN32
    ptr = _aligned_malloc(size, PAGE_SIZE);
#else
    if (posix_memalign(&ptr, PAGE_SIZE, size) != 0) {
        ptr = nullptr;
    }
#endif
#endif

    if (ptr) {
        memset(ptr, 0, size);  // �ڴ�����
        return ptr;
    }

    // ���÷�������ͨmalloc�����Ƽ����������ף�
    ptr = malloc(size);
    if (ptr) {
        memset(ptr, 0, size);
        // ���棺δ�����ڴ����Ӱ������
    }
    return ptr;
}
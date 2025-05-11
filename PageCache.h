#pragma once
#include <map>
#include <mutex>
#include "common.h"
#include <cstring>


class PageCache {

public:

    static const size_t PAGE_SIZE = 4096; // 4K页大小

    static PageCache& getInstance()
    {
        static PageCache instance;
        return instance;
    }

    
    void* allocateSpan(size_t numPages);// 分配指定页数的span

    
    void deallocateSpan(void* ptr, size_t numPages);// 释放span


private:
    PageCache() = default;
   
    void* systemAlloc(size_t numPages); // 向系统申请内存


private:
    struct Span
    {
        void* pageAddr; // 页起始地址
        size_t numPages; // 页数
        Span* next;     // 链表指针
    };

    
    std::map<size_t, Span*> freeSpans_;// 按页数管理空闲span，不同页数对应不同Span链表
    std::map<void*, Span*> spanMap_;// 页号到span的映射，用于回收. 通过内存页的起始地址快速找到对应的 Span 对象
    std::mutex mutex_;

	

};



void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找合适的空闲span
    // lower_bound函数返回第一个大于等于numPages的元素的迭代器
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span* span = it->second;

        // 将取出的span从原有的空闲链表freeSpans_[it->first]中移除
        if (span->next)
        {
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it);
        }

        // 如果span大于需要的numPages则进行分割
        if (span->numPages > numPages)
        {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) +
                numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // 将超出部分放回空闲Span*列表头部
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            span->numPages = numPages;
        }

        // 记录span信息用于回收
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // 没有合适的span，向系统申请
    void* memory = systemAlloc(numPages);
    if (!memory) return nullptr;

    // 创建新的span
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    // 记录span信息用于回收
    spanMap_[memory] = span;
    return memory;
}




void* PageCache::systemAlloc(size_t numPages) {
    const size_t size = numPages * PAGE_SIZE;

    // 使用跨平台的 aligned_alloc（C++17标准）
#if defined(_ISOC11_SOURCE) || (__cplusplus >= 201703L)
    void* ptr = aligned_alloc(PAGE_SIZE, size);
#else
// 兼容旧标准的实现
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
        memset(ptr, 0, size);  // 内存清零
        return ptr;
    }

    // 备用方案：普通malloc（不推荐，仅作保底）
    ptr = malloc(size);
    if (ptr) {
        memset(ptr, 0, size);
        // 警告：未对齐内存可能影响性能
    }
    return ptr;
}
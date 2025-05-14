#pragma once
#include <map>
#include <mutex>
#include "common.h"
#include <cstring>
#include <cassert>

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

    
    std::map<size_t, Span*> freeSpans_;// 按页数管理空闲span，不同页数对应不同Span链表，实际存储形式：{key为页数,对应的value为Span链表头指针}
    
    // 页号到span的映射，用于回收. 通过内存页的起始地址快速找到对应的 Span 对象
    //key为void*，代表内存页的起始地址（即 Span::pageAddr），例如 0x1000、0x2000。
    //value为Span*，即指向 Span 对象的指针，这个Span对象会保存该内存块的元信息（起始地址、页数、链表指针等）。
    std::map<void*, Span*> spanMap_;
    
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
        Span* span = it->second;//lt->second其实就是访问lt这个（size_t, Span*）对象的第二个属性，也即Span*。
        //当然我们也可以使用C++17引入的结构化绑定：auto& [pageCount, span] = *it;  

        // 将取出的span从原有的空闲链表freeSpans_[it->first]中移除（因为是一个链表，所以我们把链表头移动到当前这个span的next，或者直接erase
        if (span->next)
        {
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it);
        }

        //如果我们获得的span大于需要的numPages则进行分割
        //就是CentralCache申请了numPages数量的页面，我们取出的是一整个span，
        //然后我们把这个span的前面numsPages页给到CentralCache，剩下来的我们串成一个新的span，然后挂回PageCache里面去
        
        if (span->numPages > numPages)
        {
            // 我们创建新span，然后把剩余的page存到这一个span里面去
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) +
                numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            //把剩余的page组出的span挂回PageCache
            auto& cur_head = freeSpans_[newSpan->numPages];
            newSpan->next = cur_head;
            cur_head = newSpan;
            span->numPages = numPages;
        }

        // 记录span信息用于回收
        // 就是一个span，return给CentralCache之后，逻辑上它是归属于CentralCache的
        // 但是这一个span，物理上就是内存中的一段，每一个指向它的指针都可以管理它
        // 我们在spanMap_中记录，要回收的时候，我们便能通过PageCache的spanMap_找到这一个span，进行操作
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;//CentalCache拿到的是连续 numPages 页的起始地址（span->pageAddr），后续由 CentralCache 自己切割成小块供 ThreadCache 使用
    }

    // 没有合适的span，向系统申请，然后return給CentralCache
    void* memory_address = systemAlloc(numPages);
    if (!memory_address) return nullptr;

    // 为了保证接口的一致性（就是CentralCache接收到PageCache传给它的一块内存是span的形式，才能方便统一地进行划分）
    // 因此我们这里申请下来的内存块，我们要创建为一个新的span
    Span* span = new Span;
    span->pageAddr = memory_address;
    span->numPages = numPages;
    span->next = nullptr;

    // 记录span信息用于回收
    spanMap_[memory_address] = span;
    return memory_address;
}



//我们为什么要专门写一个systemAlloc，而不是直接调用malloc找系统分配呢：因为要满足申请下来的一群page是页对齐的
//内存页对齐指的是分配的内存块的起始地址必须是 页大小（PAGE_SIZE）的整数倍。例如：
//一般情况下我们的页大小通常为 4KB（4096 字节）。
//则对齐的内存地址：0x1000（4096）、0x2000（8192）、0x3000（12288）等。
//未对齐的内存地址：0x1001、0x2003 等（不是 4096 的整数倍）
void* PageCache::systemAlloc(size_t numPages) {
    const size_t size = numPages * PAGE_SIZE;

    void* ptr = _aligned_malloc(size, PAGE_SIZE);//使用 _aligned_malloc进行内存页对齐

    // 分配失败时直接返回 nullptr（不再回退到 malloc）
    if (!ptr) return nullptr;

    // 因为新分配下来的内存，上面写的都是脏数据，我们需要把这一块内存上面的值设为0
    std::memset(ptr, 0, size);
    return ptr;
}



void PageCache::deallocateSpan(void* ptr, size_t numPages) {
    // 1. 加锁保护（线程安全）
    std::lock_guard<std::mutex> lock(mutex_);

    // 2. 校验参数合法性
    if (!ptr || numPages == 0) return;

    // 3. 从 spanMap_ 中查找要释放的 Span
    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) {
        // 非法释放：地址未记录在 spanMap_ 中
        assert(false && "Attempt to deallocate unmanaged memory!");
        return;
    }

    // 4. 获取 Span 对象并移除记录
    Span* span = it->second;
    spanMap_.erase(it);  // 解除管理权

    // 5. 检查前后相邻的 Span 是否空闲，合并以减少碎片
    // 5.1 合并前一个相邻 Span
    void* prevEnd = static_cast<char*>(span->pageAddr) - 1;
    auto prevIt = spanMap_.lower_bound(prevEnd);
    if (prevIt != spanMap_.begin()) {
        --prevIt;
        Span* prevSpan = prevIt->second;
        // 检查是否物理相邻
        if (static_cast<char*>(prevSpan->pageAddr) + prevSpan->numPages * PAGE_SIZE == span->pageAddr) {
            // 合并到前一个 Span
            prevSpan->numPages += span->numPages;
            delete span;      // 释放当前 Span 对象
            span = prevSpan;  // 后续操作基于合并后的 Span
        }
    }

    // 5.2 合并后一个相邻 Span
    void* nextStart = static_cast<char*>(span->pageAddr) + span->numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextStart);
    if (nextIt != spanMap_.end()) {
        Span* nextSpan = nextIt->second;
        // 合并到当前 Span
        span->numPages += nextSpan->numPages;
        spanMap_.erase(nextIt);  // 移除被合并的 Span 记录
        delete nextSpan;         // 释放被合并的 Span 对象
    }

    // 6. 将合并后的 Span 重新插入空闲链表
    auto& list = freeSpans_[span->numPages];
    span->next = list;  // 头插法
    list = span;

    // 7. 重新记录管理权（合并后可能有新地址）
    spanMap_[span->pageAddr] = span;
}
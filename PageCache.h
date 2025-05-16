#pragma once
#include <map>
#include <mutex>
#include "common.h"
#include <cstring>
#include <cassert>

class PageCache {

public:
    static const size_t PAGE_SIZE = 4096; // 4K页大小

    static PageCache& getInstance() {
        static PageCache instance;
        return instance;
    }

    void* allocateSpan(size_t numPages); // 分配指定页数的span
    void deallocateSpan(void* ptr, size_t numPages); // 释放span

private:
    PageCache() = default;
    void* systemAlloc(size_t numPages); // 向系统申请内存

private:
    struct Span {
        void* pageAddr; // 页起始地址
        size_t numPages; // 页数
        Span* next;     // 链表指针
    };

    //针对allocate，用于记录的数据结构及其对应的锁：
    // spanMap_ 中存在的 Span 表示“正在使用”；spanMap_ 中不存在的 Span 表示“空闲”
    // key为void*，代表内存页的起始地址（即 Span::pageAddr），例如 0x1000、0x2000。
    // value为Span*，即指向 Span 对象的指针，这个Span对象会保存该内存块的元信息（起始地址、页数、链表指针等）。
    std::map<void*, Span*> spanMap_;//当然其实这里也可以优化：spanMap_可以考虑使用tbb的并发hashmap，或者是自己使用无锁操作进行处理
    std::mutex map_mutex_; // 用于spanMap_这个全局统计量的全局锁，因为我们在实际操作完毕之后，要对spanMap_进行写操作


    //针对deallocate的合并内存块部分，而新增的用于记录的数据结构及其锁：
    std::map<void*, Span*> free_span_map_; // key: Span 的起始地址，value: Span 指针
    std::mutex free_span_mutex_; // 保护 free_span_map_ 的锁



    //下面是PageCache的自由链表+针对自由链表的锁部分：
    std::map<size_t, Span*> freeSpans_;// freeSpans_ 按页数管理空闲span，不同页数对应不同Span链表

    static constexpr size_t kMaxLockedPages = 128; // 假设最大锁数量，实现分片锁机制
    std::array<std::mutex, kMaxLockedPages> page_locks_;// 针对freeSpans_的细粒度锁


};

void* PageCache::allocateSpan(size_t numPages) {
    // Step 1: 直接根据 numPages 选择锁
    std::unique_lock lock(page_locks_[numPages % kMaxLockedPages]);

    // 查找合适的空闲span
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end()) {
        Span* span = it->second;//其实就是访问lt这个（size_t, Span* ）对象的第二个属性，也即Span* 。
                                //当然我们也可以使用C++17引入的结构化绑定：auto& [pageCount, span] = *it;  

        // 将取出的span从原有的空闲链表freeSpans_[it->first]中移除
        if (span->next) {
            freeSpans_[it->first] = span->next;
        }
        else {
            freeSpans_.erase(it);
        }

        //step2:如果我们获得的span大于需要的numPages则进行分割
        //就是CentralCache申请了numPages数量的页面，我们取出的是一整个span，
        //然后我们把这个span的前面numsPages页给到CentralCache，剩下来的我们串成一个新的span，然后挂回PageCache里面去
        if (span->numPages > numPages) {
            // 创建新span存放剩余page
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // 把剩余的page组出的span挂回PageCache
            size_t target_pages = newSpan->numPages;
            std::unique_lock target_lock(page_locks_[target_pages % kMaxLockedPages]);
            newSpan->next = freeSpans_[target_pages];
            freeSpans_[target_pages] = newSpan;

            // 注意：空闲部分记录进free_span_map_
            {
                std::lock_guard<std::mutex> free_span_lock(free_span_mutex_);
                free_span_map_[newSpan->pageAddr] = newSpan;
            }

            span->numPages = numPages; // 更新当前span的大小
        }

        // 注意：在被使用的部分记录进spanMap_
        // 就是一个span，return给CentralCache之后，逻辑上它是归属于CentralCache的
        // 但是这一个span，物理上就是内存中的一段，每一个指向它的指针都可以管理它
        {
            std::lock_guard map_lock(map_mutex_);
            spanMap_[span->pageAddr] = span;
        }
        return span->pageAddr; // CentralCache拿到的是连续numPages页的起始地址
    }

    // 没有合适的span，先释放当前锁再向系统申请
    lock.unlock();
    void* memory_address = systemAlloc(numPages);
    if (!memory_address) return nullptr;

    // 创建新的span
    Span* span = new Span;
    span->pageAddr = memory_address;
    span->numPages = numPages;
    span->next = nullptr;

    //在被使用的部分记录进spanMap_
    {
        std::lock_guard map_lock(map_mutex_);
        spanMap_[memory_address] = span;
    }
    return memory_address;
}


//我们为什么要专门写一个systemAlloc，而不是直接调用malloc找系统分配呢：因为要满足申请下来的一群page是页对齐的
//内存页对齐指的是分配的内存块的起始地址必须是 页大小（PAGE_SIZE）的整数倍。例如：
//一般情况下我们的页大小通常为 4KB（4096 字节）。
//则对齐的内存地址：0x1000（4096）、0x2000（8192）、0x3000（12288）等。
//未对齐的内存地址：0x1001、0x2003 等（不是 4096 的整数倍）
void* PageCache::systemAlloc(size_t numPages) {
    const size_t size = numPages * PAGE_SIZE;
    void* ptr = _aligned_malloc(size, PAGE_SIZE);
    if (!ptr) return nullptr;
    std::memset(ptr, 0, size);// 因为新分配下来的内存，上面写的都是脏数据，我们需要把这一块内存上面的值设为0
    return ptr;
}


//deallocate的难点在于合并span
//合并操作的核心条件
//只有当相邻 Span 是空闲的（不在 spanMap_ 中，在free_span_map_）时，才能合并。
//如果相邻 Span 仍在 spanMap_ 中 → 正在被使用 → 不能合并。
//如果相邻 Span 不在 spanMap_ 中 → 已释放 → 可以合并。
//那什么是当前span的前一个span呢：物理地址空间上相邻的 Span
void PageCache::deallocateSpan(void* ptr, size_t numPages) {
    // 1. 参数校验
    if (!ptr || numPages == 0) return;

    // 2. 通过全局读锁查找Span（避免长时间阻塞其他线程）
    Span* span = nullptr;
    {
        std::lock_guard<std::mutex> map_lock(map_mutex_);
        auto it = spanMap_.find(ptr);
        if (it == spanMap_.end()) {
            assert(false && "Attempt to deallocate unmanaged memory!");
            return;
        }
        span = it->second;
        spanMap_.erase(it);  // 先从map中移除
    }

    // 3. 根据Span大小选择对应的分片锁
    size_t lock_idx = span->numPages % kMaxLockedPages;
    std::unique_lock<std::mutex> span_lock(page_locks_[lock_idx]);

    // 4. 合并前一个相邻Span
    void* prev_end = static_cast<char*>(span->pageAddr) - 1;
    {
        std::lock_guard<std::mutex> free_span_lock(free_span_mutex_);
        auto it = free_span_map_.upper_bound(prev_end);
        if (it != free_span_map_.begin()) {
            --it;
            Span* prev_span = it->second;

            // 检查物理相邻性
            if (static_cast<char*>(prev_span->pageAddr) + prev_span->numPages * PAGE_SIZE == span->pageAddr) {
                // 锁定前一个Span的分片锁
                size_t prev_lock_idx = prev_span->numPages % kMaxLockedPages;
                if (lock_idx != prev_lock_idx) {
                    span_lock.unlock();
                    std::lock(page_locks_[prev_lock_idx], page_locks_[lock_idx]);//使用std::lock一次性锁住2个mutex，避免死锁
                    span_lock.lock(); // 重新锁定当前分片
                }

                // 执行合并
                prev_span->numPages += span->numPages;
                delete span;
                span = prev_span;  // 后续操作基于合并后的Span

                // 从 free_span_map_ 中移除已合并的 Span
                free_span_map_.erase(prev_span->pageAddr);
            }
        }
    }

    // 5. 合并后一个相邻Span
    void* next_start = static_cast<char*>(span->pageAddr) + span->numPages * PAGE_SIZE;
    {
        std::lock_guard<std::mutex> free_span_lock(free_span_mutex_);
        auto it = free_span_map_.find(next_start);
        if (it != free_span_map_.end()) {
            Span* next_span = it->second;

            // 锁定后一个Span的分片锁
            size_t next_lock_idx = next_span->numPages % kMaxLockedPages;
            if (lock_idx != next_lock_idx) {
                span_lock.unlock();
                std::lock(page_locks_[lock_idx], page_locks_[next_lock_idx]);
                span_lock.lock();
            }

            // 执行合并
            span->numPages += next_span->numPages;

            // 从 free_span_map_ 中移除已合并的 Span
            free_span_map_.erase(next_span->pageAddr);

            // 从空闲链表移除被合并的 Span
            auto& next_list = freeSpans_[next_span->numPages];
            Span** indirect = &next_list;
            while (*indirect != next_span) {
                indirect = &(*indirect)->next;
            }
            *indirect = next_span->next;

            delete next_span;
        }
    }

    // 6. 将合并后的Span重新插入空闲链表
    auto& free_list = freeSpans_[span->numPages];
    span->next = free_list;
    free_list = span;

    // 插入到 free_span_map_
    {
        std::lock_guard<std::mutex> free_span_lock(free_span_mutex_);
        free_span_map_[span->pageAddr] = span;
    }

    // 7. 重新记录到全局映射（虽然已释放，但 spanMap_ 仅用于验证释放的指针是否合法）
    // 注意：这里不需要再插入 spanMap_，因为 span 已释放
}
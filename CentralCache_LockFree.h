#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include "common.h"
#include "PageCache.h"

// 支持 ABA 问题防护的带标签指针
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
        std::atomic<TaggedPtr> head;  // 使用带标签的原子指针
    };

    std::array<LockFreeList, FREE_LIST_SIZE> centralFreeList_;
};

// 每次从PageCache取8页内存
static const size_t SPAN_PAGES = 8;



//无锁编程的本质是：
//对共享数据的修改只能通过原子操作修改（如 CAS），对共享数据的读取和单线程一样，正常读取即可
//非原子操作只能在"独占"状态下进行（如 CAS 成功后）  
//比如共享链表A->B->C->D  当前线程把共享链表的头更新为了C之后，我们当前线程相当于对这个共享链表就是独占的（别的线程看不到A和B了），可以和单线程一模一样地处理A和B

void* CentralCache::fetchRange(size_t index, size_t batchNum) {
    // 1. 参数合法性检查
    if (index >= FREE_LIST_SIZE || batchNum == 0) {
        return nullptr;  // 索引越界或请求数量为0时直接返回
    }

    // 2. 计算当前索引对应的内存块大小
    size_t size = (index + 1) * ALIGNMENT;  // ALIGNMENT 是内存对齐值（如 8/16）

    // 3. 无限循环重试（无锁编程的核心模式）
    while (true) {
        // 4. 获取当前链表的头部信息（原子读）
        LockFreeList& list = centralFreeList_[index];
        TaggedPtr old_head = list.head.load(std::memory_order_acquire);
        void* current = old_head.ptr;  // 当前链表头指针
        void* batch_head = current;    // 要分配的链表头
        void* batch_tail = nullptr;    // 要分配的链表尾
        size_t count = 0;              // 统计可用内存块数量

        // 5. 遍历链表统计可用内存块（单线程逻辑）
        while (current && count < batchNum) {
            batch_tail = current;
            current = *reinterpret_cast<void**>(current);  // 通过内存块前8字节读取next
            count++;
        }

        // 6. 情况A: 链表内存足够（满足 batchNum 个内存块）
        if (count == batchNum) {
            // 构造新的链表头（指向剩余链表）
            TaggedPtr new_head{ current, old_head.tag + 1 };

            // 7. 原子更新链表头（CAS 操作是线程安全的关键）
            if (list.head.compare_exchange_weak(
                old_head, new_head,           // 原值和新值
                std::memory_order_release,     // 写操作对其他线程可见
                std::memory_order_acquire)) {  // 读操作确保看到其他线程的写入

                // 8. 截断链表（非原子操作，但此时已独占这批内存块）!!!!!!!!!!!!!!!!!!!!!!!
                //=======================================================
                //=======================================================
                //这里极好的体现了无锁编程的核心思想，虽然这里的数据还是共享链表上的
                //但是别的线程已经无法读取到这一部分的数据了（因为共享链表的head被移动到后面去了），因而可以认为是当前线程独占的
                if (batch_tail) {
                    *reinterpret_cast<void**>(batch_tail) = nullptr;
                }

                // 9. 返回分配的内存块链表
                return batch_head;
            }
            else {
                // 10. CAS失败（链表被其他线程修改），重试整个流程
                continue;
            }
        }

        // 11. 情况B: 链表内存不足（需要从 PageCache 获取新内存）
        void* newBlocks = fetchFromPageCache(size);
        if (!newBlocks) {
            continue;  // 获取失败，重试
        }


        ///////////////下面的很好的体现了无锁编程的核心思想：
        //线程独占的数据（如新分配的内存块）可以直接操作（无需原子性）。
        //共享数据（如链表头）必须通过CAS 保证原子性，防止多线程冲突。

        //比如，下面的这段内存是当前线程刚从 PageCache 申请的，尚未插入到共享链表，
        //此时没有任何其他线程能访问这段内存（未被发布到共享链表），所以它是当前线程独占的，因此我们正常写即可
        //然后我们要把剩下来的一部分头插至CentralCache的链表
        //由于这个链表是共享数据，多个线程可能同时插入或修改链表头，因此我们得使用CAS操作：
        //如果发现数据和自己预期的一致，代表别的线程还没有处理这个共享链表，那么就瞬间头插成功，
        //如果发现数据和自己预期的不一致，那么代表别的线程已经处理了这个共享链表了，那么就重新尝试插入


        // 12. 切分新内存块（单线程逻辑）
        char* start = static_cast<char*>(newBlocks);
        size_t totalBlocks = (SPAN_PAGES * PageCache::PAGE_SIZE) / size;
        size_t allocBlocks = std::min(batchNum, totalBlocks);

        // 13. 构造分配给 ThreadCache 的链表（非原子操作）
        if (allocBlocks > 1) {
            for (size_t i = 1; i < allocBlocks; ++i) {
                void* current = start + (i - 1) * size;
                void* next = start + i * size;
                *reinterpret_cast<void**>(current) = next;
            }
            *reinterpret_cast<void**>(start + (allocBlocks - 1) * size) = nullptr;
        }

        // 14. 构造剩余内存块并插入链表（需要 CAS 原子操作）
        if (totalBlocks > allocBlocks) {
            void* remainStart = start + allocBlocks * size;
            void* remainTail = start + (totalBlocks - 1) * size;
            *reinterpret_cast<void**>(remainTail) = nullptr;

            TaggedPtr remain_old_head;
            TaggedPtr remain_new_head;
            do {
                // 15. 读取当前链表头（原子读）
                remain_old_head = list.head.load(std::memory_order_acquire);

                // 16. 将剩余链表尾连接到当前链表头
                *reinterpret_cast<void**>(remainTail) = remain_old_head.ptr;

                // 17. 构造新链表头（带版本号）
                remain_new_head = { remainStart, remain_old_head.tag + 1 };

                // 18. 原子插入剩余链表（CAS 循环重试）
            } while (!list.head.compare_exchange_weak(
                remain_old_head, remain_new_head,
                std::memory_order_release, std::memory_order_acquire));
        }

        // 19. 返回新分配的内存块链表
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
        // 头插法插入新内存块
        *reinterpret_cast<void**>(start) = old_head.ptr;
        new_head = { start, old_head.tag + 1 };

    } while (!list.head.compare_exchange_weak(
        old_head, new_head,
        std::memory_order_release, std::memory_order_relaxed));
}
#pragma once
#include <array>
#include "common.h"
#include "CentralCache.h"




class ThreadCache //使用单例模式
{

public:
    static ThreadCache* getInstance()
    {
        static thread_local ThreadCache instance;//在 ThreadCache 的设计中，如果只使用 static 而不使用 thread_local,由于static 变量是全局的，因此会导致所有线程共享同一个 ThreadCache
        //所有线程访问的是 同一个 instance。所以后果是：多个线程同时调用 allocate() 或 deallocate() 时，会修改同一块内存池，导致 数据竞争（Data Race）
        return &instance;
    }

    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);


private:
    ThreadCache()
    {
        // 初始化自由链表和大小统计
        freeList_.fill(nullptr);
        //freeListSize_.fill(0);
    }


    void* fetchFromCentralCache(size_t index);// 从中心缓存获取内存

    void returnToCentralCache(void* start, size_t size, size_t bytes);// 归还内存到中心缓存

    //bool shouldReturnToCentralCache(size_t index);


private:
    //一些关于C++的STL的讲解
       //std::array（静态数组，就是python里面的不可变数组）
       //大小固定，编译时确定（不能动态扩容）。
       //存储在栈或全局内存（除非显式用 new）

       //std::list  注意其中文名：双向链表   
       //访问第 n 个元素需遍历 O(n)

       //std::vector  就是python里面的正常的可变数组


       //（1）访问速度
       //操作	        std::array	  std::list	  std::vector
       //访问[i]位置	     O(1)           O(n)	    O(1)
       //遍历（迭代器）	 O(n)           O(n)	    O(n)
       //（2）插入 / 删除
       //操作	 std::array	    std::list	std::vector
       //头部插入	 O(n)	    O(1)	        O(n)
       //中间插入	 O(n)	    O(1)	        O(n)
       //尾部插入	 O(n)	    O(1)	        O(1)
       //尾部删除	 O(n)	    O(1)	        O(1)


   //每个线程的 ThreadCache 会维护多个自由链表，每个链表专门管理一种固定大小的内存块.比如链表1，每个节点就是8B的内存块；链表2，每个节点就是16B的内存块
    std::array<void*, FREE_LIST_SIZE>  freeList_;         // 存储自由链表的头指针
    //std::array<size_t, FREE_LIST_SIZE> freeListSize_;     // 记录每个自由链表的当前大小
};





void* ThreadCache::allocate(size_t size)
{
    // 处理0大小的分配请求
    if (size == 0)
    {
        size = ALIGNMENT; // 至少分配一个对齐大小
    }

    if (size > MAX_BYTES)
    {
        // 大对象直接从系统分配
        return malloc(size);
    }

    size_t index = SizeClass::getIndex(size);

    // 更新自由链表大小
    //freeListSize_[index]--;

    // 检查线程本地自由链表
    // 如果 freeList_[index] 不为空，表示该链表中有可用内存块
    if (void* ptr = freeList_[index])
    {
        freeList_[index] = *reinterpret_cast<void**>(ptr); // 将freeList_[index]指向的内存块的下一个内存块地址（取决于内存块的实现）
        return ptr;
    }

    // 如果线程本地自由链表为空，则从中心缓存获取一批内存
    return fetchFromCentralCache(index);
}






void ThreadCache::deallocate(void* ptr, size_t size)
{
    if (size > MAX_BYTES)
    {
        free(ptr);
        return;
    }

    size_t index = SizeClass::getIndex(size);

    *reinterpret_cast<void**>(ptr) = freeList_[index];  // 让 ptr 指向原来的链表头
    freeList_[index] = ptr;                             // 让链表头指向 ptr
}



void* ThreadCache::fetchFromCentralCache(size_t index)
{
    // 从中心缓存批量获取内存
    void* start = CentralCache::getInstance().fetchRange(index);
    if (!start) return nullptr;

    // 取一个返回，其余放入自由链表
    void* result = start;
    freeList_[index] = *reinterpret_cast<void**>(start);

    return result;
}

void ThreadCache::returnToCentralCache(void* start, size_t size, size_t bytes)
{
    // 根据大小计算对应的索引
    size_t index = SizeClass::getIndex(size);

    // 计算要归还内存块数量
    size_t batchNum = size / bytes;
    if (batchNum <= 1) return; // 如果只有一个块，则不归还

    // 将内存块串成链表
    char* current = static_cast<char*>(start);
    // 删除未使用的变量 end
    // char* end = current + (batchNum - 1) * bytes;

    // 保留一部分在ThreadCache中（比如保留1/4）
    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t returnNum = batchNum - keepNum;

    // 计算要保留的最后一个节点位置
    char* splitNode = current + (keepNum - 1) * bytes;

    // 将要返回的部分和要保留的部分断开
    void* nextNode = *reinterpret_cast<void**>(splitNode);
    *reinterpret_cast<void**>(splitNode) = nullptr; // 断开连接

    // 更新ThreadCache的空闲链表
    freeList_[index] = start;

    // 将剩余部分返回给CentralCache
    if (returnNum > 0)
    {
        char* returnStart = static_cast<char*>(nextNode);
        CentralCache::getInstance().returnRange(returnStart, returnNum * bytes, index);
    }
}

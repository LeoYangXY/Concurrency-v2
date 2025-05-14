#pragma once
#include <array>
#include "common.h"
#include "CentralCache.h"

//注意！！！！！！！！！！！！！！！！！！！！！！！！！
//下面的*（void**）这样的操作，本质上是因为我们这里的链表的节点，我们是直接使用裸空间，因此对于链表的处理会显得很繁杂
//如果我们的链表节点是正常的包含next和正常存储数据的部分，那么下面的很多链表操作就会好写很多，跟python一样简单
//比如我们可以使用这样的链表节点：
//struct MemoryBlock {
//    MemoryBlock* next;  // 下一个内存块指针
//    size_t block_size;  // 当前块大小（调试用）
//
//    // 获取用户可用内存的起始地址
//    void* user_ptr() {
//        return reinterpret_cast<char*>(this) + sizeof(MemoryBlock);
//    }
//
//    // 从用户指针反推内存块头
//    static MemoryBlock* from_user_ptr(void* ptr) {
//        return reinterpret_cast<MemoryBlock*>(
//            static_cast<char*>(ptr) - sizeof(MemoryBlock));
//    }
//};


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
    size_t getBatchNum(size_t size);
    

    void returnToCentralCache(void* start, size_t size, size_t bytes);// 归还内存到中心缓存



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


    // 检查线程本地自由链表
    // 如果 freeList_[index] 不为空，表示该链表中有可用内存块
    if (void* ptr = freeList_[index])
    {
        //ptr的值是当前内存块的起始地址（也是 freeList_[index]这个指针当前指向的地址）
        //然后这个内存块的前八位中存储的值是一个地址，这个地址（不妨记为x），代表着下一个内存块的起始地址
        //我们需要取出这一个内存块，然后让 freeList_[index]这个指针指向x这个地址即可

        
        uintptr_t cur_add = (uintptr_t)freeList_[index];// 获取当前链表头的地址
        uintptr_t next = 0;// 定义一个整数变量 next，用来存储下一个块的地址
        memcpy(&next, (void*)cur_add, sizeof(void*));//从ptr地址拷贝8字节数据到next
        //freeList_[index] = (void*)next;//把next转化为指针形式，作为新的链表头
        //当然我们也可以用此一步实现：freeList_[index] = *reinterpret_cast<void**>(ptr);
        return ptr;
    }
    else {// 如果线程本地自由链表为空，则从中心缓存获取一批内存
        return fetchFromCentralCache(index);
    }
    
}






void ThreadCache::deallocate(void* ptr, size_t size)//ptr是我们要回收的内存块的地址
{
    if (size > MAX_BYTES)
    {
        free(ptr);
        return;
    }

    size_t index = SizeClass::getIndex(size);

    
    void* old_head = freeList_[index];
    memcpy(ptr, &old_head, sizeof(void*));// 把当前链表头地址写入ptr的前1个字节
    freeList_[index] = ptr;// 更新链表头为当前ptr

    //当然我们也可以使用下面的语法糖：
    //*reinterpret_cast<void**>(ptr) = freeList_[index];  // 让 ptr 指向原来的链表头
    //freeList_[index] = ptr;                             // 让链表头指向 ptr
}



void* ThreadCache::fetchFromCentralCache(size_t index)
{
    size_t size = (index + 1) * ALIGNMENT;
    // 根据对象内存大小计算批量获取的数量
    size_t batchNum = getBatchNum(size);
    // 从中心缓存批量获取内存
    void* start = CentralCache::getInstance().fetchRange(index, batchNum);
    if (!start) return nullptr;

    // 取一个返回，其余放入线程本地自由链表
    void* result = start;
    if (batchNum > 1)
    {
        freeList_[index] = *reinterpret_cast<void**>(start);
    }

    return result;
}

// 计算批量获取内存块的数量
size_t ThreadCache::getBatchNum(size_t size)
{
    // 基准：每次批量获取不超过4KB内存
    constexpr size_t MAX_BATCH_SIZE = 4 * 1024; // 4KB

    // 根据对象大小设置合理的基准批量数
    size_t baseNum;
    if (size <= 32) baseNum = 64;    // 64 * 32 = 2KB
    else if (size <= 64) baseNum = 32;  // 32 * 64 = 2KB
    else if (size <= 128) baseNum = 16; // 16 * 128 = 2KB
    else if (size <= 256) baseNum = 8;  // 8 * 256 = 2KB
    else if (size <= 512) baseNum = 4;  // 4 * 512 = 2KB
    else if (size <= 1024) baseNum = 2; // 2 * 1024 = 2KB
    else baseNum = 1;                   // 大于1024的对象每次只从中心缓存取1个

    // 计算最大批量数
    size_t maxNum = std::max(size_t(1), MAX_BATCH_SIZE / size);

    // 取最小值，但确保至少返回1
    return std::max(sizeof(1), std::min(maxNum, baseNum));
}


void ThreadCache::returnToCentralCache(void* start,   // 内存块链表的起始地址
    size_t size,    // 每个内存块的大小
    size_t bytes)   // 总字节数

//批量归还内存块：当ThreadCache中某个大小的内存块过多时，将多余的部分归还给CentralCache
//保留适当缓存：仍然保留一部分在ThreadCache中供后续快速分配
//维护链表结构：正确处理内存块之间的链接关系

{
    size_t index = SizeClass::getIndex(size);// 根据大小计算对应的索引
    size_t batchNum = bytes/size;// 计算要归还内存块数量
    if (batchNum <= 1) return; // 如果只有一个块，则不归还

    // 将内存块串成链表
    char* current = static_cast<char*>(start);

    // 保留一部分在ThreadCache中（比如保留1/4）
    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t returnNum = batchNum - keepNum;

    // 计算要保留的最后一个节点位置
    char* splitNode = current + (keepNum - 1) * bytes;

    // 将要返回的部分和要保留的部分断开
    // start~splitNode是要保留的，splitNode之后接着的就是nextNode，nextNode~末尾是要还给CentralCache的
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

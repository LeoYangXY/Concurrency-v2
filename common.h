#pragma once


constexpr size_t ALIGNMENT = 8;//所有分配的内存块大小必须是 ALIGNMENT（8字节）的整数倍
constexpr size_t MAX_BYTES = 256 * 1024; //该内存池只处理 ≤256KB 的请求，更大的请求可能直接走系统 malloc。
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // ALIGNMENT等于指针void*的大小



// 大小类管理
class SizeClass
{
public:

    static size_t getIndex(size_t bytes)//将字节大小 bytes 转换为 sizeClass 索引（自由链表数组的下标）

        //用户请求 malloc(10)。
        //调用 SizeClass::getIndex(10) 得到 sizeClass = 1。
        //从 freeLists_[1]（16B 的自由链表）中取出一个块返回。

    {
        
        bytes = std::max(bytes, ALIGNMENT);// 确保bytes至少为ALIGNMENT
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};
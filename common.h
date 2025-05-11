#pragma once


constexpr size_t ALIGNMENT = 8;//所有分配的内存块大小必须是 ALIGNMENT（8字节）的整数倍
constexpr size_t MAX_BYTES = 256 * 1024; //该内存池只处理 ≤256KB 的请求，更大的请求可能直接走系统 malloc。
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // ALIGNMENT等于指针void*的大小

// 内存块头部信息
struct BlockHeader
{
    size_t size; // 内存块的实际大小（如分配 10B 时，size 可能是 16B）
    bool   inUse; // 使用标志
    BlockHeader* next; // 指向下一个内存块
};

// 大小类管理
class SizeClass
{
public:
    static size_t roundUp(size_t bytes)
    {
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);//将任意字节数 bytes 向上对齐到 ALIGNMENT 的倍数。
    }

    static size_t getIndex(size_t bytes)//将字节大小 bytes 转换为 sizeClass 索引（自由链表数组的下标）

        //用户请求 malloc(10)。
        //调用 SizeClass::roundUp(10) 得到 16B。
        //调用 SizeClass::getIndex(16) 得到 sizeClass = 1。
        //从 freeLists_[1]（16B 的自由链表）中取出一个块返回。

    {
        // 确保bytes至少为ALIGNMENT
        bytes = std::max(bytes, ALIGNMENT);
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};
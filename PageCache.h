#pragma once
#include <map>
#include <mutex>
#include "common.h"
#include <cstring>
#include <cassert>

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

    
    std::map<size_t, Span*> freeSpans_;// ��ҳ���������span����ͬҳ����Ӧ��ͬSpan����ʵ�ʴ洢��ʽ��{keyΪҳ��,��Ӧ��valueΪSpan����ͷָ��}
    
    // ҳ�ŵ�span��ӳ�䣬���ڻ���. ͨ���ڴ�ҳ����ʼ��ַ�����ҵ���Ӧ�� Span ����
    //keyΪvoid*�������ڴ�ҳ����ʼ��ַ���� Span::pageAddr�������� 0x1000��0x2000��
    //valueΪSpan*����ָ�� Span �����ָ�룬���Span����ᱣ����ڴ���Ԫ��Ϣ����ʼ��ַ��ҳ��������ָ��ȣ���
    std::map<void*, Span*> spanMap_;
    
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
        Span* span = it->second;//lt->second��ʵ���Ƿ���lt�����size_t, Span*������ĵڶ������ԣ�Ҳ��Span*��
        //��Ȼ����Ҳ����ʹ��C++17����Ľṹ���󶨣�auto& [pageCount, span] = *it;  

        // ��ȡ����span��ԭ�еĿ�������freeSpans_[it->first]���Ƴ�����Ϊ��һ�������������ǰ�����ͷ�ƶ�����ǰ���span��next������ֱ��erase
        if (span->next)
        {
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it);
        }

        //������ǻ�õ�span������Ҫ��numPages����зָ�
        //����CentralCache������numPages������ҳ�棬����ȡ������һ����span��
        //Ȼ�����ǰ����span��ǰ��numsPagesҳ����CentralCache��ʣ���������Ǵ���һ���µ�span��Ȼ��һ�PageCache����ȥ
        
        if (span->numPages > numPages)
        {
            // ���Ǵ�����span��Ȼ���ʣ���page�浽��һ��span����ȥ
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) +
                numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            //��ʣ���page�����span�һ�PageCache
            auto& cur_head = freeSpans_[newSpan->numPages];
            newSpan->next = cur_head;
            cur_head = newSpan;
            span->numPages = numPages;
        }

        // ��¼span��Ϣ���ڻ���
        // ����һ��span��return��CentralCache֮���߼������ǹ�����CentralCache��
        // ������һ��span�������Ͼ����ڴ��е�һ�Σ�ÿһ��ָ������ָ�붼���Թ�����
        // ������spanMap_�м�¼��Ҫ���յ�ʱ�����Ǳ���ͨ��PageCache��spanMap_�ҵ���һ��span�����в���
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;//CentalCache�õ��������� numPages ҳ����ʼ��ַ��span->pageAddr���������� CentralCache �Լ��и��С�鹩 ThreadCache ʹ��
    }

    // û�к��ʵ�span����ϵͳ���룬Ȼ��return�oCentralCache
    void* memory_address = systemAlloc(numPages);
    if (!memory_address) return nullptr;

    // Ϊ�˱�֤�ӿڵ�һ���ԣ�����CentralCache���յ�PageCache��������һ���ڴ���span����ʽ�����ܷ���ͳһ�ؽ��л��֣�
    // ����������������������ڴ�飬����Ҫ����Ϊһ���µ�span
    Span* span = new Span;
    span->pageAddr = memory_address;
    span->numPages = numPages;
    span->next = nullptr;

    // ��¼span��Ϣ���ڻ���
    spanMap_[memory_address] = span;
    return memory_address;
}



//����ΪʲôҪר��дһ��systemAlloc��������ֱ�ӵ���malloc��ϵͳ�����أ���ΪҪ��������������һȺpage��ҳ�����
//�ڴ�ҳ����ָ���Ƿ�����ڴ�����ʼ��ַ������ ҳ��С��PAGE_SIZE���������������磺
//һ����������ǵ�ҳ��Сͨ��Ϊ 4KB��4096 �ֽڣ���
//�������ڴ��ַ��0x1000��4096����0x2000��8192����0x3000��12288���ȡ�
//δ������ڴ��ַ��0x1001��0x2003 �ȣ����� 4096 ����������
void* PageCache::systemAlloc(size_t numPages) {
    const size_t size = numPages * PAGE_SIZE;

    void* ptr = _aligned_malloc(size, PAGE_SIZE);//ʹ�� _aligned_malloc�����ڴ�ҳ����

    // ����ʧ��ʱֱ�ӷ��� nullptr�����ٻ��˵� malloc��
    if (!ptr) return nullptr;

    // ��Ϊ�·����������ڴ棬����д�Ķ��������ݣ�������Ҫ����һ���ڴ������ֵ��Ϊ0
    std::memset(ptr, 0, size);
    return ptr;
}



void PageCache::deallocateSpan(void* ptr, size_t numPages) {
    // 1. �����������̰߳�ȫ��
    std::lock_guard<std::mutex> lock(mutex_);

    // 2. У������Ϸ���
    if (!ptr || numPages == 0) return;

    // 3. �� spanMap_ �в���Ҫ�ͷŵ� Span
    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) {
        // �Ƿ��ͷţ���ַδ��¼�� spanMap_ ��
        assert(false && "Attempt to deallocate unmanaged memory!");
        return;
    }

    // 4. ��ȡ Span �����Ƴ���¼
    Span* span = it->second;
    spanMap_.erase(it);  // �������Ȩ

    // 5. ���ǰ�����ڵ� Span �Ƿ���У��ϲ��Լ�����Ƭ
    // 5.1 �ϲ�ǰһ������ Span
    void* prevEnd = static_cast<char*>(span->pageAddr) - 1;
    auto prevIt = spanMap_.lower_bound(prevEnd);
    if (prevIt != spanMap_.begin()) {
        --prevIt;
        Span* prevSpan = prevIt->second;
        // ����Ƿ���������
        if (static_cast<char*>(prevSpan->pageAddr) + prevSpan->numPages * PAGE_SIZE == span->pageAddr) {
            // �ϲ���ǰһ�� Span
            prevSpan->numPages += span->numPages;
            delete span;      // �ͷŵ�ǰ Span ����
            span = prevSpan;  // �����������ںϲ���� Span
        }
    }

    // 5.2 �ϲ���һ������ Span
    void* nextStart = static_cast<char*>(span->pageAddr) + span->numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextStart);
    if (nextIt != spanMap_.end()) {
        Span* nextSpan = nextIt->second;
        // �ϲ�����ǰ Span
        span->numPages += nextSpan->numPages;
        spanMap_.erase(nextIt);  // �Ƴ����ϲ��� Span ��¼
        delete nextSpan;         // �ͷű��ϲ��� Span ����
    }

    // 6. ���ϲ���� Span ���²����������
    auto& list = freeSpans_[span->numPages];
    span->next = list;  // ͷ�巨
    list = span;

    // 7. ���¼�¼����Ȩ���ϲ���������µ�ַ��
    spanMap_[span->pageAddr] = span;
}
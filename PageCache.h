#pragma once
#include <map>
#include <mutex>
#include "common.h"
#include <cstring>
#include <cassert>

class PageCache {

public:
    static const size_t PAGE_SIZE = 4096; // 4Kҳ��С

    static PageCache& getInstance() {
        static PageCache instance;
        return instance;
    }

    void* allocateSpan(size_t numPages); // ����ָ��ҳ����span
    void deallocateSpan(void* ptr, size_t numPages); // �ͷ�span

private:
    PageCache() = default;
    void* systemAlloc(size_t numPages); // ��ϵͳ�����ڴ�

private:
    struct Span {
        void* pageAddr; // ҳ��ʼ��ַ
        size_t numPages; // ҳ��
        Span* next;     // ����ָ��
    };

    //���allocate�����ڼ�¼�����ݽṹ�����Ӧ������
    // spanMap_ �д��ڵ� Span ��ʾ������ʹ�á���spanMap_ �в����ڵ� Span ��ʾ�����С�
    // keyΪvoid*�������ڴ�ҳ����ʼ��ַ���� Span::pageAddr�������� 0x1000��0x2000��
    // valueΪSpan*����ָ�� Span �����ָ�룬���Span����ᱣ����ڴ���Ԫ��Ϣ����ʼ��ַ��ҳ��������ָ��ȣ���
    std::map<void*, Span*> spanMap_;//��Ȼ��ʵ����Ҳ�����Ż���spanMap_���Կ���ʹ��tbb�Ĳ���hashmap���������Լ�ʹ�������������д���
    std::mutex map_mutex_; // ����spanMap_���ȫ��ͳ������ȫ��������Ϊ������ʵ�ʲ������֮��Ҫ��spanMap_����д����


    //���deallocate�ĺϲ��ڴ�鲿�֣������������ڼ�¼�����ݽṹ��������
    std::map<void*, Span*> free_span_map_; // key: Span ����ʼ��ַ��value: Span ָ��
    std::mutex free_span_mutex_; // ���� free_span_map_ ����



    //������PageCache����������+�����������������֣�
    std::map<size_t, Span*> freeSpans_;// freeSpans_ ��ҳ���������span����ͬҳ����Ӧ��ͬSpan����

    static constexpr size_t kMaxLockedPages = 128; // ���������������ʵ�ַ�Ƭ������
    std::array<std::mutex, kMaxLockedPages> page_locks_;// ���freeSpans_��ϸ������


};

void* PageCache::allocateSpan(size_t numPages) {
    // Step 1: ֱ�Ӹ��� numPages ѡ����
    std::unique_lock lock(page_locks_[numPages % kMaxLockedPages]);

    // ���Һ��ʵĿ���span
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end()) {
        Span* span = it->second;//��ʵ���Ƿ���lt�����size_t, Span* ������ĵڶ������ԣ�Ҳ��Span* ��
                                //��Ȼ����Ҳ����ʹ��C++17����Ľṹ���󶨣�auto& [pageCount, span] = *it;  

        // ��ȡ����span��ԭ�еĿ�������freeSpans_[it->first]���Ƴ�
        if (span->next) {
            freeSpans_[it->first] = span->next;
        }
        else {
            freeSpans_.erase(it);
        }

        //step2:������ǻ�õ�span������Ҫ��numPages����зָ�
        //����CentralCache������numPages������ҳ�棬����ȡ������һ����span��
        //Ȼ�����ǰ����span��ǰ��numsPagesҳ����CentralCache��ʣ���������Ǵ���һ���µ�span��Ȼ��һ�PageCache����ȥ
        if (span->numPages > numPages) {
            // ������span���ʣ��page
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // ��ʣ���page�����span�һ�PageCache
            size_t target_pages = newSpan->numPages;
            std::unique_lock target_lock(page_locks_[target_pages % kMaxLockedPages]);
            newSpan->next = freeSpans_[target_pages];
            freeSpans_[target_pages] = newSpan;

            // ע�⣺���в��ּ�¼��free_span_map_
            {
                std::lock_guard<std::mutex> free_span_lock(free_span_mutex_);
                free_span_map_[newSpan->pageAddr] = newSpan;
            }

            span->numPages = numPages; // ���µ�ǰspan�Ĵ�С
        }

        // ע�⣺�ڱ�ʹ�õĲ��ּ�¼��spanMap_
        // ����һ��span��return��CentralCache֮���߼������ǹ�����CentralCache��
        // ������һ��span�������Ͼ����ڴ��е�һ�Σ�ÿһ��ָ������ָ�붼���Թ�����
        {
            std::lock_guard map_lock(map_mutex_);
            spanMap_[span->pageAddr] = span;
        }
        return span->pageAddr; // CentralCache�õ���������numPagesҳ����ʼ��ַ
    }

    // û�к��ʵ�span�����ͷŵ�ǰ������ϵͳ����
    lock.unlock();
    void* memory_address = systemAlloc(numPages);
    if (!memory_address) return nullptr;

    // �����µ�span
    Span* span = new Span;
    span->pageAddr = memory_address;
    span->numPages = numPages;
    span->next = nullptr;

    //�ڱ�ʹ�õĲ��ּ�¼��spanMap_
    {
        std::lock_guard map_lock(map_mutex_);
        spanMap_[memory_address] = span;
    }
    return memory_address;
}


//����ΪʲôҪר��дһ��systemAlloc��������ֱ�ӵ���malloc��ϵͳ�����أ���ΪҪ��������������һȺpage��ҳ�����
//�ڴ�ҳ����ָ���Ƿ�����ڴ�����ʼ��ַ������ ҳ��С��PAGE_SIZE���������������磺
//һ����������ǵ�ҳ��Сͨ��Ϊ 4KB��4096 �ֽڣ���
//�������ڴ��ַ��0x1000��4096����0x2000��8192����0x3000��12288���ȡ�
//δ������ڴ��ַ��0x1001��0x2003 �ȣ����� 4096 ����������
void* PageCache::systemAlloc(size_t numPages) {
    const size_t size = numPages * PAGE_SIZE;
    void* ptr = _aligned_malloc(size, PAGE_SIZE);
    if (!ptr) return nullptr;
    std::memset(ptr, 0, size);// ��Ϊ�·����������ڴ棬����д�Ķ��������ݣ�������Ҫ����һ���ڴ������ֵ��Ϊ0
    return ptr;
}


//deallocate���ѵ����ںϲ�span
//�ϲ������ĺ�������
//ֻ�е����� Span �ǿ��еģ����� spanMap_ �У���free_span_map_��ʱ�����ܺϲ���
//������� Span ���� spanMap_ �� �� ���ڱ�ʹ�� �� ���ܺϲ���
//������� Span ���� spanMap_ �� �� ���ͷ� �� ���Ժϲ���
//��ʲô�ǵ�ǰspan��ǰһ��span�أ������ַ�ռ������ڵ� Span
void PageCache::deallocateSpan(void* ptr, size_t numPages) {
    // 1. ����У��
    if (!ptr || numPages == 0) return;

    // 2. ͨ��ȫ�ֶ�������Span�����ⳤʱ�����������̣߳�
    Span* span = nullptr;
    {
        std::lock_guard<std::mutex> map_lock(map_mutex_);
        auto it = spanMap_.find(ptr);
        if (it == spanMap_.end()) {
            assert(false && "Attempt to deallocate unmanaged memory!");
            return;
        }
        span = it->second;
        spanMap_.erase(it);  // �ȴ�map���Ƴ�
    }

    // 3. ����Span��Сѡ���Ӧ�ķ�Ƭ��
    size_t lock_idx = span->numPages % kMaxLockedPages;
    std::unique_lock<std::mutex> span_lock(page_locks_[lock_idx]);

    // 4. �ϲ�ǰһ������Span
    void* prev_end = static_cast<char*>(span->pageAddr) - 1;
    {
        std::lock_guard<std::mutex> free_span_lock(free_span_mutex_);
        auto it = free_span_map_.upper_bound(prev_end);
        if (it != free_span_map_.begin()) {
            --it;
            Span* prev_span = it->second;

            // �������������
            if (static_cast<char*>(prev_span->pageAddr) + prev_span->numPages * PAGE_SIZE == span->pageAddr) {
                // ����ǰһ��Span�ķ�Ƭ��
                size_t prev_lock_idx = prev_span->numPages % kMaxLockedPages;
                if (lock_idx != prev_lock_idx) {
                    span_lock.unlock();
                    std::lock(page_locks_[prev_lock_idx], page_locks_[lock_idx]);//ʹ��std::lockһ������ס2��mutex����������
                    span_lock.lock(); // ����������ǰ��Ƭ
                }

                // ִ�кϲ�
                prev_span->numPages += span->numPages;
                delete span;
                span = prev_span;  // �����������ںϲ����Span

                // �� free_span_map_ ���Ƴ��Ѻϲ��� Span
                free_span_map_.erase(prev_span->pageAddr);
            }
        }
    }

    // 5. �ϲ���һ������Span
    void* next_start = static_cast<char*>(span->pageAddr) + span->numPages * PAGE_SIZE;
    {
        std::lock_guard<std::mutex> free_span_lock(free_span_mutex_);
        auto it = free_span_map_.find(next_start);
        if (it != free_span_map_.end()) {
            Span* next_span = it->second;

            // ������һ��Span�ķ�Ƭ��
            size_t next_lock_idx = next_span->numPages % kMaxLockedPages;
            if (lock_idx != next_lock_idx) {
                span_lock.unlock();
                std::lock(page_locks_[lock_idx], page_locks_[next_lock_idx]);
                span_lock.lock();
            }

            // ִ�кϲ�
            span->numPages += next_span->numPages;

            // �� free_span_map_ ���Ƴ��Ѻϲ��� Span
            free_span_map_.erase(next_span->pageAddr);

            // �ӿ��������Ƴ����ϲ��� Span
            auto& next_list = freeSpans_[next_span->numPages];
            Span** indirect = &next_list;
            while (*indirect != next_span) {
                indirect = &(*indirect)->next;
            }
            *indirect = next_span->next;

            delete next_span;
        }
    }

    // 6. ���ϲ����Span���²����������
    auto& free_list = freeSpans_[span->numPages];
    span->next = free_list;
    free_list = span;

    // ���뵽 free_span_map_
    {
        std::lock_guard<std::mutex> free_span_lock(free_span_mutex_);
        free_span_map_[span->pageAddr] = span;
    }

    // 7. ���¼�¼��ȫ��ӳ�䣨��Ȼ���ͷţ��� spanMap_ ��������֤�ͷŵ�ָ���Ƿ�Ϸ���
    // ע�⣺���ﲻ��Ҫ�ٲ��� spanMap_����Ϊ span ���ͷ�
}
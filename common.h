#pragma once


constexpr size_t ALIGNMENT = 8;//���з�����ڴ���С������ ALIGNMENT��8�ֽڣ���������
constexpr size_t MAX_BYTES = 256 * 1024; //���ڴ��ֻ���� ��256KB �����󣬸�����������ֱ����ϵͳ malloc��
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // ALIGNMENT����ָ��void*�Ĵ�С

// �ڴ��ͷ����Ϣ
struct BlockHeader
{
    size_t size; // �ڴ���ʵ�ʴ�С������� 10B ʱ��size ������ 16B��
    bool   inUse; // ʹ�ñ�־
    BlockHeader* next; // ָ����һ���ڴ��
};

// ��С�����
class SizeClass
{
public:
    static size_t roundUp(size_t bytes)
    {
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);//�������ֽ��� bytes ���϶��뵽 ALIGNMENT �ı�����
    }

    static size_t getIndex(size_t bytes)//���ֽڴ�С bytes ת��Ϊ sizeClass ��������������������±꣩

        //�û����� malloc(10)��
        //���� SizeClass::roundUp(10) �õ� 16B��
        //���� SizeClass::getIndex(16) �õ� sizeClass = 1��
        //�� freeLists_[1]��16B ������������ȡ��һ���鷵�ء�

    {
        // ȷ��bytes����ΪALIGNMENT
        bytes = std::max(bytes, ALIGNMENT);
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};
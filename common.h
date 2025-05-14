#pragma once


constexpr size_t ALIGNMENT = 8;//���з�����ڴ���С������ ALIGNMENT��8�ֽڣ���������
constexpr size_t MAX_BYTES = 256 * 1024; //���ڴ��ֻ���� ��256KB �����󣬸�����������ֱ����ϵͳ malloc��
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // ALIGNMENT����ָ��void*�Ĵ�С



// ��С�����
class SizeClass
{
public:

    static size_t getIndex(size_t bytes)//���ֽڴ�С bytes ת��Ϊ sizeClass ��������������������±꣩

        //�û����� malloc(10)��
        //���� SizeClass::getIndex(10) �õ� sizeClass = 1��
        //�� freeLists_[1]��16B ������������ȡ��һ���鷵�ء�

    {
        
        bytes = std::max(bytes, ALIGNMENT);// ȷ��bytes����ΪALIGNMENT
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};
#include "obj_list.h"

// 新建元素个数为 elementNum  的 list 对象
ObjList *newObjList(VM *vm, uint32_t elementNum)
{
    Value *elementArray = NULL;

    // 分配内存
    if (elementNum > 0)
    {
        elementArray = ALLOCATE_ARRAY(vm, Value, elementNum);
    }
    ObjList *objList = ALLOCATE(vm, ObjList);

    // 申请内存失败
    if (objList == NULL)
    {
        MEM_ERROR("allocate ObjList failed!");
    }

    // 初始化对象头
    initObjHeader(vm, &objList->objHeader, OT_LIST, vm->listClass);

    objList->elements.datas = elementArray;
    objList->elements.capacity = objList->elements.count = elementNum;

    return objList;
}

// 向 objList 中索引为 index 处插入 value，相当于 objList[index] = value
void insertElement(VM *vm, ObjList *objList, uint32_t index, Value value)
{
    // 如果索引 index 超出 objList 元素总长度，则报错
    if (index > objList->elements.count - 1)
    {
        RUN_ERROR("index out bounded!");
    }

    // 新增一个 Value 的空间用于保存新增的元素
    ValueBufferAdd(vm, &objList->elements, VT_TO_VALUE(VT_NULL));

    // 将 index 索引后面的元素整体后移一位
    uint32_t idx = objList->elements.count - 1;
    while (idx > index)
    {
        objList->elements.datas[idx] = objList->elements.datas[idx - 1];
        idx--;
    }

    // 在索引 index 处插入数值
    objList->elements.datas[index] = value;
}

Value removeElement(VM *vm, ObjList *objList, uint32_t index)
{
    // 找到被删除的元素，并在最后返回
    Value valueRemoved = objList->elements.datas[index];

    // 将 index 索引后面的元素整体前移一位
    uint32_t idx = index;
    while (idx < objList->elements.count - 1)
    {
        objList->elements.datas[idx] = objList->elements.datas[idx + 1];
        idx++;
    }

    // 宏 CAPACIRY_GROW_FACTOR 为 4
    // 当列表中元素实际使用空间不足列表容量的 1/4 时，就调用 shrinkList 函数调整列表容量
    uint32_t _capacity = objList->elements.capacity / CAPACIRY_GROW_FACTOR;
    if (_capacity > objList->elements.count)
    {
        shrinkList(vm, objList, _capacity);
    }

    // 计数减少一个元素
    objList->elements.count--;

    return valueRemoved;
}

// 调整 objList 的容量为 newCapacity（容量即列表最大可容纳的元素数量）
static void shrinkList(VM *vm, ObjList *objList, uint32_t newCapacity)
{
    // 调整 objList 被分配的内存空间
    uint32_t oldSize = objList->elements.capacity * sizeof(Value);
    uint32_t newSize = newCapacity * sizeof(Value);
    memManager(vm, objList->elements.datas, oldSize, newSize);
    // 调整 objList 的容量值
    objList->elements.capacity = newObjUpvalue;
}

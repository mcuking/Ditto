#include "obj_range.h"

// 新建 range 对象
ObjRange *newObjRange(VM *vm, int from, int to)
{
    // 分配内存
    ObjRange *objRange = ALLOCATE(vm, ObjRange);

    // 申请内存失败
    if (objRange == NULL)
    {
        MEM_ERROR("allocate ObjRange failed!");
    }

    // 初始化对象头
    initObjHeader(vm, &objRange->objHeader, OT_RANGE, vm->rangeClass);

    objRange->from = from;
    objRange->to = to;

    return objRange;
}

#include "class.h"
#include "meta_obj.h"

// 新建自由变量对象
// localVarPtr 是外部函数的局部变量在运行时栈的地址
ObjUpvalue *newObjUpvalue(VM *vm, Value *localVarPtr) {
    // 申请内存
    ObjUpvalue *objUpvalue = ALLOCATE(vm, ObjUpvalue);

    // 申请内存失败
    if (objUpvalue == NULL) {
        MEM_ERROR("allocate ObjUpvalue failed!");
    }

    // 初始化对象头
    initObjHeader(vm, &objUpvalue->objHeader, OT_UPVALUE, NULL);

    // 将外部函数的局部变量在运行时栈的地址赋值给变量 localVarPtr
    objUpvalue->localVarPtr = localVarPtr;

    objUpvalue->closedUpvalue = VT_TO_VALUE(VT_NULL);

    objUpvalue->next = NULL;

    return objUpvalue;
}

// 新建函数对象
ObjFn *newObjFn(VM *vm, ObjModule *objModule, uint32_t slotNum) {
    // 申请内存
    ObjFn *objFn = ALLOCATE(vm, ObjFn);

    // 申请内存失败
    if (objFn == NULL) {
        MEM_ERROR("allocate ObjFn failed!");
    }

    // 初始化对象头
    initObjHeader(vm, &objFn->objHeader, OT_FUNCTION, vm->fnClass);

    // 用于存储函数编译后的指令流
    ByteBufferInit(&objFn->instrStream);

    // 常量表，用于储存指令流单元中的常量
    // 实际上函数并没有常量，所以这里存储的是其他指令流单元的常量
    // 例如模块中定义的全局变量、类的类名等
    ValueBufferInit(&objFn->constants);

    // 函数（包括类中的方法）所属的模块
    objFn->module = objModule;

    // 函数参数的个数
    objFn->argNum = 0;

    // 函数所引用的自由变量 upvalue 的数量
    objFn->upvalueNum = 0;

    // 函数在运行时栈中所需的最大空间
    objFn->maxStackSlotUsedNum = slotNum;

    return objFn;
}

// 新建闭包对象
// 其中 objFn->upvalueNum 指的是该函数对象所引用自由变量的个数
ObjClosure *newObjClosure(VM *vm, ObjFn *objFn) {
    // 申请内存
    ObjClosure *objClosure =
        ALLOCATE_EXTRA(vm, ObjClosure, sizeof(ObjUpvalue *) * objFn->upvalueNum);

    // 申请内存失败
    if (objClosure == NULL) {
        MEM_ERROR("allocate ObjClosure failed!");
    }

    // 初始化对象头
    initObjHeader(vm, &objClosure->objHeader, OT_CLOSURE, vm->fnClass);

    objClosure->fn = objFn;

    // 清空自由变量数组 upvalues
    // 避免在向 upvalues 塞入真正的自由变量之前触发GC
    uint32_t idx = 0;
    while (idx < objFn->upvalueNum) {
        objClosure->upvalues[idx] = NULL;
        idx++;
    }

    return objClosure;
}

#include <stdlib.h>
#include "vm.h"
#include "core.h"

// 初始化虚拟机
void initVM(VM *vm)
{
    // 记录已经分配的内存总和
    vm->allocatedBytes = 0;
    // 当前词法分析器初始化为 NULL
    vm->curLexer = NULL;
    // 指向所有已分配对象链表的首节点，用于垃圾回收
    vm->allObjects = NULL;
    // 初始化模块集合
    vm->allModules = newObjMap(vm);
    // 初始化类的方法集合
    StringBufferInit(&vm->allMethodNames);
};

// 新建虚拟机
VM *newVM()
{
    // 为虚拟机申请内存，返回一个指针指向虚拟机
    VM *vm = (VM *)malloc(sizeof(VM));

    // 申请内存失败
    if (vm == NULL)
    {
        MEM_ERROR("allocate VM failed!");
    }

    // 调用 initVM 对虚拟机进行初始化
    initVM(vm);
    // 编译核心模块
    buildCore(vm);
    return vm;
}

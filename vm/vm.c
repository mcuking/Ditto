#include "vm.h"
#include "core.h"
#include <stdlib.h>

// 初始化虚拟机
void initVM(VM *vm) {
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
VM *newVM() {
    // 为虚拟机申请内存，返回一个指针指向虚拟机
    VM *vm = (VM *)malloc(sizeof(VM));

    // 申请内存失败
    if (vm == NULL) {
        MEM_ERROR("allocate VM failed!");
    }

    // 调用 initVM 对虚拟机进行初始化
    initVM(vm);
    // 编译核心模块
    buildCore(vm);
    return vm;
}

// 确保栈的容量及数据有效
// needSlots 表示栈最少具有的容量，如果当前栈容量 stackCapacity 大于需要的栈数量，则直接返回即可
void ensureStack(VM *vm, ObjThread *objThread, uint32_t needSlots) {
    if (objThread->stackCapacity > needSlots) {
        return;
    }

    // ceilToPowerOf2 找出大于等于 v 的最小的 2 次幂
    // 即容量的值需要是 2 的倍数
    uint32_t newStackCapacity = ceilToPowerOf2(needSlots);

    // 先将栈底记录下来，用来后面判断是否原地扩容
    // 背景知识：
    // 内存管理系统为了满足 realloc 这种扩容分配，会在所分配的空间上预留一部分空间以满足将来的原地扩容。
    // 如果扩容的增量大小还在预留的空间内，则原地扩容；
    // 如果扩容的大小大于预留的空间，则会重新找一块更大且连续的空间，同时将原内存空间的数据拷贝过去，并返回新分配的内存起始地址（不会继续利用之前的预留空间，否则会破坏虚拟地址的连续性）
    // 整个过程是内存管理系统自动完成的，不需要用户干涉
    // 但是这种开辟新空间的做法，会导致地址变化，相关指针就要更新为新地址，即如果该指针涉及原内存块，就需要调整该指针的值，确保指向正确的位置
    Value *oldStack = objThread->stack;

    // 扩大容量，申请内存，memManager 会返回新分配的内存起始地址，会被转成 Value 类型，起始地址被保存到 Value 结构体的 num 属性中
    objThread->stack = (Value *)memManager(vm, objThread->stack, objThread->stackCapacity * sizeof(Value), newStackCapacity * sizeof(Value));
    objThread->stackCapacity = newStackCapacity;

    // 申请内存后，将现在的栈底和之前保存的栈底做比较，如果相等则是原地扩容（即预留空间可以满足扩容的增量），否则就是重新开辟了一块内存空间
    long offset = objThread->stack - oldStack;

    // 如果是重新开辟一块内存空间，并将原来的数据拷贝到新空间中，
    // 就需要调整原指针的值，确保指向正确的位置
    if (offset != 0) {
        // 1.调整各个函数帧栈中的起始地址
        uint32_t idx = 0;
        while (idx < objThread->usedFrameNum) {
            objThread->frames[idx].stackStart += offset;
            idx++;
        }

        // 2.调整 “大栈” 的栈顶 esp
        objThread->esp += offset;

        // 3.调整自由变量 upvalue 中 localVarPtr (用于指向对应的自由变量 upvalue)
        ObjUpvalue *upvalue = objThread->openUpvalues;
        while (upvalue != NULL) {
            upvalue->localVarPtr += offset;
            upvalue = upvalue->next;
        }
    }
}

// 背景知识：
// 线程就是函数的容器，线程对象提供了一个 “大栈”，在线程中运行的多个函数会共享这个 “大栈”，各自使用其中一部分作为该函数闭包的运行时栈
// 线程就是任务调度器，会提供一个帧栈数组 frames，为每个函数闭包分配一个帧栈 frame（包括3个部分：1.运行时栈    2.待运行的指令流    3.当前运行的指令地址 ip）
// 其中运行时栈就是使用 Value 数组来模拟，关于 Value 请参考其结构定义部分

// 为线程 objThread 中运行的闭包函数 objClosure 准备帧栈 Frame，即闭包（函数或方法）的运行资源，包括如下：
// 1.运行时栈    2.待运行的指令流    3.当前运行的指令地址 ip
inline static void createFrame(VM *vm, ObjThread *objThread, ObjClosure *objClosure, int argNum) {
    // 如果当前使用的 frame 数量（算上这次使用的一个）大于 frame 的总容量，则将总容量扩大二倍
    if (objThread->usedFrameNum + 1 > objThread->frameCapacity) {
        uint32_t newCapacity = objThread->frameCapacity * 2;

        // 扩大容量，申请内存，memManager 会返回新分配的内存起始地址，会被转成 Frame 类型，起始地址被保存到 Frame 中的 stackStart 中的 num 属性中
        objThread->frames = (Frame *)memManager(vm, objThread->frames, objThread->frameCapacity * sizeof(Frame), newCapacity * sizeof(Frame));
        objThread->frameCapacity = newCapacity;
    }

    // 先计算目前 “大栈” 的大小：栈顶地址 - 栈底地址
    uint32_t stackSlots = (uint32_t)(objThread->esp - objThread->stack);
    // 再加上函数/方法执行时需要的最大的栈数，就是创建这次帧栈需要的栈的总大小
    uint32_t needSlots = stackSlots + objClosure->fn->maxStackSlotUsedNum;
    // 确保栈的容量及数据有效
    ensureStack(vm, objThread, needSlots);

    // 为线程 objThread 中运行的闭包函数 objClosure 准备帧栈 Frame
    // 第三个参数是被调用函数的帧栈在整个 “大栈” 中的起始地址
    // 减去参数个数，是为了函数闭包 objClosure 可以访问到栈中自己的参数（TODO: 暂未搞懂，后续回填）
    prepareFrame(objThread, objClosure, objThread->esp - argNum);
}

// 背景知识：
// 内层函数在引用外层函数中的局部变量，该局部变量对内层函数来说，就是自由变量 upvalue，其中又分为 open upvalue 和 closed upvalue
// open upvalue 是其指针 upvalue->localVarPtr 所指向的局部变量未被回收，仍在运行时栈中的 upvalue
// closed upvalue 是其指针 upvalue->localVarPtr 所指向的局部变量已经被回收，不在运行时栈的 upvalue
// 例如：当外层函数执行完闭，会将其在运行时栈的内存回收掉，其中就包括了局部变量，如果此局部变量被内层函数引用，且该内层函数又被外部使用时，
// 此时，就会将指针 upvalue->localVarPtr 指向的运行时栈中的局部变量的值，保存到 upvalue->closedUpvalue 变量中，
// 同时将指针 upvalue->localVarPtr 改为指向 upvalue->closedUpvalue，这个过程就是 open upvalue 转变为 closed upvalue 的过程，也就是关闭自由变量的操作
// 从而确保了就是被内层函数引用的局部变量在运行时栈中被回收了，内层函数仍可通过 upvalue->closedUpvalue 访问该局部变量的值。

// 注意：如果某个外层函数执行完，在运行时栈的内存被回收了，其作用域以及其内嵌更深的作用域的局部变量都应该被回收，而作用域越深的变量在运行时栈中的地址就会越大，
// （因为先调用外层函数，然后调用内层函数，所以外层函数的局部变量会先压入运行时栈，内层函数居后，而越后压入，地址也就越大）
// 所以只需要将指针 upvalue->localVarPtr 的值（被内层函数引用的局部变量的地址）大于某个值（例如 lastSlot）的所有 upvalue 都执行自由变量操作即可
// （upvalue 是以链表的形式保存，其中 objThread->openUpvalues 就是指向本线程中 “已经打开过的 upvalue” 的链表的首节点）

// 关闭自由变量 upvalue（注：满足其指针 upvalue->localVarPtr 大于 lastSlot 的自由变量）
static void closedUpvalue(ObjThread *objThread, Value *lastSlot) {
    ObjUpvalue *upvalue = objThread->openUpvalues;
    // 注意：在自由变量 upvalue 链表创建的时候，就保证了是按照 upvalue->localVarPtr 的值降序排序的，首节点的自由变量的 localVarPtr 最大
    while (upvalue != NULL && upvalue >= lastSlot) {
        // 将指针 upvalue->localVarPtr 指向的运行时栈中的局部变量的值，保存到 upvalue->closedUpvalue 变量中
        upvalue->closedUpvalue = *(upvalue->localVarPtr);
        // 将指针 upvalue->localVarPtr 改为指向 upvalue->closedUpvalue
        upvalue->localVarPtr = &(upvalue->closedUpvalue);
        // 获取自由变量 upvalue 链表中下一个自由变量 upvalue
        upvalue = upvalue->next;
    }
    objThread->openUpvalues = upvalue;
}

// 创建线程中已经打开过的 upvalue 的链表
// 指针 localVarPtr 就是指向运行时栈中的局部变量，按照 localVarPtr 的值倒序插入到该链表
static ObjUpvalue *createOpenUpvalue(VM *vm, ObjThread *objThread, Value *localVarPtr) {
    // 如果 objThread->openUpvalues 链表还未创建，则创建链表，首节点为基于参数 localVarPtr 的 upvalue
    if (objThread->openUpvalues == NULL) {
        objThread->openUpvalues = newObjUpvalue(vm, localVarPtr);
        return objThread->openUpvalues;
    }

    // 否则从前到后遍历链表，找到合适的位置插入新的 upvalue
    ObjUpvalue *preUpvalue = NULL;
    ObjUpvalue *upvalue = objThread->openUpvalues;

    // 因为  upvalue 链表已经默认按照 upvalue->localVarPtr 的值倒序排列，
    // 所以只要 upvalue->localVarPtr > localVarPtr，就继续向后遍历，直到不满足 upvalue->localVarPtr > localVarPtr 为止
    while (upvalue != NULL && upvalue->localVarPtr > localVarPtr) {
        preUpvalue = upvalue;
        upvalue = upvalue->next;
    }

    // 如果之前插入了该 upvalue，直接返回即可
    if (upvalue != NULL && upvalue->localVarPtr == localVarPtr) {
        return upvalue;
    }

    // 否则就创建一个新的 upvalue，并插入到 upvalue 链表中
    ObjUpvalue *newUpvalue = newObjUpvalue(vm, localVarPtr);

    if (preUpvalue == NULL) {
        // 如果 preUpvalue 仍为 NULL，说明上面的 while 循环没有执行，也就是说参数 localVarPtr 大于首节点 objThread->openUpvalues 的 localVarPtr
        // 所以需要将基于参数 localVarPtr 的 upvalue 设置为首节点
        objThread->openUpvalues = newUpvalue;
    } else {
        // 否则就在 preUpvalue 和 upvalue 之间插入基于参数 localVarPtr 的 upvalue
        preUpvalue->next = newUpvalue;
    }
    newUpvalue->next = upvalue;
    return newUpvalue;
}

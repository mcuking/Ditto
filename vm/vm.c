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

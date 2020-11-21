#include "obj_thread.h"
#include "vm.h"
#include "class.h"

// 为线程 objThread 中运行的闭包函数 objClosure 准备运行时栈
void prepareFrame(ObjThread *objThread, ObjClosure *objClosure, Value *stackStart)
{
    // 如果分配的帧栈超出最大容量，则报错
    ASSERT(objThread->frameCapacity > objThread->usedFrameNum, "frame not enough!");

    // 从函数调用帧栈（frames）数组中取最新的帧栈（frame）准备分配给运行函数
    Frame *frame = &(objThread->frames[objThread->usedFrameNum++]);

    // 线程中分配给每个函数的帧栈（frame）都是共用线程的运行时栈的
    // 帧栈（frame）的 stackStart 指向了各自帧栈（frame）在线程的运行时栈的起始位置
    frame->stackStart = stackStart;
    // 执行的闭包为 objClosure
    frame->closure = objClosure;
    // 指令起始地址是闭包中函数的指令流的起始地址
    frame->ip = objClosure->fn->instrStream.datas;
}

// 重置线程对象，即为闭包 objClosure 中的函数初始化运行时栈
void resetThread(ObjThread *objThread, ObjClosure *objClosure)
{
    // 将线程的运行时栈的栈顶 esp 置为栈底 stack
    objThread->esp = objThread->stack;
    objThread->openUpvalues = NULL;
    objThread->caller = NULL;
    objThread->errorObj = VT_TO_VALUE(VT_NULL);
    objThread->usedFrameNum = 0;

    // 闭包 objClosure 为空则报错
    ASSERT(objClosure != NULL, "objClosure is NULL in function resetThread");

    // 将线程的运行时栈底 objThread->stack 作为闭包 objClosure 中的函数的运行时栈的起始位置
    prepareFrame(objThread, objClosure, objThread->stack);
}

// 新建线程对象，线程中运行的是闭包 objClosure 中的函数
ObjHeader *newObjThread(VM *vm, ObjClosure *objClosure)
{
    // 闭包 objClosure 为空则报错
    ASSERT(objClosure != NULL, "objClosure is NULL");

    // 申请内存
    ObjThread *objThread = ALLOCATE(vm, ObjThread);

    // 申请内存失败
    if (objThread == NULL)
    {
        MEM_ERROR("allocate ObjThread failed!");
    }

    // 初始化对象头
    initObjHeader(vm, &objThread->objHeader, OT_THREAD, vm->threadClass);

    // 为函数帧栈（frame）数组申请内存，默认为 4 个 frame，即 INITIAL_FRAME_NUM 为 4
    Frame *frames = ALLOCATE_ARRAY(vm, Frame, INITIAL_FRAME_NUM);
    objThread->frames = frames;
    objThread->frameCapacity = INITIAL_FRAME_NUM;

    // 计算线程的运行时栈的容量
    // TODO: 线程的运行时栈容量 stackCapacity 为什么是大于等于函数的 maxStackSlotUsedNum + 1 的最小 2 次幂？
    // 补充：ceilToPowerOf2 方法作用--找出大于等于 v 的最小的 2 次幂
    uint32_t stackCapacity = ceilToPowerOf2(objClosure->fn->maxStackSlotUsedNum + 1);
    objThread->stackCapacity = stackCapacity;

    // 为线程的运行时栈申请内存
    Value *newStack = ALLOCATE_ARRAY(vm, Value, stackCapacity);
    objThread->stack = newStack;

    // 重置线程对象，即为闭包 objClosure 中的函数初始化运行时栈
    resetThread(objThread, objClosure);

    return objThread;
}

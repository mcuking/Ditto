#ifndef _OBJECT_OBJ_THREAD_H
#define _OBJECT_OBJ_THREAD_H
#include "obj_fn.h"

// 定义线程对象结构
typedef struct objThread {
    ObjHeader objHeader;

    Value *stack;           // 函数运行时栈的栈底
    Value *esp;             // 函数运行时栈的栈顶
    uint32_t stackCapacity; // 函数运行时栈的容量

    Frame *frames;          // 函数调用帧栈数组
    uint32_t usedFrameNum;  // 已使用的 frame 的数量，从 0 开始计数
    uint32_t frameCapacity; // 函数调用帧栈（frame）数组的容量

    ObjUpvalue *openUpvalues; // TODO: 暂时未搞懂，后面填坑

    struct objThread *caller; // 当前线程（thread）对象的调用者，若当前线程退出，则将控制权交回调用者（调用者本身也是一个线程对象）

    Value errorObj; // 导致运行时错误的对象会放在这里，否则为空
} ObjThread;

// 为线程 objThread 中运行的闭包函数 objClosure 准备运行时栈
void prepareFrame(ObjThread *objThread, ObjClosure *objClosure, Value *stackStart);

// 重置线程对象，即为闭包 objClosure 中的函数初始化运行时栈
void resetThread(ObjThread *objThread, ObjClosure *objClosure);

// 新建线程对象，线程中运行的是闭包 objClosure 中的函数
ObjThread *newObjThread(VM *vm, ObjClosure *objClosure);

#endif

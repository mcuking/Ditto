#ifndef _OBJECT_OBJ_FN_H
#define _OBJECT_OBJ_FN_H
#include "meta_obj.h"
#include "utils.h"

// 前置知识

// 闭包
// upvalue 是指内部函数所引用的位于外层函数中的局部变量
// open upvalue 是指当外层函数仍在运行时栈，内部函数仍可以通过运行时栈访问的局部变量
// close upvalue 是指当外层函数已经执行完，在运行时栈中被收回，
// 闭包机制会将该变量从栈复制到一个安全且只有该内部函数可以访问的地方
//（在这里的实现，安全的地方是 ObjUpvalue 结构体中的 closed Upvalue 变量）

// 指令流单元
// 独立的指令集合单元成为指令流单元，例如模块就是最大的指令流单元，函数、类中的每个方法、代码块、闭包都是指令流单元
// 只要是指令流单元就可以用 ObjFn 表示，因此 ObjFn 泛指一切指令流单元

// 定义函数中的调试的结构体
typedef struct {
    char *fnName;
    IntBuffer lineNo;
} FnDebug;

// 定义自由变量 upvalue 对象的结构体
typedef struct {
    ObjHeader objHeader;
    // 该指针用于指向对应的自由变量 upvalue，以供内部函数访问
    Value *localVarPtr;
    // 当外部函数执行完后，在运行时栈被释放，对应的自由变量 upvalue 也就不存在了
    // 此时会有如下操作：
    // 1. 对应的 upvalue 会从栈中被拷复制到 closedValue 中
    // 2. 然后指针 localVarPtr 指向 closedUpvalue
    // 这样的话引用 ObjUpvalue 的内部函数，仍可以通过 localVarPtr 访问到自由变量
    Value closedUpvalue;
    // 指向下一个 upvalue，从而形成 upvalue 链表
    struct upvalue *next;
} ObjUpvalue;

// 定义函数对象的结构体
typedef struct {
    ObjHeader objHeader;
    // 用于存储函数编译后的指令流
    ByteBuffer instrStream;
    // 常量表，用于储存指令流单元中的常量
    // 实际上函数并没有常量，所以这里存储的是其他指令流单元的常量
    // 例如模块中定义的全局变量、类的类名等
    ValueBuffer constants;
    // 函数（包括类中的方法）所属的模块
    ObjModule *module;
    // 函数参数的个数
    uint8_t argNum;
    // 函数所引用的自由变量 upvalue 的数量
    uint32_t upvalueNum;
    // 函数在运行时栈中所需的最大空间，这是在编译期间计算的
    uint32_t maxStackSlotUsedNum;

#if DEBUG
    // 只有在 debug 模式下才添加
    FnDebug *debug;
#endif
} ObjFn;

// 定义闭包对象的结构体
// 闭包：引用自由变量的内部函数 + 引用的自由变量集合
typedef struct {
    ObjHeader objHeader;
    // 引用自由变量的内部函数
    ObjFn *fn;
    // 引用的自由变量集合
    ObjUpvalue *upvalues[0];
} ObjClosure;

// 定义函数调用帧栈的结构体
typedef struct {
    // 程序计算器 PC，存储的是下一条指令的地址
    uint8_t *ip;
    // 待运行的闭包（函数引用了自由变量 upvalue 就变成了闭包）
    ObjClosure *closure;
    // 函数运行时栈的起始地址
    Value *stackStart;
} Frame;

// 线程中初始化的函数调用帧栈数量
#define INITIAL_FRAME_NUM 4

// 新建自由变量对象
ObjUpvalue *newObjUpvalue(VM *vm, Value *localVarPtr);

// 新建闭包对象
ObjClosure *newObjClosure(VM *vm, ObjFn *objFn);

// 新建函数对象
ObjFn *newObjFn(VM *vm, ObjModule *objModule, uint32_t maxStackSlotUsedNum);

#endif

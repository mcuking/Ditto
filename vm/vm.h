#ifndef _VM_VM_H
#define _VM_VM_H
#include "class.h"
#include "common.h"
#include "header_obj.h"
#include "obj_map.h"
#include "obj_thread.h"

// 为定义在 opcode.inc 中的操作码加上前缀 OPCODE_
#define OPCODE_SLOTS(opcode, effect) OPCODE_##opcode,

// 定义指令
// 通过上面对 OPCODE_SLOTS 的宏定义，可以获取到加了前缀的操作码
// 例如 OPCODE_SLOTS(LOAD_CONSTANT, 1) 返回的是 OPCODE_LOAD_CONSTANT
// 然后将这些指令集合声明称枚举数据 OpCode
// 之所以后面又将宏定义 OPCODE_SLOTS 取消定义，是因为其他地方也需要自定义 OPCODE_SLOTS 宏的逻辑，来获取不同的数据
typedef enum {
#include "opcode.inc"
} OpCode;
#undef OPCODE_SLOTS

// 虚拟机执行结果
typedef enum vmResult {
    VM_RESULT_SUCESS,
    VM_RESULT_ERROR
} VMResult;

struct vm {
    Class *fnClass;
    Class *classOfClass;
    Class *objectClass;
    Class *mapClass;
    Class *listClass;
    Class *rangeClass;
    Class *stringClass;
    Class *nullClass;
    Class *boolClass;
    Class *numClass;
    Class *threadClass;

    uint32_t allocatedBytes;    // 累计已分配的内存总和
    ObjHeader *allObjects;      // 累计已分配的所有对象的链表（用于垃圾回收）
    Lexer *curLexer;            // 当前词法分析器
    ObjThread *curThread;       // 当前正在执行的线程
    ObjMap *allModules;         // 所有模块
    SymbolTable allMethodNames; // 所有类的方法
};

// 初始化虚拟机
void initVM(VM *vm);

// 新建虚拟机
VM *newVM(void);

// 确保栈的容量及数据有效
// needSlots 表示栈最少具有的容量，如果当前栈容量 stackCapacity 大于需要的栈数量，则直接返回即可
void ensureStack(VM *vm, ObjThread *objThread, uint32_t needSlots);

// 执行指令
VMResult executeInstruction(VM *vm, register ObjThread *curThread);

#endif

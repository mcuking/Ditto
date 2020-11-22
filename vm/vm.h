#ifndef _VM_VM_H
#define _VM_VM_H
#include "common.h"
#include "header_obj.h"
#include "obj_thread.h"
#include "obj_map.h"

// 虚拟机执行结果
typedef enum vmResult
{
    VM_RESULT_SUCESS,
    VM_RESULT_ERROR
} VMResult;

struct vm
{
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

#endif

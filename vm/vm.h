#ifndef _VM_VM_H
#define _VM_VM_H
#include "common.h"
#include "header_obj.h"

struct vm
{
    Class *fnClass;
    Class *listClass;
    Class *rangeClass;
    Class *stringClass;
    // 记录已经分配的内存总和
    uint32_t allocatedBytes;
    // 当前词法分析器
    Lexer *curLexer;
    // 指向所有已分配对象链表的首节点，用于垃圾回收
    ObjHeader *allObjects;
};

// 初始化虚拟机
void initVM(VM *vm);

// 新建虚拟机
VM *newVM(void);

#endif

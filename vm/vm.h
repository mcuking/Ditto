#ifndef _VM_VM_H
#define _VM_VM_H
#include "common.h"

struct vm
{
    uint32_t allocatedBytes; // 记录已经分配的内存总和
    Lexer *curLexer;         // 当前词法分析器
};

void initVM(VM *vm);
VM *newVM(void);
#endif

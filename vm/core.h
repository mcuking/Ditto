#ifndef _VM_CORE_H
#define _VM_CORE_H
#include "util.h"

// 源码文件所在的根目录
extern char *rootDir;

// 读取源码文件的方法
char *readFile(const char *sourceFile);

// 执行模块
VMResult executeModule(VM *vm, Value moduleName, const char *sourceCode);

// 编译核心模块
void buildCore(VM *vm);

// 在 table 中查找符号 symbol，找到后返回索引，否则返回 -1
int getIndexFromSymbolTable(SymbolTable *table, const char *symbol, uint32_t length);

// 向 table 中添加符号 symbol，并返回其索引
int addSymbol(VM *vm, SymbolTable *table, const char *symbol, uint32_t length);

#endif

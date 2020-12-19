#ifndef _VM_CORE_H
#define _VM_CORE_H
#include "vm.h"

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

// 绑定方法到指定类
// 将方法 method 到类 class 的 methods 数组中，位置为 index
void bindMethod(VM *vm, Class *class, uint32_t index, Method method);

// 绑定 superClass 为 subClass 的基类
// 即继承基类的属性个数和方法（通过复制实现）
void bindSuperClass(VM *vm, Class *subClass, Class *superClass);

// 确保符号 symbol 已经添加到符号表 table 中，如果查找没有，则向其中添加
int ensureSymbolExist(VM *vm, SymbolTable *table, const char *symbol, uint32_t length);

#endif

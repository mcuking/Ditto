#ifndef _VM_CORE_H
#define _VM_CORE_H

// 源码文件所在的根目录
extern char *rootDir;

// 读取源码文件的方法
char *readFile(const char *sourceFile);

// 执行模块
VMResult executeModule(VM *vm, Value moduleName, const char *sourceCode);

// 编译核心模块
void buildCore(VM *vm);

#endif

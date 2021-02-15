#ifndef _INCLUDE_COMMON_H
#define _INCLUDE_COMMON_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// 定义了 VM Lexer Class 的完整实现
typedef struct vm VM;
typedef struct lexer Lexer;
typedef struct class Class;

// 定义部分数据结构
#define bool char
#define true 1
#define false 0

// 如果函数中一部分形参并未使用，编译时会发出警告
// 下面的定义就使用 gcc 内部属性 __attribute__ ((unused)) 告诉 gcc 不要报警
#define UNUSED __attribute__((unused))

// 为方便系统调试，就需要断言 ASSERT
// 如果宏 DEBUG 定义了，表明在调试阶段，才会定义 ASSERT
// 因此我们可以在 makefile 中编译的时候来定义宏 DEBUG，不需要的时候则去掉宏 DEBUG
// 相当于一个开关控制是否需要断言 ASSERT
#ifdef DEBUG
#define ASSERT(condition, errMsg)                                          \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "ASSERT failed! %s:%d In function %s(): %s\n", \
                    __FILE__, __LINE__, __func__, errMsg);                 \
            abort();                                                       \
        }                                                                  \
    } while (0);
#else
#define ASSERT(condition, errMsg) ((void)0)
#endif

// 宏 NOT_REACHED() 表示 “不可能到达”
// 用于一些程序中不能被执行到的地方，说明程序出错了
#define NOT_REACHED()                                            \
    do {                                                         \
        fprintf(stderr, "NOT_REACHED: %s:%d In function %s()\n", \
                __FILE__, __LINE__, __func__);                   \
        while (1)                                                \
            ;                                                    \
    } while (0);

#endif

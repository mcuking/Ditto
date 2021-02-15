#ifndef _INCLUDE_UTILS_H
#define _INCLUDE_UTILS_H
#include "common.h"

//  第一部分：内存分配

// 定义内存管理函数 memManager 原型
void *memManager(VM *vm, void *ptr, uint32_t oldSize, uint32_t newSize);

// 给类型为 type 的数据申请内存
#define ALLOCATE(vmPtr, type) \
    (type *)memManager(vmPtr, NULL, 0, sizeof(type))

// 针对柔性数组，除了为主类型 mainType 申请内存外，再额外申请 extraSize 大小的内存
#define ALLOCATE_EXTRA(vmPtr, mainType, extraSize) \
    (mainType *)memManager(vmPtr, NULL, 0, sizeof(mainType) + extraSize)

// 为数组申请内存
#define ALLOCATE_ARRAY(vmPtr, type, count) \
    (type *)memManager(vmPtr, NULL, 0, sizeof(type) * count)

// 释放数组占用的内存
#define DEALLOCATE_ARRAY(vmPtr, arrayPtr, count) \
    memManager(vmPtr, arrayPtr, sizeof(arrayPtr[0]) * count, 0)

// 释放内存
#define DEALLOCATE(vmPtr, memPtr) \
    memManager(vmPtr, memPtr, 0, 0)

// 第二部分：查找满足条件数的方法

// 定义找出大于等于 v 的最小的 2 次幂的函数 ceilToPowerOf2 原型
uint32_t ceilToPowerOf2(uint32_t v);

// 第三部分：数据缓冲区
// 基于四个类型：Int/Char/Byte/String 宏定义了四种数据缓冲区，用来存储这四种数据类型的数据
// 例如 ByteBuffer 结构如下，其中 datas 数组主要就是存储 byte 类型的数据，
// count 表示 datas 数组中实际存储的 byte 类型的数据，capacity 表示 datas 数组最多可存储  byte 类型的数据的最大容量：
// typedef struct
// {
//     byte *datas;
//     uint32_t count;
//     uint32_t capacity;
// } ByteBuffer;

typedef struct
{
    char *str;
    uint32_t length;
} String;

typedef struct
{
    uint32_t length;
    char start[0];
} CharValue;

#define DECLARE_BUFFER_TYPE(type)                                                 \
    typedef struct                                                                \
    {                                                                             \
        type *datas;                                                              \
        uint32_t count;                                                           \
        uint32_t capacity;                                                        \
    } type##Buffer;                                                               \
    void type##BufferInit(type##Buffer *buf);                                     \
    void type##BufferFillWrite(VM *vm,                                            \
                               type##Buffer *buf, type data, uint32_t fillCount); \
    void type##BufferAdd(VM *vm, type##Buffer *buf, type data);                   \
    void type##BufferClear(VM *vm, type##Buffer *buf);

#define DEFINE_BUFFER_METHOD(type)                                                         \
    void type##BufferInit(type##Buffer *buf) {                                             \
        buf->datas = NULL;                                                                 \
        buf->count = buf->capacity = 0;                                                    \
    }                                                                                      \
    void type##BufferFillWrite(VM *vm, type##Buffer *buf, type data, uint32_t fillCount) { \
        uint32_t newCounts = buf->count + fillCount;                                       \
        if (newCounts > buf->capacity) {                                                   \
            size_t oldSize = buf->capacity * sizeof(type);                                 \
            buf->capacity = ceilToPowerOf2(newCounts);                                     \
            size_t newSize = buf->capacity * sizeof(type);                                 \
            ASSERT(newSize > oldSize, "faint...memory allocate!");                         \
            buf->datas = (type *)memManager(vm, buf->datas, oldSize, newSize);             \
        }                                                                                  \
        uint32_t cnt = 0;                                                                  \
        while (cnt < fillCount) {                                                          \
            buf->datas[buf->count++] = data;                                               \
            cnt++;                                                                         \
        }                                                                                  \
    }                                                                                      \
    void type##BufferAdd(VM *vm, type##Buffer *buf, type data) {                           \
        type##BufferFillWrite(vm, buf, data, 1);                                           \
    }                                                                                      \
    void type##BufferClear(VM *vm, type##Buffer *buf) {                                    \
        size_t oldSize = buf->capacity * sizeof(buf->datas[0]);                            \
        memManager(vm, buf->datas, oldSize, 0);                                            \
        type##BufferInit(buf);                                                             \
    }

#define SymbolTable StringBuffer
typedef uint8_t Byte;
typedef char Char;
typedef int Int;
DECLARE_BUFFER_TYPE(Int)
DECLARE_BUFFER_TYPE(Char)
DECLARE_BUFFER_TYPE(Byte)
DECLARE_BUFFER_TYPE(String)

void symbolTableClear(VM *vm, SymbolTable *buffer);

// 第四部分：通用报错函数

// 定义全部的错误类型
typedef enum {
    ERROR_IO,
    ERROR_MEM,
    ERROR_LEX,
    ERROR_COMPILE,
    ERROR_RUNTIME,
} ErrorType;

// 定义通用报错函数的原型
void errorReport(void *lexer, ErrorType errorType, const char *fmt, ...);

// 宏定义不同类型错误的报错
#define IO_ERROR(...) \
    errorReport(NULL, ERROR_IO, __VA_ARGS__)

#define MEM_ERROR(...) \
    errorReport(NULL, ERROR_IO, __VA_ARGS__)

#define LEX_ERROR(lexer, ...) \
    errorReport(lexer, ERROR_LEX, __VA_ARGS__)

#define COMPILE_ERROR(lexer, ...) \
    errorReport(lexer, ERROR_COMPILE, __VA_ARGS__)

#define RUN_ERROR(...) \
    errorReport(NULL, ERROR_RUNTIME, __VA_ARGS__)

#define DEFAULT_BUFFER_SIZE 512

#endif

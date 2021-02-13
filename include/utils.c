#include "utils.h"
#include "lexer.h"
#include "vm.h"
#include <stdarg.h>
#include <stdlib.h>

// 内存管理函数，根据传入的参数会有三种不同作用：
// 1.申请内存 molloc：当 ptr 为 NULL 且 newSize 不为 0时，
// realloc(ptr, newSize) 相当于 malloc(newSize)，即申请内存
// 2.释放内存 free：当 ptr 不为 NULL 且 newSize 为 0 时，调用 free 进行释放内存
// 3.修改空间大小 realloc：当 ptr 不为 NULL 且 newSize 不为 0 时，则执行 realloc(ptr, newSize)
// 相当于修改空间大小，可能是在原内存空间继续分配新的空间，或者是重新分配一个新的内存空间
void *memManager(VM *vm, void *ptr, uint32_t oldSize, uint32_t newSize) {
    // 记录系统分配的内存总和
    vm->allocatedBytes += newSize - oldSize;

    // 避免 realloc(NULL, 0) 来定义新地址，该地址不能被释放
    if (newSize == 0) {
        free(ptr);
        return NULL;
    }

    // 将 ptr 指向的内存大小调整到 newSize
    // 如果将 realloc 的返回的地址直接赋给原指针变量，当 realloc 申请内存失败（内存不足等）则会返回 NULL，
    // 这样原指针变量就会被 NULL 替换，丢失原地址空间，无法释放而产生内存泄漏
    return realloc(ptr, newSize);
}

// 找出大于等于 v 的最小的 2 次幂
uint32_t ceilToPowerOf2(uint32_t v) {
    v += (v == 0); // 兼容 v 等于 0 时结果为 0 等边界情况
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

// TODO: 暂时未搞懂，后面填坑
DEFINE_BUFFER_METHOD(String)
DEFINE_BUFFER_METHOD(Int)
DEFINE_BUFFER_METHOD(Char)
DEFINE_BUFFER_METHOD(Byte)

// TODO: 暂时未搞懂，后面填坑
void symbolTableClear(VM *vm, SymbolTable *buffer) {
    uint32_t idx = 0;
    while (idx < buffer->count) {
        memManager(vm, buffer->datas[idx++].str, 0, 0);
    }
    StringBufferClear(vm, buffer);
}

// 通用报错函数
void errorReport(void *lexer, ErrorType errorType, const char *fmt, ...) {
    char buffer[DEFAULT_BUFFER_SIZE] = {'\0'};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, DEFAULT_BUFFER_SIZE, fmt, ap);
    va_end(ap);

    switch (errorType) {
        case ERROR_IO:
        case ERROR_MEM:
            fprintf(stderr, "%s:%d In function %s():%s\n",
                    __FILE__, __LINE__, __func__, buffer);
            break;
        case ERROR_LEX:
        case ERROR_COMPILE:
            ASSERT(lexer != NULL, "lexer is null!");
            fprintf(stderr, "%s:%d \"%s\"\n", ((Lexer *)lexer)->file,
                    ((Lexer *)lexer)->preToken.lineNo, buffer);
            break;
        case ERROR_RUNTIME:
            fprintf(stderr, "%s\n", buffer);
            break;
        default:
            NOT_REACHED
    }
    exit(1);
}

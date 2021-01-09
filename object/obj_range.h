#ifndef _OBJECT_OBJ_RANG_H
#define _OBJECT_OBJ_RANG_H
#include "class.h"
#include "vm.h"

// 定义 range 对象结构
typedef struct {
    ObjHeader objHeader;
    int from; // 范围的起始
    int to;   // 范围的结束
} ObjRange;

// 新建 range 对象
ObjRange *newObjRange(VM *vm, int from, int to);

#endif

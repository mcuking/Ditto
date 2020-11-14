#ifndef _OBJECT_OBJ_STRING_H
#define _OBJECT_OBJ_STRING_H
#include "header_obj.h"

// 定义字符串对象结构
typedef struct
{
    ObjHeader objHeader; // 对象头
    CharValue value;     // 字符串值
    uint32_t hashCode;   // 由字符串值计算的哈希值
} ObjString;

// 将字符串值根据 fnv-1a 算法转成对应哈希值
uint32_t hashString(char *str, uint32_t length);

// 根据字符串对象中的值设置对应的哈希值
void hashObjString(ObjString *objString);

// 新建字符串对象
ObjString *newObjString(VM *vm, const char *str, uint32_t length);

#endif

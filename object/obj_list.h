#ifndef _OBJECT_OBJ_LIST_H
#define _OBJECT_OBJ_LIST_H
#include "class.h"
#include "vm.h"

typedef struct
{
    ObjHeader objHeader;  // 对象头
    ValueBuffer elements; // 用于存储 list 中的元素
} ObjList;

// 新建 list 对象
ObjList *newObjList(VM *vm, uint32_t elementNum);

// 向 objList 中索引为 index 处插入 value，相当于 objList[index] = value
void insertElement(VM *vm, ObjList *objList, uint32_t index, Value value);

// 删除 objList 中索引为 index 处的元素，即删除 objList[index]
Value removeElement(VM *vm, ObjList *objList, uint32_t index);

#endif

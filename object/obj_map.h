#ifndef _OBJECT_OBJ_MAP_H
#define _OBJECT_OBJ_MAP_H
#include "header_obj.h"

// map 对象装载率，即容量利用率，即 map 对象中 Entry 的实际数量占 map 对象中  Entry 的容量 的百分比
#define MAP_LOAD_PERCENT 0.8

typedef struct {
    Value key;
    Value value;
} Entry; // 键值对

// 定义 map 对象结构
typedef struct
{
    ObjHeader objHeader;
    uint32_t capacity; // map 对象中  Entry 的容量（即最多容纳的 Entry 的数量）
    uint32_t count;    // map 对象中 Entry 的实际数量
    Entry *entries;    // Entry 数组
} ObjMap;

// 新建 map 对象
ObjMap *newObjMap(VM *vm);

// 向 map 对象的键值为 key 的地方设置值 value
void mapSet(VM *vm, ObjMap *objMap, Value key, Value value);

// 获取 map 对象的键值为 key 的地方的值
Value mapGet(VM *vm, ObjMap *objMap, Value key);

// 删除 map 对象的键值为 key 的地方的值
Value removeKey(VM *vm, ObjMap *objMap, Value key);

// 删除 map 对象
void clearMap(VM *vm, ObjMap *objMap);

#endif

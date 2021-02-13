#ifndef _OBJECT_HEADER_OBJ_H
#define _OBJECT_HEADER_OBJ_H
#include "utils.h"

typedef enum {
    OT_CLASS,    // 类
    OT_LIST,     // 列表
    OT_MAP,      // 散列数组
    OT_MODULE,   // 模块作用域
    OT_RANGE,    // 步进为 1 的数字范围
    OT_STRING,   // 字符串
    OT_UPVALUE,  // 自由变量，闭包中的概念
    OT_FUNCTION, // 函数
    OT_CLOSURE,  // 闭包
    OT_INSTANCE, // 对象实例
    OT_THREAD    // 线程
} ObjType;

// 对象头，用于记录元信息和垃圾回收
typedef struct objHeader {
    ObjType type;           // 对象类型
    bool isAccess;          // 对象是否可达，用于垃圾回收
    Class *class;           // 指向对象所属的类
    struct objHeader *next; // 指向下一个创建的对象，用于垃圾回收
} ObjHeader;

// 值类型
typedef enum {
    VT_UNDEFINED, // 未定义
    VT_NULL,      // 空
    VT_FALSE,     // 布尔假
    VT_TRUE,      // 布尔真
    VT_NUM,       // 数字
    VT_OBJ        // 对象
} ValueType;

// 通用的值结构
// 可以表示所有类型的值
typedef struct {
    // union 中的值由 type 的值决定
    ValueType type; // 值类型
    union {
        double num;
        ObjHeader *objHeader;
    };
} Value;

// TODO: 待后续解释
DECLARE_BUFFER_TYPE(Value)

// 初始化对象头
void initObjHeader(VM *vm, ObjHeader *objHeader, ObjType objType, Class *class);

#endif

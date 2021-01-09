#ifndef _OBJECT_META_OBJ_H
#define _OBJECT_META_OBJ_H
#include "obj_string.h"

// 定义模块对象结构
typedef struct
{
    ObjString *name; // 模块名称
    ObjHeader objHeader;
    SymbolTable moduleVarName;  // 模块中定义的全局变量名
    ValueBuffer moduleVarValue; // 模块中定义的全局变量值
} ObjModule;

// 定义实例对象结构
typedef struct {
    ObjHeader objHeader;
    // 同一个类的不同实例对象，不同点主要在于对象之间可以有不同的属性值，
    // 而属性个数、属性名以及方法，都是相同的，所以这些都放在类的结构中，
    // 这里只需要用数组 fields 记录实例对象的属性值即可
    Value fields[0];
} ObjInstance;

// 新建模块对象
ObjModule *newObjModule(VM *vm, const char *modName);

// 新建实例对象
ObjInstance *newObjInstance(VM *vm, Class *class);

#endif

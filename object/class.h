#ifndef _OBJECT_CLASS_H
#define _OBJECT_CLASS_H
#include "common.h"
#include "utils.h"
#include "header_obj.h"
#include "obj_string.h"
#include "obj_fn.h"

// 方法类型
typedef enum
{
    MT_NONE,      // 空方法
    MT_PRIMITIVE, // 用 C 实现的原生方法
    MT_SCRIPT,    // 脚本语言中实现的方法
    MT_FN_CALL    // 关于函数对象的调用方法，用于实现函数重载，如 fun1.call()
} MethodType;

/** 值结构转换 **/

// 将类型为 vt 的值转成 Value 结构
#define VT_TO_VALUE(vt) \
    ((Value){vt, {0}})

// 将 Bool 结构转成 Value 结构
#define BOOL_TO_VALUE(boolean) \
    boolean ? VT_TO_VALUE(VT_TRUE) : VT_TO_VALUE(VT_FALSE)

// 将 Value 结构转成  Bool 结构
#define VALUE_TO_BOOL(value) \
    value.type == VT_TRUE ? true : false

// 将 Number 结构转成 Value 结构
#define NUM_TO_VALUE(num) \
    ((Value){VT_NUM, {num}})

// 将 Value结构转成 Number 结构
#define VALUE_TO_NUM(value) \
    value.num

// 将 Object 结构转成 Value 结构
#define Obj_TO_VALUE(objPtr) \
    ((Value){VT_OBJ, {(ObjHeader *)(objPtr)}})

// 将 Value结构转成 Object 结构
#define VALUE_TO_OBJ(value) \
    value.objHeader

// 将 Value结构转成 String 结构
#define VALUE_TO_OBJSTR(value) \
    (ObjString *)VALUE_TO_OBJ(value)

// 将 Value结构转成 Function 结构
#define VALUE_TO_OBJFN(value) \
    (ObjFn *)VALUE_TO_OBJ(value)

// 将 Value结构转成 Closure 结构
#define VALUE_TO_OBJCLOSURE(value) \
    (ObjClosure *)VALUE_TO_OBJ(value)

// 将 Value结构转成 Class 结构
#define VALUE_TO_OBJCLASS(value) \
    (Class *)VALUE_TO_OBJ(value)

/** 判断值类型 **/

#define VALUE_IS_UNDEFINED(value) \
    value.type == VT_UNDEFINED

#define VALUE_IS_NULL(value) \
    value.type == VT_NULL

#define VALUE_IS_TRUE(value) \
    value.type == VT_TRUE

#define VALUE_IS_FALSE(value) \
    value.type == VT_FALSE

#define VALUE_IS_NUM(value) \
    value.type == VT_NUM

#define VALUE_IS_0(value) \
    VALUE_IS_NUM(value) && value.num == 0

#define VALUE_IS_OBJ(value) \
    value.type == VT_OBJ

// 判断是否是某个特定的对象类型
#define VALUE_IS_CERTAIN_OBJ(value, objType) \
    VALUE_IS_OBJ(value) && VALUE_TO_OBJ(value)->type == objType

#define VALUE_IS_OBJSTR(value) \
    VALUE_IS_CERTAIN_OBJ(value, OT_STRING)

#define VALUE_IS_OBJINSTANCE(value) \
    VALUE_IS_CERTAIN_OBJ(value, OT_INSTANCE)

#define VALUE_IS_OBJCLOSURE(value) \
    VALUE_IS_CERTAIN_OBJ(value, OT_CLOSURE)

#define VALUE_IS_OBJRANGE(value) \
    VALUE_IS_CERTAIN_OBJ(value, OT_RANGE)

#define VALUE_IS_OBJRANGE(value) \
    VALUE_IS_CERTAIN_OBJ(value, OT_RANGE)

#define VALUE_IS_CLASS(value) \
    VALUE_IS_CERTAIN_OBJ(value, OT_CLASS)

// 定义指向原生方法的指针 Primitive
// 系统中定义的原生方法太多，后面就用这个指针指向不同的方法，统一调用
// *Primitive 表示 Primitive 所指的原生方法本身，声明它的第一个参数为 vm
typedef char (*Primitive)(VM *vm, Value *args);

// 定义方法的结构体
typedef struct
{
    // union 中的值由 type 的值决定
    MethodType type;
    union
    {
        // 指向脚本语言方法关联的 C 方法
        // 即脚本语言方法其实是由原生方法实现的
        // 当 type 为 MT_PRIMITIVE 时有效
        Primitive primFn;

        // obj 指向脚本代码编译后的 ObjClosure 或 ObjFn
        // 当 type 为 MT_SCRPT 时有效
        ObjClosure *obj;
    };
} Method;

// TODO: 待后续解释
DECLARE_BUFFER_TYPE(Method)

// 定义类的结构
struct class
{
    // 类的名称
    ObjString *name;
    // 类也有自己的类，类的类就是 meta class，meta class 存储了类的原信息
    // 既然类也有所属类，那么就要有对象头
    ObjHeader objHeader;
    // 指向 class 的基类
    struct class *superClass;
    // 类中属性的数量，此数量包括了从基类继承的属性
    uint32_t fieldNum;
    // 存储所有的方法
    MethodBuffer methods;
};

// Bits64 用于存储 64 位数据
typedef union
{
    uint64_t bits64;
    uint32_t bits32[2];
    double num;
} Bits64;

// TODO: 待后续解释
#define CAPACIRY_GROW_FACTOR 4;

// TODO: 待后续解释
#define MIN_CAPACITY 64;

#endif

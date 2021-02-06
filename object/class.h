#ifndef _OBJECT_CLASS_H
#define _OBJECT_CLASS_H
#include "common.h"
#include "header_obj.h"
#include "obj_fn.h"
#include "obj_list.h"
#include "obj_range.h"
#include "obj_string.h"
#include "utils.h"

// 方法类型
typedef enum {
    MT_NONE,      // 空方法
    MT_PRIMITIVE, // 用 C 实现的原生方法
    MT_SCRIPT,    // 脚本语言中实现的方法
    MT_FN_CALL    // 关于函数对象的调用方法，用于实现函数重载，如 fun1.call()
} MethodType;

/** 值结构转换 **/

// 将类型为 vt 的值转成 Value 结构
#define VT_TO_VALUE(vt) \
    ((Value){vt, {0}}) // 强制类型转换成 Value

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
#define OBJ_TO_VALUE(objPtr) ({              \
    Value value;                             \
    value.type = VT_OBJ;                     \
    value.objHeader = (ObjHeader *)(objPtr); \
    value;                                   \
})

// 将 Value 结构转成 Object 结构
#define VALUE_TO_OBJ(value) \
    value.objHeader

// 将 Value 结构转成 String 结构
#define VALUE_TO_OBJSTR(value) \
    (ObjString *)VALUE_TO_OBJ(value)

// 将 Value 结构转成 Function 结构
#define VALUE_TO_OBJFN(value) \
    (ObjFn *)VALUE_TO_OBJ(value)

// 将 Value 结构转成 Range 结构
#define VALUE_TO_OBJRANGE(value) \
    (ObjRange *)VALUE_TO_OBJ(value)

// 将 Value 结构转成 Instance 结构
#define VALUE_TO_OBJINSTANCE(value) \
    (ObjInstance *)VALUE_TO_OBJ(value)

// 将 Value 结构转成 List 结构
#define VALUE_TO_OBJLIST(value) \
    (ObjList *)VALUE_TO_OBJ(value)

// 将 Value 结构转成 Map 结构
#define VALUE_TO_OBJMAP(value) \
    (ObjMap *)VALUE_TO_OBJ(value)

// 将 Value 结构转成 Closure 结构
#define VALUE_TO_OBJCLOSURE(value) \
    (ObjClosure *)VALUE_TO_OBJ(value)

// 将 Value 结构转成 Thread 结构
#define VALUE_TO_OBJTHREAD(value) \
    (ObjThread *)VALUE_TO_OBJ(value)

// 将 Value 结构转成 Module 结构
#define VALUE_TO_OBJMODULE(value) \
    (ObjModule *)VALUE_TO_OBJ(value)

// 将 Value 结构转成 Class 结构
#define VALUE_TO_CLASS(value) \
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
    // 当 type 为 MT_FN_CALL 时，primFn 和 obj 均为空，实例对象本身就是待调用的函数
    MethodType type;
    union {
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
    // 存储所有的实例方法
    MethodBuffer methods;
};

// Bits64 用于存储 64 位数据
typedef union {
    uint64_t bits64;
    uint32_t bits32[2];
    double num;
} Bits64;

// 容量的扩展倍数（用于 list、map 等对象的容量设置中）
#define CAPACIRY_GROW_FACTOR 4

// 最小容量（用于 map 等对象的容量设置中）
#define MIN_CAPACITY 64

// 判断 a 和 b 是否相等
bool valueIsEqual(Value a, Value b);

// 新建名字为 name，属性个数为 fieldNum 的裸类（裸类即没有归属的类，其对象头的 class 指针为空）
Class *newRawClass(VM *vm, const char *name, uint32_t fieldNum);

// 创建一个类
// 类名为 className，属性个数为 fieldNum，基类为 superClass
Class *newClass(VM *vm, ObjString *className, uint32_t fieldNum, Class *superClass);

// 获取对象所属的类
// inline 在函数定义之前（函数声明前无用），表示该函数为内联函数，即会将函数中的代码直接内联到调用的函数中，省去了调用独立函数的开销
inline Class *getClassOfObj(VM *vm, Value object);

#endif

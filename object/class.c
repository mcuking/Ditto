#include "class.h"
#include "common.h"
#include "compiler.h"
#include "core.h"
#include "obj_range.h"
#include "string.h"

// TODO: 待后续解释
DEFINE_BUFFER_METHOD(Method)

// 判断 a 和 b 是否相等
bool valueIsEqual(Value a, Value b) {
    // 类型不同则不相等
    if (a.type != b.type) {
        return false;
    }

    // 类型为数字则比较数值
    if (a.type == VT_NUM) {
        return a.num == b.num;
    }

    // 指向同一个对象头则相等
    if (a.objHeader == b.objHeader) {
        return true;
    }

    // 对象类型不同则不相等
    if (a.objHeader->type != b.objHeader->type) {
        return false;
    }

    // 若为字符串对象则比较字符串的内容是否相等
    // 先比较字符串的长度，是为了避免一个字符串时是另一个字符串的前缀，例如 "abc" 和 "abcd"
    // int memcmp(const void *str1, const void *str2, size_t n)) 把存储区 str1 和存储区 str2 的前 n 个字节进行比较
    // 返回值 = 0，则表示 str1 等于 str2；返回值 > 0，则表示 str2 小于 str1。
    if (a.objHeader->type == OT_STRING) {
        ObjString *strA = VALUE_TO_OBJSTR(a);
        ObjString *strB = VALUE_TO_OBJSTR(b);

        return (strA->value.length == strB->value.length && memcmp(strA->value.start, strB->value.start, strA->value.length) == 0);
    }

    // 若为 range 对象，则比较两个对象的 from / to 值
    if (a.objHeader->type == OT_RANGE) {
        ObjRange *rgA = VALUE_TO_OBJRANGE(a);
        ObjRange *rgB = VALUE_TO_OBJRANGE(b);

        return (rgA->from == rgB->from && rgA->to == rgB->to);
    }

    // 默认不相等
    return false;
}

// 新建名字为 name，属性个数为 fieldNum 的裸类（裸类即没有归属的类，其对象头的 class 指针为空）
Class *newRawClass(VM *vm, const char *name, uint32_t fieldNum) {
    // 申请内存
    Class *class = ALLOCATE(vm, Class);

    // 申请内存失败
    if (class == NULL) {
        MEM_ERROR("allocate RawClass failed!");
    }

    // 初始化对象头
    initObjHeader(vm, &class->objHeader, OT_CLASS, NULL);

    class->name = newObjString(vm, name, strlen(name));
    class->fieldNum = fieldNum;
    class->superClass = NULL; // 默认没有基类
    MethodBufferInit(&class->methods);

    return class;
}

// 创建一个类
// 类名为 className，属性个数为 fieldNum，基类为 superClass
Class *newClass(VM *vm, ObjString *className, uint32_t fieldNum, Class *superClass) {
    // 1. 先创建类的 meta 类 metaClass（为了和类做区分，类的 meta 类名称是在类的名称后面追加 metaClass）
    // 1.1 先创建 meta 类的类名，加 10 是为了在类名 className 后面追加字符串 " metaClass"
    char newClassName[MAX_ID_LEN + 10] = {'\0'};
    memcpy(newClassName, className->value.start, className->value.length);
    memcpy(newClassName + className->value.length, " metaClass", 10);
    // 1.2 创建 mata 类
    Class *metaClass = newRawClass(vm, newClassName, 0);
    // 1.3 设置 meta 类的 meta 类为 classOfClass
    metaClass->objHeader.class = vm->classOfClass;
    // 1.4 设置 meta 类的基类为 classOfClass
    bindSuperClass(vm, metaClass, vm->classOfClass);

    // 2. 创建类
    // 2.1 创建类的类名，仍利用之前的字符串变量 newClassName
    memcpy(newClassName, className->value.start, className->value.length);
    newClassName[className->value.length] = '\0';
    // 2.2 创建类
    Class *class = newRawClass(vm, newClassName, fieldNum);
    // 2.3 设置类的 meta 类
    class->objHeader.class = metaClass;
    // 2.4 设置类的基类
    bindSuperClass(vm, class, superClass);

    return class;
}

// 获取对象所属的类
Class *getClassOfObj(VM *vm, Value object) {
    switch (object.type) {
        case VT_NULL:
            return vm->nullClass;
        case VT_TRUE:
        case VT_FALSE:
            return vm->boolClass;
        case VT_NUM:
            return vm->numClass;
        case VT_OBJ:
            return VALUE_TO_OBJ(object)->class;
        default:
            NOT_REACHED()
    }
}

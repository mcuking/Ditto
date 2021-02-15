#include "obj_string.h"
#include "common.h"
#include "vm.h"
#include <string.h>

// 将字符串值根据 fnv-1a 算法转成对应哈希值
uint32_t hashString(const char *str, uint32_t length) {
    uint32_t hashCode = 2166136261, idx = 0;
    while (idx < length) {
        hashCode ^= str[idx];
        hashCode *= 16777619;
        idx++;
    }
    return hashCode;
}

// 根据字符串对象中的值设置对应的哈希值
void hashObjString(ObjString *objString) {
    objString->hashCode = hashString(objString->value.start, objString->value.length);
}

// 新建字符串对象
ObjString *newObjString(VM *vm, const char *str, uint32_t length) {
    //length为 0 时 str 必为 NULL  length 不为 0 时 str 不为 NULL
    ASSERT(length == 0 || str != NULL, "str length don't match str!");

    // 根据字符串对象结构体和字符串长度申请需要的内存
    // 注：之所以需要加 1，是因为需要设置字符串结束符 \0
    // 因为 objIString->value 是一个字符串，需要额外内存存储字符串本身数据，
    // 所以使用 ALLOCATE_EXTRA 来申请内存
    ObjString *objString = ALLOCATE_EXTRA(vm, ObjString, length + 1);

    // 内存申请失败
    if (objString == NULL) {
        MEM_ERROR("Allocating ObjString failed!");
    }

    /** 1. 初始化对象头 **/
    // 注意：&objString->objHeader 中 -> 优先级高于 &
    // 所以是取的 objHeader，然后再获取它的地址
    initObjHeader(vm, &objString->objHeader, OT_STRING, vm->stringClass);

    /** 2. 设置 value **/
    // 如果字符串非空，则需要赋值其内容
    objString->value.length = length;

    if (length > 0) {
        // 将 str 所指的字符串的 length 个字符复制到 objString->value.start 所指的字符串
        memcpy(objString->value.start, str, length);
    }

    // 结尾添加字符串结束符 \0
    objString->value.start[length] = '\0';

    /** 3. 设置 hashCode **/
    hashObjString(objString);

    return objString;
}

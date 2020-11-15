#include "class.h"
#include "common.h"
#include "string.h"
#include "obj_range.h"
#include "core.h"
#include "vm.h"

// TODO: 待后续解释
DEFINE_BUFFER_METHOD(Method)

// 判断 a 和 b 是否相等
bool valueIsEqual(Value a, Value b)
{
    // 类型不同则不相等
    if (a.type != b.type)
    {
        return false;
    }

    // 类型为数字则比较数值
    if (a.type == VT_NUM)
    {
        return a.num == b.num;
    }

    // 指向同一个对象头则相等
    if (a.objHeader == b.objHeader)
    {
        return true;
    }

    // 对象类型不同则不相等
    if (a.objHeader->type != b.objHeader->type)
    {
        return false;
    }

    // 若为字符串对象则比较字符串的内容是否相等
    // 先比较字符串的长度，是为了避免一个字符串时是另一个字符串的前缀，例如 "abc" 和 "abcd"
    // int memcmp(const void *str1, const void *str2, size_t n)) 把存储区 str1 和存储区 str2 的前 n 个字节进行比较
    // 返回值 = 0，则表示 str1 等于 str2；返回值 > 0，则表示 str2 小于 str1。
    if (a.objHeader->type == OT_STRING)
    {
        ObjString *strA = VALUE_TO_OBJSTR(a);
        ObjString *strB = VALUE_TO_OBJSTR(b);

        return (strA->value.length == strB->value.length && memcmp(strA->value.start, strB->value.start, strA->value.length));
    }

    // 若为 range 对象，则比较两个对象的 from / to 值
    if (a.objHeader->type == OT_RANGE)
    {
        ObjRange *rgA = VALUE_TO_OBJRANGE(a);
        ObjRange *rgB = VALUE_TO_OBJRANGE(b);

        return (rgA->from == rgB->from && rgA->to == rgB->to);
    }

    // 默认不相等
    return false;
}
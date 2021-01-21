#ifndef _COMPILER_COMPILER_H
#define _COMPILER_COMPILER_H
#include "obj_fn.h"

// 作用域中局部变量最多 128 个
#define MAX_LOCAL_VAR_NUM 128
// upvalue 最多 128 个
#define MAX_UPVALUE_NUM 128
// 标识符最大长度 128 个字符
#define MAX_ID_LEN 128
// 方法名最大长度 128 个字符
#define MAX_METHOD_NAME_LEN MAX_ID_LEN
// 参数最多 16 个
#define MAX_ARG_NUM 16

// 方法的签名最大长度
// 方法的签名 = 方法名长度 + '(' + n 个参数 + n -1 个参数分隔符 ',' + ')'
#define MAX_SIGN_LEN MAX_METHOD_NAME_LEN + MAX_ARG_NUM * 2 + 1

// 类中的域（属性）最多 128 个
#define MAX_FIELD_NUM 128

// 定义 Upvalue 的结构
typedef struct
{
    bool isEnclosingLocalVar; // 是否是直接外层函数的局部变量
    uint32_t index;           // 如果 isEnclosingLocalVar 为 true，则 index 表示外层函数中局部变量的索引，否则是外层函数中 upvalue 中的索引
} Upvalue;

// 定义局部变量的结构
// 注：局部变量的值是存储在运行时栈的，此处不存储
typedef struct
{
    const char *name; // 局部变量名
    uint32_t length;  // 局部变量名长度
    int scopeDepth;   // 局部变量的作用域
    bool isUpvalue;   // 该局部变量是否是其内层函数所引用的 upvalue，谁引用谁设置，因此此项是其内层函数设置的
} LocalVar;

// 定义方法签名类型的枚举
typedef enum {
    SIGN_CONSTRUCT,       // 构造函数
    SIGN_METHOD,          // 普通方法
    SIGN_GETTER,          // getter 方法
    SIGN_SETTER,          // setter 方法
    SIGN_SUBSCRIPT,       // getter 形式的下标
    SIGN_SUBSCRIPT_SETTER // setter 形式的下标
} SignatureType;

// 定义方法签名的结构
typedef struct
{
    SignatureType type; // 签名类型
    const char *name;   // 签名
    uint32_t length;    // 签名长度
    uint32_t argNum;    // 方法参数个数
} Signature;

// 定义 Loop 结构（实现 while 会用到此结构）
typedef struct loop {
    int condStartIndex;         // 循环中条件的起始地址
    int bodyStartIndex;         // 循环体起始地址
    int scopeDepth;             // 循环中若有 break，告诉它需要退出的作用域深度
    int exitIndex;              // 循环条件不满足时跳出循环体的目标地址
    struct loop *enclosingLoop; // 直接外层循环
} Loop;

// 定义 ClassBookKeep 结构（用于记录类编译时的信息）
// 注：每定义一个方法，就将这个方法在 vm->allMethodNames 中的索引 index 写入到 instantMethods 或 staticMehthods 中，
// 在写入之前先检查下 instantMethods 或 staticMehthods 是否已经存在 index，如果存在则报错重复定义，否则直接写入
typedef struct
{
    ObjString *name;          // 类名
    SymbolTable fields;       // 类的属性符号表（只包含实例属性，不包括类的静态属性）
    bool isStatic;            // 当前编译静态方法则为真
    IntBuffer instantMethods; // 实例方法的集合，只保存方法对应的索引，不保存方法体
    IntBuffer staticMehthods; // 静态方法的集合，只保存方法对应的索引，不保存方法体
    Signature *signature;     // 当前正在编译的方法的签名
} ClassBookKeep;

// 定义编译单元的结构，具体定义在 compiler.c 文件中
typedef struct compileUnit CompileUnit;

// 在模块 objModule 中定义名为 name，值为 value 的模块变量
int defineModuleVar(VM *vm, ObjModule *objModule, const char *name, uint32_t length, Value value);

// 编译模块 objModule 的方法
ObjFn *compileModule(VM *vm, ObjModule *objModule, const char *moduleCode);

#endif

#include <string.h>
#include "compiler.h"
#include "lexer.h"
#include "core.h"
#if DEBUG
#include "debug.h"
#endif

// 定义编译单元的结构
// 注：编译单元就是指令流，例如函数、类的方法等独立的指令流都是编译单元
struct compileUnit
{
    // 所编译的函数，用于存储编译单元的指令
    ObjFn *fn;

    // 编译单元中局部变量的集合
    LocalVar localVars[MAX_LOCAL_VAR_NUM];

    // 已经分配的局部变量个数
    uint32_t localVarNum;

    // 本层函数索所引用的 upvalue 的集合
    Upvalue upvalues[MAX_UPVALUE_NUM];

    // 表示正在编译的代码所在的作用域
    // 注：-1 表示模块作用域，0 表示最外层，1 以上表示相应的嵌套层
    int scopeDepth;

    // 表示该编译单元内所有指令对运行时栈的最终影响
    // 注：函数或方法运行时需要运行时栈，栈的大小不仅受参数个数和局部变量个数影响，指令也可能会操作栈（压栈/弹栈）
    uint32_t stackSlotNum;

    // 当前正在编译的循环层
    Loop *curLoop;

    // 用于记录正在编译的类的编译信息，便于语法检查
    ClassBookKeep *enclosingClassBK;

    // 包含该编译单元的编译单元，即直接外层编译单元
    struct compileUnit *enclosingUnit;

    // 当前词法解析器
    // 注：每个模块都有一个单独的词法分析器
    Lexer *curLexer;
};

// 在模块 objModule 中定义名为 name，值为 value 的模块变量
int defineModuleVar(VM *vm, ObjModule *objModule, const char *name, uint32_t length, Value value)
{
    // 如果模块变量名长度大于 MAX_ID_LEN 则报错
    if (length > MAX_ID_LEN)
    {
        // name 指向的变量名不一定以 \0 结尾，为保险起见，将其从源码串拷贝出来
        char id[MAX_ID_LEN] = {'\0'};
        memcpy(id, name, length);

        // defineModuleVar 函数调用场景有多种，可能还未创建词法分析器
        if (vm->curLexer != NULL)
        {
            COMPILE_ERROR(vm->curLexer, "length of identifier\"%s\"should no more than %d", id, MAX_ID_LEN);
        }
        else
        {
            MEM_ERROR("length of identifier\"%s\"should no more than %d", id, MAX_ID_LEN);
        }
    }

    // 查找模块变量名 name 在 objModule->moduleVarName 中的索引
    // 如果为 -1，说明不存在，则分别在 objModule->moduleVarName 和 objModule->moduleVarValue 中添加模块变量的名和值
    int symbolIndex = getIndexFromSymbolTable(&objModule->moduleVarName, name, length);

    if (symbolIndex == -1)
    {
        symbolIndex = addSymbol(vm, &objModule->moduleVarName, name, length);
        ValueBufferAdd(vm, &objModule->moduleVarValue, value);
    }
    else if (VALUE_IS_NUM(objModule->moduleVarValue.datas[symbolIndex]))
    {
        // 背景：
        // 模块变量相当于一个模块中的全局变量，支持使用变量在声明变量之前，
        // 在从上到下的编译阶段中，遇到模块变量声明，会将其对应在 objModule->moduleVarValue 上的值设置成 VT_NULL 的 Value 形式
        // 但当遇到使用的模块变量被使用了，但是目前为止没有看到该变量的声明，现将其对应在·objModule->moduleVarValue 上的值设置成行号，即 VT_NUM 的 Value 形式
        // 直到继续向下编译，如果遇到该全局变量的声明，则就会进到这个判断分支，即变量对应在 objModule->moduleVarValue 的值为 VT_NUM 类型，
        // 由此可以判断该变量已经在上面使用，但是没有声明，所以在这里将其设置成 VT_NULL 的 Value 形式
        // 直到编译解阶段结束且虚拟机运行之前，检查 objModule->moduleVarValue 中是否还有值类型为 VT_NUM 的模块变量，如果有就会报错
        // 误解：
        // 为什么变量的值会被设置成 VT_NULL 的 Value 形式，而不是代码中真正赋的值，例如 a = 1？
        // 1. 编译阶段：先将模块变量的值初始化为 VT_NULL，同时生成 a = 1 赋值的对应指令
        // 2. 运行时阶段：虚拟机执行赋值对应的指令，从而将 1 写到模块变量 a 对应的 objModule->moduleVarValue 值中
        // 目前只是处在编译阶段，古可以通过判断 objModule->moduleVarValue 的值的类型来判断是否是先使用后声明的情况
        objModule->moduleVarValue.datas[symbolIndex] = value;
    }
    else
    {
        // 已定义，则返回 -1（用于判断是否重复定义）
        symbolIndex = -1;
    }

    return symbolIndex;
}

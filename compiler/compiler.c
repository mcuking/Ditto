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

// 将 opcode 对运行时栈大小的影响定义到数组 opCodeSlotsUsed 中
#define OPCODE_SLOTS(opCode, effect) effect

// 通过上面对 OPCODE_SLOTS 的宏定义，可以获取到加了操作码对运行时栈大小的影响
// 例如 OPCODE_SLOTS(LOAD_CONSTANT, 1) 返回的是 1，也即是说执行 LOAD_CONSTANT 操作码后会使运行时栈增加一个 slot
// 然后将这些指令对运行时栈大小的影响集合，定义到 opCodeSlotsUsed 数组中
// 之所以后面又将宏定义 OPCODE_SLOTS 取消定义，是因为其他地方也需要自定义 OPCODE_SLOTS 宏的逻辑，来获取不同的数据
static const int opCodeSlotsUsed[] = {
#include "opcode.inc"
};
#undef OPCODE_SLOTS

// 初始化编译单元 CompileUnit
static void initCompileUnit(Lexer *lexer, CompileUnit *cu, CompileUnit *enclosingUnit, bool isMethod)
{
    lexer->curCompileUnit = cu;
    cu->curLexer = lexer;
    cu->enclosingUnit = enclosingUnit;
    cu->curLoop = NULL;
    cu->enclosingClassBK = NULL;

    // 三种情况：1. 模块中直接定义一级函数  2. 内层函数  3. 内层方法（即类的方法）

    if (enclosingUnit == NULL) // 1. 模块中直接定义一级函数，即没有外层函数
    {
        // 当前作用域就是模块作用域
        // 因为编译代码是从上到下从外到内的顺序，即从模块作用域开始，而模块作用域值为 -1
        cu->scopeDepth = -1;
        // 模块作用域中没有局部变量
        cu->localVarNum = 0;
    }
    else
    {
        if (isMethod) // 3. 内层方法（即类的方法）
        {
            // 如果是类的方法，默认设定隐式 this 为第 0 个局部变量（this 指向的即对象实例对象）
            cu->localVars[0].name = "this";
            cu->localVars[0].length = 4;
        }
        else // 2. 内层函数
        {
            // 为了和类的方法保持统一，会空出第 0 个局部变量
            cu->localVars[0].name = NULL;
            cu->localVars[0].length = 0;
        }

        // 第 0 个局部变量比较特殊，作用域设置为模块级别
        cu->localVars[0].scopeDepth = -1;
        // 该变量不可能是 upvalue，因内层函数不可能引用上层的 this 对象，this 只能暴露给本层对象
        cu->localVars[0].isUpvalue = false;

        // 第 0 个局部变量已经被分配了，因此初始化为 1
        cu->localVarNum = 1;

        // 函数或方法只会在第 0 层局部变量出现
        cu->scopeDepth = 0;
    }

    // 对于基于栈的虚拟机，局部变量是保存在运行时栈的
    // 因此初始化运行时栈时，其大小等于局部变量的大小
    cu->stackSlotNum = cu->localVarNum;

    // 新建 objFn 对象，用于存储编译单元的指令流
    cu->fn = newObjFn(cu->curLexer->vm, cu->curLexer->curModule, cu->localVarNum);
}

// 向函数的指令流中写入 1 字节，返回其索引
static int writeByte(CompileUnit *cu, int byte)
{
#if DEBUG
    // 调试状态时，还需要额外在 fn->debug->lineNo 中写入当前 token 所在行号，方便调试
    IntBufferAdd(cu->curLexer->vm, &cu->fn->debug->lineNo, cu->curLexer->preToken.lineNo);
#endif

    ByteBufferAdd(cu->curLexer->vm, &cu->fn->instrStream, (uint8_t)byte);
    return cu->fn->instrStream.count - 1;
}

// 向函数的指令流中写入操作码
static void writeOpCode(CompileUnit *cu, OpCode opCode)
{
    writeByte(cu, opCode);
    // 计算该编译单元需要用到的运行时栈总大小
    // opCode 为操作符集合对应的枚举数据，值为对应的索引值
    // 而 opCodeSlotsUsed 也是基于操作符集合生成的数组
    // 因此可以通过索引值找到操作符对应的该操作符执行后对运行时栈大小的影响
    cu->stackSlotNum += opCodeSlotsUsed[opCode];
    // 如果计算的累计需要运行时栈大小大于当前最大运行时栈使用大小，则更新当前运行时栈使用大小
    // 注意：这里记录的是运行过程中使用 slot 数量最多的情况，即记录栈使用过程中的峰值
    // 因为指令对栈大小的影响有正有负，stackSlotNum 运行到最后可能为 0
    // 但运行过程中对栈的使用量不可能为 0
    if (cu->stackSlotNum > cu->fn->maxStackSlotUsedNum)
    {
        cu->fn->maxStackSlotUsedNum = cu->stackSlotNum;
    }
}

// 写入 1 个字节的操作数
static int writeByteOperand(CompileUnit *cu, int operand)
{
    return writeByte(cu, operand);
}

// 写入 2 个字节的操作数
// 按照大端字节序写入参数，低地址写高位，高地址写低位
inline static void writeShortOperand(CompileUnit *cu, int operand)
{
    writeByte(cu, (operand >> 8) & 0xff); // 先取高 8 位的值，也就是高地址的字节
    writeByte(cu, operand & 0xff);        // 后取低 8 位的值，也就是低地址的字节
}

// 写入操作数为 1 字节大小的指令
static int writeOpCodeByteOperand(CompileUnit *cu, OpCode opCode, int operand)
{
    // 1. 写操作码
    writeOpCode(cu, opCode);
    // 2. 写操作数
    return writeByteOperand(cu, operand);
}

// 写入操作数为 2 字节大小的指令
static void writeOpCodeShortOperand(CompileUnit *cu, OpCode opCode, int operand)
{
    // 1. 写操作码
    writeOpCode(cu, opCode);
    // 2. 写操作数
    writeShortOperand(cu, operand);
}

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

// 编译模块 objModule 的方法
// TODO: 等待后续完善
ObjFn *compileModule(VM *vm, ObjModule *objModule, const char *moduleCode)
{
}

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

// 定义操作符的绑定权值，即优先级
// 从上到下优先级递增
typedef enum
{
    BP_NONE,      // 无绑定能力
    BP_LOWEST,    // 最低绑定能力
    BP_ASSIGN,    // =
    BP_CONDITION, // ?:
    BP_LOGIC_OR,  // ||
    BP_LOGIC_AND, // &&
    BP_EQUAL,     // == !=
    BP_IS,        // is
    BP_CMP,       // < > <= >=
    BP_BIT_OR,    // |
    BP_BIT_AND,   // &
    BP_BIT_SHIFT, // << >>
    BP_RANGE,     // ..
    BP_TERM,      // + -
    BP_FACTOR,    // * / %
    BP_UNARY,     // - ! ~
    BP_CALL,      // . () []
    BP_HIGHEST    // 最高绑定能力
} BindPower;

// 函数指针，用于指向各种符号的 nud 和 led 方法
typedef void (*DenotationFn)(CompileUnit *cu, bool canAssign);

// 函数指针，用于指向类的方法（方法会用方法签名作为 ID）
typedef void (*methodSignatureFn)(CompileUnit *cu, Signature *signature);

// 符号绑定规则
typedef struct
{
    const char *id;               // 符号的字符串
    BindPower lbp;                // 左绑定权值，不关心左操作数的符号该值为 0
    DenotationFn nud;             // 字面量、变量、前缀运算符等不关心左操作数的 Token 调用该方法
    DenotationFn led;             // 中缀运算符等关心左操作数的 Token 调用该方法
    methodSignatureFn methodSign; // 表示该符号在类中被视为一个方法，所以为其生成一个方法签名，语义分析中涉及方法签名生成指令
} SymbolBindRule;

// 前缀符号（不关注左操作数的符号）
// 包括字面量、变量名、前缀符号等非运算符
#define PREFIX_SYMBOL(nud)             \
    {                                  \
        NULL, BP_NONE, nud, NULL, NULL \
    }

// 前缀运算符，例如 - ! ~
#define PREFIX_OPERATOR(id)                                    \
    {                                                          \
        id, BP_NONE, unaryOperator, NULL, unaryMethodSignature \
    }

// 中缀符号（关注左操作数的符号）
// 包括 1. 数组[  2. 函数(  3. 实例和方法之前的. 等等
#define INFIX_SYMBOL(lbp, led)     \
    {                              \
        NULL, lbp, NULL, led, NULL \
    }

// 中缀运算符，例如 - ! ~
#define INEFIX_OPERATOR(id, lbp)                           \
    {                                                      \
        id, lbp, NULL, infixOperator, infixMethodSignature \
    }

// 即可做前缀运算符，也可做中缀运算符，例如 - +
#define MIX_OPERATOR(id)                                              \
    {                                                                 \
        id, BP_TERM, unaryOperator, infixOperator, mixMethodSignature \
    }

// 对于没有规则的符号，用 UNUSED_RULE 占位用的
#define UNUSED_RULE                     \
    {                                   \
        NULL, BP_NONE, NULL, NULL, NULL \
    }

// 符号绑定规则的数组
// 按照 lexer.h 中定义的 TokenType 中各种类型的 token 顺序添加对应的符号绑定规则
SymbolBindRule Rules[] = {
    UNUSED_RULE,            // TOKEN_INVALID
    PREFIX_SYMBOL(literal), // TOKRN_NUM
    PREFIX_SYMBOL(literal), // TOKEN_STRING
};

// 初始化编译单元 CompileUnit
static void
initCompileUnit(Lexer *lexer, CompileUnit *cu, CompileUnit *enclosingUnit, bool isMethod)
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

// 向编译单元中 fn->constants 中添加常量，并返回索引
static uint32_t addConstant(CompileUnit *cu, Value constant)
{
    ValueBufferAdd(cu->curLexer->vm, &cu->fn->constants, constant);
    return cu->fn->constants.count - 1;
}

// 生成加载常量的指令
static void emitLoadConstant(CompileUnit *cu, Value constant)
{
    // 1. 将常量通过 addConstant 加载到常量表，并获取其在常量表中的索引 index
    int index = addConstant(cu, constant);
    // 2. 生成操作符为 OPCODE_LOAD_CONSTANT，操作数为 index 的指令
    writeOpCodeShortOperand(cu, OPCODE_LOAD_CONSTANT, index);
}

// 字面量（即常量，包括数字、字符串）的 nud 方法
// 即在语法分析时，遇到常量时，就调用该 nud 方法直接生成将该常量添加到运行时栈的指令即可
static void literal(CompileUnit *cu, bool canAssign UNUSED)
{
    // 是 preToken 的原因：
    // 当进入到某个 token 的 led/nud 方法时，curToken 为该 led/nud 方法所属 token 的右边的 token
    // 所以 led/nud 所属的 token 就是 preToken
    emitLoadConstant(cu, cu->curLexer->preToken.value);
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

// 编译程序
// TODO: 等待后续完善
static void compileProgram(CompileUnit *cu)
{
}

// 编译模块 objModule 的方法
ObjFn *compileModule(VM *vm, ObjModule *objModule, const char *moduleCode)
{
    // 每个模块（文件）都需要一个单独的词法分析器进行编译
    Lexer *lexer;
    lexer->parent = vm->curLexer;
    vm->curLexer = &lexer;

    // 初始化词法分析器
    if (objModule->name == NULL)
    {
        // 核心模块对应的词法分析器用 core.script.inc 作为模块名进行初始化
        initLexer(vm, lexer, "core.script.inc", moduleCode, objModule);
    }
    else
    {
        // 其余模块对应的词法分析器用该模块名进行初始化
        initLexer(vm, lexer, (const char *)objModule->name->value.start, moduleCode, objModule);
    }

    // 初始化编译单元（模块也有编译单元）
    // 有编译单元的：模块、函数、方法
    CompileUnit moduleCU;
    initCompileUnit(lexer, &moduleCU, NULL, false);

    //记录当前编译模块的变量数量，后面检查预定义模块变量时可减少遍历
    uint32_t moduleVarNumBefor = objModule->moduleVarValue.count;

    // 由于 initLexer 初始化函数中将 lexer 的 curToken 的 type 设置为 TOKEN_UNKNOWN
    // 会导致后面的 while 循环不执行（循环体用于执行真正编译的方法）
    // 需要调用 getNextToken 指向第一个合法的 token
    getNextToken(&lexer);

    // 循环调用 compileProgram 函数进行编译，直到 token 流结尾
    // TOKEN_EOF 标记文件结束，即该 token 为最后一个 token
    while (!matchToken(lexer, TOKEN_EOF))
    {
        compileProgram(&moduleCU);
    }

    // TODO: 等待后续完善
}

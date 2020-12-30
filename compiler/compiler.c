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

// 按照所处作用域类型划分变量类型
typedef enum
{
    VAR_SCOPE_INVALID,
    VAR_SCOPE_LOCAL,   // 局部变量
    VAR_SCOPE_UPVALUE, // upvalue 变量
    VAR_SCOPE_MODULE   // 模块变量
} VarScopeType;

// 变量结构体，用于内部变量查找
typedef struct
{
    VarScopeType scopeType; // 变量类型
    int index;              // 指向变量
} Variable;

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
// 按照 lexer.h 中定义的枚举 TokenType 中各种类型的 token 顺序，来添加对应的符号绑定规则
// 之所以要按照顺序填写，是为了方便用枚举值从 Rules 数组中找到某个类型的 token 的对应的符号绑定规则
SymbolBindRule Rules[] = {
    UNUSED_RULE,            // TOKEN_INVALID
    PREFIX_SYMBOL(literal), // TOKRN_NUM
    PREFIX_SYMBOL(literal), // TOKEN_STRING
};

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

// 将方法的签名对象转化成字符串
static uint32_t sign2String(Signature *sign, char *buf)
{
    uint32_t pos = 0;

    // 将方法签名中的方法名 xxx 复制到 buf 中
    memcpy(buf[pos], sign->name, sign->length);
    pos += sign->length;

    switch (sign->type)
    {
    // getter 方法无参数，形式是： xxx
    case SIGN_GETTER:
        break;
    // setter 方法只有一个参数，形式是：xxx=(_)
    case SIGN_SETTER:
        buf[pos++] = '=';
        buf[pos++] = '(';
        buf[pos++] = '_';
        buf[pos++] = ')';
        break;
    // 构造函数和普通方法形式是：xxx(_,...)
    case SIGN_CONSTRUCT:
    case SIGN_METHOD:
        buf[pos++] = '(';
        uint32_t idx = 0;
        while (idx < sign->argNum)
        {
            buf[pos++] = '_';
            buf[pos++] = ',';
            idx++;
        }

        if (idx == 0)
        {
            // 说明没有参数
            buf[pos++] = ')';
        }
        else
        {
            // 有参数的话则将最后多出来的 , 覆盖成 )
            buf[pos - 1] = ')';
        }
        break;
    // subscribe 方法形式：xxx[_,...]
    case SIGN_SUBSCRIPT:
        buf[pos++] = '[';
        uint32_t idx = 0;
        while (idx < sign->argNum)
        {
            buf[pos++] = '_';
            buf[pos++] = ',';
            idx++;
        }

        if (idx == 0)
        {
            // 说明没有参数
            buf[pos++] = ']';
        }
        else
        {
            // 有参数的话则将最后多出来的 , 覆盖成 )
            buf[pos - 1] = ']';
        }
        break;
    // subscribe setter 方法形式：xxx[_,...] = (_)
    // 这里处理的是等号左边的参数，因此需要减 1
    case SIGN_SUBSCRIPT_SETTER:
        buf[pos++] = '[';
        uint32_t idx = 0;
        while (idx < sign->argNum - 1)
        {
            buf[pos++] = '_';
            buf[pos++] = ',';
            idx++;
        }

        if (idx == 0)
        {
            // 说明没有参数
            buf[pos++] = ']';
        }
        else
        {
            // 有参数的话则将最后多出来的 , 覆盖成 )
            buf[pos - 1] = ']';
        }
        buf[pos++] = '=';
        buf[pos++] = '(';
        buf[pos++] = '_';
        buf[pos++] = ')';
        break;
    default:
        break;
    }
    buf[pos] = '\0';
    return pos;
}

// 添加局部变量到编译单元 cu 的 localVars 数组中，并返回该变量的索引值
static uint32_t addLocalVar(CompileUnit *cu, const char *name, uint32_t length)
{
    LocalVar *var = &(cu->localVars[cu->localVarNum]);
    var->name = name;
    var->name = length;
    var->scopeDepth = cu->scopeDepth;
    var->isUpvalue = false;
    return cu->localVarNum++;
}

// 声明局部变量（最后会调用上面的 addLocalVar）
static declareLocalVar(CompileUnit *cu, const char *name, uint32_t length)
{
    // 一个编译单元中所有局部作用域的局部变量总数不能超过 MAX_LOCAL_VAR_NUM，即 128 个
    if (cu->localVarNum > MAX_LOCAL_VAR_NUM)
    {
        COMPILE_ERROR(cu->curLexer, "the max amount of local variable of one compile unit (such as function) is %d", MAX_LOCAL_VAR_NUM);
    }

    // 检测当前作用域是否存在同名变量
    // 之所以倒序遍历，是因为 scopeDepth 是随着作用域越深（越局部）而越大，而 localVars 数组中的局部变量是按照作用域的深度递增排列，
    // 即越深的作用变量越排在 localVars 数组中的后面
    // 所以只需要从后向前遍历，最后的变量始终是当前作用域的变量
    int idx = (int)cu->localVarNum - 1;

    while (idx >= 0)
    {
        LocalVar *var = &(cu->localVars[idx]);

        // 当向前遍历时发现已经进入到父级作用域，则退出
        // 因为只需要检测同一个局部作用域下是否存在变量名冲突
        if (var->scopeDepth < cu->scopeDepth)
        {
            break;
        }

        // 如果在同一个局部作用域下之前已声明同名的局部变量，则报错
        if (var->length == length && memcmp(var->name, name, length) == 0)
        {
            char id[MAX_ID_LEN] = {'\0'};
            memcpy(id, name, length);
            COMPILE_ERROR(cu->curLexer, "identifier \"%s\" redefinition!", id);
        }

        idx--;
    }

    // 如果既没有超出最大局部变量数量限制，也没有同名局部变量已声明
    // 则直接添加局部变量到 cu 的 localVars 数组中
    return addLocalVar(cu, name, length);
}

// 根据作用域的类型声明变量，即声明模块变量还是局部变量
static int declareVariable(CompileUnit *cu, const char *name, uint32_t length)
{
    // 如果为模块作用域，则声明为模块变量
    if (cu->scopeDepth == -1)
    {
        // 先将变量值初始化为 NULL
        int index = defineModuleVar(cu->curLexer->vm, cu->curLexer->curModule, name, length, VT_TO_VALUE(VT_NULL));

        // 如果同名模块变量为已经定义，则报错
        if (index == -1)
        {
            char id[MAX_ID_LEN] = {'\0'};
            memcpy(id, name, length);
            COMPILE_ERROR(cu->curLexer, "identifier \"%s\" redefinition!", id);
        }
        return index;
    }

    // 否则为局部作用域，声明为局部变量
    return declareLocalVar(cu, name, length);
}

//下面调用下面三个生成方法签名的函数之时，preToken 为方法名，curToken 为方法名右边的符号
// 例如 test(a)，preToken 为 test，curToken 为 (
// 在调用方 compileMethod 中，方法名已经获取了，只需要下面的函数获取符号方法的类型、方法参数个数

// 为单运算符的符号方法生成方法签名
static void unaryMethodSignature(CompileUnit *cu UNUSED, Signature *sign)
{
    // 单运算符的符号方法类型 Getter 方法，且没有方法参数
    sign->type = SIGN_GETTER;
}

// 为中缀运算符的符号方法生成方法签名
static void infixMethodSignature(CompileUnit *cu UNUSED, Signature *sign UNUSED)
{
    // 中缀运算符的符号方法类型普通方法
    sign->type = SIGN_METHOD;

    // 方法参数个数为 1 个
    // 例如 2 + 3 相当于 2.+(3) ，其中 + 方法参数为 3，参数个数为 1
    sign->argNum = 1;

    // 期待当前 token 类型为 TOKEN_LEFT_PAREN，并读入下个 token
    assertCurToken(cu->curLexer, TOKEN_LEFT_PAREN, "expect '(' after infix operator!");

    // 期待当前 token 类型为 TOKEN_ID，并读入下个 token
    assertCurToken(cu->curLexer, TOKEN_ID, "expect variable name!");

    // 声明中缀运算符的符号方法参数为变量
    // 此处之所以是 preToken，是因为上个 assertCurToken 方法已经读入了参数
    declareVariable(cu, cu->curLexer->preToken.start, cu->curLexer->preToken.length);

    // 期待当前 token 类型为 TOKEN_RIGHT_PAREN，并读入下个 token
    assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect ')' after paramter!");
}

// 为既可做单运算符也可做中缀运算符的符号方法生成方法签名
static void mixMethodSignature(CompileUnit *cu UNUSED, Signature *sign UNUSED)
{
    // 先默认为单运算符
    sign->type = SIGN_GETTER;

    // 如果 curToken 为 (，则为中缀运算符
    // 注意此处 matchToken 方法中如果满足条件，会读入下一个 token
    if (matchToken(cu->curLexer, TOKEN_LEFT_PAREN))
    {
        // 中缀运算符的符号方法类型普通方法
        sign->type = SIGN_METHOD;

        // 方法参数个数为 1 个
        // 例如 2 + 3 相当于 2.+(3) ，其中 + 方法参数为 3，参数个数为 1
        sign->argNum = 1;

        // 期待当前 token 类型为 TOKEN_ID，并读入下个 token
        assertCurToken(cu->curLexer, TOKEN_ID, "expect variable name!");

        // 声明中缀运算符的符号方法参数为变量
        // 此处之所以是 preToken，是因为上个 assertCurToken 方法已经读入了参数
        declareVariable(cu, cu->curLexer->preToken.start, cu->curLexer->preToken.length);

        // 期待当前 token 类型为 TOKEN_RIGHT_PAREN，并读入下个 token
        assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect ')' after paramter!");
    }
}

// 语法分析的核心方法 expression，用来解析表达式结果
// 只是负责调用符号的 led 或 nud 方法，不负责语法分析，至于 led 或 nud 方法中是否有语法分析功能，则是该符号自己协调的事
// 这里以中缀运算符表达式 aSwTeUg 为例进行注释讲解
// 其中大写字符代表运算符，小写字符代表操作数
// expression 开始由运算符 S 调用的，所以 rbp 为运算符 S 的绑定权值
static void expression(CompileUnit *cu, BindPower rbp)
{
    // expression 是由运算符 S 调用的，对于中缀运算符来说，此时 curToken 为操作数 w
    // 找到操作数 w 的 nud 方法
    DenotationFn nud = Rules[cu->curLexer->curToken.type].nud;

    ASSERT(nud != NULL, "nud is NULL!");

    // 获取下一个 token
    // 执行后 curToken 为运算符 T
    getNextToken(cu->curLexer);

    // canAssign 用于判断是否具备可赋值的环境
    // 即当运算符 S 的绑定权值 rbp 小于 BP_ASSIGN 时，才能保证左值属于赋值运算符
    bool canAssign = rbp < BP_ASSIGN;
    // 执行操作数 w 的 nud 方法，计算操作数 w 的值
    nud(cu, canAssign);

    // rbp 为运算符 S 的对操作数的绑定权值
    // 因 curToken 目前为运算符 T，所以 Rules[cu->curLexer->curToken.type].lbp 为运算符 T 对操作数的绑定权值
    // 如果运算符 S 绑定权值大于运算符 T 绑定权值，则操作数 w 为运算符 S 的右操作数，则不进入循环，直接将操作数 w 作为运算符 S 的右操作数返回
    // 反之，则操作数 w 为运算符 T 的左操作数，进入循环
    while (rbp < Rules[cu->curLexer->curToken.type].lbp)
    {
        // curToken 为运算符 T，因此该函数是获取运算符 T 的 led 方法
        DenotationFn led = Rules[cu->curLexer->curToken.type].led;

        // 获取下一个 token
        // 执行后 curToken 为操作数 e
        getNextToken(cu->curLexer);

        // 执行运算符 T 的 led 方法，去构建以运算符 T 为根，以操作数 w 为左节点的语法树
        // 右节点是通过 led 方法里继续递归调用 expression 去解析的
        led(cu, canAssign);
    }
}

// 基于方法签名 生成 调用方法的指令
// 包括 callX 和 superX，即普通方法和基类方法
static void emitCallBySignature(CompileUnit *cu, Signature *sign, OpCode opcode)
{
    // MAX_SIGN_LEN 为方法签名的最大长度
    char signBuffer[MAX_SIGN_LEN];
    // 将方法的签名对象转化成字符串 signBuffer
    uint32_t length = sign2String(sign, signBuffer);

    // 确保名为 signBuffer 的方法已经在 cu->curLexer->vm->allMethodNames 中，没有查找到，则向其中添加
    int symbolIndex = ensureSymbolExist(cu->curLexer->vm, &cu->curLexer->vm->allMethodNames, signBuffer, length);

    // 写入调用方法的指令，其中：
    // 操作码为 callX 或 superX，X 表示调用方法的参数个数，例如 OPCODE_CALL15
    // 操作数为方法在 cu->curLexer->vm->allMethodNames 的索引值
    writeOpCodeShortOperand(cu, opcode + sign->argNum, symbolIndex);

    // 如果是调用基类方法，则再写入一个值为 VT_NULL 的操作数，为基类方法预留一个空位
    // 因为基类方法可能是在子类方法之后定义，因此不能保证基类方法已经被编译完成
    // 将来绑定方法是在装入基类
    if (opcode == OPCODE_SUPER0)
    {
        writeShortOperand(cu, addConstant(cu, VT_TO_VALUE(VT_NULL)));
    }
}

// 基于方法签名 生成 调用方法的指令
// 仅限于 callX，即普通方法
// 方法名 name  方法名长度 length  方法参数个数 argNum
static void emitCall(CompileUnit *cu, const char *name, int length, int argNum)
{
    // 确保名为 name 的方法已经在 cu->curLexer->vm->allMethodNames 中，没有查找到，则向其中添加
    int symbolIndex = ensureSymbolExist(cu->curLexer->vm, &cu->curLexer->vm->allMethodNames, name, length);
    // 写入调用方法的指令，其中：
    // 操作码为 callX，X 表示调用方法的参数个数，例如 OPCODE_CALL15
    // 操作数为方法在 cu->curLexer->vm->allMethodNames 的索引值
    writeOpCodeShortOperand(cu, OPCODE_CALL0 + argNum, symbolIndex);
}

// 中缀运算符（例如 + - * /）的 led 方法
// 即调用此方法对中缀运算符进行语法分析
// 切记，进入任何一个符号的 led 或 nud 方法时，preToken 都是该方法所属符号（即操作符），curToken 为该方法所属符号的右边符号（即操作数）
static void infixOperator(CompileUnit *cu, bool canAssign UNUSED)
{
    // 获取该方法所属符号对应的绑定规则
    SymbolBindRule *rule = &Rules[cu->curLexer->preToken.type];

    // 对于中缀运算符，其对左右操作数的绑定权值相同
    BindPower rbp = rule->lbp;
    // 解析操作符的右操作数
    expression(cu, rbp);

    // 例如表达式 3+2 就会被视为 3.+(2)
    // 其中 3 为对象，+ 是方法，2 为参数
    // 即 op1.operator(op2)，只有一个参数 op2
    // 下面定义该方法对应的签名
    Signature sign = {
        SIGN_METHOD,      // 类型为普通方法
        rule->id,         // 方法名即操作运算符的名字
        strlen(rule->id), // 方法名长度即操作运算符的名字长度
        1                 // 参数只有一个，即 op2。实际上在虚拟机中会有个默认参数，即调用该方法的对象实例，在这里就是 op1，所以就可以计算 op1 和 op2 的结果
    };

    // 基于该中缀操作符方法的签名 生成 调用该中缀运算符方法的指令
    // 方法的参数就是该符号的右操作数，或者右操作树的计算结果
    emitCallBySignature(cu, &sign, OPCODE_CALL0);
}

// 前缀运算符（例如 ! -）的 nud 方法
// 即调用此方法对前缀运算符进行语法分析
static void unaryOperator(CompileUnit *cu, bool canAssign UNUSED)
{
    // 获取该方法所属符号对应的绑定规则
    SymbolBindRule *rule = &Rules[cu->curLexer->preToken.type];

    // 解析操作符的右操作数，绑定权值为 BP_UNARY，绑定权值较高
    // 不能用前缀运算符的对左操作数的绑定权值（其值最低，为 BP_NONE）
    // 因为前缀运算符只关心右操作数，不关系左操作数
    expression(cu, BP_UNARY);

    // 生成调用该前缀运算符方法的指令
    // 前缀运算符方法的名字长度，即 strlen(rule->id) 为 1
    // 前缀运算符方法的参数为 0
    // 例如表达式 !3 会被视为 3.!，即 3 调用 ! 操作符对应的方法
    // 实际上在虚拟机中会有个默认参数，即调用该方法的对象实例，在这里就是数字对象 3，所以就可以计算 3 取反的结果
    emitCall(cu, rule->id, strlen(rule->id), 0);
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

// 声明模块变量
// 区别于 defineModuleVar 函数，该函数不做重定义检查，默认直接声明
static int declareModuleVar(VM *vm, ObjModule *objModule, const char *name, uint32_t length, Value value)
{
    ValueBufferAdd(vm, &objModule->moduleVarValue, value);
    return addSymbol(vm, &objModule->moduleVarName, name, length);
}

// 获取包含 cu->enclosingClassBK 的最近的编译单元 CompileUnit，注：
// 目前模块、类的方法、函数会有对应的编译单元，类本身没有
// 在编译某个模块中的类的方法时，为了快捷地找到该方法所属的类的，
// 会将该类的 ClassBookKeep 结构赋值给该模块对应的编译单元的 cu->enclosingClassBK
// 这样类的方法要找到所属的类，只需要在对应的的编译单元的父编译单元（或父编译单元的父编译单元）即模块的编译单元中，找到其中的 cu->enclosingClassBK 即可
static CompileUnit *getEnclosingClassBKUnit(CompileUnit *cu)
{
    while (cu != NULL)
    {
        if (cu->enclosingClassBK != NULL)
        {
            return cu;
        }
        // 向上找父编译单元，即直接外层编译单元
        cu = cu->enclosingUnit;
    }
    return NULL;
}

// 获取包含 cu->enclosingClassBK 的最近的编译单元 CompileUnit 中的 cu->enclosingClassBK
static ClassBookKeep *getEnclosingClassBK(CompileUnit *cu)
{
    CompileUnit *ncu = getEnclosingClassBKUnit(cu);

    if (ncu != NULL)
    {
        return ncu->enclosingClassBK;
    }
    return NULL;
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

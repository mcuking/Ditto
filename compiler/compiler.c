#include "compiler.h"
#include "core.h"
#include "lexer.h"
#include <string.h>
#if DEBUG
#include "debug.h"
#endif

// 定义编译单元的结构
// 注：编译单元就是指令流，例如函数、类的方法等独立的指令流都是编译单元
struct compileUnit {
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
#define OPCODE_SLOTS(opCode, effect) effect,

// 通过上面对 OPCODE_SLOTS 的宏定义，可以获取到加了操作码对运行时栈大小的影响
// 例如 OPCODE_SLOTS(LOAD_CONSTANT, 1) 返回的是 1，也即是说执行 LOAD_CONSTANT 操作码后会使运行时栈增加一个 slot
// 然后将这些指令对运行时栈大小的影响集合，定义到 opCodeSlotsUsed 数组中
// 之所以后面又将宏定义 OPCODE_SLOTS 取消定义，是因为其他地方也需要自定义 OPCODE_SLOTS 宏的逻辑，来获取不同的数据
static const int opCodeSlotsUsed[] = {
#include "opcode.inc"
};
#undef OPCODE_SLOTS

// 按照所处作用域类型划分变量类型
typedef enum {
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
typedef enum {
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

static uint32_t addConstant(CompileUnit *cu, Value constant);
static void expression(CompileUnit *cu, BindPower rbp);
static void compileProgram(CompileUnit *cu);
static void infixOperator(CompileUnit *cu, bool canAssign UNUSED);
static void unaryOperator(CompileUnit *cu, bool canAssign UNUSED);
static void compileStatement(CompileUnit *cu);

// 初始化编译单元 CompileUnit
// enclosingUnit 表示直接外层编译单元
// isMethod 表示是否是类的方法
static void initCompileUnit(Lexer *lexer, CompileUnit *cu, CompileUnit *enclosingUnit, bool isMethod) {
    lexer->curCompileUnit = cu;
    cu->curLexer = lexer;
    cu->enclosingUnit = enclosingUnit;
    cu->curLoop = NULL;
    cu->enclosingClassBK = NULL;

    // 三种情况：1. 模块中直接定义一级函数  2. 内层函数  3. 内层方法（即类的方法）

    // enclosingUnit == NULL 说明没有直接外层单元，即当前处在模块的编译单元，也就是正在编译模块
    if (enclosingUnit == NULL) {
        // 1. 模块中直接定义一级函数，即没有外层函数
        // 当前作用域就是模块作用域
        // 因为编译代码是从上到下从外到内的顺序，即从模块作用域开始，而模块作用域值为 -1
        cu->scopeDepth = -1;
        // 模块作用域中没有局部变量
        cu->localVarNum = 0;
    } else {
        // 3. 内层方法（即类的方法）
        if (isMethod) {
            // 如果是类的方法，默认设定隐式 this 为第 0 个局部变量（this 指向的即对象实例对象）
            cu->localVars[0].name = "this";
            cu->localVars[0].length = 4;
        } else {
            // 2. 内层函数
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
static int writeByte(CompileUnit *cu, int byte) {
#if DEBUG
    // 调试状态时，还需要额外在 fn->debug->lineNo 中写入当前 token 所在行号，方便调试
    IntBufferAdd(cu->curLexer->vm, &cu->fn->debug->lineNo, cu->curLexer->preToken.lineNo);
#endif

    ByteBufferAdd(cu->curLexer->vm, &cu->fn->instrStream, (uint8_t)byte);
    return cu->fn->instrStream.count - 1;
}

// 向函数的指令流中写入操作码
static void writeOpCode(CompileUnit *cu, OpCode opCode) {
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
    if (cu->stackSlotNum > cu->fn->maxStackSlotUsedNum) {
        cu->fn->maxStackSlotUsedNum = cu->stackSlotNum;
    }
}

// 写入 1 个字节的操作数
static int writeByteOperand(CompileUnit *cu, int operand) {
    return writeByte(cu, operand);
}

// 写入 2 个字节的操作数
// 按照大端字节序写入参数，低地址写高位，高地址写低位
inline static void writeShortOperand(CompileUnit *cu, int operand) {
    writeByte(cu, (operand >> 8) & 0xff); // 先取高 8 位的值，也就是高地址的字节
    writeByte(cu, operand & 0xff);        // 后取低 8 位的值，也就是低地址的字节
}

// 写入操作数为 1 字节大小的指令
static int writeOpCodeByteOperand(CompileUnit *cu, OpCode opCode, int operand) {
    // 1. 写操作码
    writeOpCode(cu, opCode);
    // 2. 写操作数
    return writeByteOperand(cu, operand);
}

// 写入操作数为 2 字节大小的指令
static void writeOpCodeShortOperand(CompileUnit *cu, OpCode opCode, int operand) {
    // 1. 写操作码
    writeOpCode(cu, opCode);
    // 2. 写操作数
    writeShortOperand(cu, operand);
}

// 获取 ip 所指向的操作码的操作数占用的字节数
uint32_t getBytesOfOperands(Byte *instrStream, Value *constants, int ip) {
    switch ((OpCode)instrStream[ip]) {
        case OPCODE_CONSTRUCT:
        case OPCODE_RETURN:
        case OPCODE_END:
        case OPCODE_CLOSE_UPVALUE:
        case OPCODE_PUSH_NULL:
        case OPCODE_PUSH_FALSE:
        case OPCODE_PUSH_TRUE:
        case OPCODE_POP:
            return 0;

        case OPCODE_CREATE_CLASS:
        case OPCODE_LOAD_THIS_FIELD:
        case OPCODE_STORE_THIS_FIELD:
        case OPCODE_LOAD_FIELD:
        case OPCODE_STORE_FIELD:
        case OPCODE_LOAD_LOCAL_VAR:
        case OPCODE_STORE_LOCAL_VAR:
        case OPCODE_LOAD_UPVALUE:
        case OPCODE_STORE_UPVALUE:
            return 1;

        case OPCODE_CALL0:
        case OPCODE_CALL1:
        case OPCODE_CALL2:
        case OPCODE_CALL3:
        case OPCODE_CALL4:
        case OPCODE_CALL5:
        case OPCODE_CALL6:
        case OPCODE_CALL7:
        case OPCODE_CALL8:
        case OPCODE_CALL9:
        case OPCODE_CALL10:
        case OPCODE_CALL11:
        case OPCODE_CALL12:
        case OPCODE_CALL13:
        case OPCODE_CALL14:
        case OPCODE_CALL15:
        case OPCODE_CALL16:
        case OPCODE_LOAD_CONSTANT:
        case OPCODE_LOAD_MODULE_VAR:
        case OPCODE_STORE_MODULE_VAR:
        case OPCODE_LOOP:
        case OPCODE_JUMP:
        case OPCODE_JUMP_IF_FALSE:
        case OPCODE_AND:
        case OPCODE_OR:
        case OPCODE_INSTANCE_METHOD:
        case OPCODE_STATIC_METHOD:
            return 2;

        case OPCODE_SUPER0:
        case OPCODE_SUPER1:
        case OPCODE_SUPER2:
        case OPCODE_SUPER3:
        case OPCODE_SUPER4:
        case OPCODE_SUPER5:
        case OPCODE_SUPER6:
        case OPCODE_SUPER7:
        case OPCODE_SUPER8:
        case OPCODE_SUPER9:
        case OPCODE_SUPER10:
        case OPCODE_SUPER11:
        case OPCODE_SUPER12:
        case OPCODE_SUPER13:
        case OPCODE_SUPER14:
        case OPCODE_SUPER15:
        case OPCODE_SUPER16:
            // OPCODE_SUPERx 的操作数是分别由 writeOpCodeShortOperand
            // 和 writeShortOperand 写入的,共 1 个操作码和 4 个字节的操作数
            return 4;

        case OPCODE_CREATE_CLOSURE: {
            // 操作码 OPCODE_CREATE_CLOSURE 的操作数是待创建闭包的函数在常量表中的索引，占 2 个字节

            // 但当虚拟机执行该命令时，已经在直接外层编译单元了，是没办法直到内层函数 upvalue 数组 cu->upvalues 中哪些是直接外层编译单元的局部变量，哪些是直接外层编译单元的 upvalue
            // 只有知道，才能在运行时栈找到对应的值，所以需要将 是直接外层编译单元的局部变量还是 upvalue 信息记录下来
            // 这里就直接写入了直接外层编译单元的指令流 cu->enclosingUnit->fn->instrStream 中
            // 按照 {upvalue 是否是直接编译外层单元的局部变量，upvalue 在直接外层编译单元的索引} 成对信息写入的（占两个字节），该逻辑在 endCompileUnit 函数中

            // 所以最终操作数的字节数为 2 + upvalueNum * 2

            // 按照大端字节序，其中 instrStream[ip + 1] 为操作数中的低位地址端，保存的是索引的高 8 位
            // instrStream[ip + 2] 为操作数中的高位地址端，保存的是索引的低 8 位，下面就是基于两个字节的操作数的值计算出函数在常量表中的索引值
            uint32_t fnIdx = (instrStream[ip + 1] << 8) | instrStream[ip + 2];

            return 2 + (VALUE_TO_OBJFN(constants[fnIdx]))->upvalueNum * 2;
        }

        default:
            NOT_REACHED();
    }
}

// 向编译单元中 fn->constants 中添加常量，并返回索引
static uint32_t addConstant(CompileUnit *cu, Value constant) {
    ValueBufferAdd(cu->curLexer->vm, &cu->fn->constants, constant);
    return cu->fn->constants.count - 1;
}

// 生成【加载常量到常量表，并将常量压入到运行时栈顶】的指令
static void emitLoadConstant(CompileUnit *cu, Value constant) {
    // 1. 将常量通过 addConstant 加载到常量表，并获取其在常量表中的索引 index
    int index = addConstant(cu, constant);
    // 2. 生成【将常量压入到运行时栈顶】的指令
    writeOpCodeShortOperand(cu, OPCODE_LOAD_CONSTANT, index);
}

// 编译字面量（即常量，包括数字、字符串），即字面量的 nud 方法
// 即在语法分析时，遇到常量时，就调用该 nud 方法直接生成将该常量添加到运行时栈的指令即可
static void literal(CompileUnit *cu, bool canAssign UNUSED) {
    // 是 preToken 的原因：
    // 当进入到某个 token 的 led/nud 方法时，curToken 为该 led/nud 方法所属 token 的右边的 token
    // 所以 led/nud 所属的 token 就是 preToken
    emitLoadConstant(cu, cu->curLexer->preToken.value);
}

// 将方法的签名对象转化成字符串
static uint32_t sign2String(Signature *sign, char *buf) {
    uint32_t pos = 0;

    // 将方法签名中的方法名 xxx 复制到 buf 中
    memcpy(buf[pos], sign->name, sign->length);
    pos += sign->length;

    switch (sign->type) {
        // getter 方法无参数，形式是： xxx
        case SIGN_GETTER:
            break;
        // setter 方法只有一个参数，形式是：xxx=(_)
        case SIGN_SETTER: {
            buf[pos++] = '=';
            buf[pos++] = '(';
            buf[pos++] = '_';
            buf[pos++] = ')';
            break;
        }
        // 构造函数和普通方法形式是：xxx(_,...)
        case SIGN_CONSTRUCT:
        case SIGN_METHOD: {
            buf[pos++] = '(';
            uint32_t idx = 0;
            while (idx < sign->argNum) {
                buf[pos++] = '_';
                buf[pos++] = ',';
                idx++;
            }

            if (idx == 0) {
                // 说明没有参数
                buf[pos++] = ')';
            } else {
                // 有参数的话则将最后多出来的 , 覆盖成 )
                buf[pos - 1] = ')';
            }
            break;
        }
        // subscribe 方法形式：xxx[_,...]
        case SIGN_SUBSCRIPT: {
            buf[pos++] = '[';
            uint32_t idx = 0;
            while (idx < sign->argNum) {
                buf[pos++] = '_';
                buf[pos++] = ',';
                idx++;
            }

            if (idx == 0) {
                // 说明没有参数
                buf[pos++] = ']';
            } else {
                // 有参数的话则将最后多出来的 , 覆盖成 )
                buf[pos - 1] = ']';
            }
            break;
        }
        // subscribe setter 方法形式：xxx[_,...] = (_)
        // 这里处理的是等号左边的参数，因此需要减 1
        case SIGN_SUBSCRIPT_SETTER: {
            buf[pos++] = '[';
            uint32_t idx = 0;
            while (idx < sign->argNum - 1) {
                buf[pos++] = '_';
                buf[pos++] = ',';
                idx++;
            }

            if (idx == 0) {
                // 说明没有参数
                buf[pos++] = ']';
            } else {
                // 有参数的话则将最后多出来的 , 覆盖成 )
                buf[pos - 1] = ']';
            }
            buf[pos++] = '=';
            buf[pos++] = '(';
            buf[pos++] = '_';
            buf[pos++] = ')';
            break;
        }
        default:
            break;
    }
    buf[pos] = '\0';
    return pos;
}

// 添加局部变量到编译单元 cu 的 localVars 数组中，并返回该变量的索引值
static uint32_t addLocalVar(CompileUnit *cu, const char *name, uint32_t length) {
    LocalVar *var = &(cu->localVars[cu->localVarNum]);
    var->name = name;
    var->name = length;
    var->scopeDepth = cu->scopeDepth;
    var->isUpvalue = false;
    return cu->localVarNum++;
}

// 声明局部变量（最后会调用上面的 addLocalVar）
static declareLocalVar(CompileUnit *cu, const char *name, uint32_t length) {
    // 一个编译单元中所有局部作用域的局部变量总数不能超过 MAX_LOCAL_VAR_NUM，即 128 个
    if (cu->localVarNum > MAX_LOCAL_VAR_NUM) {
        COMPILE_ERROR(cu->curLexer, "the max amount of local variable of one compile unit (such as function) is %d", MAX_LOCAL_VAR_NUM);
    }

    // 检测当前作用域是否存在同名变量
    // 之所以倒序遍历，是因为 scopeDepth 是随着作用域越深（越局部）而越大，而 localVars 数组中的局部变量是按照作用域的深度递增排列，
    // 即越深的作用域变量越排在 localVars 数组中的后面
    // 所以只需要从后向前遍历，最后的变量始终是当前作用域的变量
    int idx = (int)cu->localVarNum - 1;

    while (idx >= 0) {
        LocalVar *var = &(cu->localVars[idx]);

        // 当向前遍历时发现已经进入到父级作用域，则退出
        // 因为只需要检测同一个局部作用域下是否存在变量名冲突
        if (var->scopeDepth < cu->scopeDepth) {
            break;
        }

        // 如果在同一个局部作用域下之前已声明同名的局部变量，则报错
        if (var->length == length && memcmp(var->name, name, length) == 0) {
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
static int declareVariable(CompileUnit *cu, const char *name, uint32_t length) {
    // 如果为模块作用域，则声明为模块变量
    if (cu->scopeDepth == -1) {
        // 先将变量值初始化为 NULL
        int index = defineModuleVar(cu->curLexer->vm, cu->curLexer->curModule, name, length, VT_TO_VALUE(VT_NULL));

        // 如果同名模块变量为已经定义，则报错
        if (index == -1) {
            char id[MAX_ID_LEN] = {'\0'};
            memcpy(id, name, length);
            COMPILE_ERROR(cu->curLexer, "identifier \"%s\" redefinition!", id);
        }
        return index;
    }

    // 否则为局部作用域，声明为局部变量
    return declareLocalVar(cu, name, length);
}

// 查找局部变量
static int findLocalVar(CompileUnit *cu, const char *name, uint32_t length) {
    // 内层作用域的变量会覆盖外层
    // 而 localVars 数组中的局部变量是按照作用域的深度递增排列，即越深的作用域变量越排在 localVars 数组中的后面
    // 所以只需要从后向前遍历，即从内层向外找
    int index = cu->localVarNum - 1;
    while (index >= 0) {
        if (cu->localVars[index].length == length && memcmp(cu->localVars[index].name, name, length) == 0) {
            return index;
        }
        index--;
    }
    return -1;
}

// 丢掉作用域 scopeDepth 以内（包括子作用域）的局部变量
// 返回被丢掉的局部变量的个数
static uint32_t discardLocalVar(CompileUnit *cu, int scopeDepth) {
    // 如果 cu->scopeDepth == -1 ，即当前处在模块作用域中，则报错，因为模块作用域作为顶级作用域不能退出，确保类的静态属性得以保留
    ASSERT(cu->scopeDepth > -1, "upmost scope can't exit!");
    int idx = cu->localVarNum - 1;

    // 背景知识：
    // localVars 数组中的局部变量是按照作用域的深度递增排列，即越深的作用域变量越排在 localVars 数组中的后面
    // 作用域的 scopeDepth 范围大于 -1 的整数，作用域越深，值就越大，-1 表示模块作用域
    // 所以遍历作用域 scopeDepth 以内（包括子作用域）的局部变量，只需要从 localVars 数组后面开始遍历，
    // 并且保证变量所在作用域的 scopeDepth 小于传入的 scopeDepth 即可
    while (idx >= 0 && cu->localVars[idx].scopeDepth >= scopeDepth) {
        if (cu->localVars[idx].isUpvalue) {
            // 如果该局部变量被内层函数使用，即对内层函数来说是自由变量 upvalue
            // 则生成【运行时栈顶保存的是地址界限，关闭地址大于地址界限的 upvalue，然后将运行时栈顶弹出】的指令
            // TODO: 没太搞懂该指令在这里的作用，后续实现虚拟机时再回填
            writeByte(cu, OPCODE_CLOSE_UPVALUE);
        } else {
            // 否则该局部变量没有被被内层函数使用，只是普通局部变量
            // 则生成【将运行时栈顶弹出】的指令
            // TODO: 没太搞懂该指令在这里的作用，后续实现虚拟机时再回填
            writeByte(cu, OPCODE_POP);
        }
        // 此处之所以使用 writeByte 而不是 writeOpCode，目的是不想影响 cu->fn->maxStackSlotUsedNum
        // 注：writeOpCode 函数中，不仅向指令流中写入指令，还会计算该指令对运行时栈大小的影响，并实时更新 cu->fn->maxStackSlotUsedNum
        // 因为局部变量是存储在运行时栈中的，退出作用域后函数的运行时栈并未回收，也就是说变量仍然在运行时栈中，不可影响的实际的栈大小
        // TODO: 没太搞懂该指令在这里的作用，后续实现虚拟机时再回填
        idx--;
    }

    // 返回被丢掉的局部变量的个数
    return cu->localVarNum - 1 - idx;
}

// 添加自由变量 upvalue
// 添加 upvalue 到 cu->upvalues，并返回其索引值，若以存在则只返回索引即可
// isEnclosingLocalVar 表示 upvalue 是否是直接外层编译单元中的局部变量
// 如果是，则 index 表示的是此 upvalue 在直接外层编译单元的局部变量在该编译单元运行时栈的索引
// 如果不是，则 index 表示的是此 upvalue 在直接外层编译单元的 upvalue 的索引
// 也就是说内层函数可能引用的不是直接外层函数的局部变量，而是更外层函数的局部变量
// 注：upvalue 是针对引用外层函数局部变量的内层函数的
static int addUpvalue(CompileUnit *cu, bool isEnclosingLocalVar, uint32_t index) {
    uint32_t idx = 0;
    while (idx < cu->fn->upvalueNum) {
        // 如果该 upvalue 已经添加过，则直接返回其索引值
        if (cu->upvalues[idx].index == index && cu->upvalues[idx].isEnclosingLocalVar == isEnclosingLocalVar) {
            return idx;
        }
        idx--;
    }

    // 否则直接添加并返回索引
    cu->upvalues[cu->fn->upvalueNum].index = index;
    cu->upvalues[cu->fn->upvalueNum].isEnclosingLocalVar = isEnclosingLocalVar;
    return cu->fn->upvalueNum++;
}

// 查找自由变量
// 供内层函数向上查找被自己引用的与 name 变量同名的自由变量 upvalue，在哪个外层函数或模块中
static int findUpvalue(CompileUnit *cu, const char *name, uint32_t length) {
    // 已经到了模块对应的编译单元，即已经到了最外层了，直接外层编译单元为 NULL，仍找不到故返回 -1
    if (cu->enclosingUnit == NULL) {
        return -1;
    }

    // 前置知识：

    // 目前模块、类的方法、函数会有对应的编译单元，类本身没有
    // 在编译某个模块中的类的方法时，为了快捷地找到该方法所属的类的，
    // 会将该类的 ClassBookKeep 结构赋值给该模块对应的编译单元的 cu->enclosingClassBK
    // 所以 cu->enclosingUnit->enclosingClassBK != NULL （cu->enclosingUnit 为 cu 的直接外层编译单元）
    // 说明 cu 是类的方法的编译单元，cu->enclosingUnit 为该类所在模块的编译单元（类没有编译单元）

    // 另外类的静态属性，会被定义为类所在模块的局部变量，且为了区分模块中可能存在多个类有同名的属性，所以局部变量的格式为 'Class类名 静态属性名'
    // 所以 strchr(name, ' ') 为 NULL，说明查找的自由变量 upvalue 名没有空格，也就不是类的静态属性，也就不是模块的局部变量（模块中无法直接定义局部变量）
    // 所以没有必要再从模块的局部变量中查找了，直接返回 -1 即可
    if (cu->enclosingUnit->enclosingClassBK != NULL && !strchr(name, ' ')) {
        return -1;
    }

    // 从直接外层编译单元中查找自由变量 upvalue
    int directOuterLocalIndex = findLocalVar(cu->enclosingUnit, name, length);
    // 如果找到，则将 localVars 数组中对应变量的 isUpvalue 属性设置成 true，并将该变量添加到 cu->upvalues 数组中
    if (directOuterLocalIndex != -1) {
        cu->localVars[directOuterLocalIndex].isUpvalue = true;
        // 由于是从直接外层编译单元的局部变量中找到，所以 isEnclosingLocalVar 为 true
        // 且 directOuterLocalIndex 为在直接外层编译单元的局部变量的索引
        return addUpvalue(cu, true, (uint32_t)directOuterLocalIndex);
    }

    // 否则递归调用 findUpvalue 函数继续向上层查找自由变量 upvalue
    // 递归过程会将 upvalue 添加到编译单元链上的所有中间层编译单元的 upvalue 数组 upvalues 中
    int directOuterUpvalueIndex = findUpvalue(cu->enclosingUnit, name, length);
    // 如果找到，则将变量添加到 cu->upvalues 数组中
    if (directOuterUpvalueIndex != -1) {
        // 由于是从非直接外层编译单元的局部变量中找到，所以 isEnclosingLocalVar 为 false
        // 且 directOuterUpvalueIndex 为直接外层编译单元的 upvalue 的索引
        return addUpvalue(cu, false, (uint32_t)directOuterUpvalueIndex);
    }

    // 没有找到名为 name 的 upvalue，则返回 -1
    return -1;
}

// 供内层函数在局部变量和自由变量 upvalue 中查找符号 name
// 先从内层函数中的局部变量查找，找到则更正变量类型为局部变量类型，并返回
// 否则即为自由变量 upvalue，需要调用 findUpvalue 函数向上层编译单元中查找，如果找到则更正变量类型为 upvalue 变量类型，并返回
static Variable getVarFromLocalOrUpvalue(CompileUnit *cu, const char *name, uint32_t length) {
    Variable var;
    // 变量类型默认为无效作用域类型，查找到后会被更正
    var.scopeType = VAR_SCOPE_INVALID;

    var.index = findLocalVar(cu, name, length);

    if (var.index != -1) {
        var.scopeType = VAR_SCOPE_LOCAL;
        return var;
    }

    var.index = findUpvalue(cu, name, length);
    if (var.index != -1) {
        var.scopeType = VAR_SCOPE_UPVALUE;
    }
    return var;
}

// 赋值变量（主要是为模块变量赋值）
static void defineVariable(CompileUnit *cu, uint32_t index) {
    //局部变量的值只需要压入到运行时栈中即可，故无需处理
    //模块变量并不存储在运行时栈中,因此需要将其写入相应位置
    if (cu->scopeDepth == -1) {
        // 生成【将运行时栈顶数据保存到索引为 index 的模块变量中】
        writeOpCodeShortOperand(cu, OPCODE_STORE_MODULE_VAR, index);
        // 上一指令已经将栈顶数据保存，所以此时只需要弹出栈顶数据即可
        writeOpCode(cu, OPCODE_POP);
    }
}

// 依次从局部变量、upvalue、模块变量中查找变量
static Variable findVariable(CompileUnit *cu, const char *name, uint32_t length) {
    // 先从局部变量、upvalue 中查找变量
    Variable var = getVarFromLocalOrUpvalue(cu, name, length);
    if (var.index != -1) {
        return var;
    }

    // 再从模块变量中查找变量
    var.index = getIndexFromSymbolTable(&cu->curLexer->curModule->moduleVarName, name, length);
    if (var.index != -1) {
        var.scopeType = VAR_SCOPE_MODULE;
    }
    return var;
}

// 生成【将运行时栈中索引为 index 的 slot，即变量的值压入栈顶】的指令
static void emitLoadVariable(CompileUnit *cu, Variable var) {
    switch (var.scopeType) {
        case VAR_SCOPE_LOCAL:
            // 生成【将局部变量的值压入栈顶】的指令
            writeOpCodeByteOperand(cu, OPCODE_LOAD_LOCAL_VAR, var.index);
            break;
        case VAR_SCOPE_UPVALUE:
            // 生成【将自由变量的值压入栈顶】的指令
            writeOpCodeByteOperand(cu, OPCODE_LOAD_UPVALUE, var.index);
            break;
        case VAR_SCOPE_MODULE:
            // 生成【将模块变量的值压入栈顶】的指令
            writeOpCodeByteOperand(cu, OPCODE_LOAD_MODULE_VAR, var.index);
            break;
        default:
            NOT_REACHED();
    }
}

// 生成【将栈顶数据存入索引为 index 的变量】的指令
static void emitStoreVariable(CompileUnit *cu, Variable var) {
    switch (var.scopeType) {
        case VAR_SCOPE_LOCAL:
            // 生成【将栈顶数据存入局部变量】的指令
            writeOpCodeByteOperand(cu, OPCODE_STORE_LOCAL_VAR, var.index);
            break;
        case VAR_SCOPE_UPVALUE:
            // 生成【将栈顶数据存入自由变量】的指令
            writeOpCodeByteOperand(cu, OPCODE_STORE_UPVALUE, var.index);
            break;
        case VAR_SCOPE_MODULE:
            // 生成【将栈顶数据存入模块变量】的指令
            writeOpCodeByteOperand(cu, OPCODE_STORE_MODULE_VAR, var.index);
            break;
        default:
            NOT_REACHED();
    }
}

// 生成【压入变量值到栈顶】或者【保存栈顶数据到变量】的指令
static void emitLoadOrStoreVariable(CompileUnit *cu, Variable var, bool canAssign) {
    // canAssign 为 true 表示具备可赋值的环境，且当前 token 为等号 =，则可判断等号右边就是需要赋值的表达式
    // 先调用 expression 方法生成计算等号右边的表达式值的指令（这些指令执行完后，运行时栈顶的值就是计算结果）
    // 然后将栈顶的值，即表达式计算结果，保存到变量中，即调用 emitStoreVariable 方法
    if (canAssign && matchToken(cu->curLexer, TOKEN_ASSIGN)) {
        expression(cu, BP_LOWEST);
        emitStoreVariable(cu, var);
    } else {
        // 否则生成【加载变量值到栈顶】的指令
        emitLoadVariable(cu, var);
    }
}

// 生成【将实例对象 this 压入到运行时栈顶】的指令
static void emitLoadThis(CompileUnit *cu) {
    Variable var = getVarFromLocalOrUpvalue(cu, "this", 4);
    // 如果找不到，即 var 的 scopeType 属性为 VAR_SCOPE_INVALID，则报错
    ASSERT(var.scopeType != VAR_SCOPE_INVALID, "get this variable failed!");
    // 否则生成指令
    emitLoadVariable(cu, var);
}

// 编译代码块
// 代码块是包括在 {} 中间的代码，例如函数体、方法体、方法的块参数
// 一个代码块就被视为一个独立的指令流，也就是编译单元
// 遇到 { 就调用该函数，函数中调用 compileProgram 编译，直到遇到 }
static void compileBlock(CompileUnit *cu) {
    // 调用该函数时，已经读入了 {
    while (!matchToken(cu->curLexer, TOKEN_RIGHT_BRACE)) {
        // 如果在 } 之前遇到了 标识文件结束的 token 类型 TOKEN_EOF，则报错
        if (cu->curLexer->curToken.type == TOKEN_EOF) {
            COMPILE_ERROR(cu->curLexer, "expect '}' at the end of block!");
        }
        compileProgram(cu);
    }
}

// 编译函数体/方法体
static void compileBody(CompileUnit *cu, bool isConstruct) {
    // 进入函数前已经读入了 {
    compileBlock(cu);

    // 当函数执行完后，会生成【将栈顶值（即函数运行的结果）返回，并将该函数对应运行时栈的部分销毁】的指令
    // 所以函数执行完成后，需要将函数运行结果压入到栈顶
    if (isConstruct) {
        // 如果是构造函数，则默认返回的函数运行结果是实例对象，即 this 的指向，
        // 而对于构造函数，默认设定 this 为第 0 个局部变量，所以只需要将该函数的第一个局部变量的值压入栈中即可
        // 注意这里的第 0 个局部变量不是指 cu->localVars 数组中的值，该数组只是保存局部变量的名字，例如第 0 个元素为 'this'
        // 函数的局部变量的值是存储在运行时栈中的，所以需要从运行时栈中获取第 0 个局部变量 this 的值，然后将其压入栈顶
        writeOpCodeByteOperand(cu, OPCODE_LOAD_LOCAL_VAR, 0);
    } else {
        // 如果是普通函数，则默认返回的函数运行结果是 NULL
        // 所以将 NULL 压入栈顶即可
        writeOpCode(cu, OPCODE_PUSH_NULL);
    }

    // 当函数执行完后，会生成【将栈顶值（即函数运行的结果）返回，并将该函数对应运行时栈的部分销毁】的指令
    writeOpCode(cu, OPCODE_RETURN);
}

// 结束编译单元的编译工作，在直接外层编译单元中为其创建闭包
// 编译单元本质就是指令流单元
#if DEBUG
static ObjFn *endCompileUnit(CompileUnit *cu, const char *debugName, uint32_t debugNameLen) {
    // 如果处于调试阶段，会额外调用 bindDebugFnName 将函数名 debugName 写入到 cu->fn->debug 中
    bindDebugFnName(cu->curLexer->vm, cu->fn->debug, debugName, debugNameLen);
#else
static ObjFn *endCompileUnit(CompileUnit *cu) {
#endif

    // 生成【标识编译单元编译结束】的指令
    writeOpCode(cu, OPCODE_END);

    if (cu->enclosingUnit != NULL) {
        // 将当前编译单元的 cu->fn (其中就包括了该编译单元的指令流 cu->fn->instrStream)
        // 添加到直接外层编译单元即父编译单元的常量表中
        uint32_t index = addConstant(cu, OBJ_TO_VALUE(cu->fn));

        // 在直接外层编译单元的 fn->instrStream 指令流中，添加 为当前内层函数创建闭包 的指令，其中 index 就是上面添加到常量表得到的索引值
        // 也就是说，内层函数是以闭包形式在外层函数中存在
        writeOpCodeShortOperand(cu->enclosingUnit, OPCODE_CREATE_CLOSURE, index);

        // 上面向直接外层编译单元的指令流 cu->enclosingUnit->fn->instrStream 中写入为当前内层函数创建闭包的指令 OPCODE_CREATE_CLOSURE，
        // 当虚拟机执行该命令时，已经在直接外层编译单元了，是没办法直到内层函数 upvalue 数组 cu->upvalues 中哪些是直接外层编译单元的局部变量，哪些是直接外层编译单元的 upvalue
        // 只有知道，才能在运行时栈找到对应的值，所以需要将 是直接外层编译单元的局部变量还是 upvalue 信息记录下来
        // 这里就直接写入了直接外层编译单元的指令流 cu->enclosingUnit->fn->instrStream 中
        // 按照 {upvalue 是否是直接编译外层单元的局部变量，upvalue 在直接外层编译单元的索引} 成对信息写入的
        // 以供虚拟机后面读入
        index = 0;
        while (index < cu->fn->upvalueNum) {
            writeByte(cu->enclosingUnit, cu->upvalues[index].isEnclosingLocalVar ? 1 : 0);
            writeByte(cu->enclosingUnit, cu->upvalues[index].index);
            index++;
        }
    }

    // 将当前编译单元设置成直接外层编译单元
    cu->curLexer->curCompileUnit = cu->enclosingUnit;
    return cu->fn;
}

// 处理函数/方法实参
// 基于实参列表生成【加载实参到运行时栈顶】的指令
// 虚拟机执行指令后，会从左到右依次将实参压入到运行时栈，被调用的函数/方法会从栈中获取参数
// 注：expression 就是用来计算表达式，即会生成计算表达式的一系列指令，虚拟机在执行这些指令后，就会计算出表达式的结果
static void processArgList(CompileUnit *cu, Signature *sign) {
    // 确保实参列表不为空
    ASSERT(cu->curLexer->curToken.type != TOKEN_RIGHT_PAREN &&
               cu->curLexer->curToken.type != TOKEN_RIGHT_BRACKET,
           "empty argument list!");

    do {
        // 确保实参个数不超过 MAX_ARG_NUM
        if (++sign->argNum > MAX_ARG_NUM) {
            COMPILE_ERROR(cu->curLexer, "the max number of argument is %d", MAX_ARG_NUM);
        }

        // 基于实参列表生成加载实参到运行时栈中的指令
        expression(cu, BP_LOWEST);
    } while (matchToken(cu->curLexer, TOKEN_COMMA));
}

// 处理函数/方法形参
// 将形参列表中的形参声明为函数/方法中的局部变量
static void processParaList(CompileUnit *cu, Signature *sign) {
    // 确保形参列表不为空
    ASSERT(cu->curLexer->curToken.type != TOKEN_RIGHT_PAREN &&
               cu->curLexer->curToken.type != TOKEN_RIGHT_BRACKET,
           "empty argument list!");

    do {
        // 确保形参个数不超过 MAX_ARG_NUM
        if (++sign->argNum > MAX_ARG_NUM) {
            COMPILE_ERROR(cu->curLexer, "the max number of argument is %d", MAX_ARG_NUM);
        }

        // 确保形参对应的 token 类型为变量名
        assertCurToken(cu->curLexer, TOKEN_ID, "expect variable name!");

        // 将形参列表中的形参声明为函数/方法中的局部变量
        declareVariable(cu, cu->curLexer->preToken.start, cu->curLexer->preToken.length);
    } while (matchToken(cu->curLexer, TOKEN_COMMA));
}

// 基于方法签名生成【调用方法】的指令
// 包括 callX 和 superX，即普通方法和基类方法
static void emitCallBySignature(CompileUnit *cu, Signature *sign, OpCode opcode) {
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

    // 背景知识：
    // 操作码 OPCODE_SUPERx 用于调用基类的方法的
    // 其操作数有 4 个字节，其中前两个字节存储 基类方法在基类中的索引 methodIndex，即 super.method[methodIndex] 表示基类的方法
    // 后两个字节存储 基类在常量中的索引 superClassIndex，即 constants[superClassIndex] 表示基类

    // 如果是调用基类方法，操作数除了表示 方法索引的两个字节外，还需要再写两个字节的操作数表示 基类在常量中的索引
    // 因为基类可能是在子类之后定义，不能保证基类此时已经被编译完成，
    // 所以先向常量表中添加 VT_NULL，并获得 VT_NULL 在常量表中的的索引，将该索引作为后两个操作数
    // 等到执行修正操作数的函数 patchOperand 时，再将常量表中的 VT_NULL 换成正确的基类 superClass，操作数无需修改
    if (opcode == OPCODE_SUPER0) {
        writeShortOperand(cu, addConstant(cu, VT_TO_VALUE(VT_NULL)));
    }
}

// 生成【调用 getter 方法或普通方法】的指令
// 形参 sign 是待调用方法的签名，在调用本函数之前，已经处理了方法名，
// 本函数主要是处理方法名后面的部分
// 因此此时只知道方法名，并不知道方法类型等信息，所以形参 sign 是不完整的，需要在本函数进一步完善
static void emitGetterMethodCall(CompileUnit *cu, Signature *sign, OpCode opCode) {
    Signature newSign;
    // 默认方法类型为 getter 类型
    newSign.type = SIGN_GETTER;
    newSign.name = sign->name;
    newSign.length = sign->length;
    newSign.argNum = 0;

    // 如果是普通方法，可能会有参数列表，在调用该方法之前，必须先将参数压入运行时栈，否则运行方法时，
    // 会获取到错误的参数（即栈中已有数据），还会在方法运行结束时，错误的回收对应的空间，导致栈失衡
    // 因此如果后面有参数，则需要先生成【将实参压入到运行时栈】的指令

    // 如果后面有 (，可能有参数列表
    if (matchToken(cu->curLexer, TOKEN_LEFT_PAREN)) {
        // 设置成普通函数类型
        newSign.type = SIGN_METHOD;

        // 如果后面不是 )，说明有参数列表
        if (!matchToken(cu->curLexer, TOKEN_RIGHT_PAREN)) {
            processArgList(cu, &newSign);
            // 参数之后需为 )，否则报错
            assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect ')' after argument list!");
        }
    }

    // 如果后面有 {，可能有块参数，块参数就是传入的参数是一个代码块，也就是函数
    // 注意此处是函数调用，而不是函数定义
    // 块参数即函数的参数，是夹在 |形参列表| 之间的形参列表，可参考下面 ruby 的写法：
    // class A {
    //     new () {

    //     }

    //     bar(fn) {
    //         fn.call(123)
    //     }
    // }
    // var a = A.new()
    // a.bar {|n| System.print(n)}
    // 其中 n 就是块参数（即传入的函数）的参数
    if (matchToken(cu->curLexer, TOKEN_LEFT_BRACKET)) {
        // 参数加 1
        newSign.argNum++;
        // 设置成普通函数类型
        newSign.type = SIGN_METHOD;

        // 开始编译传入的函数
        CompileUnit fnCU;
        initCompileUnit(cu->curLexer, &fnCU, cu, false);
        // 临时的函数签名，用于编译传入的函数
        Signature tempFnSign = {SIGN_METHOD, "", 0, 0};
        // 如果下一个字符为 |，说明该传入的函数也有参数
        if (matchToken(cu->curLexer, TOKEN_BIT_OR)) {
            processParaList(&fnCU, &tempFnSign);
            // 参数之后需为 ｜，否则报错
            assertCurToken(cu->curLexer, TOKEN_BIT_OR, "expect '｜' after argument list!");
        }
        fnCU.fn->argNum = tempFnSign.argNum;
        // 开始编译传入的函数的函数体，将该指令写进该函数自己的指令流中
        compileBody(&fnCU, false);
#if DEBUG
        // 以接受块参数（即传入函数）的方法来命名传入函数，传入函数名=方法名+" block arg"
        // 其中加 10，是因为 " block arg" 有 10 个字符
        char fnName[MAX_SIGN_LEN + 10] = {'\0'};
        // 将接受块参数的方法的签名转化成字符串，并写入到 fnName 中，最后返回字符串的长度
        uint32_t len = sign2String(&newSign, fnName);
        // void *memmove(void *str1, const void *str2, size_t n) 从 str2 复制 n 个字符到 str1
        // 在上一行生成的字符串结尾追加字符串 " block arg"
        memmove(fnName + len, " block arg", 10);
        endCompileUnit(&fnCU, fnName, len + 10);
#else
        // 结束编译传入的函数
        endCompileUnit(&fnCU);
#endif
    }

    // 如果是构造函数类型的方法，
    if (sign->type == SIGN_CONSTRUCT) {
        // 如果此处 newSign.type 不是 SIGN_METHOD，仍旧是 SIGN_GETTER
        // 说明没有满足上面的两个条件判断，也就是方法名后面没有 ( 或 {
        // 也就是直接按照 getter 类型方法的形式调用，即直接就是方法名
        // 但是有根据 sign->type 判断该方法为构造函数，调用形式应类似 super() 或者 super(arguments)
        // 所以调用形式有问题，直接报编译错误即可
        if (newSign.type != SIGN_METHOD) {
            COMPILE_ERROR(cu->curLexer, "the form of supercall is super() or super(arguments)");
        }
        newSign.type = SIGN_CONSTRUCT;
    }

    // 根据函数签名生成【调用函数】指令
    emitCallBySignature(cu, &newSign, opCode);
}

// 生成【调用方法】的指令，包含所有方法
static void emitMethodCall(CompileUnit *cu, const char *name, uint32_t length, OpCode opCode, bool canAssign) {
    Signature sign;
    sign.name = name;
    sign.length = length;
    sign.type = SIGN_GETTER;

    // 如果后面是 = 且是可赋值环境，则判定该方法为 setter 类型方法
    if (matchToken(cu->curLexer, TOKEN_ASSIGN) && canAssign) {
        sign.type = SIGN_SETTER;
        sign.argNum = 1;

        // 加载实参（即等号 = 后面的表达式的计算结果），为下面方法调用传参
        expression(cu, BP_LOWEST);

        emitCallBySignature(cu, &sign, opCode);
    } else {
        emitGetterMethodCall(cu, &sign, opCode);
    }
}

// 调用下面的生成方法签名的函数之时，preToken 为方法名，curToken 为方法名右边的符号
// 例如 test(a)，preToken 为 test，curToken 为 (
// 在调用方 compileMethod 中，方法名已经获取了，同时方法签名已经创建了，只需要下面的函数获取符号方法的类型、方法参数个数，来完善方法签名

// 判断是否为 setter 方法，如果是则将方法签名设置成 setter 类型，且返回 true，否则返回 false
// setter 方法判断依据是是否符合 xxx=(_)
static bool trySetter(CompileUnit *cu UNUSED, Signature *sign) {
    // 方法名后面没有 = ，则可判断不是 setter 方法
    if (!matchToken(cu->curLexer, TOKEN_ASSIGN)) {
        return false;
    }

    if (sign->type == SIGN_SUBSCRIPT) {
        // subscribe setter 方法形式：xxx[_,...] = (_)
        sign->type = SIGN_SUBSCRIPT_SETTER;
    } else {
        // setter 方法形式：xxx = (_)
        sign->type = SIGN_SETTER;
    }

    // = 后面必须为 (
    assertCurToken(cu->curLexer, TOKEN_LEFT_PAREN, "expect '(' after '='!");

    // ( 后面必须为标识符
    assertCurToken(cu->curLexer, TOKEN_ID, "expect identifier");

    // 声明形参为局部变量
    // subscribe setter 或者 setter 方法只有一个参数
    declareVariable(cu, cu->curLexer->preToken.start, cu->curLexer->preToken.length);

    // 标识符后面必须为 )
    assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect ')' after argument list!");

    // 方法签名中的 argNum 加 1
    sign->argNum++;

    return true;
}

// 为标识符生成方法签名
static void idMethodSignature(CompileUnit *cu UNUSED, Signature *sign) {
    // 先默认设置成 getter 类型方法
    sign->type = SIGN_GETTER;

    // 如果方法签名的名字为 new 则说明是构造函数，即形式为 new(_,...)
    if (sign->length == 3 && memcmp(sign->name, "new", 3) == 0) {
        // 构造函数的方法名后面不能接 =
        if (matchToken(cu->curLexer, TOKEN_ASSIGN)) {
            COMPILE_ERROR(cu->curLexer, "constructor shoudn't be setter!");
        }

        // 构造函数的方法名 new 后面必须为 (
        if (!matchToken(cu->curLexer, TOKEN_LEFT_PAREN)) {
            COMPILE_ERROR(cu->curLexer, "constructor must be method!");
        }

        sign->type = SIGN_CONSTRUCT;

        // 如果形参后面为 )，说明构造函数没有参数则直接返回，不用走后面声明形参的逻辑
        // 注意此时的 preToken 已经是 ( 了，因为上面有判断 new 后面是否有 ( 的逻辑，满足条件才会走到这里， matchToken 满足条件会读入下一个 token
        if (matchToken(cu->curLexer, TOKEN_RIGHT_PAREN)) {
            return;
        }
    } else {
        // 若不是构造函数，则判断是否为 setter 或者 subscribe setter
        // 如果是，trySetter 中已经将 type 设置了，并且处理了，直接返回即可
        if (trySetter(cu, sign)) {
            return;
        }

        // 如果方法名后面不是 (，则说明是 getter 类型方法，开头已经默认设置，且 getter 方法没有参数，所以直接返回即可
        if (!matchToken(cu->curLexer, TOKEN_LEFT_BRACKET)) {
            return;
        }

        // 经过上面的判断，最后就可以判断该方法应该是普通方法了，即形式为 xxx(_,...)
        sign->type = SIGN_METHOD;

        // ( 后面为 )，说明没有参数，直接返回即可，不用走后面声明形参的逻辑
        if (matchToken(cu->curLexer, TOKEN_RIGHT_PAREN)) {
            return;
        }
    }

    // 声明形参为局部变量
    processParaList(cu, sign);

    // 形参后面必须为 )
    assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect ')' after parameter list!");
}

// 为单运算符的符号方法生成方法签名
static void unaryMethodSignature(CompileUnit *cu UNUSED, Signature *sign) {
    // 单运算符的符号方法类型 getter 方法，且没有方法参数
    sign->type = SIGN_GETTER;
}

// 为中缀运算符的符号方法生成方法签名
static void infixMethodSignature(CompileUnit *cu UNUSED, Signature *sign UNUSED) {
    // 中缀运算符的符号方法类型普通方法
    sign->type = SIGN_METHOD;

    // 方法参数个数为 1 个
    // 例如 2 + 3 相当于 2.+(3) ，其中 + 方法参数为 3，参数个数为 1
    sign->argNum = 1;

    // 方法名后面必须为 (
    assertCurToken(cu->curLexer, TOKEN_LEFT_PAREN, "expect '(' after infix operator!");

    // ( 后面必须为标识符
    assertCurToken(cu->curLexer, TOKEN_ID, "expect variable name!");

    // 声明中缀运算符的符号方法参数为变量
    // 此处之所以是 preToken，是因为上个 assertCurToken 方法已经读入了参数
    declareVariable(cu, cu->curLexer->preToken.start, cu->curLexer->preToken.length);

    // 参数后面必须为 )
    assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect ')' after paramter!");
}

// 为既可做单运算符也可做中缀运算符的符号方法生成方法签名
static void mixMethodSignature(CompileUnit *cu UNUSED, Signature *sign UNUSED) {
    // 先默认为单运算符，即方法类型为 getter 方法，方法形式为 xxx
    sign->type = SIGN_GETTER;

    // 方法名后面为 ( 则说明是普通方法，方法形式为 xxx(_,...)，进而可以判断为中缀运算符
    if (matchToken(cu->curLexer, TOKEN_LEFT_PAREN)) {
        // 中缀运算符的符号方法类型普通方法
        sign->type = SIGN_METHOD;

        // 方法参数个数为 1 个
        // 例如 2 + 3 相当于 2.+(3) ，其中 + 方法参数为 3，参数个数为 1
        sign->argNum = 1;

        // ( 后面必须为标识符
        assertCurToken(cu->curLexer, TOKEN_ID, "expect variable name!");

        // 声明中缀运算符的符号方法参数为变量
        // 此处之所以是 preToken，是因为上个 assertCurToken 方法已经读入了参数
        declareVariable(cu, cu->curLexer->preToken.start, cu->curLexer->preToken.length);

        // 参数后面必须为 )
        assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect ')' after paramter!");
    }
}

// 为下标操作符 [ 生成方法签名
static void subscriptMethodSignature(CompileUnit *cu, Signature *sign) {
    sign->type = SIGN_SUBSCRIPT;
    sign->length = 0;
    // 处理中括号之间的形参
    processParaList(cu, sign);
    // 参数后面必须为 ]
    assertCurToken(cu->curLexer, TOKEN_RIGHT_BRACKET, "expect ']' after index list!");
    // 判断是否有 =，如果有则 sign->type 设置为 SIGN_SUBSCRIPT_SETTER
    trySetter(cu, sign);
}

// 基于方法签名 生成 调用方法的指令
// 仅限于 callX，即普通方法
// 方法名 name  方法名长度 length  方法参数个数 argNum
static void emitCall(CompileUnit *cu, const char *name, int length, int argNum) {
    // 确保名为 name 的方法已经在 cu->curLexer->vm->allMethodNames 中，没有查找到，则向其中添加
    int symbolIndex = ensureSymbolExist(cu->curLexer->vm, &cu->curLexer->vm->allMethodNames, name, length);
    // 写入调用方法的指令，其中：
    // 操作码为 callX，X 表示调用方法的参数个数，例如 OPCODE_CALL15
    // 操作数为方法在 cu->curLexer->vm->allMethodNames 的索引值
    writeOpCodeShortOperand(cu, OPCODE_CALL0 + argNum, symbolIndex);
}

// 在模块 objModule 中定义名为 name，值为 value 的模块变量
int defineModuleVar(VM *vm, ObjModule *objModule, const char *name, uint32_t length, Value value) {
    // 如果模块变量名长度大于 MAX_ID_LEN 则报错
    if (length > MAX_ID_LEN) {
        // name 指向的变量名不一定以 \0 结尾，为保险起见，将其从源码串拷贝出来
        char id[MAX_ID_LEN] = {'\0'};
        memcpy(id, name, length);

        // defineModuleVar 函数调用场景有多种，可能还未创建词法分析器
        if (vm->curLexer != NULL) {
            COMPILE_ERROR(vm->curLexer, "length of identifier\"%s\"should no more than %d", id, MAX_ID_LEN);
        } else {
            MEM_ERROR("length of identifier\"%s\"should no more than %d", id, MAX_ID_LEN);
        }
    }

    // 查找模块变量名 name 在 objModule->moduleVarName 中的索引
    // 如果为 -1，说明不存在，则分别在 objModule->moduleVarName 和 objModule->moduleVarValue 中添加模块变量的名和值
    int symbolIndex = getIndexFromSymbolTable(&objModule->moduleVarName, name, length);

    if (symbolIndex == -1) {
        symbolIndex = addSymbol(vm, &objModule->moduleVarName, name, length);
        ValueBufferAdd(vm, &objModule->moduleVarValue, value);
    } else if (VALUE_IS_NUM(objModule->moduleVarValue.datas[symbolIndex])) {
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
        // 目前只是处在编译阶段，故可以通过判断 objModule->moduleVarValue 的值的类型来判断是否是先使用后声明的情况
        objModule->moduleVarValue.datas[symbolIndex] = value;
    } else {
        // 已定义，则返回 -1（用于判断是否重复定义）
        symbolIndex = -1;
    }

    return symbolIndex;
}

// 声明模块变量
// 区别于 defineModuleVar 函数，该函数不做重定义检查，默认直接声明
static int declareModuleVar(VM *vm, ObjModule *objModule, const char *name, uint32_t length, Value value) {
    ValueBufferAdd(vm, &objModule->moduleVarValue, value);
    return addSymbol(vm, &objModule->moduleVarName, name, length);
}

// 获取包含 cu->enclosingClassBK 的最近的编译单元 CompileUnit，注：
// 目前模块、类的方法、函数会有对应的编译单元，类本身没有
// 在编译某个模块中的类的方法时，为了快捷地找到该方法所属的类的，
// 会将该类的 ClassBookKeep 结构赋值给该模块对应的编译单元的 cu->enclosingClassBK
// 这样类的方法要找到所属的类，只需要在对应的的编译单元的父编译单元（或父编译单元的父编译单元）即模块的编译单元中，找到其中的 cu->enclosingClassBK 即可
static CompileUnit *getEnclosingClassBKUnit(CompileUnit *cu) {
    while (cu != NULL) {
        if (cu->enclosingClassBK != NULL) {
            return cu;
        }
        // 向上找父编译单元，即直接外层编译单元
        cu = cu->enclosingUnit;
    }
    return NULL;
}

// 获取包含 cu->enclosingClassBK 的最近的编译单元 CompileUnit 中的 cu->enclosingClassBK
static ClassBookKeep *getEnclosingClassBK(CompileUnit *cu) {
    CompileUnit *ncu = getEnclosingClassBKUnit(cu);

    if (ncu != NULL) {
        return ncu->enclosingClassBK;
    }
    return NULL;
}

// 判断是否是局部变量
// 如果是具有全局变量性质的变量名建议使用大写字母开头，例如：类名、模块变量名
// 如果是具有局部变量性质的变量名建议使用小写字母开头，例如：方法名、局部变量名
// 当不遵守规范，全局变量也使用了小写字母开头，恰好和类中的 getter 方法名同名，则优先选择 getter 方法，示例如下：
// class Foo {
//     var name
//     new(n) {
//         name = n
//     }
//     myName {
//         return name
//     }
//     hi() {
//         System.print("hello, I am " + myName + "!")
//     }
// }

// var f = Foo.new("da hong");
// var myName = "xiao sa"
// f.hi()
// 其中，hi 方法会优先选择 getter 方法 myName 而非全局变量 myName
// 注意：在同一个类中调用类中的其他方法，不需要显示地指明对象，即 “对象.方法名”，例如 this.myName  this.hi()
// 而是直接写方法名即可，编译器会从当前对象所属类的方法中查找
static bool isLocalName(const char *name) {
    return (name[0] >= 'a' && name[0] <= 'z');
}

// 编译标识符的引用，即标识符的 nud 方法，
// 调用该函数时preToken 为该标识符，curToken 为标识符右边的符号
// 标识符可以是函数名、变量名、类静态属性、对象实例属性等
// 当同名时优先级：函数调用 > 局部变量和 upvalue > 对象实例属性 > 类静态属性 > 类的 getter 方法调用 > 模块变量
static void id(CompileUnit *cu, bool canAssign) {
    // 备份变量名
    Token name = cu->curLexer->preToken;
    ClassBookKeep *classBK = getEnclosingClassBK(cu);

    // 标识符可以是任意字符，按照此顺序处理：
    // 函数调用 > 局部变量和 upvalue > 对象实例属性 > 类静态属性 > 类的 getter 方法调用 > 模块变量

    // 1. 按照【函数调用】处理
    // cu->enclosingUnit == NULL 说明此时处于模块的编译单元，即正在编译模块（因为模块已经是最顶级的编译单元了，已经没有直接外层编译单元了）
    // 且下一个字符是 (，故可以判断是函数调用
    if (cu->enclosingUnit == NULL && matchToken(cu->curLexer, TOKEN_LEFT_PAREN)) {
        // 在编译函数定义时，是按照 “Fn 函数名” 的形式作为模块变量存储起来，目的是和用户定义的局部变量做区分
        // 所以在查找函数名之前，需要在前面添加 “Fn ” 前缀
        char id[MAX_ID_LEN] = {'\0'};
        memmove(id, "Fn ", 3);
        memmove(id + 3, name.start, name.length);

        Variable var;
        var.scopeType = VAR_SCOPE_MODULE;
        // 从当前模块的模块变量名字表 moduleVarName 中查找
        var.index = getIndexFromSymbolTable(&cu->curLexer->curModule->moduleVarName, id, strlen(id));
        // 如果没有找到对应的 “Fn 函数名”，则报编译错误，提示该函数没有定义
        if (var.index == -1) {
            memmove(id, name.start, name.length);
            id[name.length] = '\0';
            COMPILE_ERROR(cu->curLexer, "Undefined function: '%s'", id);
        }

        // 如果找到的模块变量，即函数闭包，则生成【将函数闭包压入到运行时栈顶】的指令
        emitLoadVariable(cu, var);

        // 创建函数签名，方便后面生成调用函数的指令
        Signature sign;
        // 函数调用是以 “函数闭包.call” 的形式，也就是说 call 是待调用的方法名
        sign.name = "call";
        sign.length = 4;
        sign.argNum = 0;
        // 函数调用的形式和 method 类型方法类似
        sign.type = SIGN_METHOD;

        // 如果 ( 后面不是 )，则说明有参数列表，调用 processArgList 将参数压入运行时栈
        if (!matchToken(cu->curLexer, TOKEN_RIGHT_PAREN)) {
            processArgList(cu, &sign);
            assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect ')' after argument list!");
        }

        // 生成【调用函数】的指令
        emitCallBySignature(cu, &sign, OPCODE_CALL0);
    } else {
        // 2. 按照【局部变量和 upvalue】处理
        Variable var = getVarFromLocalOrUpvalue(cu, name.start, name.length);
        if (var.index != -1) {
            // 如果找到，则生成【压入变量值到栈顶】或者【保存栈顶数据到变量】的方法
            emitLoadOrStoreVariable(cu, var, canAssign);
            return;
        }

        // 3. 按照【对象实例的属性】处理
        // 如果正在编译一个类的时候，会将 cu->enclosingClassBK 设为所编译的类的 classBookKeep 结构，
        // 所以如果 classBK != NULL，说明此时在编译类
        // 对象实例的属性在类中的定义形式是 “var 对象实例属性”
        if (classBK != NULL) {
            // 从类的符号属性表中查找
            int fieldIndex = getIndexFromSymbolTable(&classBK->fields, name.start, name.length);
            // 如果找到
            if (fieldIndex != -1) {
                // 编辑是使用对象实例属性，还是给对象实例属性赋值
                bool isRead = true;
                // 如果是可赋值环境，且对象实例的属性后面的字符是等号，则说明是给对象实例属性进行赋值
                // 所以调用 expression 解析等号后面的表达式，生成【计算表达式】的指令，虚拟机执行指令后，该表达式的计算结果就保存在了运行时栈顶，方便下面使用
                if (canAssign && matchToken(cu->curLexer, TOKEN_ASSIGN)) {
                    isRead = false;
                    expression(cu, BP_LOWEST);
                }

                // 如果当前则会跟你在编译类的方法，则按照在类的方法引用当前对象的属性处理
                if (cu->enclosingUnit != NULL) {
                    // 如果是使用属性，则生成【压入变量值到栈顶】指令；如果是赋值给属性，则生成【保存栈顶数据到变量】指令
                    writeOpCodeByteOperand(cu, isRead ? OPCODE_LOAD_THIS_FIELD : OPCODE_STORE_THIS_FIELD, fieldIndex);
                } else {
                    // 否则按照在类外引用对象实例属性处理
                    // 先生成【将实例对象 this 压入到运行时栈顶】的指令
                    emitLoadThis(cu);
                    // 如果是使用属性，则生成【压入变量值到栈顶】指令；如果是赋值给属性，则生成【保存栈顶数据到变量】指令
                    writeOpCodeByteOperand(cu, isRead ? OPCODE_LOAD_FIELD : OPCODE_STORE_FIELD, fieldIndex);
                }
                return;
            }
        }

        // 4. 按照【类的静态属性】处理
        // 如果正在编译一个类的时候，会将 cu->enclosingClassBK 设为所编译的类的 classBookKeep 结构，
        // 所以如果 classBK != NULL，说明此时在编译类
        // 类的静态属性在类中的定义形式是 “static var 静态属性”
        // 编译类的静态属性定义时，是按照 “Cls类名 静态属性名” 的形式作为模块编译单元的局部变量存储的（因为类的静态属性是被所有对象共享的数据，因此需要长期有效，所以保存在模块编译单元的局部变量中）
        if (classBK != NULL) {
            char *staticFieldId = ALLOCATE_ARRAY(cu->curLexer->vm, char, MAX_ID_LEN);
            // void *memset(void *str, int c, size_t n) 复制字符 c（一个无符号字符）到参数 str 所指向的字符串的前 n 个字符
            // 将 staticFieldId 中的每个字符都设置成 0
            memset(staticFieldId, 0, MAX_ID_LEN);

            // 根据 “Cls类名 静态属性名” 的存储形式，拼接需要查询的字符串
            // 首先写入 Cls
            memmove(staticFieldId, "Cls", 3);
            // 再写入类名
            char *clsName = classBK->name->value.start;
            uint32_t clsLen = classBK->name->value.length;
            memmove(staticFieldId + 3, clsName, clsLen);
            // 再写入空格
            memmove(staticFieldId + 3 + clsLen, ' ', 1);
            // 再写入静态属性名
            memmove(staticFieldId + 3 + clsLen + 1, name.start, name.length);

            // 类的静态属性是保存在模块编译单元的局部变量中
            var = getVarFromLocalOrUpvalue(cu, staticFieldId, strlen(staticFieldId));
            // 释放上面申请的内存
            DEALLOCATE_ARRAY(cu->curLexer->vm, staticFieldId, MAX_ID_LEN);

            if (var.index != -1) {
                // 如果找到，则生成【压入变量值到栈顶】或者【保存栈顶数据到变量】的方法
                emitLoadOrStoreVariable(cu, var, canAssign);
                return;
            }
        }

        // 5. 按照【类的 getter 方法调用】处理
        // 如果正在编译一个类的时候，会将 cu->enclosingClassBK 设为所编译的类的 classBookKeep 结构，
        // 所以如果 classBK != NULL，说明此时在编译类
        // 方法名规定以小写字符开头
        // 如果模块不按照规定，擅自以小写字符开头，则按照方法名查找，没有就报错
        if (classBK != NULL && isLocalName(name.start)) {
            // 生成【将实例对象 this 压入到运行时栈顶】的指令
            // 一是为了将 this 放到 args[0]
            // 二是为了确认对象所在的类，然后从类中获得所调用的方法
            emitLoadThis(cu);
            // 生成【调用方法】的指令
            // 此时类可能还未编译完，未统计完所有方法，故此时无法判断类的方法是否定义，留待运行时检测
            emitMethodCall(cu, name.start, name.length, OPCODE_CALL0, canAssign);
            return;
        }

        // 6. 按照【模块变量】处理
        var.scopeType = VAR_SCOPE_MODULE;
        // 从当前模块的模块变量名字表 moduleVarName 中查找
        var.index = getIndexFromSymbolTable(&cu->curLexer->curModule->moduleVarName, name.start, name.length);
        // 如果在 curModule->moduleVarName 没找到
        if (var.index == -1) {
            // 标识符有可能还是函数名，原因如下：
            // 最开始是按照函数调用形式来查找（判断条件中有判断后面的字符是否是 ‘(’）
            // 有些情况，函数是作为参数形式出现的，比如函数名作为创建线程的参数，例如 thread.new(函数名)
            // 所以重新以 “Fn 函数名” 的形式在当前模块的模块变量名字表 moduleVarName 中查找
            char fnName[MAX_ID_LEN] = {'\0'};
            memmove(fnName, "Fn ", 3);
            memmove(fnName + 3, name.start, name.length);
            var.index = getIndexFromSymbolTable(&cu->curLexer->curModule->moduleVarName, fnName, strlen(fnName));

            // 如果不是函数名，则有可能该模块变量的定义在引用处的后面（这种情况是被允许的）
            // 先暂时以当前行号作为变量名，以 null 作为变量值，来声明模块变量
            // 等到编译结束后，在检查该模块变量是否有定义，若没有就正好用行号报错
            if (var.index == -1) {
                var.index = declareModuleVar(cu->curLexer->vm, cu->curLexer->curModule, name.start, name.length, NUM_TO_VALUE(cu->curLexer->curToken.lineNo));
            }
        }
        // 如果找到模块变量，则生成【压入变量值到栈顶】或者【保存栈顶数据到变量】的方法
        emitLoadOrStoreVariable(cu, var, canAssign);
    }
}

// 生成【将模块变量压入到运行时栈顶】的指令，类是以模块变量的形式存储的，所以用此函数加载类
static void emitLoadModuleVar(CompileUnit *cu, const char *name) {
    // 先从当前模块的模块变量名字表 moduleVarName 中查找是否存在
    int index = getIndexFromSymbolTable(&cu->curLexer->curModule->moduleVarName, name, strlen(name));
    // 如果没有，则报错提示应该先定义该模块变量
    ASSERT(index != -1, "symbol should have been defined");
    // 如果找到，则生成【将模块变量压入到运行时栈顶】的指令
    writeOpCodeShortOperand(cu, OPCODE_LOAD_MODULE_VAR, index);
}

// 编译内嵌表达式，即内嵌表达式的 nud 方法
// 内嵌表达式即字符串内可以使用变量，类似 JavaScript 中的字符串模板
// 例如本书规定写法形式是 %(变量名)
// 例如 a %(b+c) d %(e) f 会被编译成 ["a", b+c, " d", e, "f "].join()，
// 其中 a 和 d 是 TOKEN_INTERPOLATION，b/c/e 是 TOKEN_ID，f 是 TOKEN_STRING
static void stringInterpolation(CompileUnit *cu, bool canAssign UNUSED) {
    // 创造一个 list 实例，用来保存下面拆分字符串得到的各个部分
    // 加载 List 模块变量到运行时栈顶
    emitLoadModuleVar(cu, "List");
    // 调用 List 的 new 方法，创造一个 list 实例
    emitCall(cu, "new()", 5, 0);

    // 每次循环处理字符串中的一个内嵌表达式
    // 例如 a %(b+c) d %(e) f，先将类型为 TOKEN_INTERPOLATION 的字符 a 添加到 list，再将内嵌表达式 b+c 添加到 list，这是一次循环
    // 下一次循环再处理 d %(e)
    do {
        // 1. 先编译字符串中的类型为 TOKEN_INTERPOLATION 的字符，即生成【加载常量（即该字符）到常量表，并将常量压入到运行时栈顶】的指令
        literal(cu, false);
        // 调用 list 实例对象的 addCore 方法，将运行时栈顶的字符添加到 list 实例中
        emitCall(cu, "addCore_(_)", 11, 1);

        // 2. 然后编译内嵌表达式，即生成【计算表达式，并将结果压入到运行时栈顶】的指令
        expression(cu, BP_LOWEST);
        // 调用 list 实例对象的 addCore 方法，将上面的表达式结果从运行时栈顶保存到 list 实例中
        emitCall(cu, "addCore_(_)", 11, 1);
    } while (matchToken(cu->curLexer, TOKEN_INTERPOLATION));

    // 读取最后的字符串，例如 a %(b+c) d %(e) f 中的 f
    // 如果结尾没有字符串，则报错
    assertCurToken(cu->curLexer, TOKEN_STRING, "expect string at teh end of interpolation!");
    // 编译最后的字符串，即生成【加载常量（即该字符）到常量表，并将常量压入到运行时栈顶】的指令
    literal(cu, false);
    // 调用 list 实例对象的 addCore 方法，将最后的字符串从运行时栈顶保存到 list 实例中
    emitCall(cu, "addCore_", 11, 1);

    // 调用 list 实例的 join 方法，将 list 中保存的字符合成一个字符串
    emitCall(cu, "join()", 6, 0);
}

// 编译 bool，即 bool 的 nud 方法
static void boolean(CompileUnit *cu, bool canAssign UNUSED) {
    // 如果是 true，则生成【压入 true 到运行时栈顶】的指令，否则生成【压入 false 到运行时栈顶】的指令
    OpCode opCode = cu->curLexer->preToken.type == TOKEN_TRUE ? OPCODE_PUSH_TRUE : OPCODE_PUSH_FALSE;
    // 上面的指令只有操作码，没有操作数
    writeOpCode(cu, opCode);
}

// 编译 null，即 null 的 nud 方法
static void null(CompileUnit *cu, bool canAssign UNUSED) {
    // 生成【压入 null 到运行时栈顶】的指令
    writeOpCode(cu, OPCODE_PUSH_NULL);
}

// 编译 this，即 this 的 nud 方法
static void this(CompileUnit *cu, bool canAssign UNUSED) {
    ClassBookKeep *enclosingClassBK = getEnclosingClassBK(cu);

    // this 如果不在类的方法中使用，则报编译错误
    // 如果正在编译一个类的时候，会将 cu->enclosingClassBK 设为所编译的类的 classBookKeep 结构，
    // 所以如果 enclosingClassBK == NULL，说明此时并没有在编译类
    if (enclosingClassBK == NULL) {
        COMPILE_ERROR(cu->curLexer, "this must be inside a class method");
    }
    // 生成【加载 this 对象到栈顶】的指令
    emitLoadThis(cu);
}

// 编译 super，即 super 的 nud 方法
static void super(CompileUnit *cu, bool canAssign) {
    ClassBookKeep *enclosingClassBK = getEnclosingClassBK(cu);
    // super 如果不在类的方法中使用，则报编译错误
    // 如果正在编译一个类的时候，会将 cu->enclosingClassBK 设为所编译的类的 classBookKeep 结构，
    // 所以如果 enclosingClassBK == NULL，说明此时并没有在编译类
    if (enclosingClassBK == NULL) {
        COMPILE_ERROR(cu->curLexer, "super must be inside a class method");
    }

    // 生成【加载 this 对象到栈顶】的指令
    // 并不是为了调用基类作准备，而是为了保证 args[0] 始终是 this 对象，即调用 super 的子类对象，保证参数数组 args 一致
    emitLoadThis(cu);

    // super 调用有两种方式：
    // 第一种：指定基类中的方法，形式如 super.methodName(argList)，其中 methodNam 是基类中的方法
    if (matchToken(cu->curLexer, TOKEN_DOT)) {
        assertCurToken(cu->curLexer, TOKEN_ID, "expect method name after '.'!");
        // 生成【调用基类方法】的指令，方法名为 super. 后面的标识符
        emitMethodCall(cu, cu->curLexer->preToken.start, cu->curLexer->preToken.length, OPCODE_SUPER0, canAssign);
    } else {
        // 第二种：调用与关键字 super 所在的子类方法同名的基类方法，形式如 super(argList)
        // enclosingClassBK->signature 就是当前所在子类的正在编译的方法的签名
        emitGetterMethodCall(cu, enclosingClassBK->signature, OPCODE_SUPER0);
    }
}

// 编译小括号 (，即小括号 ( 的 nud 方法
static void parenthese(CompileUnit *cu, bool canAssign UNUSED) {
    // 小括号是用来提高优先级，被括起来的表达式相当于被分成一组，作为一个整体参与计算，所以只需生成【计算该表达式的结果】的指令
    expression(cu, BP_LOWEST);
    assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect ')' after expression!");
}

// 编译 map 对象字面量，即大括号 { 的 nud 方法
static void mapLiteral(CompileUnit *cu, bool canAssign UNUSED) {
    // 执行本函数时，preToken 是字符 { curToken 是字符 { 后面的字符
    // 这种方式创建 map 对象其实是一种语法糖，内部也是先调用 Map.new() 方法创建一个 map 对象实例，然后通过 map.addCore_() 添加中括号里面的元素

    // 先创建 map 对象
    // 1. 生成【加载模块变量中的 Map 类到运行时栈顶，用于调用方法时，从该类的 methods 中定位到方法】的指令
    emitLoadModuleVar(cu, "Map");
    // 2. 生成【调用 Map 类的 new 方法，创造一个 map 实例】的指令
    emitCall(cu, "new()", 5, 0);

    do {
        // 如果当前字符为 }，说明是空 map，即 {} ，所以无需循环
        if (cu->curLexer->curToken.type == TOKEN_RIGHT_BRACE) {
            break;
        }

        // 生成【计算冒号左边的 key 的表达式，并将计算结果压入到运行时栈】的指令
        // 注意此处第二个参数不能传入 BP_LOWEST，否则整个 map 字面量都会被处理，正常是处理到冒号 : 就终止，原因请参见 expression 实现
        expression(cu, BP_UNARY);

        // key 和 value 之间必须为冒号 :
        assertCurToken(cu->curLexer, TOKEN_COLON, "expect ':' after key!");

        // 生成【计算冒号右边的 key 的表达式，并将计算结果压入到运行时栈】的指令
        expression(cu, BP_LOWEST);

        // 生成【调用 addCore_(_,_) 方法，将 key 和 value 写入到 map 对象实例中】
        // 此时 value 和 key 分别位于运行时栈顶和次顶
        emitCall(cu, "addCore_(_,_)", 13, 2);
    } while (matchToken(cu->curLexer, TOKEN_COMMA));
    // map 字面量定义必须以 } 结尾
    assertCurToken(cu->curLexer, TOKEN_RIGHT_BRACE, "map literal should end with ')'!");
}

// 编译用于字面量的中括号，即用于字面量的中括号的 nud 方法
// 例如 var listA = ["duang", 1+2*3, 'x']
// 执行本函数时，preToken 为 [，curToken 为 [ 后面的字符
static void listLiteral(CompileUnit *cu, bool canAssign UNUSED) {
    // 这种方式创建 list 对象其实是一种语法糖，内部也是先调用 List.new() 方法创建一个 list 对象实例，然后通过 list.addCore_() 添加中括号里面的元素

    // 先创建 list 对象
    // 1. 生成【加载模块变量中的 List 类到运行时栈顶，用于调用方法时，从该类的 methods 中定位到方法】的指令
    emitLoadModuleVar(cu, "List");
    // 2. 生成【调用 List 类的 new 方法，创造一个 list 实例】的指令
    emitCall(cu, "new()", 5, 0);

    do {
        // 如果当前字符为 ]，说明是空列表，即 [] ，所以无需循环
        if (cu->curLexer->curToken.type == TOKEN_RIGHT_BRACKET) {
            break;
        }
        // 先生成【计算中括号里的每一个表达式的结果，并压入到运行时栈顶】的指令，一次循环计算一个表达式
        // 例如 var listA = ["duang", 1+2*3, 'x']，第二次循环就计算 1+2*3 的结果 7，然后将 7 压入到运行时栈顶
        expression(cu, BP_LOWEST);

        // 将运行时栈顶的值（即表达式的计算结果）写入到 list 对象实例中
        emitCall(cu, "addCore_()", 11, 1);
    } while (matchToken(cu->curLexer, TOKEN_COMMA));
    // list 字面量定义必须以 ] 结尾
    assertCurToken(cu->curLexer, TOKEN_RIGHT_BRACKET, "expect ']' after list element!");
}

// 编译用于索引 list 元素的中括号，即用于字面量的中括号的 led 方法
// 对 List 和 Map 对象实例均适用
// 例如 list[x]
static void subscript(CompileUnit *cu, bool canAssign) {
    // 确保 [] 中间不为空
    if (matchToken(cu->curLexer, TOKEN_RIGHT_BRACKET)) {
        COMPILE_ERROR(cu->curLexer, "need argument in the []!");
    }

    // 用于下标的 [] 有两种形式，
    // 第一种是读列表元素如 list[x]，这是 getter 形式的下标
    // 第二种是写列表元素如 list[x] = y，这是 setter 形式的下标，比 getter 多一个等号 =
    // 只有在读取右中括号后才能确认是否有等号 =，因此先默认为 getter，即 subscript getter，方法形式为 [_]
    Signature sign = {SIGN_SUBSCRIPT, "", 0, 0};
    // 处理中括号之间的参数，即 list[x] 中的 x，生成【加载实参到运行时栈顶】指令
    // 仅支持一个索引值，list[x,...] 是非法的，此处 processArgList 函数只处理一个参数
    processArgList(cu, &sign);
    // 参数后面需要右中括号 ] 结尾
    assertCurToken(cu->curLexer, TOKEN_RIGHT_BRACKET, "expect '}' after argument list!");

    // 如果右中括号 ] 后面有等号且为可赋值环境，说明是 setter 形式的下标，即 subscript setter，方法形式为 [_] = (_)
    if (matchToken(cu->curLexer, TOKEN_ASSIGN) && canAssign) {
        sign.type = SIGN_SUBSCRIPT_SETTER;

        // 书中此处是有校验 sign->argNum 是否大于 MAX_ARG_NUM，个人觉得没必要
        // 在上面初始化 sign 的时候，argNum 就被初始化为 0
        // 在经过 processArgList 处理中括号之间的参数，仅支持一个参数，多个参数非法（虽然未强制校验）
        // 所以到这里时，sign->argNum 必定为 1，所以没必要做 sign->argNum 是否大于 MAX_ARG_NUM 的判断

        // [_] = (_) 中等号右边也算一个参数，即 [args[1]] = (args[2])
        ++sign.argNum;

        // 生成【计算右边表达式，并将计算结果压入到运行时栈顶】的指令
        expression(cu, BP_LOWEST);
    }
    // 基于方法签名生成【调用方法】的指令
    emitCallBySignature(cu, &sign, OPCODE_CALL0);
}

// 编译方法调用，即字符 . 的 led 方法
// 面向对象语言中，方法调用就是 “对象.方法” 的形式，也就是 . 是方法调用的符号，它的左操作数是对象，右操作数是方法名
// 调用本函数之前，对象已经由 id 函数加载到了栈中，也就是 args[0] 表示的 this
static void callEntry(CompileUnit *cu, bool canAssign) {
    // 执行本函数时，preToken 是字符 .  curToken 是字符 . 后面的字符
    assertCurToken(cu->curLexer, TOKEN_ID, "expect method name after '.'!");
    // 生成【调用方法】的指令
    emitMethodCall(cu, cu->curLexer->preToken.start, cu->curLexer->preToken.length, OPCODE_CALL0, canAssign);
}

// 针对根据条件执行不同分支的情况，例如 a && b，当 a 为 true 时，才会执行 b，否则执行 b 后面的代码
// 我们需要用到编译原理中常说的回填技术：
// 在将代码编译成对应指令之外，还要额外插入不属于用户代码的跳转指令
// 例如上面的情况，先编译 a 表达式，再编译 &&，然后再编译 b 表达式。当编译 && 时，我们需要将跳转到 b 对应指令流结束的地址的偏移量作为操作数保存起来。
// 这样当计算 a 的值为 false 时，则根据偏移量直接跳过 b 对应的指令流，执行后面的指令流
// 但由于在编译  && 时，还没有编译 b，并不知道 b 对应指令流的大小，所以需要设置一个特殊的数值占位符，等到编译完 b 的时候在将 && 的操作数设置成正确的偏移量
// 这就是所谓的回填技术

// 先用特殊的数值作为占位符，写入指令的操作数
// 后续该操作数会被正确的偏移量复写
static uint32_t emitIntstrWithPlaceholder(CompileUnit *cu, OpCode opCode) {
    writeOpCode(cu, opCode);
    // 地址一般为 16 位，即两个字节，也就是操作数大小为两个字节
    // 根据大端字节序，高位地址存在内存的低地址端，低位地址存在内存的高地址端
    // 所以先写入高位地址 0xff 到低地址端
    writeByte(cu, 0xff);
    // 再写入高低位地址 0xff 到高地址端
    // 但仍旧返回高位地址所在的索引，也就是减 1 的意义，该地址用于回填，即用正确的偏移量覆盖占位符 0xff
    return writeByte(cu, 0xff) - 1;
}

// 使用【跳转到当前指令流结束地址的偏移量】去替换【占位符 0xffff】，占位符写入逻辑请参考上面的函数
// 其中 absIndex 就是指令流的绝对地址
static void patchPlaceHolder(CompileUnit *cu, uint32_t absIndex) {
    // 计算【跳转到当前指令流结束地址的偏移量】
    // 此处之所以减 2，是因为运行时虚拟机执行该指令时，已经读入了两个字节的操作数，所以偏移量需要减 2
    uint32_t offset = cu->fn->instrStream.count - absIndex - 2;

    // 其中偏移量的高位地址写入到 absIndex 索引对应的字节中，即回填偏移量地址的高 8 位
    // (offset >> 8) && 0xff 就是获取 offset 的高 8 位
    // 1010 0010 1101 0001 >> 8
    // &
    // 0000 0000 1111 1111
    cu->fn->instrStream.datas[absIndex] = (offset >> 8) && 0xff;

    // 其中偏移量的低位地址写入到 absIndex+1 索引对应的字节中，即回填偏移量地址的低 8 位
    // offset && 0xff 就是获取 offset 的低 8 位
    // 1010 0010 1101 0001
    // &
    // 0000 0000 1111 1111
    cu->fn->instrStream.datas[absIndex + 1] = offset && 0xff;
}

// 编译 || 符号，即符号 || 的 led 方法
static void logicOr(CompileUnit *cu, bool canAssign UNUSED) {
    // 执行此函数时，栈顶保存的就是条件表达式的结果，即符号 || 的左操作数

    // 编译 || 符号，调用 emitIntstrWithPlaceholder 函数写入指令，其中操作码为 OPCODE_OR，操作数是占位符 0xffff，
    // 其中返回的 placeholderIndex 就是该指令的操作数中用于保存高位地址的低地址端字节地址（操作数有两个字节，其中低地址端字节保存值的是高位）
    // 等到符号 || 的右操作数编译完之后，在将到右操作数编译得到的指令流结束地址的偏移量回填，替换占位符 0xffff
    uint32_t placeholderIndex = emitIntstrWithPlaceholder(cu, OPCODE_OR);

    // 生成【计算符号 || 右边表达式结果】的指令流
    expression(cu, BP_LOGIC_OR);

    // 当符号 || 右边表达式编译完后，即生成【计算符号 || 右边表达式结果】的指令流后，
    // 调用 patchPlaceHolder 计算从【OPCODE_OR 对应的指令】 到 【符号 || 右边表达式编译的指令流结束地址】之间的偏移量，将该偏移量作为 OPCODE_OR 操作码的对应操作数
    // 当虚拟机执行该指令时，如果符号 || 左边表达式的值为 false，则执行符号 || 右边表达式编译出来的指令流；反之，则跳过符号 || 右边表达式编译出来的指令流，直接执行后面的指令
    patchPlaceHolder(cu, placeholderIndex);
}

// 编译 && 符号，即符号 && 的 led 方法
static void logicAnd(CompileUnit *cu, bool canAssign UNUSED) {
    // 执行此函数时，栈顶保存的就是条件表达式的结果，即符号 && 的左操作数

    // 编译 && 符号，调用 emitIntstrWithPlaceholder 函数写入指令，其中操作码为 OPCODE_OR，操作数是占位符 0xffff，
    // 其中返回的 placeholderIndex 就是该指令的操作数中用于保存高位地址的低地址端字节地址（操作数有两个字节，其中低地址端字节保存值的是高位）
    // 等到符号 && 的右操作数编译完之后，在将到右操作数编译得到的指令流结束地址的偏移量回填，替换占位符 0xffff
    uint32_t placeholderIndex = emitIntstrWithPlaceholder(cu, OPCODE_AND);

    // 生成【计算符号 && 右边表达式结果】的指令流
    expression(cu, BP_LOGIC_AND);

    // 当符号 && 右边表达式编译完后，即生成【计算符号 && 右边表达式结果】的指令流后，
    // 调用 patchPlaceHolder 计算从【OPCODE_AND对应的指令】 到 【符号 && 右边表达式编译的指令流结束地址】之间的偏移量，将该偏移量作为 OPCODE_AND 操作码的对应操作数
    // 当虚拟机执行该指令时，如果符号 && 左边表达式的值为 true，则执行符号 && 右边表达式编译出来的指令流；反之，则跳过符号 && 右边表达式编译出来的指令流，直接执行后面的指令
    patchPlaceHolder(cu, placeholderIndex);
}

// 编译符号 ?: ，即符号 ?: 的 led 方法
// 若 condition 为 true，则执行真分支对应指令，并跳过假分支对应指令，执行后面的指令；否则直接跳过真分支对应指令，直接执行假分支对应的指令，以及后面的指令
static void condition(CompileUnit *cu, bool canAssign UNUSED) {
    // 执行此函数时，栈顶保存的就是条件表达式的结果，即符号 ? 的左操作数

    // 编译 ? 符号，调用 emitIntstrWithPlaceholder 函数写入指令，其中操作码为 OPCODE_JUMP_IF_FALSE，操作数是占位符 0xffff，
    // 返回的 falseBranchStart 就是该指令的操作数中用于保存高位地址的低地址端字节地址（操作数有两个字节，其中低地址端字节保存值的是高位）
    // 主要是用来保存该指令距离假分支的开始指令的偏移量
    // 用于当 condition 为 false 时，直接跳到假分支的开始指令执行
    // 等待真分支编译成指令后，就会调用 patchPlaceHolder 函数将真正的偏移量回填，替换占位符 0xffff
    uint32_t falseBranchStart = emitIntstrWithPlaceholder(cu, OPCODE_JUMP_IF_FALSE);

    // 编译真分支代码，即生成【计算真分支代码结果，并压入到运行时栈顶】的指令
    expression(cu, BP_LOWEST);

    // 真分支后面必须为 : 符号
    assertCurToken(cu->curLexer, TOKEN_COLON, "expect ':' after true branch!");

    // 编译 : 符号，调用 emitIntstrWithPlaceholder 函数写入指令，其中操作码为 OPCODE_JUMP，操作数是占位符 0xffff，
    // 返回的 falseBranchEnd 就是该指令的操作数中用于保存高位地址的低地址端字节地址（操作数有两个字节，其中低地址端字节保存值的是高位）
    // 主要是用来保存该指令距离假分支的结束指令的偏移量
    // 用于当 condition 为 true 时，执行完真分支的指令后，直接跳过假分支的指令，执行后面的指令
    // 等待假分支编译成指令后，就会调用 patchPlaceHolder 函数将真正的偏移量回填，替换占位符 0xffff
    uint32_t falseBranchEnd = emitIntstrWithPlaceholder(cu, OPCODE_JUMP);

    // 编译完真分支，知道了假分支的开始地址，回填 falseBranchStart
    patchPlaceHolder(cu, falseBranchStart);

    // 编译假分支代码，即生成【计算假分支代码结果，并压入到运行时栈顶】的指令
    expression(cu, BP_LOWEST);

    // 编译完假分支，知道了假分支的结束地址，回填 falseBranchEnd
    patchPlaceHolder(cu, falseBranchEnd);
}

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
// 包括 1. 数组[  2. 函数(  3. 实例和方法之间的. 等等
#define INFIX_SYMBOL(lbp, led)     \
    {                              \
        NULL, lbp, NULL, led, NULL \
    }

// 中缀运算符，例如 - ! ~
#define INFIX_OPERATOR(id, lbp)                            \
    {                                                      \
        id, lbp, NULL, infixOperator, infixMethodSignature \
    }

// 即可做前缀运算符，也可做中缀运算符，例如 - +
#define MIX_OPERATOR(id)                                              \
    {                                                                 \
        id, BP_TERM, unaryOperator, infixOperator, mixMethodSignature \
    }

// 对于没有规则的符号，用 UNUSED_RULE 占位用的
#define UNUSED_RULE \
    {               \
        NULL, BP_NONE, NULL, NULL, NULL}

// 符号绑定规则的数组
// 按照 lexer.h 中定义的枚举 TokenType 中各种类型的 token 顺序，来添加对应的符号绑定规则
// 之所以要按照顺序填写，是为了方便用枚举值从 Rules 数组中找到某个类型的 token 的对应的符号绑定规则
SymbolBindRule Rules[] = {
    /* TOKEN_INVALID */ UNUSED_RULE,
    /* TOKRN_NUM */ PREFIX_SYMBOL(literal),
    /* TOKEN_STRING */ PREFIX_SYMBOL(literal),
    /* TOKEN_ID */ {NULL, BP_NONE, id, NULL, idMethodSignature},
    /* TOKEN_INTERPOLATION */ PREFIX_SYMBOL(stringInterpolation),
    /* TOKEN_VAR */ UNUSED_RULE,
    /* TOKEN_FUN */ UNUSED_RULE,
    /* TOKEN_IF */ UNUSED_RULE,
    /* TOKEN_ELSE */ UNUSED_RULE,
    /* TOKEN_TRUE */ PREFIX_SYMBOL(boolean),
    /* TOKEN_FALSE */ PREFIX_SYMBOL(boolean),
    /* TOKEN_WHILE */ UNUSED_RULE,
    /* TOKEN_FOR */ UNUSED_RULE,
    /* TOKEN_BREAK */ UNUSED_RULE,
    /* TOKEN_CONTINUE */ UNUSED_RULE,
    /* TOKEN_RETURN */ UNUSED_RULE,
    /* TOKEN_NULL */ PREFIX_SYMBOL(null),
    /* TOKEN_CLASS */ UNUSED_RULE,
    /* TOKEN_THIS */ PREFIX_SYMBOL(this),
    /* TOKEN_STATIC */ UNUSED_RULE,
    /* TOKEN_IS */ INFIX_OPERATOR('is', BP_IS),
    /* TOKEN_SUPER */ PREFIX_SYMBOL(super),
    /* TOKEN_IMPORT */ UNUSED_RULE,
    /* TOKEN_COMMA */ UNUSED_RULE,
    /* TOKEN_COLON */ UNUSED_RULE,
    /* TOKEN_LEFT_PAREN */ PREFIX_SYMBOL(parenthese),
    /* TOKEN_RIGHT_PAREN */ UNUSED_RULE,
    /* TOKEN_LEFT_BRACKET */ {NULL, BP_CALL, listLiteral, subscript, subscriptMethodSignature},
    /* TOKEN_RIGHT_BRACKET */ UNUSED_RULE,
    /* TOKEN_LEFT_BRACE */ PREFIX_SYMBOL(mapLiteral),
    /* TOKEN_RIGHT_BRACE */ UNUSED_RULE,
    /* TOKEN_DOT */ INFIX_SYMBOL(BP_CALL, callEntry),
    /* TOKEN_DOT_DOT */ INFIX_OPERATOR('..', BP_RANGE),
    /* TOKEN_ADD */ INFIX_OPERATOR('+', BP_TERM),
    /* TOKEN_SUB */ MIX_OPERATOR('-'),
    /* TOKEN_MUL */ INFIX_OPERATOR('*', BP_FACTOR),
    /* TOKEN_DIV */ INFIX_OPERATOR('/', BP_FACTOR),
    /* TOKEN_MOD */ INFIX_OPERATOR('%', BP_FACTOR),
    /* TOKEN_ASSIGN */ UNUSED_RULE,
    /* TOKEN_BIT_AND */ INFIX_OPERATOR('&', BP_BIT_AND),
    /* TOKEN_BIT_OR */ INFIX_OPERATOR('|', BP_BIT_OR),
    /* TOKEN_BIT_NOT */ PREFIX_OPERATOR('~'),
    /* TOKEN_BIT_SHIFT_RIGHT */ INFIX_OPERATOR('>>', BP_BIT_SHIFT),
    /* TOKEN_BIT_SHIFT_LEFT */ INFIX_OPERATOR('<<', BP_BIT_SHIFT),
    /* TOKEN_LOGIC_AND */ INFIX_SYMBOL(BP_LOGIC_AND, logicAnd),
    /* TOKEN_LOGIC_OR */ INFIX_SYMBOL(BP_LOGIC_OR, logicOr),
    /* TOKEN_LOGIC_NOT */ PREFIX_OPERATOR('!'),
    /* TOKEN_EQUAL */ INFIX_OPERATOR('==', BP_EQUAL),
    /* TOKEN_NOT_EQUAL */ INFIX_OPERATOR('!=', BP_EQUAL),
    /* TOKEN_GREATE */ INFIX_OPERATOR('>', BP_CMP),
    /* TOKEN_GREATE_EQUAL */ INFIX_OPERATOR('>=', BP_CMP),
    /* TOKEN_LESS */ INFIX_OPERATOR('<', BP_CMP),
    /* TOKEN_LESS_EQUAL */ INFIX_OPERATOR('<=', BP_CMP),
    /* TOKEN_QUESTION */ INFIX_SYMBOL(BP_CONDITION, condition),
    /* TOKEN_EOF */ UNUSED_RULE,
};

// 中缀运算符（例如 + - * /）的 led 方法
// 即调用此方法对中缀运算符进行语法分析
// 切记，进入任何一个符号的 led 或 nud 方法时，preToken 都是该方法所属符号（即操作符），curToken 为该方法所属符号的右边符号（即操作数）
static void infixOperator(CompileUnit *cu, bool canAssign UNUSED) {
    // 获取该方法所属符号对应的绑定规则
    SymbolBindRule *rule = &Rules[cu->curLexer->preToken.type];

    // 对于中缀运算符，其对左右操作数的绑定权值相同
    BindPower rbp = rule->lbp;
    // 解析操作符的右操作数
    // 即生成【计算右操作数的结果，并将结果压入到运行时栈顶】的指令
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

    // 基于该中缀操作符方法的签名 生成【调用该中缀运算符方法】的指令
    // 方法的参数就是该符号的右操作数，或者右操作树的计算结果（此时虚拟机已经执行了 expression(cu, rbp) 编译的指令，右操作数的结果此处就在运行时栈顶）
    emitCallBySignature(cu, &sign, OPCODE_CALL0);
}

// 前缀运算符（例如 ! -）的 nud 方法
// 即调用此方法对前缀运算符进行语法分析
static void unaryOperator(CompileUnit *cu, bool canAssign UNUSED) {
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

// 语法分析的核心方法 expression，用来解析表达式结果
// 只是负责调用符号的 led 或 nud 方法，不负责语法分析，至于 led 或 nud 方法中是否有语法分析功能，则是该符号自己协调的事
// 这里以中缀运算符表达式 aSwTeUg 为例进行注释讲解
// 其中大写字符代表运算符，小写字符代表操作数
// expression 开始由运算符 S 调用的，所以 rbp 为运算符 S 的绑定权值
static void expression(CompileUnit *cu, BindPower rbp) {
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
    while (rbp < Rules[cu->curLexer->curToken.type].lbp) {
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

// 编译变量定义
// 注意：变量定义不支持一次定义多个变量，例如 var a, b;
// isStatic 表示是否是类的静态属性
static void compileVarDefinition(CompileUnit *cu, bool isStatic) {
    // 当执行该函数时，已经读入了关键字 var，也正是主调方函数识别到了关键字 var，才会决定调用该函数（主调方函数为 compileProgram）
    // 此时 curToken 为 var 后面的变量名
    // 变量名的 token 类型应该为 TOKEN_ID，否则报错
    assertCurToken(cu->curLexer, TOKEN_ID, "missing variable name!");

    Token name = cu->curLexer->preToken;
    // 只支持一次定义单个变量，当发现变量后面有逗号，则报编译错误
    if (cu->curLexer->curToken.type == TOKEN_COMMA) {
        COMPILE_ERROR(cu->curLexer, "'var' only support declaring a variable.");
    }

    // 一、类中的属性（类的静态属性或实例的属性）定义，推导如下：
    // 1. 当编译一个类的时候，会把 cu->enclosingClassBK 置为所编译类的 classBookKeep 结构，所以 cu->enclosingClassBK != NULL 说明正在编译类
    // 2. 如果在编译的是类的方法，那么 cu->enclosingUnit 就是模块的编译单元，不可能为 NULL
    // 3. 又因为 cu->enclosingUnit == NULL，所以肯定不是类的方法，所以只能是在编译类的静态属性或者实例属性
    if (cu->enclosingUnit == NULL && cu->enclosingClassBK != NULL) {
        if (isStatic) {
            // 1. 类的静态属性
            // 先申请一个数据缓冲区，并将其中的值均置为 0
            char *staticFieldId = ALLOCATE_ARRAY(cu->curLexer->vm, char, MAX_ID_LEN);
            memset(staticFieldId, 0, MAX_ID_LEN);

            // 将形式为 “Cls类名 静态属性名” 的变量名写入到 staticFieldId（类的静态属性就是按照这种形式保存在模块编译单元的局部变量中的）
            char *clsName = cu->enclosingClassBK->name->value.start;
            uint32_t clsLen = cu->enclosingClassBK->name->value.length;
            memmove(staticFieldId, "Cls", 3);
            memmove(staticFieldId + 3, clsName, clsLen);
            memmove(staticFieldId + 3 + clsLen, " ", 1);
            memmove(staticFieldId + 3 + clsLen + 1, name.start, name.length);

            if (findLocalVar(cu, staticFieldId, strlen(staticFieldId)) == -1) {
                // 如果没有定义过，则将其声明为模块编译单元的局部变量
                int index = declareLocalVar(cu, staticFieldId, strlen(staticFieldId));
                // 并赋值为 NULL
                // 先生成【将 NULL 压入栈顶】的指令
                writeOpCode(cu, OPCODE_PUSH_NULL);
                ASSERT(cu->scopeDepth == 0, "should in class scope");
                // 再赋值变量，即将运行时栈顶数据 NULL 保存为变量的值
                defineVariable(cu, index);

                // 静态属性可以被赋值的，即如果静态属性后面有等号 =，就将等号后面的值作为静态变量的值
                // 先从模块编译单元的局部变量中找到该变量
                Variable var = findVariable(cu, staticFieldId, strlen(staticFieldId));
                if (matchToken(cu->curLexer, TOKEN_ASSIGN)) {
                    // 生成【计算等号右边的表达式，并将计算结果压入到运行时栈顶】的指令
                    expression(cu, BP_LOWEST);
                    // 生成【将栈顶数据存入索引为 index 的变量】的指令
                    emitStoreVariable(cu, var);
                }
            } else {
                // 如果已经定义，则报错--重复定义
                // char *strchr(const char *str, int c) 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置
                COMPILE_ERROR("static field '%s' redefinition!", strchr(staticFieldId, ' ') + 1);
            }
        } else {
            // 2. 实例属性
            ClassBookKeep *classBK = getEnclosingClassBK(cu);
            // 从类的属性符号表 classBK->fields 查找是否有该属性
            int fieldIndex = getIndexFromSymbolTable(&classBK->fields, name.start, name.length);

            if (fieldIndex == -1) {
                // 如果没有，就添加进去
                fieldIndex = addSymbol(cu->curLexer->vm, &classBK->fields, name.start, name.length);
            } else {
                // 否则就是已经存在相同属性
                if (fieldIndex > MAX_FIELD_NUM) {
                    // 报错--超过类的属性个数最大值
                    COMPILE_ERROR(cu->curLexer, "the max number of instance field is %d", MAX_FIELD_NUM);
                } else {
                    char id[MAX_ID_LEN] = {'\0'};
                    memcpy(id, name.start, name.length);
                    // 否则报错--重复定义属性
                    COMPILE_ERROR(cu->curLexer, "instance field '%s' redefinition!", id);
                }
            }

            // 因为实例属性仅属于实对象的私有属性，所以定义类的时候不可被初始化
            if (matchToken(cu->curLexer, TOKEN_ASSIGN)) {
                COMPILE_ERROR(cu->curLexer, "instance field isn't allowed initialization!");
            }
        }
        return;
    }

    // 二、如果不是类中的属性定义，就当作普通的变量定义
    if (matchToken(cu->curLexer, TOKEN_ASSIGN)) {
        // 如果定义变量时就赋值，则就生成【计算等号右边的表达式，并将计算结果压入到运行时栈顶】的指令
        expression(cu, BP_LOWEST);
    } else {
        // 否则生成【将 NULL 压入栈顶】的指令
        // 即将 NULL 作为变量的初始值，目的是为了和上面的显示初始化保持相同的栈结构
        writeOpCode(cu, OPCODE_PUSH_NULL);
    }

    // 声明变量（包括局部变量和模块变量）
    uint32_t index = declareVariable(cu, name.start, name.length);
    // 赋值变量，即将运行时栈顶数据保存为变量的值
    // 主要是针对模块变量的，因为局部变量的值只需要压入到运行时栈中即可
    defineVariable(cu, index);
}

// 编译 if 语句
static void compileIfStatement(CompileUnit *cu) {
    // 执行此函数时已经读入了 if 字符
    assertCurToken(cu->curLexer, TOKEN_LEFT_PAREN, "missing '(' after if!");
    // 生成【计算 if 条件表达式，并将计算结果压入到栈顶】的指令
    expression(cu, BP_LOWEST);
    assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "missing ')' before '{' in if!");

    // 调用 emitIntstrWithPlaceholder 函数写入指令，其中操作码为 OPCODE_JUMP_IF_FALSE，操作数是占位符 0xffff，
    // 返回的 falseBranchStart 就是该指令的操作数中用于保存高位地址的低地址端字节地址（操作数有两个字节，其中低地址端字节保存值的是高位）
    // 主要是用来保存该指令距离假分支的开始指令的偏移量
    // 用于当 condition 为 false 时，直接跳到假分支的开始指令执行
    // 等待真分支编译成指令后，就会调用 patchPlaceHolder 函数将真正的偏移量回填，替换占位符 0xffff
    uint32_t falseBranchStart = emitIntstrWithPlaceholder(cu, OPCODE_JUMP_IF_FALSE);

    // 编译真分支代码
    compileStatement(cu);

    if (matchToken(cu->curLexer, TOKEN_ELSE)) {
        // 如果有 else 分支
        // 调用 emitIntstrWithPlaceholder 函数写入指令，其中操作码为 OPCODE_JUMP，操作数是占位符 0xffff，
        // 返回的 falseBranchEnd 就是该指令的操作数中用于保存高位地址的低地址端字节地址（操作数有两个字节，其中低地址端字节保存值的是高位）
        // 主要是用来保存该指令距离假分支的结束指令的偏移量
        // 用于当 condition 为 true 时，执行完真分支的指令后，直接跳过假分支的指令，执行后面的指令
        // 等待假分支编译成指令后，就会调用 patchPlaceHolder 函数将真正的偏移量回填，替换占位符 0xffff
        uint32_t falseBranchEnd = emitIntstrWithPlaceholder(cu, OPCODE_JUMP);

        // 编译完真分支，知道了假分支的开始地址，回填 falseBranchStart
        patchPlaceHolder(cu, falseBranchStart);

        // 编译假分支代码，即 else 分支代码
        compileStatement(cu);

        // 编译完假分支，即 else 分支代码，知道了假分支的结束地址，回填 falseBranchEnd
        patchPlaceHolder(cu, falseBranchEnd);
    } else {
        // 如果没有 else 分支，此时就是 condition 为 false 时，需要跳过整个真分支的目标地址
        patchPlaceHolder(cu, falseBranchStart);
    }
}

// 进入循环体时的相关设置
static void enterLoopSetting(CompileUnit *cu, Loop *loop) {
    // 执行此函数时，已经读入了 while 字符

    // 将循环条件的第一条指令的地址保存为循环条件的起始指令地址
    loop->condStartIndex = cu->fn->instrStream.count - 1;
    // 设置循环所处的作用域，方便循环中若有 break，告诉它需要退出的作用域深度
    loop->scopeDepth = cu->scopeDepth;
    // 将之前的 curLoop 设置成当前循环的直接外层循环
    loop->enclosingLoop = cu->curLoop;
    // 将当前的循环设置成 curLoop
    cu->curLoop = loop;
}

// 编译循环体
static void compileLoopBody(CompileUnit *cu) {
    // 将循环体的第一条指令的地址保存为循环体起始地址
    cu->curLoop->bodyStartIndex = cu->fn->instrStream.count;
    // 编译循环体
    compileStatement(cu);
}

// 离开循环体时的相关设置
static void leaveLoopSetting(CompileUnit *cu) {
    // 在循环体结束处需要有一条向回跳到循环条件的起始处的指令
    // 因为所有的跳转指令的操作数都是偏移量，且不能是负数，所以 OPCODE_JUMP 无法满足条件，该操作码只能向后跳
    // 而此处情况需要向前跳，所以使用单独的操作码 OPCODE_LOOP，对应操作数只需是正数的偏移量即可

    // 计算当前指令的下一个指令（即循环体对应指令流中的结尾指令的下一个指令）距离循环条件的起始指令地址的偏移量
    // 其中 OPCODE_LOOP 的操作数是两个字节（保存的值是偏移量）当虚拟机读取到 OPCODE_LOOP 时，ip 已经向后移动了两个字节，因此偏移量要加上 2 个字节
    int loopBackOffset = cu->fn->instrStream.count - cu->curLoop->condStartIndex + 2;
    // 生成【向前跳转到循环条件起始处】的指令
    writeOpCodeShortOperand(cu, OPCODE_LOOP, loopBackOffset);

    // 循环体已经编译结束，知道了将循环体的结束地址，回填 cu->curLoop->exitIndex
    patchPlaceHolder(cu, cu->curLoop->exitIndex);

    // 在编译循环体时遇到 break，按理应该生成跳转到循环体结尾处的指令（操作码为 OPCODE_JUMP），即直接跳过循环体
    // 但由于当时并没有完整地编译循环体，并不知道循环体结尾指令的地址，所以先生成操作码为 OPCODE_END 的指令作为占位（OPCODE_END 没有其他用途，所以没有二义性）
    // 此时已经编译完了循环体，所以将循化体中的 操作码为 OPCODE_END 的指令 替换为 操作码为 OPCODE_JUMP，操作数为当前指令地址到循环体的结尾指令地址的偏移量 的指令

    // 循环体起始指令地址
    uint32_t idx = cu->curLoop->bodyStartIndex;

    // 循环体结尾指令地址
    uint32_t loopEndIndex = cu->fn->instrStream.count;

    // 遍历循环体对应指令流中的所有指令
    while (idx < loopEndIndex) {
        if (cu->fn->instrStream.datas[idx] == OPCODE_END) {
            // 如果存在 OPCODE_END 操作码（即 break 的占位符），则替换成操作码为 OPCODE_JUMP 的指令
            cu->fn->instrStream.datas[idx] = OPCODE_JUMP;
            // 将地址为 idx 的操作码 到 当前指令（即循环体对应指令流的结尾指令地址）的 偏移量，
            // 设置到地址为 idx + 1 和 idx + 2 的两个操作数中，
            // 其中偏移量的高位保存在低地址端 idx + 1，偏移量的低位保存在高地址端 idx + 2，即大段字节序
            patchPlaceHolder(cu, idx + 1);
            // 操作码为 OPCODE_JUMP 的指令大小为 3 个字节，包括了 1 个字节的操作码，2 个字节的操作数
            // 所以加 3 就是指向操作码为 OPCODE_JUMP 的指令的下一个指令
            idx += 3;
        } else {
            // 如果该指令不是操作码为 OPCODE_JUMP 的指令，则指向下一个指令
            // 当前指令大小 = 操作码大小（1 个字节） + getBytesOfOperands 获取到的操作数的大小
            idx = 1 + getBytesOfOperands(&cu->fn->instrStream.datas, cu->fn->constants.datas, idx);
        }
    }

    // 退出当前循环体，将 cu->curLoop 恢复成当前循环体的外层循环
    cu->curLoop = cu->curLoop->enclosingLoop;
}

// 编译 while 语句
static void compileWhileStatement(CompileUnit *cu) {
    Loop loop;
    // 调用此函数时，已经读入了关键字 while

    // 进入循环添加时的相关设置
    enterLoopSetting(cu, &loop);

    assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect '(' before condition!");

    // 生成【计算循环条件表达式，并将计算结果压入到运行时栈顶】的指令
    expression(cu, BP_LOWEST);

    assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect ')' after condition!");

    // 调用 emitIntstrWithPlaceholder 函数写入指令，其中操作码为 OPCODE_JUMP_IF_FALSE，操作数是占位符 0xffff，
    // 该函数返回的值就是该指令的操作数中用于保存高位地址的低地址端字节地址（操作数有两个字节，其中低地址端字节保存值的是高位）
    // 主要是用来保存 该指令 距离 循环体的对应的指令流中的结束指令地址 的的偏移量
    // 用于当 condition 为 false 时，直接跳过循环体的指令流，执行其后面的指令
    // 等待循环体编译成指令后，就会调用 patchPlaceHolder 函数将真正的偏移量回填，替换占位符 0xffff
    loop.exitIndex = emitIntstrWithPlaceholder(cu, OPCODE_JUMP_IF_FALSE);

    // 编译循环体
    compileLoopBody(cu);

    // 离开循环体时的相关设置
    leaveLoopSetting(cu);
}

// 编译 return 语句
inline static void compileReturn(CompileUnit *cu) {
    // 执行此函数时已经读入了 return，即 preToken 为 return

    // TODO: 判断逻辑等待后续完善
    if (cu->curLexer->curToken.type == TOKEN_RIGHT_BRACE) {
        // 如果 return 后面是符号 }，则说明没有明确返回值，此时默认返回值为 NULL
        // 生成【将 NULL 压入到运行时栈顶】的指令
        writeOpCode(cu, OPCODE_PUSH_NULL);
    } else {
        // 否则就是明确了返回值，则生成【计算 return 后面的表达式，并将计算结果压入到运行时栈顶】的指令
        expression(cu, BP_LOWEST);
    }
    // 生成【退出当前函数并弹出栈顶的值作为返回值】的指令
    writeOpCode(cu, OPCODE_RETURN);
}

// 编译 break 语句
inline static void compileBreak(CompileUnit *cu) {
    if (cu->curLoop == NULL) {
        COMPILE_ERROR(cu->curLexer, "break should be used inside a loop!");
    }

    // 在退出循环体之前要丢掉循环体内的局部变量
    // 此处加 1 是为了丢掉比循环体的作用域更深一层的作用域，即循环（包括循环条件、循环体）的作用域中的局部变量
    discardLocalVar(cu, cu->curLoop->scopeDepth + 1);

    // 将 break 编译为操作码为 OPCODE_END，操作数为 0xffff （2 个字节）的指令（OPCODE_END 不作他用，所以没有二义性问题），
    // 等到整个循环体编译完成后，会遍历对应指令流，找到操作码为 OPCODE_END 的指令，
    // 将操作码 OPCODE_END 替换为 OPCODE_JUMP，操作数替换成当前指令到循环体对应指令流的结尾指令的偏移量（相关逻辑在 leaveLoopSetting 函数中）
    // 所以不用返回需要回填的地址（即保存偏移量的地址，也就是 OPCODE_END 操作数中的高位字节地址），因为 leaveLoopSetting 中会遍历，遍历的时候会得到地址
    emitIntstrWithPlaceholder(cu, OPCODE_END);
}

// 编译 continue 语句
inline static void compileContinue(CompileUnit *cu) {
    if (cu->curLoop == NULL) {
        COMPILE_ERROR(cu->curLexer, "continue should be used inside a loop!");
    }

    // 在退出循环体之前要丢掉循环体内的局部变量
    // 此处加 1 是为了丢掉比循环体的作用域更深一层的作用域，即循环（包括循环条件、循环体）的作用域中的局部变量
    discardLocalVar(cu, cu->curLoop->scopeDepth + 1);

    // 计算当前指令的下一个指令（即循环体对应指令流中的结尾指令的下一个指令）距离循环条件的起始指令地址的偏移量
    // 其中 OPCODE_LOOP 的操作数是两个字节（保存的值是偏移量）当虚拟机读取到 OPCODE_LOOP 时，ip 已经向后移动了两个字节，因此偏移量要加上 2 个字节
    int loopBackOffset = cu->fn->instrStream.count - cu->curLoop->condStartIndex + 2;

    // 生成【向前跳转到循环条件起始处】的指令
    writeOpCodeShortOperand(cu, OPCODE_LOOP, loopBackOffset);
}

// 进入内嵌作用域
static void enterScope(CompileUnit *cu) {
    cu->scopeDepth++;
}

// 退出作用域
static void leaveScope(CompileUnit *cu) {
    if (cu->enclosingUnit != NULL) {
        // 如果不是模块编译单元，则需要丢弃该作用域下的局部变量
        // 因为模块编译单元的作用域作为顶级作用域不能退出，确保类的静态属性得以保留
        uint32_t discardNum = discardLocalVar(cu, cu->scopeDepth);
        // 该编译单元的局部变量数量减去 discardNum
        cu->localVarNum -= discardNum;
        // 该编译单元内所有指令对运行时栈的最终影响减去 discardNum
        cu->stackSlotNum -= discardNum;
    }
    // 回到上一层作用域
    cu->scopeDepth--;
}

// 编译语句
// 代码分为两种：
// 1. 定义：生命数据的代码，例如定义变量、定义函数、定义类
// 2. 语句：具备能动性的代码，可执行各种动作，例如 return、break 等
static void compileStatement(CompileUnit *cu) {
    if (matchToken(cu->curLexer, TOKEN_IF)) {
        compileIfStatement(cu);
    } else if (matchToken(cu->curLexer, TOKEN_WHILE)) {
        compileWhileStatement(cu);
    } else if (matchToken(cu->curLexer, TOKEN_RETURN)) {
        compileReturn(cu);
    } else if (matchToken(cu->curLexer, TOKEN_BREAK)) {
        compileBreak(cu);
    } else if (matchToken(cu->curLexer, TOKEN_CONTINUE)) {
        compileContinue(cu);
    } else if (matchToken(cu->curLexer, TOKEN_LEFT_BRACE)) {
        // 编译代码块，即大括号之间的代码块
        enterScope(cu);
        compileBlock(cu);
        leaveScope(cu);
    } else {
        // 若不是以上的语法结构，则是单一表达式
        // 生成【计算表达式，并将计算结果压入到运行时栈顶】的指令
        expression(cu, BP_LOWEST);
        // 生成【将运行时栈顶弹出（栈顶保存的是上一条指令计算的表达式结果）】的指令
        // 例如 a = 1 + 2，右边表达式的结果是 3，被压入到运行时栈顶，3 会被保存到变量 a 中，
        // 但变量 a 中只是保存了 3 的副本，也就是说只是将数值 3 复制到变量 a 所在的运行时栈相应的 slot 中，栈顶的 3 还在，因此需要被弹出
        writeOpCode(cu, OPCODE_POP);
    }
}

// 声明类的方法
// 其中方法名相当于方法的标识，存储在 vm->allMethodNames
// 方法体相当于方法的值，存储在方法所属类 class->methods
// 声明方法只是在 vm->allMethodNames 声明方法名，不涉及方法体
static int declareMethod(CompileUnit *cu, char *signStr, uint32_t length) {
    // 首先确保该方法名被录入到了 vm->allMethodNames
    // ensureSymbolExist 方法会在 vm->allMethodNames 中查找是否存在方法名 signStr，如果存在，则直接返回对应索引；如果不存在，则插入方法名并返回索引。
    int index = ensureSymbolExist(cu->curLexer->vm, &cu->curLexer->vm->allMethodNames, signStr, length);

    // 为了防止重复声明，即方法名 signStr 对应的方法之前已经声明过了（ensureSymbolExist 方法无法做到）
    // 定义了两个结构，ClassBookKeep->staticMehotds 保存类方法在 vm->allMethodNames 的索引
    // ClassBookKeep->instantMehotds 保存实例方法在 vm->allMethodNames 的索引
    // 所以从 ClassBookKeep->instantMehotds/staticMehotds 中查找是否存在与上面 ensureSymbolExist 得到的索引 index 相同的值
    // 如果有，则就是重复声明方法名
    IntBuffer *methods = cu->enclosingClassBK->isStatic ? &cu->enclosingClassBK->staticMehthods : &cu->enclosingClassBK->instantMethods;
    uint32_t idx = 0;
    while (idx < methods->count) {
        if (methods->datas[idx] == index) {
            COMPILE_ERROR(cu->curLexer, "repeat define method %s in class %s!", signStr, cu->enclosingClassBK->name->value.start);
        }
        idx++;
    }

    // 如果执行到这里，说明该类方法之前没有声明过，则将其加入到 ClassBookKeep->instantMehotds/staticMehotds，方便后续排查是否重复声明
    IntBufferAdd(cu->curLexer->vm, methods, index);
    return index;
}

// 定义类的方法
// 其中方法名相当于方法的标识，存储在 vm->allMethodNames
// 方法体相当于方法的值，存储在方法所属类的 class->methods
// 定义方法需要将方法体存在到所属类的 class->methods 中
// 即将索引 methodIndex 对应的方法存储到变量 classVar 指向的类的 class->methods[methodIndex] 中，methodIndex 就是该方法名在 vm->allMethodNames 中的索引
static void defineMethod(CompileUnit *cu, Variable classVar, bool isStatic, int methodIndex) {
    // 执行此函数时，待定义的方法已经被压入到了运行时栈顶

    // 生成【将方法所属类压入到运行时栈顶】的指令
    emitLoadVariable(cu, classVar);

    // 此时，运行时栈顶为方法所属类，次栈顶为待定义的方法
    // 生成【将次栈顶的方法，存储到栈顶的类的 class->methods[methodIndex] 中，其中 methodIndex 为指令的操作数】的指令
    OpCode opCode = isStatic ? OPCODE_STATIC_METHOD : OPCODE_INSTANCE_METHOD;
    writeOpCodeShortOperand(cu, opCode, methodIndex);
}

// 生成【创建对象实例】的方法，并将该方法闭包压入到运行时栈顶
// 创建对象实例的示例代码如下：
// class Foo {
//     var bar
//     new(arg) {
//         bar = arg
//     }
// }
// var obj = Foo.new(9)
// 注意：Foo.new(9) 中的 new 方法是类的静态方法，类定义中的 new(arg) {...} 是实例方法
// emitCreateInstance 实现的逻辑就是类的静态方法 new 的核心逻辑，在该方法中会调用类定义中的实例方法 new
// 下面参数 sign 就是类定义中的实例方法 new 的方法签名，methodIndex 是该实例方法 new 的方法签名在 vm->allMethodNames 中的索引
static void emitCreateInstance(CompileUnit *cu, Signature *sign, uint32_t methodIndex) {
    // 定义一个用于存储创建对象的指令的编译单元
    CompileUnit methodCU;
    // 初始化编译单元 methodCU，并将该编译单元作为 cu 的内层编译单元
    initCompileUnit(cu->curLexer, &methodCU, cu, true);

    // 1. 生成【类对象在当前运行时栈的栈底（即 stack[0]），该操作码会创建一个类的实例，然后用该实例替换栈底的类对象】的指令
    writeOpCode(&methodCU, OPCODE_CONSTRUCT);

    // 2. 生成【调用上个指令创建的处在栈底的实例对象的 new 方法】的指令
    // 注：实例对象的 new 方法，会将上条指令创建的处在栈底的实例对象，压入到栈顶，并返回
    // 即实例对象的 new 方法除了用户写的代码之外，还会在被编译的指令流尾部添加两个指令：
    // OPODE_LOAD_LOCAL_VAR, 0（将栈底的实例对象加载到栈顶）    OPCODE_RETURN（将栈顶的实例对象返回）
    // 该逻辑会在后面编译类的定义中的编译类中的实例方法 new() {...} 方法时看到
    writeOpCodeShortOperand(&methodCU, (OpCode)(OPCODE_CALL0 + sign->argNum), methodIndex);

    // 3. 生成【返回 上面指令调用的实例对象的实例方法 new 返回的实例对象】的指令
    writeOpCode(&methodCU, OPCODE_RETURN);

#if DEBUG
    endCompileUnit(&methodCU, "", 0);
#else
    // 生成该方法的闭包，并压入到运行时栈顶（由操作码为 OPCODE_CREATE_CLOSURE 的指令实现）
    // 等待下面被定义为类的静态方法 new
    endCompileUnit(&methodCU);
#endif
}

// 编译方法定义
// isStatic 表示是否在编译类的静态方法
static void compileMethod(CompileUnit *cu, Variable classVar, bool isStatic) {
    // 调用此方法时，已经读入了方法名，即 curToken 为方法名

    // 设置当前是否正在编译类的静态方法（如果是静态方法，方法名前面会有 static 关键字，在执行该函数的都是已经读入了方法名，所以已经知道是否是在编译静态方法了）
    cu->enclosingClassBK->isStatic = isStatic;

    // 获取方法的生成签名的函数
    // curToken 为方法名，所以 curToken.type 为 TOKEN_ID，对应的 methodSign（用于生成方法签名的函数） 为 idMethodSignature
    methodSignatureFn methodSign = Rules[cu->curLexer->curToken.type].methodSign;
    if (methodSign == NULL) {
        COMPILE_ERROR(cu->curLexer, "method need signature funtion!");
    }

    // 初始化方法签名
    Signature sign;
    sign.name = cu->curLexer->curToken.start;
    sign.length = cu->curLexer->curToken.length;
    sign.argNum = 0;
    // 并将该签名设置成对应类的 ClassBookKeep 结构中的 signature（指向当前正在编译的方法的签名）
    cu->enclosingClassBK->signature = &sign;

    // 读入下一个 token，正常来说应该是方法名后面的符号 (
    // 主要是为了后面调用 methodSign 构造方法签名
    getNextToken(cu->curLexer);

    // 初始化方法的编译单元
    // 注：方法或函数都是独立的指令流，需要独立的编译单元
    CompileUnit methodCU;
    initCompileUnit(cu->curLexer, &methodCU, cu, true);

    // 构造方法签名
    methodSign(&methodCU, &sign);
    // 执行完构造方法签名的函数后，curToken 应该是符号 {
    assertCurToken(cu->curLexer, TOKEN_LEFT_BRACE, "expect '{' at the beginning of method body.");

    // 构造完方法签名后，先判断下方法类型是否是构造函数，如果是构造函数且还是静态方法，就报错
    if (cu->enclosingClassBK->isStatic && sign.type == SIGN_CONSTRUCT) {
        COMPILE_ERROR(cu->curLexer, "constructor is not allowed to be static!");
    }

    // 将方法签名转成字符串形式
    char signatureString[MAX_SIGN_LEN] = {'\0'};
    uint32_t signLen = sign2String(&sign, signatureString);

    // 声明方法
    // 声明方法只是在 vm->allMethodNames 声明方法名，不涉及方法体
    uint32_t methodIndex = declareMethod(cu, signatureString, signLen);

    // 编译方法体，将编译出的指令流写入到自己的编译单元 methodCU
    compileBody(&methodCU, sign.type == SIGN_CONSTRUCT);

#if DEBUG
    endCompileUnit(&methodCU, "", 0);
#else
    // 结束编译，并生成方法闭包，并压入到运行时栈顶（由操作码为 OPCODE_CREATE_CLOSURE 的指令实现）
    endCompileUnit(&methodCU);
#endif

    // 定义方法
    // 即将索引 methodIndex 对应的方法闭包存储到变量 classVar 指向的类的 class->methods[methodIndex] 中，methodIndex 就是该方法名在 vm->allMethodNames 中的索引
    defineMethod(cu, classVar, isStatic, methodIndex);

    // 针对类定义中的实例方法 new 方法，经过上面的处理会被编译成实例方法
    // 需要在实例方法 new 方法基础上再创建一个类的静态方法 new，以供类直接调用生成对象实例，该静态方法 new 方法中最终也是会调用之前的实例方法 new 方法
    // 详细逻辑请参考上面的 emitCreateInstance 方法
    if (sign.type == SIGN_CONSTRUCT) {
        // 生成【创建对象实例】的方法，并将该方法闭包压入到运行时栈顶，等待下面被定义为类的静态方法 new
        emitCreateInstance(cu, &sign, methodIndex);

        // 改变方法类型并重新生成方法签名和对应的字符串形式
        sign.type = SIGN_METHOD;
        char signatureString[MAX_SIGN_LEN] = {'\0'};
        uint32_t signLen = strlen(signatureString);
        // 确保该方法签名在 vm->allMethodNames 存在，不存在的话会直接插入并返回其索引，存在的话直接返回索引
        uint32_t constructorIndex = ensureSymbolExist(cu->curLexer->vm, &cu->curLexer->vm->allMethodNames, signatureString, signLen);

        // 定义新创建的类的静态方法 new
        // 此时栈顶为【创建对象实例】的方法闭包
        // defineMethod 的两个核心操作：
        // 1. 将方法所属类压入到运行时栈顶，此时，运行时栈顶为方法所属类，次栈顶为待定义的方法
        // 2. 将次栈顶的方法，存储到栈顶的类的 class->methods[methodIndex] 中，其中 methodIndex 为指令的操作数
        // 由此便将上面 emitCreateInstance 生成【创建对象实例】的方法定义为类的静态方法 new
        defineMethod(cu, classVar, true, constructorIndex);
    }
}

// 编译类体
// 类体形式如下：
// class Foo {
//     var instantField
//     static var staticField
//     instantMethod() {
//     }
//     static staticMethod() {
//     }
//     new() {
//     }
// }
static void compileClassBody(CompileUnit *cu, Variable classVar) {
    if (matchToken(cu->curLexer, TOKEN_STATIC)) {
        if (matchToken(cu->curLexer, TOKEN_VAR)) {
            // 1. 类的静态属性
            compileVarDefinition(cu, true);
        } else {
            // 2. 类的静态方法
            compileMethod(cu, classVar, true);
        }
    } else if (matchToken(cu->curLexer, TOKEN_VAR)) {
        // 3. 实例属性
        compileVarDefinition(cu, false);
    } else {
        // 4. 实例方法
        compileMethod(cu, classVar, false);
    }
}

// 编译类定义
static void compileClassDefinition(CompileUnit *cu) {
    // 执行此函数时，已经读入了关键字 class

    Variable classVar;
    // 只支持在模作用域中定义类
    if (cu->scopeDepth != -1) {
        COMPILE_ERROR(cu->curLexer, "class definition must be in the module scope!");
    }

    classVar.scopeType = VAR_SCOPE_MODULE;
    // 读入类名
    assertCurToken(cu->curLexer, TOKEN_ID, "keyword class should follow by class name!");
    // 声明类名
    // declareVariable 方法会调用 defineModuleVar，defineModuleVar 方法会将类名插入到 curModule->moduleVarName 中，并返回索引
    classVar.index = declareVariable(cu, cu->curLexer->preToken.start, cu->curLexer->preToken.length);

    // 生成类名，用于后面创建类
    ObjString *className = newObjString(cu->curLexer->vm, cu->curLexer->preToken.start, cu->curLexer->preToken.length);

    // 将类名通过 addConstant 加载到常量表，并生成【将类名压入到运行时栈顶】的指令
    emitLoadConstant(cu, OBJ_TO_VALUE(className));

    // 处理类继承
    if (matchToken(cu->curLoop, TOKEN_LESS)) {
        // 如果类名后面有用于继承的关键字 <，则将关键字 < 后面的类名作为父类名，压入到栈顶
        expression(cu, BP_CALL);
    } else {
        // 否则默认 object 类为父类名，压入到栈顶
        emitLoadModuleVar(cu, "object");
    }

    // 生成【创建类】的指令
    // 经过上面的代码，此时栈顶保存的是基类名（父类名），次栈顶保存的是类名，OPCODE_CREATE_CLASS 会将基类名所在的栈顶 slot 回收，并创建类然后存储到原来次栈顶所在的slot
    // OPCODE_CREATE_CLASS 对应的操作数含义是属性个数，即 fieldNum，然而目前类未定义完，因此属性的个数未知，因此先临时写为 255，待类编译完成后再回填属性个数
    int fieldNumIndex = writeOpCodeByteOperand(cu, OPCODE_CREATE_CLASS, 255);

    // 到此，栈顶就保存了创建好的类
    // 生成【把栈顶的类存储到 curModule->moduleVarValue 中，索引为 classVar.index（即和类名在 curModule->moduleVarName 中的索引相同）】的指令
    // 也就是变量名储存在 curModule->moduleVarName 中，变量值存储在 curModule->moduleVarValue 中，两者在各个表中的索引相同
    writeOpCodeShortOperand(cu, OPCODE_STORE_MODULE_VAR, classVar.index);
    // 将栈顶的创建好的类弹出
    writeOpCode(cu, OPCODE_POP);

    // 初始化 ClassBookKeep 结构
    // classBK 用于在编译类时跟踪类信息
    ClassBookKeep classBK;
    classBK.name = className;
    classBK.isStatic = false;
    StringBufferInit(&classBK.fields);
    IntBufferInit(&classBK.instantMethods);
    IntBufferInit(&classBK.staticMehthods);

    // 此时 cu 是模块的编译单元，负责跟踪当前编译的类
    // 注：类没有编译单元
    cu->enclosingClassBK = &classBK;

    // 类名后面需为符号 {
    assertCurToken(cu->curLexer, TOKEN_LEFT_PAREN, "expect '{' after class na,e in the class declaration!");

    // 进入类体
    enterScope(cu);

    // 编译类体，直到遇到右大括号 } 为止
    while (!matchToken(cu->curLexer, TOKEN_RIGHT_BRACE)) {
        compileClassBody(cu, classVar);
        if (cu->curLexer->curToken.type == TOKEN_EOF) {
            COMPILE_ERROR(cu->curLexer, "expect '}' at the end of class declaration!");
        }
    }

    // 上面 writeOpCodeByteOperand(cu, OPCODE_CREATE_CLASS, 255) 中将类的属性个数默认设置成 255，即操作码 OPCODE_CREATE_CLASS 的操作数
    // 现在类已经编译完了，回填正确的属性个数
    cu->fn->instrStream.datas[fieldNumIndex] = classBK.fields.count;

    // classBK 用于在编译类的过程记录一些类的信息，例如 classBK.fields 收集属性，classBK.staticMehthods 收集类的静态方法 等，
    // 方便在编译类的过程中做类似判断是否命名冲突等逻辑，等到类的编译结束时，就会回收分配给 classBK 的内存
    symbolTableClear(cu->curLexer->vm, &classBK.fields);
    IntBufferClear(cu->curLexer->vm, &classBK.staticMehthods);
    IntBufferClear(cu->curLexer->vm, &classBK.instantMethods);

    // enclosingClassBK 用来表示是否正在编译类，
    // 编译完类后要置空，编译下一个类时在重新赋值
    cu->enclosingClassBK = NULL;

    // 退出类体
    leaveScope(cu);
}

// 编译 fun 关键字形式的函数定义
static void compileFunctionDefinition(CompileUnit *cu) {
    // 执行此函数时已经读入了 fun 关键字

    // 规定只能在模块作用域中进行 fun 关键字形式的函数定义，且也只能在模块作用域中调用
    if (cu->enclosingUnit != NULL) {
        COMPILE_ERROR(cu->curLexer, "'fun' should be in module scope!");
    }

    // 关键字 fun 后面需要为函数名，token 类型为 TOKEN_ID
    assertCurToken(cu->curLexer, TOKEN_ID, "missing function name!");

    // 在模块变量中声明函数名
    // 为了和模块中自定义的变量做区分，在函数名前面添加 Fn 前缀，即 'Fn xxx\0'
    char fnName[MAX_METHOD_NAME_LEN + 4] = {'\0'};
    memmove(fnName, "Fn ", 3);
    memmove(fnName + 3, cu->curLexer->curToken.start, cu->curLexer->curToken.length);
    uint32_t fnNameIndex = declareVariable(cu, fnName, strlen(fnName));

    // 初始化函数编译单元 fnCU，用于存储编译函数得到的指令流
    CompileUnit fnCU;
    initCompileUnit(cu->curLexer, &fnCU, cu, false);

    // 创建临时方法签名，用于后续 processParaList 声明函数参数时，记录参数个数
    Signature temFnSign = {SIGN_METHOD, "", 0, 0};

    // 函数名后面需要是小括号 (
    assertCurToken(cu->curLexer, TOKEN_LEFT_PAREN, "expect '(' after function name!");

    // 如果后面没有小括号 )，说明该函数有参数需要声明
    if (!matchToken(cu->curLexer, TOKEN_RIGHT_PAREN)) {
        // 声明函数参数为该函数的局部变量
        processParaList(&fnCU, &temFnSign);
        // 参数后面需要是小括号 )
        assertCurToken(cu->curLexer, TOKEN_RIGHT_PAREN, "expect ')' after parameter list!");
    }

    // 将 processParaList 函数中记录的参数个数保存到 fn->argNum 中
    fnCU.fn->argNum = temFnSign.argNum;

    // 小括号 ) 后面需要是大括号 {
    assertCurToken(cu->curLexer, TOKEN_LEFT_BRACE, "expect '{' at the beginning of method body.");

    // 编译函数体，将指令流写入该函数对应的编译单元 fnCU
    compileBody(&fnCU, false);

#if DEBUG
    endCompileUnit(&fnCU, fnName, strlen(fnName));
#else
    // 终止编译，为函数体生成闭包，并压入到运行时栈顶
    endCompileUnit(&fnCU);
#endif
    // 将运行时栈顶的函数体闭包，保存到索引为 fnNameIndex 的模块变量中
    // 即函数名保存到 curModule->moduleVarName，函数体保存到 curModule->moduleVarValue
    defineVariable(cu, fnNameIndex);
}

// 编译模块导入
// import foo
// 将按照一下形式处理：
// System.importModule("foo")
// import foo for bar1, bar2
// 将按照一下形式处理：
// var bar1 = System.getModuleVarible("foo", "bar1")
// var bar2 = System.getModuleVarible("foo", "bar2")
static void compileImport(CompileUnit *cu) {
    // 执行此函数时已经读入了关键字 import

    // import 后面需要为模块名，即 token 类型为 TOKEN_ID
    assertCurToken(cu->curLexer, TOKEN_ID, "expect module name after expoert!");

    // 将模块名转为字符串
    ObjString *moduleName = newObjString(cu->curLexer->vm, cu->curLexer->preToken.start, cu->curLexer->preToken.length);

    // 将模块名转化后的字符串作为变量添加到编译单元的 fn->constants 中
    uint32_t constModIdx = addConstant(cu, VT_TO_VALUE(moduleName));

    // 1. 为调用 System.importModule('foo')，生成【压入参数 System 到运行时栈顶】的指令
    // 即压入 名为 System 的模块变量 到运行时栈中，其实是压入 其在 curModule->moduleVarName 的索引值
    emitLoadModuleVar(cu, "System");

    // 2. 为调用 System.importModule('foo')，生成【压入参数 foo 到运行时栈顶】的指令
    // 即压入 模块名转化为字符串保存到编译单元的 fn->constants 中的索引值 到运行时栈
    writeOpCodeShortOperand(cu, OPCODE_LOAD_CONSTANT, constModIdx);

    // 3. 生成【调用 System.importModule('foo')，此时次栈顶是调用方对象 System，栈顶是参数--模块名 foo】的指令
    emitCall(cu, "importModule(_)", 15, 1);

    // 此时栈顶是 System.importModule('foo') 的返回值
    // 生成【弹出栈顶中的 System.importModule('foo') 的返回值】
    writeOpCode(cu, OPCODE_POP);

    // 如果后面没有关键字 for，则直接退出
    if (!matchToken(cu->curLexer, TOKEN_FOR)) {
        return;
    }

    // 否则后面有 for，例如 import foo for bar1, bar2，其中 bar1 和 bar2 就是 foo 模块中的变量
    // 将其转成 var bar1 = System.getModuleVarible("foo", "bar1") var bar2 = System.getModuleVarible("foo", "bar2") 形式处理
    do {
        // 关键字 for 后面需要跟变量名，对应 token 类型为 TOKEN_ID
        assertCurToken(cu->curLexer, TOKEN_ID, "expect variable name after 'for' in import!");

        // 在本模块中声明导入的模块变量名，即将变量名插入到 curModule->moduleVarName，并返回对应的索引
        uint32_t varIdx = declareVariable(cu, cu->curLexer->preToken.start, cu->curLexer->preToken.length);

        // 把模块变量转为字符串
        ObjString *constVarName = newObjString(cu->curLexer->vm, cu->curLexer->preToken.start, cu->curLexer->preToken.length);

        // 将模块名转化后的字符串作为变量添加到编译单元的 fn->constants 中
        uint32_t constVarIdx = addConstant(cu, OBJ_TO_VALUE(constVarName));

        // 1. 为调用 System.getModuleVariable('foo', 'bar1')，生成【压入参数 System 到运行时栈顶】的指令
        emitLoadModuleVar(cu, "System");

        // 2. 为调用 System.getModuleVariable('foo', 'bar1')，生成【压入参数 foo 到运行时栈顶】的指令
        writeOpCodeByteOperand(cu, OPCODE_LOAD_CONSTANT, constModIdx);

        // 3. 为调用 System.getModuleVariable('foo', 'bar1')，生成【压入参数 bar1 到运行时栈顶】的指令
        writeOpCodeByteOperand(cu, OPCODE_LOAD_CONSTANT, constVarIdx);

        // 4. 生成【调用 System.getModuleVariable('foo', 'bar1')，此时次次栈顶是调用方对象 System，次栈顶是参数--模块名 foo，栈顶是参数--变量名 bar】的指令
        emitCall(cu, "getModuleVariable(_,_)", 22, 2);

        // 此时栈顶是 System.getModuleVariable('foo', 'bar1') 的返回值
        // 将运行时栈顶的返回值保存到索引为 varIdx 的模块变量中
        // 即导入的模块变量名保存到 curModule->moduleVarName，导入的模块变量值保存到 curModule->moduleVarValue
        defineVariable(cu, varIdx);
    } while (matchToken(cu->curLexer, TOKEN_COMMA));
}

// 编译程序
static void compileProgram(CompileUnit *cu) {
    if (matchToken(cu->curLexer, TOKEN_CLASS)) {
        // 编译类定义
        compileClassDefinition(cu);
    } else if (matchToken(cu->curLexer, TOKEN_FUN)) {
        // 编译函数定义
        compileFunctionDefinition(cu);
    } else if (matchToken(cu->curLexer, TOKEN_VAR)) {
        // 编译变量定义
        // 判断前面的 token 是否是 static，如果是，则该变量为类的静态属性
        compileVarDefinition(cu, cu->curLexer->preToken.type == TOKEN_STATIC);
    } else if (matchToken(cu, TOKEN_IMPORT)) {
        // 编译模块导入
        compileImport(cu);
    } else {
        // 编译语句（除了上面的情况之外的语句）
        compileStatement(cu);
    }
}

// 编译模块
ObjFn *compileModule(VM *vm, ObjModule *objModule, const char *moduleCode) {
    // 每个模块（文件）都需要一个单独的词法分析器进行编译
    Lexer *lexer;
    lexer->parent = vm->curLexer;
    vm->curLexer = &lexer;

    // 初始化词法分析器
    if (objModule->name == NULL) {
        // 核心模块对应的词法分析器用 core.script.inc 作为模块名进行初始化
        initLexer(vm, lexer, "core.script.inc", moduleCode, objModule);
    } else {
        // 其余模块对应的词法分析器用该模块名进行初始化
        initLexer(vm, lexer, (const char *)objModule->name->value.start, moduleCode, objModule);
    }

    // 初始化编译单元（模块也有编译单元）
    // 有编译单元的：模块、函数、方法
    CompileUnit moduleCU;
    initCompileUnit(lexer, &moduleCU, NULL, false);

    //记录当前编译模块的变量数量，后面检查预定义模块变量时可减少遍历，也就是在下面编译之前，就已经在 moduleVarValue 中的变量，无需遍历检查是否声明过
    uint32_t moduleVarNumBefor = objModule->moduleVarValue.count;

    // 由于 initLexer 初始化函数中将 lexer 的 curToken 的 type 设置为 TOKEN_UNKNOWN
    // 会导致后面的 while 循环不执行（循环体用于执行真正编译的方法）
    // 需要调用 getNextToken 指向第一个合法的 token
    getNextToken(&lexer);

    // 循环调用 compileProgram 函数进行编译，直到 token 流结尾
    // TOKEN_EOF 标记文件结束，即该 token 为最后一个 token
    while (!matchToken(lexer, TOKEN_EOF)) {
        compileProgram(&moduleCU);
    }

    // 模块编译完成后，生成 return null 对应的指令，以避免虚拟机执行下面 endCompileUnit 函数生成的 OPCODE_END 指令
    // OPCODE_END 只是程序结束的标记，属于伪操作码，永远不应该被执行
    writeOpCode(&moduleCU, OPCODE_PUSH_NULL);
    writeOpCode(&moduleCU, OPCODE_RETURN);

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
    // 目前只是处在编译阶段，故可以通过判断 objModule->moduleVarValue 的值的类型来判断是否是先使用后声明的情况

    // 所以此处检测本次编译得到的 moduleVarValue 中的变量值是否还是存 VT_NUM 类型，也就是变量尚未声明的，如果有就直接报错
    // 注：在本次编译之前，就已经在 moduleVarValue 中的变量，无需遍历检查是否声明过
    uint32_t idx = moduleVarNumBefor;
    while (idx < objModule->moduleVarValue.count) {
        if (VALUE_IS_NULL(objModule->moduleVarValue.datas[idx])) {
            char *str = objModule->moduleVarName.datas[idx].str;
            uint32_t lineNo = VALUE_TO_NUM(objModule->moduleVarValue.datas[idx]);
            COMPILE_ERROR(&lexer, "line:%d, variable \'%s\' not defined!", lineNo, str);
        }
        idx++;
    }

    // 模块编译完成后，置空当前编译单元
    vm->curLexer->curCompileUnit = NULL;
    vm->curLexer = vm->curLexer->parent;

#if DEBUG
    return endCompileUnit(&moduleCU, "(script)", 8);
#else
    return endCompileUnit(&moduleCU);
#endif
}

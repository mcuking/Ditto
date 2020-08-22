#ifndef _LEXER_LEXER_H
#define _LEXER_LEXER_H
#include "common.h"
#include "vm.h"
#include "meta_obj.h"

// 定义脚本语言的所有 Token 类型
typedef enum
{
    // 未知类型
    TOKEN_UNKNOWN,

    // 数据类型
    TOKEN_NUM,           // 数字
    TOKEN_STRING,        // 字符串
    TOKEN_ID,            // 变量名
    TOKEN_INTERPOLATION, // 内嵌表达式

    // 关键字(系统保留字)
    TOKEN_VAR,      // 'var'
    TOKEN_FUN,      // 'fun'
    TOKEN_IF,       // 'if'
    TOKEN_ELSE,     // 'else'
    TOKEN_TRUE,     // 'true'
    TOKEN_FALSE,    // 'false'
    TOKEN_WHILE,    // 'while'
    TOKEN_FOR,      // 'for'
    TOKEN_BREAK,    // 'break'
    TOKEN_CONTINUE, // 'continue'
    TOKEN_RETURN,   // 'return'
    TOKEN_NULL,     // 'null'

    // 以下是关于类和模块导入的 token
    TOKEN_CLASS,  // 'class'
    TOKEN_THIS,   // 'this'
    TOKEN_STATIC, // 'static'
    TOKEN_IS,     // 'is'
    TOKEN_SUPER,  // 'super'
    TOKEN_IMPORT, // 'import'

    // 分隔符
    TOKEN_COMMA,         // ','
    TOKEN_COLON,         // ':'
    TOKEN_LEFT_PAREN,    // '('
    TOKEN_RIGHT_PAREN,   // ')'
    TOKEN_LEFT_BRACKET,  // '['
    TOKEN_RIGHT_BRACKET, // ']'
    TOKEN_LEFT_BRACE,    // '{'
    TOKEN_RIGHT_BRACE,   // '}'
    TOKEN_DOT,           // '.'
    TOKEN_DOT_DOT,       // '..'

    // 简单双目运算符
    TOKEN_ADD, // '+'
    TOKEN_SUB, // '-'
    TOKEN_MUL, // '*'
    TOKEN_DIV, // '/'
    TOKEN_MOD, // '%'

    // 赋值运算符
    TOKEN_ASSIGN, // '='

    // 位运算符
    TOKEN_BIT_AND,         // '&'
    TOKEN_BIT_OR,          // '|'
    TOKEN_BIT_NOT,         // '~'
    TOKEN_BIT_SHIFT_RIGHT, // '>>'
    TOKEN_BIT_SHIFT_LEFT,  // '<<'

    // 逻辑运算符
    TOKEN_LOGIC_AND, // '&&'
    TOKEN_LOGIC_OR,  // '||'
    TOKEN_LOGIC_NOT, // '!'

    //关系操作符
    TOKEN_EQUAL,        // '=='
    TOKEN_NOT_EQUAL,    // '!='
    TOKEN_GREATE,       // '>'
    TOKEN_GREATE_EQUAL, // '>='
    TOKEN_LESS,         // '<'
    TOKEN_LESS_EQUAL,   // '<='

    TOKEN_QUESTION, // '?'

    // 文件结束标记,仅词法分析时使用
    TOKEN_EOF // 'EOF'
} TokenType;

// 定义表示一个 Token 的结构体
typedef struct
{
    TokenType type;
    const char *start; // 指向源码串中单词的起始地址
    uint32_t length;   // 该单词的长度
    uint32_t lineNo;   // 该单词所在源码中的行数
    Value value;
} Token;

// 定义词法分析器的结构
struct lexer
{
    const char *file;        // 该指针指向源码文件名，用于标记当前正在编译哪个文件
    const char *sourceCode;  // 该指针指向源码字符串，将源码读出来后存储到某缓冲区，然后 sourceCode 指向该缓冲区
    const char *nextCharPtr; // 该指针指向 sourceCode 中下一个字符
    char curChar;            // 保存 sourceCode 中当前字符
    Token curToken;
    Token preToken;
    ObjModule *curModule;                 //当前正在编译的模块
    int interpolationExpectRightParenNum; // 记录内嵌表达式 %() 中括号对的数量
    VM *vm;                               // 表示该 lexer 属于那个 vm
};

// 获取 Token 方法
void getNextToken(Lexer *lexer);

// 如果当前 token 类型为期望类型，则读如下一个 token 并返回 true
// 否则直接返回 false
bool matchToken(Lexer *lexer, TokenType expectTokenType);

// 断言当前 token 类型为期望类型，并读取下一个 token，否则报错
void assertCurToken(Lexer *lexer, TokenType expectTokenType, const char *errMsg);

// 断言下一个 token 类型为期望类型，否则报错
void assertNextToken(Lexer *lexer, TokenType expectTokenType, const char *errMsg);

// 初始化词法分析器
void initLexer(VM *vm, Lexer *lexer, const char *file, const char *sourceCode);

#endif
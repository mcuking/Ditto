#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"
#include "utils.h"

// 定义关键字对应 Token 的结构体
struct keywordToken
{
    char *keyword;
    uint8_t length;
    TokenType type;
};

// 定义了关键字 Token 的数组，用于后面词法分析识别关键词时进行查找
struct keywordToken keywordsToken[] = {
    {"var", 3, TOKEN_VAR},
    {"fun", 3, TOKEN_FUN},
    {"if", 2, TOKEN_IF},
    {"else", 4, TOKEN_ELSE},
    {"true", 4, TOKEN_TRUE},
    {"false", 5, TOKEN_FALSE},
    {"while", 5, TOKEN_WHILE},
    {"for", 3, TOKEN_FOR},
    {"break", 5, TOKEN_BREAK},
    {"continue", 8, TOKEN_CONTINUE},
    {"return", 6, TOKEN_RETURN},
    {"null", 4, TOKEN_NULL},
    {"class", 5, TOKEN_CLASS},
    {"is", 2, TOKEN_IS},
    {"static", 6, TOKEN_STATIC},
    {"this", 4, TOKEN_THIS},
    {"super", 5, TOKEN_SUPER},
    {"import", 6, TOKEN_IMPORT},
    {NULL, 0, TOKEN_UNKNOWN}};

// 判断以 start 开头，长度为 length 的单词是否是关键字，然后返回相应的 TokenType
static TokenType keywordOrId(const *start, uint32_t length)
{
    uint32_t idx = 0;
    while (keywordsToken[idx].keyword != NULL)
    {
        // 遍历关键字 Token 数组，查看和 start 开头，长度为 length 的单词是否相同
        if (keywordsToken[idx].length == length &&
            memcmp(keywordsToken[idx].keyword, start, length))
        {
            // 找到则返回该 Token 的类型
            return keywordsToken[idx].type;
        }
        idx++;
    }

    // 找不到则返回变量名类型
    return TOKEN_ID;
}

// 用于获取下一个字符的内容
static char getNextChar(Lexer *lexer)
{
    return *lexer->nextCharPtr;
}

// 用于更新词法分析器所指向的字符，读进下一个字符
static void scanNextChar(Lexer *lexer)
{
    // 将 nextCharPtr 所指的字符赋给 curChar
    lexer->curChar = *lexer->nextCharPtr;

    // 然后 nextCharPtr 加 1，即指向下下个字符
    lexer->nextCharPtr += 1;
}

// 匹配下一个字符，如果匹配则读进该字符并返回 true，否则直接返回 false
static bool matchNextChar(Lexer *lexer, char expectedChar)
{
    if (getNextChar(lexer) == expectedChar)
    {
        scanNextChar(lexer);
        return true;
    }
    return false;
}

// 跳过空白字符
static void skipBlanks(Lexer *lexer)
{
    while (isspace(lexer->curChar))
    {
        if (lexer->curChar == '\n')
        {
            lexer->curToken.lineNo++;
        }
        scanNextChar(lexer);
    }
}

// 跳过一行
static void skipOneLine(Lexer *lexer)
{
    getNextChar(lexer);
    while (lexer->curChar != '\0')
    {
        if (lexer->curChar == '\n')
        {
            lexer->curToken.lineNo++;
            getNextChar(lexer);
            break;
        }
        getNextChar(lexer);
    }
}

// 词法分析关键字
static void lexKeyword(Lexer *lexer, TokenType type)
{
    // 判断当前字符 curChar 是否是字母/数字/_，如果是则继续读下个字符
    while (isalnum(lexer->curChar) || lexer->curChar == '_')
    {
        scanNextChar(lexer);
    }

    // 将指向下一个字符地址的 nextCharPtr 值减去 curToken 的 start 再减去 1（nextCharPtr 所指的字符不满足条件），即可得到当前读进的字符数
    // 注意 curToken 是调用本函数的函数，在调用本函数之前已经设置好的
    uint32_t length = (uint32_t)(lexer->nextCharPtr - lexer->curToken.start - 1);

    lexer->curToken.length = length;

    if (type == TOKEN_UNKNOWN)
    {
        // 如果 token 类型未知，则可以通过 keywordOrId 查找该单词对应的 token type
        lexer->curToken.type = keywordOrId(lexer->curToken.start, length);
    }
    else
    {
        lexer->curToken.type = type;
    }
}

// 词法分析 unicode 码点
// 前置知识：
// unicode 码点是 4 个十六进制数字，比如 “字” 的码点就是 0x5B57
// 为了和数字区分，用转义字符 \u 做前缀，即 \u5B57
// 该函数将 unicode 码点按照 UTF-8 编码然后写入制定缓冲区 buf 中
static void lexUnicodeCodePoint(Lexer *lexer, ByteBuffer *buf)
{
    int value = 0;
    uint32_t idx = 0;
    uint8_t digit = 0;

    // 循环的过程就是将十六进制转成十进制
    // 例如将 \u5B57 中的 5B57（十六进制）转成十进制 23383 (十进制)
    while (idx < 4)
    {
        scanNextChar(lexer);

        // 先获取每一位数，然后转成十进制的数
        if (lexer->curChar == '\0')
        {
            LEX_ERROR(lexer, "unterminated unicode!");
        }

        if (lexer->curChar >= '0' && lexer->curChar <= '9')
        {
            digit = lexer->curChar - '0';
        }
        else if (lexer->curChar >= 'a' && lexer->curChar <= 'f')
        {
            digit = lexer->curChar - 'a' + 10;
        }
        else if (lexer->curChar >= 'A' && lexer->curChar <= 'F')
        {
            digit = lexer->curChar - 'A' + 10;
        }
        else
        {
            LEX_ERROR(lexer, "invalid unicode!");
        }
        // 将每一位的值转换成十进制后，累积到 value 中
        value = value * 16 | digit;
        idx++;
    }

    // 根据转换成十进制的数值 value，计算其如果用 UTF-8 表示时，需要几个字节
    uint32_t byteNum = getByteNumOfDecodeUtf8(value);
    ASSERT(byteNum != 0, "utf8 encode byte number should be between 1 and 4");

    // 在真正将 value 写入 buf 之前，先写入 byteNum 个 0，以确保事先有 byteNum 个空间
    // 因为 encodeUtf8 方法中是直接将每个字节写入到 buf 中的，例如 *buf = value & 0x7f
    // 并不是调用 ByteBufferAdd 方法，因如果空间不足，则无法为 buf 自动扩容
    ByteBufferFillWrite(lexer->vm, buf, 0, byteNum);

    // 将十进制 value 进行 UTF-8 编码，然后写入指定缓冲区 buf
    encodeUtf8(buf->datas + buf->count - byteNum, value);
}

// 词法分析字符串
static void lexString(Lexer *lexer)
{
    ByteBuffer str;
    ByteBufferInit(&str);

    while (true)
    {
        // 循环扫描下一个字符
        scanNextChar(lexer);

        // 如果在遇到右双引号 “"” 之前遇到字符串结束符 \0，说明字符串是不完整的
        if (lexer->curChar == '\0')
        {
            LEX_ERROR(lexer, "unterminated string!");
        }

        // 如果遇到了右双引号 “"”，则说明字符串已结束，跳出循环
        if (lexer->curChar == '"')
        {
            lexer->curToken.type = TOKEN_STRING;
            break;
        }

        // 如果遇到了 % 说明可能是内嵌表达式
        // 即 %(表达式) ，其中括号里相当于一段代码，类似 JS 中的 eval 方法
        // % 不能单独使用
        // 以上为该脚本语言定义的语法
        if (lexer->curChar == '%')
        {
            // 如果 % 后面没有根 ( 则报错
            if (!matchNextChar(lexer, '('))
            {
                LEX_ERROR(lexer, "'%' should followed by '('!");
            }

            // 一般遇到 %，interpolationExpectRightParenNum 会赋值 1
            // 如果下一次循环中 interpolationExpectRightParenNum 为 1
            // 说明之前的循环中已经遇到了 1，则说明由多个 %
            // 即内嵌表达式会嵌套使用，则报错提示不支持嵌套使用内嵌表达式
            if (lexer->interpolationExpectRightParenNum > 0)
            {
                COMPILE_ERROR(lexer, "Don't support nest interpolate expression!");
            }

            lexer->interpolationExpectRightParenNum = 1;
            lexer->curToken.type = TOKEN_INTERPOLATION;
            break;
        }

        // 当遇到 \ 说明可能遇到了转义字符，例如 \n，所以读取下个字符进一步确认
        // 之所以判断条件中是 '\\'，则是在 C 语言中也需要转义字符
        if (lexer->curChar == '\\')
        {
            scanNextChar(lexer);
            switch (lexer->curChar)
            {
            case '0':
                ByteBufferAdd(lexer->vm, &str, '\0');
                break;
            case 'a':
                ByteBufferAdd(lexer->vm, &str, '\a');
                break;
            case 'b':
                ByteBufferAdd(lexer->vm, &str, '\b');
                break;
            case 'f':
                ByteBufferAdd(lexer->vm, &str, '\f');
                break;
            case 'n':
                ByteBufferAdd(lexer->vm, &str, '\n');
                break;
            case 'r':
                ByteBufferAdd(lexer->vm, &str, '\r');
                break;
            case 't':
                ByteBufferAdd(lexer->vm, &str, '\t');
                break;
            case 'u':
                // 如果是 \u 说明是 unicode 码点
                // 则使用 lexUnicodeCodePoint 处理其后面的 4 个十六进制数字
                lexUnicodeCodePoint(lexer, &str);
                break;
            case '"':
                ByteBufferAdd(lexer->vm, &str, '"');
                break;
            case '\\':
                ByteBufferAdd(lexer->vm, &str, '\\');
                break;
            default:
                LEX_ERROR(lexer, "unsupport eacape \\%c", lexer->curChar);
                break;
            }
        }
        else
        {
            // 如果不是转义字符，仅是普通字符，则直接写入
            ByteBufferAdd(lexer->vm, &str, lexer->curChar);
        }
    }
    ByteBufferClear(lexer->vm, &str);
}

// 跳过行注释和区块注释
static void skipComment(Lexer *lexer)
{
    char nextChar = getNextChar(lexer);
    if (lexer->curChar == '/')
    {
        // 行注释
        skipOneLine(lexer);
    }
    else
    {
        // 区块注释
        // TODO: 该逻辑可能有 bug，当在注释中有 * 作为注释的内容时，
        // 不应该简单的跳出循环，然后判断下一个字符是不是 /
        // 后续完善
        while (nextChar != '*' && nextChar != '\0')
        {
            // 不停地读入注释中下一个字符
            scanNextChar(lexer);

            // 如果注释有换行，则更新 lineNo
            // 主要是为了当某段代码报错时，能准确报出出错行数
            if (lexer->curChar == '\n')
            {
                lexer->curToken.lineNo++;
            }
            // 获取下一个字符，用作下一个循环的判断
            nextChar = getNextChar(lexer);
        }

        // 循环退出后，下一个字符要么是 * 要么是 \0
        // 如果下一个字符是 *，再判断下下个字符是不是 /
        // 如果是，则说明注释结束，读取下个字符
        if (matchNextChar(lexer, '*'))
        {
            if (!matchNextChar(lexer, '/'))
            {
                LEX_ERROR(lexer, "expect '/' after '*'!");
            }
            scanNextChar(lexer);
        }
        else
        {
            // 如果下一个字符是 \0，则报错
            LEX_ERROR(lexer, "expect '*/' before comment end!");
        };
    }
    // 注释之后可能会有空白符
    skipBlanks(lexer);
}

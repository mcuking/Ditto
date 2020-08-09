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
    int idx = 0;
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

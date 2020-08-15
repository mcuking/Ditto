#include <stdio.h>
#include <string.h>
#include "lexer.h"
#include "vm.h"
#include "core.h"

char *tokenArray[] = {
    "UNKNOWN",
    "NUM",
    "STRING",
    "ID",
    "INTERPOLATION",
    "VAR",
    "FUN",
    "IF",
    "ELSE",
    "TRUE",
    "FALSE",
    "WHILE",
    "FOR",
    "BREAK",
    "CONTINUE",
    "RETURN",
    "NULL",
    "CLASS",
    "THIS",
    "STATIC",
    "IS",
    "SUPER",
    "IMPORT",
    "COMMA",
    "COLON",
    "LEFT_PAREN",
    "RIGHT_PAREN",
    "LEFT_BRACKET",
    "RIGHT_BRACKET",
    "LEFT_BRACE",
    "RIGHT_BRACE",
    "DOT",
    "DOT_DOT",
    "ADD",
    "SUB",
    "MUL",
    "DIV",
    "MOD",
    "ASSIGN",
    "BIT_AND",
    "BIT_OR",
    "BIT_NOT",
    "BIT_SHIFT_RIGHT",
    "BIT_SHIFT_LEFT",
    "LOGIC_AND",
    "LOGIC_OR",
    "LOGIC_NOT",
    "EQUAL",
    "NOT_EQUAL",
    "GREATE",
    "GREATE_EQUAL",
    "LESS",
    "LESS_EQUAL",
    "QUESTION",
    "EOF"};

// 运行脚本文件
static void runFile(const char *path)
{
    // 搜索字符串 path 中最后一次出现 / 的位置
    const char *lastSlash = strrchr(path, '/');
    // 如果不存在 /，则说明文件就在当前目录下，无需设置 rootDir
    if (lastSlash != NULL)
    {
        // 申请 lastSlash - path + 2 个字节大小的内存空间，root 指向该空间
        // 即用字符串最后一个 / 的地址减去字符串的首地址，再加 1，就是 / 左边所有的字符
        // 例如 examples/demo/a.di 中 examples/demo 的长度
        char *root = (char *)malloc(lastSlash - path + 2);
        // 将 path 中 lastSlash - path + 1 个字节复制到 root 中
        memcpy(root, path, lastSlash - path + 1);
        // root 中最后一个字节设置成字符串结束符号 \0
        root[lastSlash - path + 1] = '\0';
        // 将 root 赋值给 rootDir
        rootDir = root;
    }

    VM *vm = newVM();
    const char *sourceCode = readFile(path);

    struct lexer lexer;
    initLexer(vm, &lexer, path, sourceCode);

    while (lexer.curToken.type != TOKEN_EOF)
    {
        getNextToken(&lexer);
        // 读取一个 token 然后打印
        printf("%dL: %s [", lexer.curToken.lineNo, tokenArray[lexer.curToken.type]);
        // 在打印 token 对应的源码串
        uint32_t idx = 0;
        while (idx < lexer.curToken.length)
        {
            printf("%c", *(lexer.curToken.start + idx++));
        }
        printf("]\n");
    }
}

int main(int argc, const char **argv)
{
    if (argc == 1)
    {
        // TODO: 调用执行脚本语言的命令行
    }
    else
    {
        // 运行脚本文件
        runFile(argv[1]);
    }
    return 0;
}

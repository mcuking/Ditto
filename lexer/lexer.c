#include "lexer.h"
#include "common.h"
#include "obj_string.h"
#include "unicodeUtf8.h"
#include "utils.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// 定义关键字对应 Token 的结构体
struct keywordToken {
    char *keyword;
    uint8_t length;
    TokenType token;
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
    {NULL, 0, TOKEN_UNKNOWN},
};

// 判断以 start 开头，长度为 length 的单词是否是关键字，然后返回相应的 TokenType
static TokenType keywordOrId(const char *start, uint32_t length) {
    uint32_t idx = 0;
    while (keywordsToken[idx].keyword != NULL) {
        // 遍历关键字 Token 数组，查看和 start 开头，长度为 length 的单词是否相同
        // memcmp 比较 keywordsToken[idx].keyword 指向的字符串 和 start 指向的字符串的前 length 个字符
        // 如果返回 0，则说明两者相等
        if (keywordsToken[idx].length == length &&
            memcmp(keywordsToken[idx].keyword, start, length) == 0) {
            // 找到则返回该 Token 的类型
            return keywordsToken[idx].token;
        }
        idx++;
    }

    // 找不到则返回变量名类型
    return TOKEN_ID;
}

// 向前看一个字符
static char lookAheadChar(Lexer *lexer) {
    return *lexer->nextCharPtr;
}

// 用于更新词法分析器所指向的字符，读进下一个字符
static void getNextChar(Lexer *lexer) {
    // 将 nextCharPtr 所指的字符赋给 curChar，然后 nextCharPtr 加 1，即指向下下个字符
    lexer->curChar = *lexer->nextCharPtr++;
}

// 匹配下一个字符，如果匹配则读进该字符并返回 true，否则直接返回 false
static bool matchNextChar(Lexer *lexer, char expectedChar) {
    if (lookAheadChar(lexer) == expectedChar) {
        getNextChar(lexer);
        return true;
    }
    return false;
}

// 跳过空白字符
static void skipBlanks(Lexer *lexer) {
    while (isspace(lexer->curChar)) {
        if (lexer->curChar == '\n') {
            lexer->curToken.lineNo++;
        }
        getNextChar(lexer);
    }
}

// 解析关键字
static void lexId(Lexer *lexer, TokenType type) {
    // 判断当前字符 curChar 是否是字母/数字/_，如果是则继续读下个字符
    while (isalnum(lexer->curChar) || lexer->curChar == '_') {
        getNextChar(lexer);
    }

    // 将指向下一个字符地址的 nextCharPtr 值减去 curToken 的 start 再减去 1（nextCharPtr 所指的字符不满足条件），即可得到当前读进的字符数
    // 注意 curToken 是调用本函数的函数，在调用本函数之前已经设置好的
    uint32_t length = (uint32_t)(lexer->nextCharPtr - lexer->curToken.start - 1);

    lexer->curToken.length = length;

    if (type == TOKEN_UNKNOWN) {
        // 如果 token 类型未知，则可以通过 keywordOrId 查找该单词对应的 token type
        lexer->curToken.type = keywordOrId(lexer->curToken.start, length);
    } else {
        lexer->curToken.type = type;
    }
}

// 解析十六进制数字
static void lexHexNum(Lexer *lexer) {
    // 判断当前字符是否是合法的十六进制数字
    while (isxdigit(lexer->curChar)) {
        getNextChar(lexer);
    }
}

// 解析十进制数字
static void lexDecNum(Lexer *lexer) {
    // 判断当前字符是否是合法的十进制数字
    while (isdigit(lexer->curChar)) {
        getNextChar(lexer);
    }

    if (lexer->curChar == '.' && isdigit(lookAheadChar(lexer))) {
        getNextChar(lexer);
        while (isdigit(lexer->curChar)) {
            // 遇到小数点，则跳过小数点解析后面的数字
            getNextChar(lexer);
        }
    }
}

// 解析八进制数字
static void lexOctNum(Lexer *lexer) {
    // 判断当前字符是否是合法的八进制数字
    while (lexer->curChar >= '0' && lexer->curChar < '8') {
        getNextChar(lexer);
    }
}

// 解析数字
static void lexNum(Lexer *lexer) {
    // 十六进制以 0x 开头
    if (lexer->curChar == '0' && matchNextChar(lexer, 'x')) {
        // 跳过 x
        getNextChar(lexer);
        // 解析十六进制数字
        lexHexNum(lexer);
        // strtol 方法是将 lexer->curToken.start 指向的字符串转成数字
        // 第三个参数 16 表示是转成十六进制数字
        // 第二个参数 NULL 表示不需要返回转换失败的字符串地址
        lexer->curToken.value = NUM_TO_VALUE(strtol(lexer->curToken.start, NULL, 16));
    } else if (lexer->curChar == '0' && isdigit(lookAheadChar(lexer))) {
        // 解析八进制数字
        lexOctNum(lexer);
        // strtol 同上
        lexer->curToken.value = NUM_TO_VALUE(strtol(lexer->curToken.start, NULL, 8));
    } else {
        // 解析十进制数字
        lexDecNum(lexer);
        // strtod 方法是将 lexer->curToken.start 指向的字符串转成十进制数字
        // 第二个参数 NULL 表示不需要返回转换失败的字符串地址
        lexer->curToken.value = NUM_TO_VALUE(strtod(lexer->curToken.start, NULL));
    }

    // nextCharPtr 会指向第一个不合法字符的下一个字符，所以需要再减 1
    lexer->curToken.length = (uint32_t)(lexer->nextCharPtr - lexer->curToken.start - 1);
    lexer->curToken.type = TOKEN_NUM;
}

// 解析 unicode 码点
// 前置知识：
// unicode 码点是 4 个十六进制数字，比如 “字” 的码点就是 0x5B57
// 为了和数字区分，用转义字符 \u 做前缀，即 \u5B57
// 该函数将 unicode 码点按照 UTF-8 编码然后写入制定缓冲区 buf 中
static void lexUnicodeCodePoint(Lexer *lexer, ByteBuffer *buf) {
    uint32_t idx = 0;
    int value = 0;
    uint8_t digit = 0;

    // 循环的过程就是将十六进制转成十进制
    // 例如将 \u5B57 中的 5B57（十六进制）转成十进制 23383 (十进制)
    while (idx++ < 4) {
        getNextChar(lexer);

        // 先获取每一位数，然后转成十进制的数
        if (lexer->curChar == '\0') {
            LEX_ERROR(lexer, "unterminated unicode!");
        }

        if (lexer->curChar >= '0' && lexer->curChar <= '9') {
            digit = lexer->curChar - '0';
        } else if (lexer->curChar >= 'a' && lexer->curChar <= 'f') {
            digit = lexer->curChar - 'a' + 10;
        } else if (lexer->curChar >= 'A' && lexer->curChar <= 'F') {
            digit = lexer->curChar - 'A' + 10;
        } else {
            LEX_ERROR(lexer, "invalid unicode!");
        }
        // 将每一位的值转换成十进制后，累积到 value 中
        value = value * 16 | digit;
    }

    // 根据转换成十进制的数值 value，计算其如果用 UTF-8 表示时，需要几个字节
    uint32_t byteNum = getByteNumOfEncodeUtf8(value);
    ASSERT(byteNum != 0, "utf8 encode byte number should be between 1 and 4");

    // 在真正将 value 写入 buf 之前，先写入 byteNum 个 0，以确保事先有 byteNum 个空间
    // 因为 encodeUtf8 方法中是直接将每个字节写入到 buf 中的，例如 *buf = value & 0x7f
    // 并不是调用 ByteBufferAdd 方法，因如果空间不足，则无法为 buf 自动扩容
    ByteBufferFillWrite(lexer->vm, buf, 0, byteNum);

    // 将十进制 value 进行 UTF-8 编码，然后写入指定缓冲区 buf
    encodeUtf8(buf->datas + buf->count - byteNum, value);
}

// 解析字符串
static void lexString(Lexer *lexer) {
    ByteBuffer str;
    ByteBufferInit(&str);

    while (true) {
        // 循环扫描下一个字符
        getNextChar(lexer);

        // 如果在遇到右双引号 “"” 之前遇到字符串结束符 \0，说明字符串是不完整的
        if (lexer->curChar == '\0') {
            LEX_ERROR(lexer, "unterminated string!");
        }

        // 如果遇到了右双引号 “"”，则说明字符串已结束，跳出循环
        if (lexer->curChar == '"') {
            lexer->curToken.type = TOKEN_STRING;
            break;
        }

        // 如果遇到了 % 说明可能是内嵌表达式
        // 即 %(表达式) ，其中括号里相当于一段代码，类似 JS 中的 eval 方法
        // % 不能单独使用
        // 以上为该脚本语言定义的语法
        if (lexer->curChar == '%') {
            // 如果 % 后面没有根 ( 则报错
            if (!matchNextChar(lexer, '(')) {
                LEX_ERROR(lexer, "'%' should followed by '('!");
            }

            // 一般遇到 %，interpolationExpectRightParenNum 会赋值 1
            // 如果下一次循环中 interpolationExpectRightParenNum 为 1
            // 说明之前的循环中已经遇到了 1，则说明由多个 %
            // 即内嵌表达式会嵌套使用，则报错提示不支持嵌套使用内嵌表达式
            if (lexer->interpolationExpectRightParenNum > 0) {
                COMPILE_ERROR(lexer, "Don't support nest interpolate expression!");
            }

            lexer->interpolationExpectRightParenNum = 1;
            lexer->curToken.type = TOKEN_INTERPOLATION;
            break;
        }

        // 当遇到 \ 说明可能遇到了转义字符，例如 \n，所以读取下个字符进一步确认
        // 之所以判断条件中是 '\\'，则是在 C 语言中也需要转义字符
        if (lexer->curChar == '\\') {
            getNextChar(lexer);
            switch (lexer->curChar) {
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
                    LEX_ERROR(lexer, "Not support eacape \\%c", lexer->curChar);
                    break;
            }
        } else {
            // 如果不是转义字符，仅是普通字符，则直接写入
            ByteBufferAdd(lexer->vm, &str, lexer->curChar);
        }
    }
    // 用解析到的字符串新建字符串对象，并存储到 curToken 的 value 中
    ObjString *objString = newObjString(lexer->vm, (const char *)str.datas, str.count);
    lexer->curToken.value = OBJ_TO_VALUE(objString);
    ByteBufferClear(lexer->vm, &str);
}

// 跳过一行
static void skipAline(Lexer *lexer) {
    getNextChar(lexer);
    while (lexer->curChar != '\0') {
        if (lexer->curChar == '\n') {
            lexer->curToken.lineNo++;
            getNextChar(lexer);
            break;
        }
        getNextChar(lexer);
    }
}

// 跳过行注释和区块注释
static void skipComment(Lexer *lexer) {
    char nextChar = lookAheadChar(lexer);
    if (lexer->curChar == '/') {
        // 行注释
        skipAline(lexer);
    } else {
        // 区块注释
        // TODO: 该逻辑可能有 bug，当在注释中有 * 作为注释的内容时，
        // 不应该简单的跳出循环，然后判断下一个字符是不是 /
        // 后续完善
        while (nextChar != '*' && nextChar != '\0') {
            // 不停地读入注释中下一个字符
            getNextChar(lexer);

            // 如果注释有换行，则更新 lineNo
            // 主要是为了当某段代码报错时，能准确报出出错行数
            if (lexer->curChar == '\n') {
                lexer->curToken.lineNo++;
            }
            // 获取下一个字符，用作下一个循环的判断
            nextChar = lookAheadChar(lexer);
        }

        // 循环退出后，下一个字符要么是 * 要么是 \0
        // 如果下一个字符是 *，再判断下下个字符是不是 /
        // 如果是，则说明注释结束，读取下个字符
        if (matchNextChar(lexer, '*')) {
            if (!matchNextChar(lexer, '/')) {
                LEX_ERROR(lexer, "expect '/' after '*'!");
            }
            getNextChar(lexer);
        } else {
            // 如果下一个字符是 \0，则报错
            LEX_ERROR(lexer, "expect '*/' before comment end!");
        }
    }
    // 注释之后可能会有空白符
    skipBlanks(lexer);
}

// 获取 Token 方法
void getNextToken(Lexer *lexer) {
    // 将 curToken 备份到 preToken
    lexer->preToken = lexer->curToken;
    // 跳过待识别单词之前的空格
    skipBlanks(lexer);

    // 初始化 curToken

    // 默认为文件结束类型 TOKEN_EOF
    lexer->curToken.type = TOKEN_EOF;
    lexer->curToken.length = 0;
    // 将下一个字符的地址减 1 即是当前字符地址，也就是 curToken 开始地址
    lexer->curToken.start = lexer->nextCharPtr - 1;
    // 初始化 curToken 值为 VT_UNDEFINED 的 Value 结构形式
    lexer->curToken.value = VT_TO_VALUE(VT_UNDEFINED);

    // 因词法分析器并不采用有限状态机，所以并不需要循环
    // 之所以这里有循环，是因为如果解析碰到注释，没有循环的话，本次函数调用无法获得一个正常的 Token
    // 所以需要继续循环，直到碰到非注释的 Token
    while (lexer->curChar != '\0') {
        switch (lexer->curChar) {
            case ',':
                lexer->curToken.type = TOKEN_COMMA;
                break;
            case ':':
                lexer->curToken.type = TOKEN_COLON;
                break;
            case '(':
                // 如果 interpolationExpectRightParenNum 大于 0，说明遇到了内嵌表达式
                // 即以 % 开头的 %()
                // 因为不支持嵌套使用内嵌表达式
                // 则 interpolationExpectRightParenNum 加 1，用来后面判断内嵌表达式是否嵌套
                // 判断的具体代码在 lexString 中
                if (lexer->interpolationExpectRightParenNum > 0) {
                    lexer->interpolationExpectRightParenNum++;
                }
                lexer->curToken.type = TOKEN_LEFT_PAREN;
                break;
            case ')':
                if (lexer->interpolationExpectRightParenNum > 0) {
                    // 如果 interpolationExpectRightParenNum 大于 0
                    // 说明之前已经遇到了 %(，因此当 ) 出现后，说明已经完结
                    // interpolationExpectRightParenNum 减 1
                    lexer->interpolationExpectRightParenNum--;
                    // 如果 interpolationExpectRightParenNum 减 1 后为 0
                    // 说明没有嵌套
                    // 然后调用 lexString 词法分析内嵌表达式后面的字符串
                    // 例如 "ab%(4+6)cb" 中的 cb
                    // 因为内嵌表达式一定会在字符串中
                    if (lexer->interpolationExpectRightParenNum == 0) {
                        lexString(lexer);
                        break;
                    }
                }
                lexer->curToken.type = TOKEN_RIGHT_PAREN;
                break;
            case '[':
                lexer->curToken.type = TOKEN_LEFT_BRACKET;
                break;
            case ']':
                lexer->curToken.type = TOKEN_RIGHT_BRACKET;
                break;
            case '{':
                lexer->curToken.type = TOKEN_LEFT_BRACE;
                break;
            case '}':
                lexer->curToken.type = TOKEN_RIGHT_BRACE;
                break;
            case '.':
                // .. 用于便是范围，例如 1 2 3 4  可以用 1..4 表示
                lexer->curToken.type = matchNextChar(lexer, '.') ? TOKEN_DOT_DOT : TOKEN_DOT;
                break;
            case '=':
                lexer->curToken.type = matchNextChar(lexer, '=') ? TOKEN_EQUAL : TOKEN_ASSIGN;
                break;
            case '+':
                lexer->curToken.type = TOKEN_ADD;
                break;
            case '-':
                lexer->curToken.type = TOKEN_SUB;
                break;
            case '*':
                lexer->curToken.type = TOKEN_MUL;
                break;
            case '/':
                if (matchNextChar(lexer, '/') || matchNextChar(lexer, '*')) {
                    // 如果是注释，则调用跳过注释函数
                    skipComment(lexer);
                    // 跳过注释后，重置 curToken 的起始地址
                    lexer->curToken.start = lexer->nextCharPtr - 1;
                    // 开始下一个循环，不会执行下面的逻辑
                    // 直到匹配到非注释的正常 TOKEN
                    continue;
                } else {
                    lexer->curToken.type = TOKEN_DIV;
                }
                break;
            case '%':
                lexer->curToken.type = TOKEN_MOD;
                break;
            case '&':
                lexer->curToken.type = matchNextChar(lexer, '&') ? TOKEN_LOGIC_AND : TOKEN_BIT_AND;
                break;
            case '|':
                lexer->curToken.type = matchNextChar(lexer, '|') ? TOKEN_LOGIC_OR : TOKEN_BIT_OR;
                break;
            case '~':
                lexer->curToken.type = TOKEN_BIT_NOT;
                break;
            case '?':
                lexer->curToken.type = TOKEN_QUESTION;
                break;
            case '>':
                if (matchNextChar(lexer, '=')) {
                    lexer->curToken.type = TOKEN_GREAT_EQUAL;
                } else if (matchNextChar(lexer, '>')) {
                    lexer->curToken.type = TOKEN_BIT_SHIFT_RIGHT;
                } else {
                    lexer->curToken.type = TOKEN_GREAT;
                }
                break;
            case '<':
                if (matchNextChar(lexer, '=')) {
                    lexer->curToken.type = TOKEN_LESS_EQUAL;
                } else if (matchNextChar(lexer, '<')) {
                    lexer->curToken.type = TOKEN_BIT_SHIFT_LEFT;
                } else {
                    lexer->curToken.type = TOKEN_LESS;
                }
                break;
            case '!':
                lexer->curToken.type = matchNextChar(lexer, '=') ? TOKEN_NOT_EQUAL : TOKEN_LOGIC_NOT;
                break;
            case '"':
                // 遇到 " 字符，则调用解析字符串的函数 lexString
                lexString(lexer);
                break;
            default:
                // 如果首字符是字母或者 _ 则说明是变量名（包含保留的关键字）
                if (isalpha(lexer->curChar) || lexer->curChar == '_') {
                    lexId(lexer, TOKEN_UNKNOWN);
                } else if (isdigit(lexer->curChar)) {
                    lexNum(lexer);
                } else {
                    if (lexer->curChar == '#' && matchNextChar(lexer, '!')) {
                        skipAline(lexer);
                        lexer->curToken.start = lexer->nextCharPtr - 1; //重置下一个token起始地址
                        continue;
                    }
                    LEX_ERROR(lexer, "Not support char: \'%c\', quit.", lexer->curChar);
                }
                return;
        }

        // 计算当前 Token 的长度
        lexer->curToken.length = (uint32_t)(lexer->nextCharPtr - lexer->curToken.start);
        // 读进下一个字符
        getNextChar(lexer);
        return;
    }
}

// 如果当前 token 类型为期望类型，则读入下一个 token 并返回 true
// 否则直接返回 false
bool matchToken(Lexer *lexer, TokenType expectTokenType) {
    if (lexer->curToken.type == expectTokenType) {
        getNextToken(lexer);
        return true;
    }
    return false;
}

// 断言当前 token 类型为期望类型，并读取下一个 token，否则报错
void assertCurToken(Lexer *lexer, TokenType expectTokenType, const char *errMsg) {
    if (lexer->curToken.type != expectTokenType) {
        COMPILE_ERROR(lexer, errMsg);
    }
    getNextToken(lexer);
}

// 初始化词法分析器
void initLexer(VM *vm, Lexer *lexer, const char *file, const char *sourceCode, ObjModule *objModule) {
    // 由于 sourceCode 未必源自文件
    // 当源码是直接输入的，则 file 只是个字符串
    lexer->file = file;
    // sourceCode 本身就是源码串中首字符地址
    lexer->sourceCode = sourceCode;
    lexer->curChar = *lexer->sourceCode;
    lexer->nextCharPtr = lexer->sourceCode + 1;
    lexer->curToken.lineNo = 1;
    lexer->curToken.type = TOKEN_UNKNOWN;
    lexer->curToken.start = NULL;
    lexer->curToken.length = 0;
    lexer->preToken = lexer->curToken;
    lexer->interpolationExpectRightParenNum = 0;
    // 当前正在解析的模块
    lexer->vm = vm;
    lexer->curModule = objModule;
}

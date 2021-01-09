#include "unicodeUtf8.h"
#include "common.h"

// 前置知识
// UTF-8 是 unicode 的一种实现方式，其他实现还包括 UTF-16 UTF-32，不过在互联网上基本不用
// UTF-8 要求使用 1 ~ 4 个字节表示一个符号，根据不同符号变化字节长度
// 1. 对于单字节的符号，字节第一位设为 0（标记位），后面 7 位表示字符（数据位）（对于英文字母，UTF-8 和 ASCII 相同）
// 2. 对于 n 个字节的符号（n>1），第一个字节的前 n 位均设为 1，第 n+1 位设为 0，后面字节的前两位设为 0。其余位用来表示符号。
// 单字节：0xxxxxxx 其中数据位有 7 个，因此可表示的范围是 0 ~ 2^7，即 0x00 ~ 0x7f
// 两字节：110xxxxx 10xxxxxx 其中数据位有 11 个，因此可表示的范围是 2^7+1 ~ 2^11，即 0x80 ~ 0x7ff
// 三字节：1110xxxx 10xxxxxx 10xxxxxx 其中数据位有 16 个，因此可表示的范围是 2^11+1 ~ 2^16，即 0x800 ~ 0xffff
// 四字节：11110xxx 10xxxxxx 10xxxxxx 10xxxxxx 其中数据位有 21 个，因此可表示的范围是 2^16+1 ~ 2^21，即 0x10000 ~ 0x1fffff
// 注意：实际上四字节规定的范围是 0x10000 ~ 0x10ffff 是为了照顾 UTF16
// 对于多字节的第 1 个字节的标记位如此设计原因：1 的个数表示一共有几个字节，0 用来区分标记位的 1 和数据位的 1
// 对于多字节的低位字节的标记位如此设计原因：标记位 10 用来和 ASCII 码作区分，以及和第 1 个字节作区分（第 1 个字节不可能是 10 开头）

// 计算已经 UTF-8 编码的字符的 UTF-8 编码字节数，其中参数 byte 是 UTF-8 编码的高字节
// 原理：判断 UTF-8 高字节的标记位中 1 的个数，有几个 1 就表示有几个字节
uint32_t getByteNumOfEncodeUtf8(uint8_t byte) {
    if ((byte & 0xc0) == 0x80) {
        // byte 以 10 开头，说明是低字节
        return 0;
    } else if ((byte & 0xf8) == 0xf0) {
        // byte 以 11110 开头
        return 4;
    } else if ((byte & 0xf0) == 0xe0) {
        // byte 以 1110 开头
        return 3;
    } else if ((byte & 0xe0) == 0xc0) {
        // byte 以 110 开头
        return 2;
    } else {
        // 最后就是单字节 UTF-8，等价于 ASCII 码
        return 1;
    }
}

// 计算没有 UTF-8 编码的字符的 UTF-8 编码字节数
// 原理：根据 UTF-8 不同字节数的编码所能表示的范围和 value 进行比较
uint32_t getByteNumOfDecodeUtf8(int value) {
    ASSERT(value > 0, "Can't encode negative value!");

    if (value <= 0x7f) {
        return 1;
    } else if (value <= 0x7ff) {
        return 2;
    } else if (value <= 0xffff) {
        return 3;
    } else if (value <= 0x10ffff) {
        return 4;
    } else {
        return 0; // 超出范围返回 0
    }
}

// 将 value 编码成 UTF-8 后，写入缓冲区 buf，并返回写入的字节数
// 编码：就是按照 UTF-8 规则将数据装入 UTF-8 的字节中
// 例如 2 字节的 UTF-8 编码形式为 "110xxxxx 10xxxxxx"，只需要将数据装入 x 所在的位即可
// 本函数按照大端字节序编码，即先写数据的高字节，再写低字节
uint8_t encodeUtf8(uint8_t *buf, int value) {
    ASSERT(value > 0, "Can't encode negative value!");

    if (value <= 0x7f) {
        // 若单字节，则用 7 位 1 做位与运算
        *buf = value & 0x7f;
        return 1;
    } else if (value <= 0x7ff) {
        *buf++ = 0xc0 | ((value & 0x7c0) >> 6);
        *buf = 0x80 | (value & 0x3f);
        return 2;
    } else if (value <= 0xffff) {
        *buf++ = 0xe0 | ((value & 0xf000) >> 12);
        *buf++ = 0x80 | ((value & 0xfc0) >> 6);
        *buf = 0x80 | (value & 0x3f);
        return 3;
    } else if (value <= 0x10ffff) {
        *buf++ = 0xf0 | ((value & 0x1c0000) >> 18);
        *buf++ = 0x80 | ((value & 0x3f000) >> 12);
        *buf++ = 0x80 | ((value & 0xfc0) >> 6);
        *buf = 0x80 | (value & 0x3f);
        return 4;
    } else {
        NOT_REACHED();
        return 0; // 超出范围返回 0
    }
}

// 将 UTF-8 编码的字符序列解码成 value
// 其中参数 bytePtr 是一个指针，指向 UTF-8 编码的高字节或者 ASCII 码（单字节情况）
// 参数 length 位序列的最大长度
// 解码就是编码逆过程，判断是多少字节的编码，然后将其中数据部分读取出来
// 例如 2 字节：就是将 "110xxxxx 10xxxxxx" 中 x 读出来
int decodeUtf8(const uint8_t *bytePtr, uint32_t length) {
    if (*bytePtr <= 0x7f) {
        // 小于 127，说明是单字节的 ASCII 码，直接返回
        return *bytePtr;
    }

    int value;
    uint32_t byteNum;

    // 先读取高字节，根据高字节的前 n 位判断相应字节数的 UTF-8 编码
    // 从而得到一共有几个字节，以及高字节的数据位放进 value 中
    if ((*bytePtr & 0xe0) == 0xc0) {
        // 若是 2 字节的 UTF-8 (110 开头)
        value = *bytePtr & 0x1f;
        byteNum = 2;
    } else if ((*bytePtr & 0xf0) == 0xe0) {
        // 若是 3 字节的 UTF-8 (1110 开头)
        value = *bytePtr & 0x0f;
        byteNum = 3;
    } else if ((*bytePtr & 0xf8) == 0xf0) {
        // 若是 4 字节的 UTF-8 (11110 开头)
        value = *bytePtr & 0x07;
        byteNum = 4;
    } else {
        // 非法编码
        return -1;
    }

    // 如果 UTF-8 被截断了就不再读过去了
    if (byteNum > length) {
        return -1;
    }

    // 再读取低字节中的数据位
    while (byteNum - 1 > 0) {
        bytePtr++;
        byteNum--;
        // 低字节的前 2 位必须是 10
        if ((*bytePtr & 0xc0) != 0x80) {
            return -1;
        }

        // 从次高字节往低字节读下去,不断累加各个低字节的低6位
        value = value << 6 | (*bytePtr & 0x3f);
    }
    return value;
}

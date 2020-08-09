#ifndef _INCLUDE_UTF8_H
#define _INCLUDE_UTF8_H
#include <stdint.h>

// 计算已经 UTF-8 编码的字符的 UTF-8 编码字节数，其中参数 byte 是 UTF-8 编码的高字节
uint32_t getByteNumOfEncodeUtf8(uint8_t byte);

// 计算没有 UTF-8 编码的字符的 UTF-8 编码字节数
uint32_t getByteNumOfDecodeUtf8(int value);

// 将 value 编码成 UTF-8 后，写入缓冲区 buf，并返回写入的字节数
uint8_t encodeUtf8(uint8_t *buf, int value);

// 将 UTF-8 编码的字符序列解码成 value
int decodeUtf8(const uint8_t *bytePtr, uint32_t length);

#endif

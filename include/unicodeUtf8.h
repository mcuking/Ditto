#ifndef _INCLUDE_UTF8_H
#define _INCLUDE_UTF8_H
#include <stdint.h>

// 返回 value 按照 UTF-8 编码后的字节数
uint32_t getByteNumOfEncodeUtf8(int value);

// 返回 UTF-8 解码的字节数，参数 byte 是 UTF-8 编码的高字节
uint32_t getByteNumOfDecodeUtf8(uint8_t byte);

// 将 value 编码成 UTF-8 后，写入缓冲区 buf，并返回写入的字节数
uint8_t encodeUtf8(uint8_t *buf, int value);

// 将 UTF-8 编码的字符序列解码成 value
int decodeUtf8(const uint8_t *bytePtr, uint32_t length);

#endif

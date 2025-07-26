#include "base64.h"
#include <string.h>
#include <ctype.h>

// Base64编码表（标准RFC 4648）
static const char base64_encoding_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
"abcdefghijklmnopqrstuvwxyz"
"0123456789+/";

// Base64解码表（反向映射，-1表示无效字符）
static const int8_t base64_decoding_table[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

size_t base64_encode_len(size_t input_len) {
    // 每3字节输入对应4字节输出，不足3字节补'='，最后加终止符
    return (input_len + 2) / 3 * 4 + 1;
}

int base64_encode(const uint8_t* input, size_t input_len, char* output) {
    if (input == NULL || output == NULL) {
        return -1;
    }

    size_t i = 0;  // 输入数据索引
    size_t j = 0;  // 输出字符串索引
    uint32_t triple;  // 临时存储3字节输入

    while (i < input_len) {
        // 读取3字节（不足则补0）
        triple = 0;
        for (int k = 0; k < 3; k++) {
            if (i < input_len) {
                triple |= (uint32_t)input[i] << (16 - 8 * k);
                i++;
            }
        }

        // 拆分4个6位组，映射到编码表
        output[j++] = base64_encoding_table[(triple >> 18) & 0x3F];
        output[j++] = base64_encoding_table[(triple >> 12) & 0x3F];

        // 根据输入长度决定是否补'='
        if (i > input_len + 1) {
            output[j++] = base64_encoding_table[(triple >> 6) & 0x3F];
        }
        else {
            output[j++] = '=';  // 输入不足2字节，补一个'='
        }

        if (i > input_len) {
            output[j++] = base64_encoding_table[triple & 0x3F];
        }
        else {
            output[j++] = '=';  // 输入不足3字节，补一个'='
        }
    }

    output[j] = '\0';  // 终止符
    return 0;
}

size_t base64_decode_max_len(size_t input_len) {
    // 每4字节Base64最多解码3字节，移除'='后计算
    return input_len / 4 * 3;
}

int base64_decode(const char* input, size_t input_len, uint8_t* output, size_t* output_len) {
    if (input == NULL || output == NULL || output_len == NULL) {
        return -1;
    }

    // 检查输入长度是否为4的倍数（Base64标准要求）
    if (input_len % 4 != 0) {
        return -1;
    }

    size_t i = 0;  // 输入字符串索引
    size_t j = 0;  // 输出数据索引
    uint32_t triple;  // 临时存储4个6位组

    while (i < input_len) {
        // 读取4个字符，跳过空格（兼容可能的空白字符）
        uint8_t quartet[4] = { 0 };
        int valid_count = 0;
        for (int k = 0; k < 4; k++) {
            // 跳过空白字符（空格、制表符等）
            while (i < input_len && isspace((unsigned char)input[i])) {
                i++;
            }
            if (i >= input_len) {
                return -1;  // 输入不完整
            }

            char c = input[i];
            i++;

            if (c == '=') {
                quartet[k] = 0;  // '='用0填充，后续忽略
            }
            else {
                int8_t val = base64_decoding_table[(unsigned char)c];
                if (val == -1) {
                    return -1;  // 无效字符
                }
                quartet[k] = (uint8_t)val;
                valid_count++;
            }
        }

        // 合并4个6位组为3字节
        triple = (uint32_t)quartet[0] << 18 |
            (uint32_t)quartet[1] << 12 |
            (uint32_t)quartet[2] << 6 |
            (uint32_t)quartet[3];

        // 提取3字节输出（根据有效字符数决定提取多少）
        if (valid_count >= 2) {
            output[j++] = (uint8_t)(triple >> 16);
        }
        if (valid_count >= 3) {
            output[j++] = (uint8_t)(triple >> 8);
        }
        if (valid_count >= 4) {
            output[j++] = (uint8_t)triple;
        }
    }

    *output_len = j;
    return 0;
}
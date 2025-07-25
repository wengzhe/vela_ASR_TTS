#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief 计算Base64编码后的长度
 * @param input_len 输入数据的字节长度
 * @return 编码后的字符串长度（包含终止符'\0'）
 */
size_t base64_encode_len(size_t input_len);

/**
 * @brief 对二进制数据进行Base64编码
 * @param input 输入的二进制数据
 * @param input_len 输入数据的字节长度
 * @param output 输出的Base64编码字符串（需预先分配内存，长度至少为base64_encode_len返回值）
 * @return 成功返回0，失败返回-1
 */
int base64_encode(const uint8_t* input, size_t input_len, char* output);

/**
 * @brief 计算Base64解码后的最大可能长度
 * @param input_len 输入Base64字符串的长度（不含终止符）
 * @return 解码后的最大字节长度
 */
size_t base64_decode_max_len(size_t input_len);

/**
 * @brief 对Base64编码字符串进行解码
 * @param input 输入的Base64编码字符串
 * @param input_len 输入字符串的长度（不含终止符）
 * @param output 输出的二进制数据（需预先分配内存，长度至少为base64_decode_max_len返回值）
 * @param output_len 实际解码后的字节长度（输出参数）
 * @return 成功返回0，失败返回-1（如输入包含无效字符）
 */
int base64_decode(const char* input, size_t input_len, uint8_t* output, size_t* output_len);

#endif // BASE64_H
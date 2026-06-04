#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "tinykv/buffer.h"

namespace tinykv {

// 一条已解析的命令：argv[0] 是命令名，其余为参数。
// 用 std::string 存储以保证二进制安全（可含 '\0'）。
using RespCommand = std::vector<std::string>;

// 单条命令的最大参数个数，防止恶意构造超长数组。
constexpr int kRespMaxArgc = 16;

enum class RespParseResult {
    Ok = 1,        // 解析出一条完整命令并消费输入
    Incomplete = 0, // 数据不够（半包），不消费
    Error = -1,    // 协议错误
};

// 从 in 中尝试解析一条 RESP Array-of-BulkStrings 命令。
// 成功时填充 out 并从 in 消费掉对应字节。
RespParseResult resp_try_parse(Buffer& in, RespCommand& out);

// ---- 回包构造：写入 out buffer，成功返回 true ----

bool resp_reply_simple(Buffer& out, const std::string& s);   // +s\r\n
bool resp_reply_error(Buffer& out, const std::string& msg);  // -ERR msg\r\n
bool resp_reply_integer(Buffer& out, long long v);           // :v\r\n
bool resp_reply_bulk(Buffer& out, const void* data, std::size_t len);  // $len\r\n data \r\n
bool resp_reply_bulk(Buffer& out, const std::string& s);
bool resp_reply_nil(Buffer& out);                            // $-1\r\n
bool resp_reply_array_header(Buffer& out, long long n);      // *n\r\n

}  // namespace tinykv

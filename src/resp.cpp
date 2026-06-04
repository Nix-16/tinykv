#include "tinykv/resp.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tinykv {

namespace {

// 在 [p, p+n) 中找 "\r\n"，返回偏移，找不到返回 -1。
long find_crlf(const char* p, std::size_t n) {
    if (!p || n < 2) {
        return -1;
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (p[i] == '\r' && p[i + 1] == '\n') {
            return static_cast<long>(i);
        }
    }
    return -1;
}

// 解析十进制整数。要求整段都是合法数字。
bool parse_long(const char* s, std::size_t len, long* out) {
    if (!s || !out || len == 0 || len >= 64) {
        return false;
    }
    char tmp[64];
    std::memcpy(tmp, s, len);
    tmp[len] = '\0';

    char* end = nullptr;
    long v = std::strtol(tmp, &end, 10);
    if (end == tmp || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

}  // namespace

RespParseResult resp_try_parse(Buffer& in, RespCommand& out) {
    out.clear();

    std::size_t nread = in.readable_bytes();
    if (nread == 0) {
        return RespParseResult::Incomplete;
    }

    const char* p = in.peek();

    // 必须以 '*' 开头（暂不支持 inline command）。
    if (p[0] != '*') {
        return RespParseResult::Error;
    }

    long pos = find_crlf(p, nread);
    if (pos < 0) {
        return RespParseResult::Incomplete;
    }
    if (pos < 2) {
        return RespParseResult::Error;
    }

    long argc = 0;
    if (!parse_long(p + 1, static_cast<std::size_t>(pos) - 1, &argc)) {
        return RespParseResult::Error;
    }
    if (argc <= 0 || argc > kRespMaxArgc) {
        return RespParseResult::Error;
    }

    // 第一遍：扫描判断是否够一整条命令。
    std::size_t off = static_cast<std::size_t>(pos) + 2;
    for (long i = 0; i < argc; ++i) {
        if (off >= nread) {
            return RespParseResult::Incomplete;
        }
        if (p[off] != '$') {
            return RespParseResult::Error;
        }

        long pos2 = find_crlf(p + off, nread - off);
        if (pos2 < 0) {
            return RespParseResult::Incomplete;
        }
        if (pos2 < 2) {
            return RespParseResult::Error;
        }

        long blen = 0;
        if (!parse_long(p + off + 1, static_cast<std::size_t>(pos2) - 1, &blen)) {
            return RespParseResult::Error;
        }
        if (blen < 0) {
            return RespParseResult::Error;
        }

        off += static_cast<std::size_t>(pos2) + 2;

        std::size_t need = static_cast<std::size_t>(blen) + 2;
        if (nread - off < need) {
            return RespParseResult::Incomplete;
        }
        if (p[off + blen] != '\r' || p[off + blen + 1] != '\n') {
            return RespParseResult::Error;
        }
        off += need;
    }

    // 第二遍：真正提取 argv。
    out.reserve(static_cast<std::size_t>(argc));
    std::size_t off2 = static_cast<std::size_t>(pos) + 2;
    for (long i = 0; i < argc; ++i) {
        long pos3 = find_crlf(p + off2, nread - off2);
        long blen = 0;
        parse_long(p + off2 + 1, static_cast<std::size_t>(pos3) - 1, &blen);
        off2 += static_cast<std::size_t>(pos3) + 2;

        out.emplace_back(p + off2, static_cast<std::size_t>(blen));
        off2 += static_cast<std::size_t>(blen) + 2;
    }

    in.retrieve(off2);
    return RespParseResult::Ok;
}

bool resp_reply_simple(Buffer& out, const std::string& s) {
    return out.append("+", 1) && out.append(s) && out.append("\r\n", 2);
}

bool resp_reply_error(Buffer& out, const std::string& msg) {
    return out.append("-", 1) && out.append("ERR ", 4) && out.append(msg) &&
           out.append("\r\n", 2);
}

bool resp_reply_integer(Buffer& out, long long v) {
    char tmp[64];
    int n = std::snprintf(tmp, sizeof(tmp), ":%lld\r\n", v);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(tmp)) {
        return false;
    }
    return out.append(tmp, static_cast<std::size_t>(n));
}

bool resp_reply_bulk(Buffer& out, const void* data, std::size_t len) {
    if (!data && len != 0) {
        return false;
    }
    char head[64];
    int n = std::snprintf(head, sizeof(head), "$%zu\r\n", len);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(head)) {
        return false;
    }
    if (!out.append(head, static_cast<std::size_t>(n))) {
        return false;
    }
    if (len > 0 && !out.append(data, len)) {
        return false;
    }
    return out.append("\r\n", 2);
}

bool resp_reply_bulk(Buffer& out, const std::string& s) {
    return resp_reply_bulk(out, s.data(), s.size());
}

bool resp_reply_nil(Buffer& out) {
    return out.append("$-1\r\n", 5);
}

bool resp_reply_array_header(Buffer& out, long long n) {
    char tmp[64];
    int k = std::snprintf(tmp, sizeof(tmp), "*%lld\r\n", n);
    if (k <= 0 || static_cast<std::size_t>(k) >= sizeof(tmp)) {
        return false;
    }
    return out.append(tmp, static_cast<std::size_t>(k));
}

}  // namespace tinykv

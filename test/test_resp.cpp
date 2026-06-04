#include "tinykv/resp.h"

#include <cstring>
#include <string>

#include "test_util.h"

using namespace tinykv;

static void feed(Buffer& b, const std::string& s) { CHECK(b.append(s)); }

static void test_parse_basic() {
    Buffer in;
    feed(in, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");

    RespCommand cmd;
    CHECK(resp_try_parse(in, cmd) == RespParseResult::Ok);
    CHECK(cmd.size() == 3);
    CHECK(cmd[0] == "SET");
    CHECK(cmd[1] == "k");
    CHECK(cmd[2] == "v");
    CHECK(in.readable_bytes() == 0);
}

static void test_parse_incomplete() {
    Buffer in;
    feed(in, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n");  // 少最后一个参数
    RespCommand cmd;
    CHECK(resp_try_parse(in, cmd) == RespParseResult::Incomplete);
    // 不消费。
    CHECK(in.readable_bytes() > 0);

    // 补齐后应能解析。
    feed(in, "$1\r\nv\r\n");
    CHECK(resp_try_parse(in, cmd) == RespParseResult::Ok);
    CHECK(cmd.size() == 3 && cmd[2] == "v");
}

static void test_parse_multiple() {
    Buffer in;
    feed(in, "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");

    RespCommand cmd;
    CHECK(resp_try_parse(in, cmd) == RespParseResult::Ok);
    CHECK(cmd.size() == 1 && cmd[0] == "PING");

    CHECK(resp_try_parse(in, cmd) == RespParseResult::Ok);
    CHECK(cmd.size() == 2 && cmd[0] == "GET" && cmd[1] == "k");

    CHECK(resp_try_parse(in, cmd) == RespParseResult::Incomplete);
}

static void test_parse_error() {
    Buffer in;
    feed(in, "GET k\r\n");  // inline 命令不支持
    RespCommand cmd;
    CHECK(resp_try_parse(in, cmd) == RespParseResult::Error);
}

static void test_parse_binary_safe() {
    // value 含 '\0'
    std::string payload = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\n";
    payload.append("a\0b", 3);
    payload += "\r\n";

    Buffer in;
    feed(in, payload);
    RespCommand cmd;
    CHECK(resp_try_parse(in, cmd) == RespParseResult::Ok);
    CHECK(cmd[2].size() == 3);
    CHECK(cmd[2] == std::string("a\0b", 3));
}

static void test_reply_builders() {
    Buffer out;
    CHECK(resp_reply_simple(out, "OK"));
    CHECK(resp_reply_error(out, "bad"));
    CHECK(resp_reply_integer(out, 42));
    CHECK(resp_reply_bulk(out, std::string("hi")));
    CHECK(resp_reply_nil(out));

    std::string s(out.peek(), out.readable_bytes());
    CHECK(s == "+OK\r\n-ERR bad\r\n:42\r\n$2\r\nhi\r\n$-1\r\n");
}

int main() {
    test_parse_basic();
    test_parse_incomplete();
    test_parse_multiple();
    test_parse_error();
    test_parse_binary_safe();
    test_reply_builders();
    std::puts("ALL resp tests PASSED");
    return 0;
}

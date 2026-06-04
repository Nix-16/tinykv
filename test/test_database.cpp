// Database 级别：直接驱动命令分发，校验 RESP 回复与三命名空间隔离。

#include "tinykv/buffer.h"
#include "tinykv/config.h"
#include "tinykv/database.h"

#include <cstdio>
#include <string>

#include "test_util.h"

using namespace tinykv;

static std::string run(Database& db, const RespCommand& cmd) {
    Buffer out;
    CHECK(db.execute(cmd, out));
    return std::string(out.peek(), out.readable_bytes());
}

int main() {
    Config cfg;
    cfg.appendonly = false;       // 关闭持久化，纯内存测试
    cfg.snapshot_enabled = false;
    Database db(cfg);

    // PING
    CHECK(run(db, {"PING"}) == "+PONG\r\n");
    CHECK(run(db, {"PING", "hi"}) == "$2\r\nhi\r\n");

    // 默认命名空间（数组）。
    CHECK(run(db, {"SET", "k", "v"}) == "+OK\r\n");
    CHECK(run(db, {"GET", "k"}) == "$1\r\nv\r\n");
    CHECK(run(db, {"EXISTS", "k"}) == ":1\r\n");
    CHECK(run(db, {"GET", "missing"}) == "$-1\r\n");

    // 三套命名空间相互隔离：SET 写入数组，HGET/RGET 取不到。
    CHECK(run(db, {"HGET", "k"}) == "$-1\r\n");
    CHECK(run(db, {"RGET", "k"}) == "$-1\r\n");

    CHECK(run(db, {"HSET", "k", "hv"}) == "+OK\r\n");
    CHECK(run(db, {"HGET", "k"}) == "$2\r\nhv\r\n");
    CHECK(run(db, {"GET", "k"}) == "$1\r\nv\r\n");  // 数组里仍是 v

    CHECK(run(db, {"RSET", "k", "rv"}) == "+OK\r\n");
    CHECK(run(db, {"RGET", "k"}) == "$2\r\nrv\r\n");

    // DEL 返回删除条数。
    CHECK(run(db, {"DEL", "k"}) == ":1\r\n");
    CHECK(run(db, {"DEL", "k"}) == ":0\r\n");
    CHECK(run(db, {"EXISTS", "k"}) == ":0\r\n");
    // Hash 命名空间里的 k 不受影响。
    CHECK(run(db, {"HEXISTS", "k"}) == ":1\r\n");

    // 参数个数错误。
    std::string err = run(db, {"SET", "only-key"});
    CHECK(err.rfind("-ERR", 0) == 0);

    // 未知命令。
    CHECK(run(db, {"NOPE"}).rfind("-ERR", 0) == 0);

    std::puts("ALL database tests PASSED");
    return 0;
}

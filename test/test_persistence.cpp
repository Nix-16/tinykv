// 持久化往返测试：快照 save/load 与 AOF append/load。
// 在临时目录下进行，结束清理。

#include "tinykv/aof.h"
#include "tinykv/snapshot.h"
#include "tinykv/store.h"

#include <cstdio>
#include <string>
#include <vector>

#include "test_util.h"

using namespace tinykv;

static void test_snapshot_roundtrip() {
    const std::string path = "test_snap.kvs";
    std::remove(path.c_str());

    // 写入三套命名空间。
    {
        ArrayStore a;
        HashStore h;
        RBTreeStore r;
        a.set("ak", "av");
        a.set("ak2", std::string("a\0v", 3));  // 二进制安全
        h.set("hk", "hv");
        r.set("rk", "rv");
        r.set("rk2", "rv2");

        Snapshot snap(a, h, r);
        CHECK(snap.save(path) == 0);
    }

    // 读回到新实例并校验。
    {
        ArrayStore a;
        HashStore h;
        RBTreeStore r;
        Snapshot snap(a, h, r);
        CHECK(snap.load(path) == 0);

        CHECK(a.count() == 2);
        CHECK(a.get("ak").value() == "av");
        CHECK(a.get("ak2").value() == std::string("a\0v", 3));
        CHECK(h.get("hk").value() == "hv");
        CHECK(r.count() == 2);
        CHECK(r.get("rk").value() == "rv");
        CHECK(r.get("rk2").value() == "rv2");
    }

    // 文件不存在 -> 返回 1。
    {
        ArrayStore a;
        HashStore h;
        RBTreeStore r;
        Snapshot snap(a, h, r);
        CHECK(snap.load("definitely_missing.kvs") == 1);
    }

    std::remove(path.c_str());
}

static void test_aof_roundtrip() {
    const std::string path = "test_append.aof";
    std::remove(path.c_str());

    // 追加若干写命令。
    {
        Aof aof;
        CHECK(aof.init(path, /*enabled=*/true, AofFsync::No));
        CHECK(aof.append_command({"SET", "k", "v"}));
        CHECK(aof.append_command({"HSET", "hk", "hv"}));
        CHECK(aof.append_command({"DEL", "k"}));
        aof.close();
    }

    // 回放并收集命令序列。
    {
        Aof aof;
        CHECK(aof.init(path, true, AofFsync::No));
        std::vector<RespCommand> replayed;
        CHECK(aof.load([&](const RespCommand& cmd) {
            replayed.push_back(cmd);
            return true;
        }));
        CHECK(replayed.size() == 3);
        CHECK(replayed[0] == RespCommand({"SET", "k", "v"}));
        CHECK(replayed[1] == RespCommand({"HSET", "hk", "hv"}));
        CHECK(replayed[2] == RespCommand({"DEL", "k"}));
        aof.close();
    }

    // reset 后回放为空。
    {
        Aof aof;
        CHECK(aof.init(path, true, AofFsync::No));
        CHECK(aof.reset());
        std::vector<RespCommand> replayed;
        CHECK(aof.load([&](const RespCommand& cmd) {
            replayed.push_back(cmd);
            return true;
        }));
        CHECK(replayed.empty());
        aof.close();
    }

    std::remove(path.c_str());
}

static void test_aof_disabled_is_noop() {
    Aof aof;
    CHECK(aof.init("unused.aof", /*enabled=*/false, AofFsync::No));
    CHECK(aof.append_command({"SET", "k", "v"}));  // no-op，成功
    CHECK(!aof.is_loading());
    aof.close();
}

int main() {
    test_snapshot_roundtrip();
    test_aof_roundtrip();
    test_aof_disabled_is_noop();
    std::puts("ALL persistence tests PASSED");
    return 0;
}

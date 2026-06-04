#pragma once

#include <string>

#include "tinykv/aof.h"
#include "tinykv/buffer.h"
#include "tinykv/config.h"
#include "tinykv/resp.h"
#include "tinykv/snapshot.h"
#include "tinykv/store.h"

namespace tinykv {

// 数据库核心：持有三套命名空间与持久化组件，负责命令分发与执行。
//
// 与网络层解耦——execute() 接收一条已解析命令，把回复写入 out buffer。
// 这样命令逻辑既可被 Reactor 调用，也可被 AOF 回放与单元测试直接驱动。
class Database {
public:
    explicit Database(const Config& config);

    // 启动期恢复：先加载快照，再回放 AOF。返回 true 成功。
    bool load_persistence();

    // 执行一条命令，回复写入 out。返回 false 表示发生了应写盘失败等致命错误
    // （调用方应关闭连接）；命令级错误（参数错误/未知命令）通过 RESP 错误回复
    // 表达，仍返回 true。
    bool execute(const RespCommand& cmd, Buffer& out);

    // 优雅退出时调用：若启用快照则做一次 SAVE 并重置 AOF，最后关闭 AOF。
    void shutdown();

    Aof& aof() { return aof_; }

private:
    // 写命令应用到 store 后，按需追加 AOF。仅在非回放阶段追加。
    bool maybe_append_aof(const RespCommand& cmd);

    // AOF 回放回调：把一条命令应用到内存（不再触发追加）。
    bool apply_from_aof(const RespCommand& cmd);

    const Config& config_;

    ArrayStore array_;
    HashStore hash_;
    RBTreeStore rbtree_;

    Aof aof_;
    Snapshot snapshot_;
};

}  // namespace tinykv

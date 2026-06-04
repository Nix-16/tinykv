#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "tinykv/config.h"
#include "tinykv/resp.h"

namespace tinykv {

// Append Only File：每条写命令以 RESP 数组追加，格式与 C 版/Redis 一致。
//
// fsync 策略：
//   Always   每次追加后立即 fsync
//   Everysec 由事件循环周期性调用 maybe_fsync()，每秒一次
//   No       交给内核
//
// 回放期间 append 被自动抑制（is_loading），避免重复写入与递归。
class Aof {
public:
    // 应用一条回放命令的回调；返回 false 表示该命令应用失败，回放中止。
    using ApplyFn = std::function<bool(const RespCommand&)>;

    Aof() = default;
    ~Aof();

    Aof(const Aof&) = delete;
    Aof& operator=(const Aof&) = delete;

    // 初始化。enabled=false 时所有操作变为 no-op。成功返回 true。
    bool init(const std::string& filename, bool enabled, AofFsync policy);

    // 关闭：强制 fsync 再 close，保证不丢数据。可重复调用。
    void close();

    bool enabled() const { return enabled_; }
    bool is_loading() const { return loading_; }

    // 追加一条写命令（已是 argv 形式）。回放中或未启用时为 no-op。
    // 返回 false 表示写盘失败。
    bool append_command(const std::vector<std::string>& argv);

    // 读取整个 AOF 并逐条回放，对每条命令调用 apply。
    // 回放期间 is_loading() 为 true。成功返回 true。
    bool load(const ApplyFn& apply);

    // everysec 策略下由事件循环周期调用。返回 false 表示 fsync 失败。
    bool maybe_fsync();

    // 截断重置为一个空的 AOF 基线（SAVE 后调用）。成功返回 true。
    bool reset();

private:
    bool enabled_ = false;
    int fd_ = -1;
    AofFsync policy_ = AofFsync::No;
    std::string filename_;
    bool loading_ = false;
    long long last_fsync_ms_ = 0;
};

}  // namespace tinykv

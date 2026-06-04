#pragma once

#include <cstdint>
#include <string>

#include "tinykv/allocator.h"

namespace tinykv {

enum class NetworkType {
    Reactor = 0,
    Proactor,  // 预留，未实现
    Ntyco,     // 预留，未实现
};

enum class AofFsync {
    No = 0,
    Always,
    Everysec,
};

// 运行配置，对应 kvs.conf。字段命名与 C 版保持一致，确保旧配置文件可直接复用。
struct Config {
    std::string bind_ip = "127.0.0.1";
    int port = 6380;

    AllocatorType allocator = AllocatorType::System;
    NetworkType network = NetworkType::Reactor;

    bool appendonly = false;
    std::string appendfilename = "appendonly.aof";
    AofFsync appendfsync = AofFsync::Always;

    bool snapshot_enabled = false;
    std::string snapshot_file = "dump.kvs";

    // 从配置文件加载。返回 true 成功，false 表示打开失败或某项取值非法。
    // 失败时 error 会写入可读的原因（含行号）。
    bool load_file(const std::string& path, std::string* error = nullptr);
};

}  // namespace tinykv

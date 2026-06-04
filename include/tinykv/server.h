#pragma once

#include "tinykv/config.h"
#include "tinykv/database.h"
#include "tinykv/reactor.h"

namespace tinykv {

// 服务端：组合 Config / Database / Reactor，提供启动与优雅退出。
//
// 信号处理通过一个进程级指针把 SIGINT/SIGTERM 转为 reactor.stop()。
class Server {
public:
    explicit Server(const Config& config);
    ~Server();

    // 启动：恢复持久化数据 -> 监听 -> 安装信号 -> 进入事件循环（阻塞）。
    // 返回进程退出码（0 成功）。
    int run();

private:
    // 处理某连接 in 中累积的所有完整命令。
    bool on_message(Connection& c);

    Config config_;
    Database db_;
    Reactor reactor_;
};

}  // namespace tinykv

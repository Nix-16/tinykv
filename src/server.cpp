#include "tinykv/server.h"

#include <csignal>
#include <cstdio>

namespace tinykv {

namespace {
// 信号处理只能触及 async-signal-safe 状态，这里仅翻转一个标志位，
// 由事件循环在下一轮观察到并停止。
Reactor* g_reactor = nullptr;

void on_signal(int) {
    if (g_reactor) {
        g_reactor->stop();
    }
}
}  // namespace

Server::Server(const Config& config) : config_(config), db_(config_) {}

Server::~Server() {
    if (g_reactor == &reactor_) {
        g_reactor = nullptr;
    }
}

bool Server::on_message(Connection& c) {
    // 解析并执行尽可能多的完整命令。
    for (;;) {
        RespCommand cmd;
        RespParseResult prc = resp_try_parse(c.in(), cmd);
        if (prc == RespParseResult::Incomplete) {
            return true;  // 半包，等待更多数据。
        }
        if (prc == RespParseResult::Error) {
            return false;  // 协议错误，关闭连接。
        }
        if (!db_.execute(cmd, c.out())) {
            return false;  // 致命错误（如 AOF 写盘失败）。
        }
    }
}

int Server::run() {
    if (!db_.load_persistence()) {
        return 1;
    }

    reactor_.set_on_message([this](Connection& c) { return on_message(c); });
    reactor_.set_on_tick([this]() {
        if (!db_.aof().maybe_fsync()) {
            std::fprintf(stderr, "aof maybe_fsync failed\n");
        }
    });

    uint16_t port = static_cast<uint16_t>(config_.port);
    if (reactor_.listen(config_.bind_ip, port) != 0) {
        std::fprintf(stderr, "listen failed on %s:%u\n", config_.bind_ip.c_str(), port);
        return 1;
    }

    g_reactor = &reactor_;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);  // 写已关闭连接不应杀进程。

    std::printf("tinykv_server listening on %s:%u\n", config_.bind_ip.c_str(), port);
    std::fflush(stdout);

    reactor_.run();

    // 优雅退出：落盘收尾。
    db_.shutdown();
    return 0;
}

}  // namespace tinykv

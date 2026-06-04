#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "tinykv/connection.h"

struct epoll_event;

namespace tinykv {

// 单线程 Reactor：epoll + 非阻塞 IO，单监听端口。
//
// 用法：
//   Reactor r;
//   r.set_on_message(...);            // 解析并处理 in 中的命令
//   r.set_on_tick(...);               // 每轮事件循环末尾调用（如 AOF everysec）
//   r.listen(ip, port);
//   r.run();                          // 阻塞，直到 stop()
class Reactor {
public:
    // 处理某连接 in 缓冲区中的数据；返回 false 表示出错，连接将被关闭。
    using OnMessage = std::function<bool(Connection&)>;
    // 每轮事件循环末尾回调。
    using OnTick = std::function<void()>;

    explicit Reactor(int max_events = 1024);
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    void set_on_message(OnMessage cb) { on_message_ = std::move(cb); }
    void set_on_tick(OnTick cb) { on_tick_ = std::move(cb); }

    // 监听端口。返回 0 成功，-1 失败，-2 已监听过。
    int listen(const std::string& ip, uint16_t port, int backlog = 128);

    int run();
    void stop() { running_ = false; }

    // 业务层在 on_message 内写完 out 后，事件循环会自动开启 EPOLLOUT；
    // 此方法供需要主动关闭某连接时使用。
    void close_connection(Connection* c);

private:
    int epoll_update(int fd, uint32_t events);
    void handle_accept();
    bool handle_read(Connection* c);
    int  flush_out(Connection* c);  // 0 ok, -1 error
    bool ensure_cap(int fd);
    Connection* conn_of(int fd);

    int epfd_ = -1;
    int listen_fd_ = -1;
    bool running_ = false;

    int max_events_;
    std::vector<epoll_event> evlist_;

    // fd -> Connection 映射，按 fd 直接索引。
    std::vector<std::unique_ptr<Connection>> conns_;

    OnMessage on_message_;
    OnTick on_tick_;
};

}  // namespace tinykv

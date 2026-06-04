#pragma once

#include <cstdint>

#include "tinykv/buffer.h"

namespace tinykv {

// 一个客户端连接：持有 fd 与收发缓冲区。
//
// in  : 读事件把数据追加到这里，业务层从中解析命令。
// out : 业务层把响应写到这里，写事件负责发送。
// 生命周期由 Reactor 管理（unique_ptr）。
class Connection {
public:
    explicit Connection(int fd) : fd_(fd) {}

    int fd() const { return fd_; }
    Buffer& in() { return in_; }
    Buffer& out() { return out_; }

    uint32_t events() const { return events_; }
    void set_events(uint32_t ev) { events_ = ev; }

    bool closed() const { return closed_; }
    void mark_closed() { closed_ = true; }

private:
    int fd_;
    Buffer in_;
    Buffer out_;
    uint32_t events_ = 0;  // 当前关注的 epoll 事件掩码
    bool closed_ = false;
};

}  // namespace tinykv

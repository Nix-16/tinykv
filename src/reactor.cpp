#include "tinykv/reactor.h"

#include <cerrno>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tinykv {

namespace {

int set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

int create_listen_fd(const std::string& ip, uint16_t port, int backlog) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (ip.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, backlog) != 0 || set_nonblocking(fd) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

Reactor::Reactor(int max_events)
    : max_events_(max_events > 0 ? max_events : 1024),
      evlist_(static_cast<std::size_t>(max_events_)) {
    epfd_ = ::epoll_create1(0);
}

Reactor::~Reactor() {
    // 关闭所有客户端连接。
    for (auto& c : conns_) {
        if (c) {
            ::close(c->fd());
        }
    }
    conns_.clear();

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (epfd_ >= 0) {
        ::close(epfd_);
        epfd_ = -1;
    }
}

bool Reactor::ensure_cap(int fd) {
    if (fd < 0) {
        return false;
    }
    if (static_cast<std::size_t>(fd) < conns_.size()) {
        return true;
    }
    if (fd > (1 << 27)) {
        return false;
    }
    conns_.resize(static_cast<std::size_t>(fd) + 1);
    return true;
}

Connection* Reactor::conn_of(int fd) {
    if (fd < 0 || static_cast<std::size_t>(fd) >= conns_.size()) {
        return nullptr;
    }
    return conns_[static_cast<std::size_t>(fd)].get();
}

int Reactor::epoll_update(int fd, uint32_t events) {
    if (fd < 0) {
        return -1;
    }
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;

    if (events == 0) {
        ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        return 0;
    }
    // 先尝试 MOD，不存在再 ADD。
    if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
        return 0;
    }
    if (errno == ENOENT && ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0) {
        return 0;
    }
    return -1;
}

int Reactor::listen(const std::string& ip, uint16_t port, int backlog) {
    if (backlog <= 0) {
        backlog = 128;
    }
    if (listen_fd_ >= 0) {
        return -2;
    }
    int fd = create_listen_fd(ip, port, backlog);
    if (fd < 0) {
        return -1;
    }
    if (epoll_update(fd, EPOLLIN) != 0) {
        ::close(fd);
        return -1;
    }
    listen_fd_ = fd;
    return 0;
}

void Reactor::handle_accept() {
    for (;;) {
        sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &len);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;  // EAGAIN 或其他：本轮结束。
        }
        if (set_nonblocking(cfd) != 0 || !ensure_cap(cfd)) {
            ::close(cfd);
            continue;
        }

        auto c = std::make_unique<Connection>(cfd);
        if (epoll_update(cfd, EPOLLIN) != 0) {
            ::close(cfd);
            continue;
        }
        c->set_events(EPOLLIN);
        conns_[static_cast<std::size_t>(cfd)] = std::move(c);
    }
}

bool Reactor::handle_read(Connection* c) {
    int saved_errno = 0;
    bool eof = false;
    ssize_t n = c->in().read_fd(c->fd(), &saved_errno, &eof);
    if (n < 0) {
        return false;
    }
    if (n == 0) {
        if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
            return true;  // 暂无数据。
        }
        return false;  // EOF。
    }

    if (on_message_) {
        // 防御业务层死循环：最多 N 轮，且每轮必须有消费。
        constexpr int kMaxRounds = 16;
        for (int round = 0; round < kMaxRounds; ++round) {
            std::size_t before = c->in().readable_bytes();
            if (!on_message_(*c)) {
                return false;
            }
            std::size_t after = c->in().readable_bytes();
            if (after >= before || after == 0) {
                break;
            }
        }
    }
    return true;
}

int Reactor::flush_out(Connection* c) {
    Buffer& out = c->out();
    while (out.readable_bytes() > 0) {
        const char* p = out.peek();
        std::size_t nleft = out.readable_bytes();
        ssize_t n = ::write(c->fd(), p, nleft);
        if (n > 0) {
            out.retrieve(static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // 内核发送缓冲满，等下次 EPOLLOUT。
        }
        return -1;
    }
    return 0;
}

void Reactor::close_connection(Connection* c) {
    if (!c || c->closed()) {
        return;
    }
    c->mark_closed();
    int fd = c->fd();
    epoll_update(fd, 0);
    ::close(fd);
    if (fd >= 0 && static_cast<std::size_t>(fd) < conns_.size()) {
        conns_[static_cast<std::size_t>(fd)].reset();  // 释放 Connection。
    }
}

int Reactor::run() {
    if (epfd_ < 0) {
        return -1;
    }
    running_ = true;

    while (running_) {
        int n = ::epoll_wait(epfd_, evlist_.data(), max_events_, 100);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        for (int i = 0; i < n; ++i) {
            int fd = evlist_[i].data.fd;
            uint32_t ev = evlist_[i].events;

            if (fd == listen_fd_) {
                if (ev & EPOLLIN) {
                    handle_accept();
                }
                continue;
            }

            Connection* c = conn_of(fd);
            if (!c || c->closed()) {
                continue;
            }

            if (ev & (EPOLLERR | EPOLLHUP)) {
                close_connection(c);
                continue;
            }
#ifdef EPOLLRDHUP
            if (ev & EPOLLRDHUP) {
                close_connection(c);
                continue;
            }
#endif

            if (ev & EPOLLIN) {
                if (!handle_read(c)) {
                    close_connection(c);
                    continue;
                }
            }

            if (ev & EPOLLOUT) {
                if (flush_out(c) != 0) {
                    close_connection(c);
                    continue;
                }
                if (c->out().readable_bytes() == 0) {
                    uint32_t want = (c->events() & ~EPOLLOUT) | EPOLLIN;
                    if (want != c->events()) {
                        if (epoll_update(fd, want) != 0) {
                            close_connection(c);
                            continue;
                        }
                        c->set_events(want);
                    }
                }
            }

            // 业务层写了响应则确保开启 EPOLLOUT。
            if (!c->closed() && c->out().readable_bytes() > 0) {
                uint32_t want = c->events() | EPOLLOUT | EPOLLIN;
                if (want != c->events()) {
                    if (epoll_update(fd, want) != 0) {
                        close_connection(c);
                        continue;
                    }
                    c->set_events(want);
                }
            }
        }

        if (on_tick_) {
            on_tick_();
        }
    }
    return 0;
}

}  // namespace tinykv

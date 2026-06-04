#include "tinykv/aof.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#include "tinykv/buffer.h"

namespace tinykv {

namespace {

long long now_ms() {
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);
    return static_cast<long long>(tv.tv_sec) * 1000LL + tv.tv_usec / 1000;
}

bool write_all(int fd, const void* buf, std::size_t len) {
    const char* p = static_cast<const char*>(buf);
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

Aof::~Aof() {
    close();
}

bool Aof::init(const std::string& filename, bool enabled, AofFsync policy) {
    enabled_ = enabled;
    policy_ = policy;
    last_fsync_ms_ = now_ms();
    filename_ = filename.empty() ? "appendonly.aof" : filename;

    if (!enabled_) {
        return true;
    }

    fd_ = ::open(filename_.c_str(), O_CREAT | O_APPEND | O_WRONLY, 0644);
    return fd_ >= 0;
}

void Aof::close() {
    if (fd_ >= 0) {
        // 优雅退出：无论日常策略如何，关闭前强制 fsync。
        ::fsync(fd_);
        ::close(fd_);
        fd_ = -1;
    }
}

bool Aof::append_command(const std::vector<std::string>& argv) {
    if (!enabled_ || loading_) {
        return true;
    }
    if (fd_ < 0 || argv.empty()) {
        return false;
    }

    char hdr[64];
    int n = std::snprintf(hdr, sizeof(hdr), "*%zu\r\n", argv.size());
    if (n <= 0 || !write_all(fd_, hdr, static_cast<std::size_t>(n))) {
        return false;
    }

    for (const auto& a : argv) {
        n = std::snprintf(hdr, sizeof(hdr), "$%zu\r\n", a.size());
        if (n <= 0 || !write_all(fd_, hdr, static_cast<std::size_t>(n))) {
            return false;
        }
        if (!a.empty() && !write_all(fd_, a.data(), a.size())) {
            return false;
        }
        if (!write_all(fd_, "\r\n", 2)) {
            return false;
        }
    }

    if (policy_ == AofFsync::Always) {
        if (::fsync(fd_) != 0) {
            return false;
        }
        last_fsync_ms_ = now_ms();
    }
    return true;
}

bool Aof::load(const ApplyFn& apply) {
    if (!enabled_) {
        return true;
    }

    int fd = ::open(filename_.c_str(), O_RDONLY);
    if (fd < 0) {
        return errno == ENOENT;  // 不存在视为正常。
    }

    Buffer in(4096);
    char tmp[4096];
    for (;;) {
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        if (!in.append(tmp, static_cast<std::size_t>(n))) {
            ::close(fd);
            return false;
        }
    }
    ::close(fd);

    loading_ = true;
    bool ok = true;
    for (;;) {
        RespCommand cmd;
        RespParseResult prc = resp_try_parse(in, cmd);
        if (prc == RespParseResult::Incomplete) {
            break;  // 文件结束；末尾若不完整也在此停下。
        }
        if (prc == RespParseResult::Error) {
            ok = false;
            break;
        }
        if (cmd.empty()) {
            continue;
        }
        if (!apply(cmd)) {
            ok = false;
            break;
        }
    }
    loading_ = false;
    return ok;
}

bool Aof::maybe_fsync() {
    if (!enabled_ || fd_ < 0) {
        return true;
    }
    if (policy_ != AofFsync::Everysec) {
        return true;
    }
    long long now = now_ms();
    if (now - last_fsync_ms_ >= 1000) {
        if (::fsync(fd_) != 0) {
            return false;
        }
        last_fsync_ms_ = now;
    }
    return true;
}

bool Aof::reset() {
    if (!enabled_) {
        return true;
    }
    if (loading_) {
        return false;
    }

    if (fd_ >= 0) {
        if (policy_ != AofFsync::No && ::fsync(fd_) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        ::close(fd_);
        fd_ = -1;
    }

    int fd = ::open(filename_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        return false;
    }
    if (policy_ != AofFsync::No && ::fsync(fd) != 0) {
        ::close(fd);
        return false;
    }
    ::close(fd);

    fd_ = ::open(filename_.c_str(), O_CREAT | O_APPEND | O_WRONLY, 0644);
    if (fd_ < 0) {
        return false;
    }
    last_fsync_ms_ = now_ms();
    return true;
}

}  // namespace tinykv

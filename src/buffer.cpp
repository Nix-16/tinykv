#include "tinykv/buffer.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace tinykv {

Buffer::Buffer(std::size_t initial) {
    if (initial == 0) {
        initial = kDefaultSize;
    }
    if (initial > kMaxCapacity) {
        initial = kMaxCapacity;
    }
    data_.resize(initial);
}

bool Buffer::ensure_writable(std::size_t len) {
    if (len == 0) {
        return true;
    }
    // 溢出/上限保护。
    if (write_index_ > kMaxCapacity - len) {
        return false;
    }

    // 快路径：尾部空间足够。
    if (writable_bytes() >= len) {
        return true;
    }

    // 1) 压缩：回收已消费的头部空间。
    std::size_t readable = readable_bytes();
    if (read_index_ > 0) {
        std::memmove(data_.data(), data_.data() + read_index_, readable);
        read_index_ = 0;
        write_index_ = readable;
    }

    if (writable_bytes() >= len) {
        return true;
    }

    // 2) 扩容：capacity >= write_index + len，2 倍增长并夹到上限。
    std::size_t required = write_index_ + len;
    if (required > kMaxCapacity) {
        return false;
    }

    std::size_t new_cap = data_.empty() ? kDefaultSize : data_.size();
    while (new_cap < required) {
        if (new_cap > kMaxCapacity / 2) {
            new_cap = kMaxCapacity;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap < required) {
        return false;
    }

    data_.resize(new_cap);
    return true;
}

bool Buffer::append(const void* data, std::size_t len) {
    if (len == 0) {
        return true;
    }
    if (!data) {
        return false;
    }
    if (!ensure_writable(len)) {
        return false;
    }
    std::memcpy(begin_write(), data, len);
    write_index_ += len;
    return true;
}

void Buffer::retrieve(std::size_t len) {
    if (len >= readable_bytes()) {
        retrieve_all();
        return;
    }
    read_index_ += len;
}

void Buffer::retrieve_all() {
    read_index_ = 0;
    write_index_ = 0;
}

ssize_t Buffer::read_fd(int fd, int* saved_errno, bool* eof) {
    if (eof) {
        *eof = false;
    }
    ssize_t total = 0;

    for (;;) {
        char tmp[8192];
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (!append(tmp, static_cast<std::size_t>(n))) {
                if (saved_errno) {
                    *saved_errno = ENOMEM;
                }
                return -1;
            }
            total += n;
            continue;
        }

        if (n == 0) {
            // 对端关闭写端。
            if (saved_errno) {
                *saved_errno = 0;
            }
            if (eof) {
                *eof = true;
            }
            return total;
        }

        // n < 0
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (saved_errno) {
                *saved_errno = (total == 0) ? EAGAIN : 0;
            }
            break;
        }

        if (saved_errno) {
            *saved_errno = errno;
        }
        return -1;
    }

    return total;
}

}  // namespace tinykv

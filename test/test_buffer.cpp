#include "tinykv/buffer.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "test_util.h"

using namespace tinykv;

static void test_append_peek_retrieve() {
    Buffer b(16);
    CHECK(b.append("hello", 5));
    CHECK(b.readable_bytes() == 5);
    CHECK(std::memcmp(b.peek(), "hello", 5) == 0);

    b.retrieve(2);
    CHECK(b.readable_bytes() == 3);
    CHECK(std::memcmp(b.peek(), "llo", 3) == 0);

    b.retrieve_all();
    CHECK(b.readable_bytes() == 0);
}

static void test_compact_path() {
    Buffer b(16);
    char a[12];
    std::memset(a, 'A', sizeof(a));
    CHECK(b.append(a, sizeof(a)));
    CHECK(b.readable_bytes() == 12);

    b.retrieve(10);  // 剩 2 字节
    CHECK(b.readable_bytes() == 2);

    char x[10];
    std::memset(x, 'X', sizeof(x));
    CHECK(b.append(x, sizeof(x)));  // 触发压缩
    CHECK(b.readable_bytes() == 12);

    const char* p = b.peek();
    CHECK(p[0] == 'A' && p[1] == 'A');
    CHECK(std::memcmp(p + 2, x, 10) == 0);
}

static void test_expand_path() {
    Buffer b(8);
    char big[1000];
    for (int i = 0; i < (int)sizeof(big); ++i) {
        big[i] = (char)('a' + (i % 26));
    }
    CHECK(b.append(big, sizeof(big)));
    CHECK(b.readable_bytes() == sizeof(big));
    CHECK(std::memcmp(b.peek(), big, sizeof(big)) == 0);
}

static void test_cap_enforced() {
    Buffer b(16);
    CHECK(b.append("TAG", 3));
    // 超过上限：失败且不破坏内容。
    CHECK(!b.ensure_writable(Buffer::kMaxCapacity));
    CHECK(b.readable_bytes() == 3);
    CHECK(std::memcmp(b.peek(), "TAG", 3) == 0);
}

static void test_read_fd_pipe() {
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    int rfd = pfd[0], wfd = pfd[1];

    int flags = fcntl(rfd, F_GETFL, 0);
    CHECK(fcntl(rfd, F_SETFL, flags | O_NONBLOCK) == 0);

    Buffer b(16);
    const char* msg = "hello_pipe";
    CHECK(write(wfd, msg, (int)std::strlen(msg)) == (ssize_t)std::strlen(msg));

    int saved = 0;
    bool eof = false;
    ssize_t n = b.read_fd(rfd, &saved, &eof);
    CHECK(n == (ssize_t)std::strlen(msg));
    CHECK(!eof);
    CHECK(b.readable_bytes() == std::strlen(msg));
    CHECK(std::memcmp(b.peek(), msg, std::strlen(msg)) == 0);

    // 无更多数据 -> EAGAIN -> 返回 0，eof=false。
    saved = 0;
    eof = false;
    n = b.read_fd(rfd, &saved, &eof);
    CHECK(n == 0);
    CHECK(!eof);

    // 关闭写端 -> EOF。
    close(wfd);
    saved = 0;
    eof = false;
    n = b.read_fd(rfd, &saved, &eof);
    CHECK(n == 0);
    CHECK(eof);

    close(rfd);
}

int main() {
    test_append_peek_retrieve();
    test_compact_path();
    test_expand_path();
    test_cap_enforced();
    test_read_fd_pipe();
    std::puts("ALL buffer tests PASSED");
    return 0;
}

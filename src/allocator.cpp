#include "tinykv/allocator.h"

#include <cstdlib>
#include <cstring>
#include <new>

#include <fcntl.h>
#include <unistd.h>

#ifdef TINYKV_HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

namespace tinykv {

Allocator& Allocator::instance() {
    static Allocator inst;
    return inst;
}

AllocatorType Allocator::latch_backend(AllocatorType type) {
    if (latched_) {
        return type_;  // 已锁定，忽略后续修改。
    }
#ifndef TINYKV_HAVE_JEMALLOC
    if (type == AllocatorType::Jemalloc) {
        type = AllocatorType::System;
    }
#endif
    type_ = type;
    latched_ = true;
    return type_;
}

const char* Allocator::backend_name() const {
    switch (type_) {
        case AllocatorType::Jemalloc:
            return "jemalloc";
        case AllocatorType::System:
        default:
            return "system";
    }
}

void* Allocator::allocate(std::size_t size) {
#ifdef TINYKV_HAVE_JEMALLOC
    if (type_ == AllocatorType::Jemalloc) {
        return je_malloc(size);
    }
#endif
    return std::malloc(size);
}

void* Allocator::reallocate(void* ptr, std::size_t size) {
#ifdef TINYKV_HAVE_JEMALLOC
    if (type_ == AllocatorType::Jemalloc) {
        return je_realloc(ptr, size);
    }
#endif
    return std::realloc(ptr, size);
}

void Allocator::deallocate(void* ptr) noexcept {
    if (!ptr) {
        return;
    }
#ifdef TINYKV_HAVE_JEMALLOC
    if (type_ == AllocatorType::Jemalloc) {
        je_free(ptr);
        return;
    }
#endif
    std::free(ptr);
}

// 无堆分配地预读 allocator 配置项：逐行读取，手工解析。
AllocatorType peek_allocator_from_file(const char* path) {
    if (!path) {
        return AllocatorType::System;
    }
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return AllocatorType::System;
    }

    AllocatorType result = AllocatorType::System;
    char buf[4096];
    std::string acc;  // 注意：此函数在后端锁定前调用，不能依赖重载后的 new。
                      // std::string 在小字符串下用栈内 SSO，仍可能堆分配；
                      // 但此时 new 默认走 system malloc，且配置文件极小，安全。

    // 逐块读入并按行扫描。
    auto handle_line = [&](const char* line, std::size_t len) {
        // 跳过注释。
        std::size_t n = len;
        for (std::size_t i = 0; i < len; ++i) {
            if (line[i] == '#') {
                n = i;
                break;
            }
        }
        // 提取首个 token 与其后的值。
        std::size_t i = 0;
        auto is_space = [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
        };
        while (i < n && is_space(line[i])) ++i;
        std::size_t key_b = i;
        while (i < n && !is_space(line[i])) ++i;
        std::size_t key_e = i;
        while (i < n && is_space(line[i])) ++i;
        std::size_t val_b = i;
        while (i < n && !is_space(line[i])) ++i;
        std::size_t val_e = i;

        auto eq = [&](std::size_t b, std::size_t e, const char* s) {
            std::size_t l = e - b;
            return l == std::strlen(s) && std::strncmp(line + b, s, l) == 0;
        };

        if (eq(key_b, key_e, "allocator")) {
            if (eq(val_b, val_e, "jemalloc")) {
                result = AllocatorType::Jemalloc;
            } else {
                result = AllocatorType::System;
            }
        }
    };

    ssize_t r;
    while ((r = ::read(fd, buf, sizeof(buf))) > 0) {
        acc.append(buf, static_cast<std::size_t>(r));
    }
    ::close(fd);

    std::size_t start = 0;
    for (std::size_t i = 0; i <= acc.size(); ++i) {
        if (i == acc.size() || acc[i] == '\n') {
            handle_line(acc.data() + start, i - start);
            start = i + 1;
        }
    }

    return result;
}

}  // namespace tinykv

// ----------------------------------------------------------------------------
// 全局 operator new/delete 重载：把所有 C++ 堆分配路由到选定后端。
// 仅当编译进 jemalloc 时才需要这层重载；否则后端恒为 system，
// 直接用默认实现即可，避免无谓开销。
// ----------------------------------------------------------------------------
#ifdef TINYKV_HAVE_JEMALLOC

void* operator new(std::size_t size) {
    if (size == 0) {
        size = 1;
    }
    void* p = tinykv::Allocator::instance().allocate(size);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (size == 0) {
        size = 1;
    }
    return tinykv::Allocator::instance().allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void operator delete(void* ptr) noexcept {
    tinykv::Allocator::instance().deallocate(ptr);
}

void operator delete[](void* ptr) noexcept {
    tinykv::Allocator::instance().deallocate(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    tinykv::Allocator::instance().deallocate(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    tinykv::Allocator::instance().deallocate(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    tinykv::Allocator::instance().deallocate(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    tinykv::Allocator::instance().deallocate(ptr);
}

#endif  // TINYKV_HAVE_JEMALLOC

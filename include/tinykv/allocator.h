#pragma once

#include <cstddef>
#include <string>

namespace tinykv {

// 分配器后端，与配置项 allocator 对应。
enum class AllocatorType {
    System = 0,
    Jemalloc,
};

// 进程级可切换的内存分配后端（单例）。
//
// 机制：本项目重载了全局 operator new/delete（见 allocator.cpp），全部 C++
// 堆分配（std::string / std::vector / std::map 节点等）都会路由到这里，再按
// 选定后端转发给 malloc 或 je_malloc。这样即便存储层使用朴素 STL 容器，
// allocator 配置依然对整个数据路径生效。
//
// 安全前提：后端在 main 最早期、任何 C++ 堆分配发生之前“锁定”一次，
// 之后不再改变，从而避免 malloc 的指针被 je_free（或反之）。配置文件中
// 的 allocator 选项通过 peek_allocator_from_file() 以无堆分配方式预读。
class Allocator {
public:
    static Allocator& instance();

    // 锁定后端，仅首次调用生效。若请求 Jemalloc 但未编译进 jemalloc，
    // 回退到 System。返回实际生效的类型。
    AllocatorType latch_backend(AllocatorType type);
    AllocatorType backend() const { return type_; }
    const char* backend_name() const;

    void* allocate(std::size_t size);
    void* reallocate(void* ptr, std::size_t size);
    void  deallocate(void* ptr) noexcept;

    static constexpr bool jemalloc_available() {
#ifdef TINYKV_HAVE_JEMALLOC
        return true;
#else
        return false;
#endif
    }

private:
    Allocator() = default;
    AllocatorType type_ = AllocatorType::System;
    bool latched_ = false;
};

// 以无堆分配方式预读配置文件中的 allocator 选项。
// 找不到或无法打开时返回 System。仅用于 main 启动早期决定后端。
AllocatorType peek_allocator_from_file(const char* path);

}  // namespace tinykv

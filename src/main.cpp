#include <cstdio>

#include "tinykv/allocator.h"
#include "tinykv/config.h"
#include "tinykv/server.h"

using namespace tinykv;

int main(int argc, char** argv) {
    const char* conf_path = (argc > 1) ? argv[1] : "kvs.conf";

    // 关键顺序：在任何 C++ 容器分配发生之前锁定分配后端。
    // 先以无堆分配方式预读配置里的 allocator 选项，再 latch。
    AllocatorType want = peek_allocator_from_file(conf_path);
    AllocatorType got = Allocator::instance().latch_backend(want);
    std::printf("allocator: %s\n", Allocator::instance().backend_name());
    if (want == AllocatorType::Jemalloc && got != AllocatorType::Jemalloc) {
        std::printf("(jemalloc requested but not compiled in; using system)\n");
    }

    Config config;
    std::string err;
    if (!config.load_file(conf_path, &err)) {
        std::fprintf(stderr, "load config failed: %s\n", err.c_str());
        return 1;
    }

    Server server(config);
    return server.run();
}

cmake_minimum_required(VERSION 3.16)
project(TinyKv LANGUAGES CXX)

# ---------------- 标准与默认构建类型 ----------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 与原项目一致的优化/调试级别。
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

add_compile_options(-Wall -Wextra)

# ---------------- 可选 jemalloc ----------------
# 默认开启；从自带的 git submodule（third_party/jemalloc）源码编译。
#
# 运行时可切换的设计要求 jemalloc 以“带 je_ 前缀”方式构建（je_malloc/je_free
# 与系统 malloc/free 并存），因此从源码以 --with-jemalloc-prefix=je_ 编译。
# 子模块未初始化时自动回退到系统分配器（不报错）。
# 关闭可用 -DUSE_JEMALLOC=OFF。
option(USE_JEMALLOC "Use jemalloc as the pluggable allocator backend" ON)

set(TINYKV_JEMALLOC_FOUND OFF)
if(USE_JEMALLOC)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
    include(jemalloc)
    tinykv_setup_jemalloc()
endif()

# ---------------- 核心库 ----------------
add_library(tinykv_core STATIC
    src/allocator.cpp
    src/buffer.cpp
    src/config.cpp
    src/resp.cpp
    src/store.cpp
    src/aof.cpp
    src/snapshot.cpp
    src/reactor.cpp
    src/database.cpp
    src/server.cpp
)
target_include_directories(tinykv_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(TINYKV_JEMALLOC_FOUND)
    target_compile_definitions(tinykv_core PUBLIC TINYKV_HAVE_JEMALLOC)
    # PUBLIC：可执行文件与测试都需要链接，保证 operator new 重载里的
    # je_malloc/je_free 解析到同一份库。tinykv::jemalloc 已带 include 与 dl/pthread。
    target_link_libraries(tinykv_core PUBLIC tinykv::jemalloc)
endif()

# ---------------- 服务端可执行 ----------------
add_executable(tinykv_server src/main.cpp)
target_link_libraries(tinykv_server PRIVATE tinykv_core)

# ---------------- 测试 ----------------
enable_testing()
add_subdirectory(test)

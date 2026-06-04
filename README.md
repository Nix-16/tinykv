# TinyKv

基于 C++17 实现的内存 KV 存储服务，使用 [RESP](https://redis.io/docs/reference/protocol-spec/) 协议与客户端通信，支持多种底层数据结构与持久化。

本项目是对 C 版 [`kvstore`](../kvstore) 的 C++ 重构：在保持**线协议、AOF 格式、快照二进制格式完全兼容**的前提下，用面向对象与 STL 重写，简化命令分发、用 RAII 管理资源、用统一的 `IStore` 接口抽象三套命名空间。

## 功能概览

- **三种独立命名空间**：默认（数组）、Hash（哈希表）、RBTree（红黑树），各自一套 SET/GET/DEL/EXISTS 风格命令
- **持久化**：AOF 增量日志 + 全量快照（snapshot），启动时先加载快照再回放 AOF
- **可选 jemalloc**：通过全局 `operator new`/`delete` 重载，让分配器配置对整个数据路径生效
- **Reactor 网络模型**：单线程 epoll；proactor / ntyco 已在配置中预留，待后续实现

详细架构见 **[docs/design.md](docs/design.md)**。

## 依赖与编译

- **g++ 9+**（需 C++17）、**CMake 3.16+**
- jemalloc 以 **git submodule** 形式自带于 `third_party/jemalloc`（仅源码）。默认开启时会在首次构建阶段**从源码自动编译**出带 `je_` 前缀的静态库（约 1-2 分钟，只编一次）。因此构建机还需 **autotools**（autoconf / automake）与 make
- 子模块未初始化或 `-DUSE_JEMALLOC=OFF` 时，自动回退到系统分配器，编译不会失败

```bash
# 克隆后先拉取 submodule（首次必做）
git submodule update --init --recursive

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# 首次构建会先从源码编译 jemalloc（约 1-2 分钟）

# 生成：build/tinykv_server、build/libtinykv_core.a

# 关闭 jemalloc（纯系统分配器，无需 submodule 与 autotools）：
cmake -S . -B build -DUSE_JEMALLOC=OFF && cmake --build build -j
```

## 运行

```bash
# 使用默认配置 kvs.conf（端口 6380）
./build/tinykv_server

# 指定配置文件
./build/tinykv_server /path/to/kvs.conf
```

服务默认监听 `0.0.0.0:6380`。配置文件格式与 C 版一致，可直接复用旧的 `kvs.conf`。

## 配置说明（kvs.conf）

| 配置项 | 说明 | 示例 |
|--------|------|------|
| `bind` | 监听地址 | `0.0.0.0` |
| `port` | 监听端口 | `6380` |
| `network` | 网络模型：`reactor`（已实现）；`proactor`、`ntyco` 待实现 | `reactor` |
| `allocator` | 内存分配器：`system`、`jemalloc` | `jemalloc` |
| `appendonly` | 是否开启 AOF | `yes` / `no` |
| `appendfilename` | AOF 文件名 | `appendonly.aof` |
| `appendfsync` | fsync 策略：`always`、`everysec`、`no` | `everysec` |
| `snapshot_file` | 快照文件名 | `dump.kvs` |
| `snapshot_enabled` | 是否启用快照 | `yes` / `no` |

## 支持的命令（RESP）

- **通用**：`PING`、`PING <msg>`；`SAVE`（做一次全量快照并重置 AOF）
- **默认命名空间（数组）**：`SET key value`、`GET key`、`DEL key`、`EXISTS key`
- **Hash 命名空间**：`HSET key value`、`HGET key`、`HDEL key`、`HEXISTS key`
- **RBTree 命名空间**：`RSET key value`、`RGET key`、`RDEL key`、`REXISTS key`

命令与参数错误时返回 RESP 错误回复；未知命令返回 `unknown command`。

## 持久化与恢复

- **AOF**：写命令（SET/DEL、HSET/HDEL、RSET/RDEL）在非加载阶段追加到 AOF；`appendfsync` 控制刷盘策略
- **快照**：`SAVE` 将当前内存数据写入 `snapshot_file`，并重置 AOF，便于下次启动以快照为主
- **启动顺序**：先加载快照，再回放 AOF
- **优雅退出**：收到 SIGINT/SIGTERM 后停止事件循环，若启用快照则执行一次 SAVE，关闭前对 AOF 做 fsync 再退出
- **兼容性**：快照文件头仍为 `KVS1` + version 1，KV 编码（klen/vlen/key/value）与 C 版一致；AOF 仍是 RESP 数组。两边的 `dump.kvs` / `appendonly.aof` 可互相加载。

## 测试

```bash
cmake --build build -j
cd build && ctest --output-on-failure
```

覆盖：buffer 增长/压缩/fd 读、三种 store 的统一语义、RESP 解析与回包、快照/AOF 往返、Database 命令分发与命名空间隔离。

集成压测可直接复用 C 版脚本（同一套协议）：

```bash
HOST=127.0.0.1 PORT=6380 OPS=HSET,HGET CONNS=50 DURATION=5 \
  python3 ../kvstore/test/intergration/bench_kvstore.py
```

## 与 C 版的行为差异

- **数组命名空间不再有 1024 条上限**。C 版 `kvs_array` 固定容量 1024，超出即报错；TinyKv 的 `ArrayStore` 基于 `std::vector`，可无限增长。**代价**：数组后端的 SET/GET/DEL 仍是 O(n) 线性扫描，键数很大时会明显变慢。需要大键空间时应使用 Hash（`HSET`，O(1)）或 RBTree（`RSET`，O(log n)）命名空间——数组后端定位为“小数据、教学演示”。
- 命令级行为（回复格式、命名空间隔离、DEL 返回值、PING/SAVE 语义）与 C 版保持一致。

## 项目结构

```
include/tinykv/   # 公开头文件
  buffer.h  resp.h  store.h  config.h  allocator.h
  aof.h  snapshot.h  connection.h  reactor.h  database.h  server.h
src/              # 实现 + main.cpp
test/             # 单元/集成测试（CTest）
cmake/jemalloc.cmake  # 从源码编译 jemalloc 的构建逻辑
third_party/jemalloc  # jemalloc git submodule（仅源码）
docs/design.md    # 设计文档
```

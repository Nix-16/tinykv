# tinykv

基于 C++17 实现的内存 KV 存储服务，使用 [RESP](https://redis.io/docs/reference/protocol-spec/) 协议与客户端通信，支持多种底层数据结构与持久化。

定位为单机内存 KV + 持久化：面向对象设计，用统一的 `IStore` 接口抽象三套命名空间，用 RAII 管理资源，命令分发简洁，分配器可插拔。

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

服务默认监听 `0.0.0.0:6380`。

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
- **格式**：快照文件头为 `KVS1` + version 1，KV 编码为 `klen/vlen/key/value`；AOF 为 RESP 数组。

## 测试

单元测试（CTest）：

```bash
cmake --build build -j
cd build && ctest --output-on-failure
```

覆盖：buffer 增长/压缩/fd 读、三种 store 的统一语义、RESP 解析与回包、快照/AOF 往返、Database 命令分发与命名空间隔离。

## 压力测试

`test/bench/bench.py` 是一个基于 asyncio 的 RESP 压测客户端，多连接 + pipeline 打满单线程服务端，统计 QPS 与延迟分位。

```bash
# 先启动服务
./build/tinykv_server kvs.conf

# Hash 命名空间，50 连接、跑 5 秒、读写各半
OPS=HSET,HGET CONNS=50 DURATION=5 python3 test/bench/bench.py

# RBTree 命名空间
OPS=RSET,RGET python3 test/bench/bench.py

# 默认（数组）命名空间
OPS=SET,GET python3 test/bench/bench.py
```

可调环境变量：`HOST`、`PORT`、`OPS`、`CONNS`（并发连接）、`DURATION`（秒）、`PIPELINE`（批深度）、`KEYSPACE`（key 空间）、`VALUE_LEN`、`READ_RATIO`（读比例）。

### 参考结果

环境：2 vCPU 虚拟机、`-O2` Release、jemalloc 后端、`CONNS=50 PIPELINE=16 DURATION=5 VALUE_LEN=16`、读写各半。QPS 与延迟随机器、连接数、pipeline 深度变化，仅供横向对比。

| 命名空间 | KEYSPACE | QPS | avg (ms) | p99 (ms) |
|----------|---------:|----:|---------:|---------:|
| Hash（`HSET/HGET`） | 100,000 | ~140k | 0.35 | 0.42 |
| RBTree（`RSET/RGET`） | 100,000 | ~139k | 0.35 | 0.83 |
| 数组（`SET/GET`） | 1,000 | ~132k | 0.37 | 0.58 |
| 数组（`SET/GET`） | 100,000 | ~22k | 2.28 | 5.37 |

数组后端在小键空间下与 Hash/RBTree 同级，但键数增大时 QPS 显著下降——见下节。

### 分配器对比（system vs jemalloc）

`allocator` 配置项在启动时一次性锁定，全部 C++ 堆分配（`std::string`、各容器节点等）经重载的 `operator new` 汇入选定后端。`test/bench/compare_alloc.sh` 用仅 `allocator` 一行不同、持久化均关闭的两份配置，对同一组负载各跑一轮：

```bash
bash test/bench/compare_alloc.sh
```

同机一次测得（2 vCPU、Release、`CONNS=50 PIPELINE=16 DURATION=5`，QPS）：

| 场景 | system | jemalloc | 差异 |
|------|-------:|---------:|-----:|
| Hash（`HSET/HGET`） | 249,664 | 247,469 | -0.9% |
| RBTree（`RSET/RGET`） | 245,222 | 244,970 | ~0% |
| 数组（`SET/GET`，k=1k） | 243,299 | 241,152 | -0.9% |
| Hash 纯写、`VALUE_LEN=256` | 21,510 | 22,144 | +2.9% |

**结论：在本项目当前架构下，两个分配器无可测差异，全部落在 ±3% 噪声内。** 这并非测错：

- 瓶颈不在分配器。三种数据结构（O(1) hash、O(log n) rbtree、小数组）QPS 全挤在 ~245k，说明客户端事件循环 + loopback 网络先到顶，服务端 `malloc` 路径占比很小。
- jemalloc 的主要收益来自**多线程 arena 分片**（降锁竞争）、长跑抗碎片、海量小对象分配。tinykv 是**单线程 Reactor**，没有多线程分配竞争，短时压测也跑不出碎片差异，jemalloc 的卖点几乎都没被触发。

也就是说，jemalloc 的价值要在多线程服务、长跑抗碎片或更激进的小对象分配下才体现；当前单线程数据路径下选哪个对吞吐没有实质影响。

## 数组命名空间的性能特征

数组后端（默认命名空间）基于 `std::vector`，SET/GET/DEL 都是 **O(n) 线性扫描**：键空间从 1k 增到 100k 时，QPS 从 ~132k 跌到 ~22k。它定位为“小数据、教学演示”。需要大键空间时应使用：

- **Hash**（`HSET`，O(1) 均摊）——通用首选
- **RBTree**（`RSET`，O(log n)，且按字典序遍历）——需要有序时

三套命名空间互相隔离，不共享 key。

## 项目结构

```
include/tinykv/   # 公开头文件
  buffer.h  resp.h  store.h  config.h  allocator.h
  aof.h  snapshot.h  connection.h  reactor.h  database.h  server.h
src/              # 实现 + main.cpp
test/             # 单元测试（CTest）
test/bench/       # 压测脚本 bench.py、分配器对比 compare_alloc.sh
cmake/jemalloc.cmake  # 从源码编译 jemalloc 的构建逻辑
third_party/jemalloc  # jemalloc git submodule（仅源码）
docs/design.md    # 设计文档
```

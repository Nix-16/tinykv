# tinykv 设计文档

本文档描述 tinykv 的架构、模块职责、持久化格式与关键设计取舍。

---

## 1. 总体架构

```
                        ┌──────────────────────────────┐
                        │            main.cpp           │
                        │  锁定 allocator → 读配置 → Server │
                        └───────────────┬──────────────┘
                                        │
                              ┌─────────▼─────────┐
                              │      Server        │
                              │  组合 Reactor / DB  │
                              │  信号 / 事件循环     │
                              └────┬──────────┬────┘
                                   │          │
                   on_message      │          │  on_tick (AOF everysec)
                                   │          │
                        ┌──────────▼───┐  ┌───▼─────────────┐
                        │   Reactor     │  │    Database      │
                        │ epoll/非阻塞   │  │  命令分发/执行    │
                        │ Connection    │  └───┬─────────┬───┘
                        │  in/out Buffer│      │         │
                        └───────────────┘      │         │
                          ▲   Resp 解析/回包     │         │
                          └─────────────────────┘         │
                              ┌───────────────────────────┼──────────────┐
                              ▼                            ▼              ▼
                      ┌──────────────┐         ┌──────────────┐  ┌──────────────┐
                      │ IStore (接口) │         │     Aof       │  │   Snapshot    │
                      │ ArrayStore   │         │  RESP 追加/回放 │  │  KVS1 全量    │
                      │ HashStore    │         └──────────────┘  └──────────────┘
                      │ RBTreeStore  │
                      └──────┬───────┘
                             ▼
                      全局 operator new/delete → Allocator(system / jemalloc)
```

- **网络**：单线程 Reactor + epoll，每连接维护收发 `Buffer`；解析 RESP 后交给 `Database::execute`。
- **存储**：三套命名空间通过统一的 `IStore` 接口抽象，分别由 STL 容器实现，互不共享 key。
- **持久化**：`Aof` + `Snapshot` 为独立类，由 `Database` 持有与编排。
- **内存**：全局 `operator new`/`delete` 重载，把所有 C++ 堆分配路由到选定后端。

---

## 2. 模块职责

| 模块 | 实现 | 说明 |
|------|------|------|
| `Buffer` | `std::vector<char>` 内核 | 每连接收发缓冲，read/write index + 压缩/扩容策略，RAII 管理内存 |
| `resp.{h,cpp}` | `RespCommand = std::vector<std::string>` | RESP 解析与回包；字符串天然二进制安全 |
| `IStore` + `ArrayStore`/`HashStore`/`RBTreeStore` | `std::vector` / `std::unordered_map` / `std::map` | 三套命名空间统一到一个接口，命令分发只依赖接口 |
| `Aof` | 单个 `append_command(argv)` | AOF 追加与回放；回放用 `std::function` 回调解耦 |
| `Snapshot` | 持有三个 `IStore&` | 用 `IStore::for_each` 遍历落盘，KVS1 全量格式 |
| `Config` | `std::ifstream` + `std::string` | 解析 kvs.conf |
| `Allocator` | 单例 + 全局 `operator new` 重载 | 可插拔分配器，见 §5 |
| `Reactor` + `Connection` | `vector<unique_ptr<Connection>>` | 单线程 epoll；连接生命周期用 unique_ptr |
| `Database::execute` | lambda + `IStore&` | 命令分发与执行，见下 |

### 命令分发的简化

12 个数据命令（三套命名空间 × SET/GET/DEL/EXISTS）逻辑相同、只是作用的 store 不同。tinykv 把这部分抽成 4 个 lambda（`do_set`/`do_get`/`do_del`/`do_exists`），每个接受一个 `IStore&`：

```cpp
if (iequals(op, "SET"))  return do_set(array_,  "set");
if (iequals(op, "HSET")) return do_set(hash_,   "hset");
if (iequals(op, "RSET")) return do_set(rbtree_, "rset");
```

AOF 写盘的“仅在非回放阶段追加”逻辑同样收敛到 `do_set`/`do_del` 内部一处。

---

## 3. 持久化

### 3.1 AOF

格式与 Redis 一致：每条写命令以 RESP 数组追加。
- 支持命令：SET、DEL、HSET、HDEL、RSET、RDEL。
- fsync 策略：`always`（每次追加后 fsync）、`everysec`（事件循环每秒一次，经 `Reactor::on_tick` → `Aof::maybe_fsync`）、`no`。
- 回放：`Aof::load` 把整个文件读入 `Buffer`，复用 RESP 解析器逐条解析，通过 `ApplyFn` 回调应用到内存。回放期间 `is_loading()` 为 true，`append_command` 自动 no-op，避免重复写入与递归。

### 3.2 快照

二进制格式紧凑，文件可独立加载：
- 头部：`magic[4]="KVS1"`、`version(u32)=1`、`array_count`/`hash_count`/`rbtree_count`（u32）。
- 数据区：按 array → hash → rbtree 顺序，每个 KV 为 `klen(u32) vlen(u32) key value`，无分隔符。
- 写入：写 `*.tmp` → `fflush` + `fsync` → `rename`，保证原子覆盖，崩溃不会留下半写文件。

### 3.3 恢复顺序

启动时 `Database::load_persistence` 先 `Snapshot::load`（全量），再 `Aof::load`（增量）。先全量后增量，与 Redis 的 RDB+AOF 思路一致；顺序反了会让快照覆盖 AOF 已恢复的更新。

`SAVE` 命令与优雅退出都执行“快照 + AOF reset”，使下次启动只需加载快照、AOF 从空积累，避免 AOF 无限增长。

---

## 4. 三种存储结构

| 命名空间 | 后端 | SET | GET | DEL | 遍历顺序 | 适用 |
|----------|------|-----|-----|-----|----------|------|
| 默认（数组） | `std::vector<pair>` | O(n) | O(n) | O(n) | 插入序 | 小数据、教学演示 |
| Hash | `std::unordered_map` | O(1) 均摊 | O(1) 均摊 | O(1) 均摊 | 无序 | 通用大键空间 |
| RBTree | `std::map` | O(log n) | O(log n) | O(log n) | **字典序** | 需要有序/可扩展范围查询 |

三套命名空间隔离，避免 key 冲突；单线程写，存储层无需加锁。

> **数组后端的性能特征**：`ArrayStore` 用 `std::vector`，无容量上限，但 SET/GET/DEL 都是 O(n) 线性扫描。键空间从 1k 增到 100k 时，QPS 从 ~132k 跌到 ~22k。大键空间下应改用 Hash/RBTree。详见 README「压力测试」与「数组命名空间的性能特征」。

---

## 5. 可插拔分配器

要让 `allocator` 配置对**朴素 STL 容器**（`std::string`/`std::map` 节点等）也生效，tinykv 重载了全局 `operator new`/`delete`，统一路由到 `Allocator` 单例，再按后端转发给 `malloc` 或 `je_malloc`。

**安全前提——后端只在最早期锁定一次**：若运行中途切换后端，先前用 `malloc` 分配的指针可能被 `je_free` 释放，造成崩溃。因此：

1. `main` 启动时先用 `peek_allocator_from_file()` **以无堆分配方式**预读配置里的 `allocator` 选项；
2. 调用 `Allocator::latch_backend()` 锁定后端（此后不可更改）；
3. 之后才构造 `Config` / `Server` 等会触发堆分配的对象。

**前缀要求与来源**：运行时切换要求 jemalloc 以带 `je_` 前缀方式构建（`je_malloc`/`je_free` 与系统 `malloc`/`free` 并存），这样两套分配器可在同一进程内并存、按配置二选一。系统上非前缀的 jemalloc 会全局接管 `malloc`、无法切回 system，故不适用。

tinykv 把 jemalloc 作为自己的 **git submodule** 自带于 `third_party/jemalloc`（仅源码），由 `cmake/jemalloc.cmake` 在构建阶段经 `autogen.sh → configure --with-jemalloc-prefix=je_ --enable-static --disable-shared → make build_lib_static` **从源码编译**出带前缀的静态库，链接进 `tinykv_core`。这样项目自包含、可独立推送，不依赖任何外部预编译库。子模块未初始化或 `-DUSE_JEMALLOC=OFF` 时回退系统分配器，`operator new` 重载也随 `TINYKV_HAVE_JEMALLOC` 一并关闭（无开销）。

---

## 6. 网络与请求路径

- **Reactor**：单线程 epoll，监听 `listen_fd` 与所有客户端 fd；可读时 `Buffer::read_fd` 读入，交给 `on_message`。
- **请求路径**：`Server::on_message` 循环调用 `resp_try_parse` 解析出完整命令 → `Database::execute` 分发 → `resp_reply_*` 写入 `Connection::out`；事件循环在 out 非空时自动开启 `EPOLLOUT` 发回客户端。
- **连接生命周期**：`Reactor` 用 `vector<unique_ptr<Connection>>` 按 fd 索引持有连接；关闭时 `reset()` 即释放，无手工 free。
- **防御**：单次读事件最多触发 16 轮 `on_message`，且每轮必须消费输入，避免业务层 bug 造成死循环。

---

## 7. 待实现

- **网络模型**：`proactor`、`ntyco` 已解析但未实现，仅使用 reactor。
- 无集群、主从、ACL、多 DB；定位为单机内存 KV + 持久化。
- 可考虑增加 `INFO` 命令返回运行时长、三空间键数、AOF/快照状态。

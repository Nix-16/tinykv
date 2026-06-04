#pragma once

#include <string>

#include "tinykv/store.h"

namespace tinykv {

// 全量快照：紧凑二进制格式，dump.kvs 可独立加载。
//
// 文件布局：
//   header: magic[4]="KVS1", version(u32)=1,
//           array_count(u32), hash_count(u32), rbtree_count(u32)
//   data  : 按 array -> hash -> rbtree 顺序，每个 KV 为
//           klen(u32) vlen(u32) key[klen] value[vlen]，无分隔符。
//
// 写入采用 “写临时文件 -> fsync -> rename” 保证原子覆盖。
class Snapshot {
public:
    Snapshot(IStore& array, IStore& hash, IStore& rbtree)
        : array_(array), hash_(hash), rbtree_(rbtree) {}

    // 保存到 path。成功返回 0，失败返回负值。
    int save(const std::string& path) const;

    // 从 path 加载。返回：
    //   0  成功
    //   1  文件不存在（视为正常，首次启动）
    //  <0  打开/格式/读/写回失败
    int load(const std::string& path);

private:
    IStore& array_;
    IStore& hash_;
    IStore& rbtree_;
};

}  // namespace tinykv

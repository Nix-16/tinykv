#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tinykv {

// 三套独立命名空间的统一存储接口。
//
// 三种后端（数组 / 哈希 / 红黑树）分别用 STL 容器实现同一套语义，调用方
// （Database）只依赖接口，命令分发因而大幅简化。
//
// 语义约定：
//   set    : upsert，存在则覆盖 value，恒成功。
//   get    : 命中返回 value，未命中返回 std::nullopt。
//   del    : 删除成功返回 true；key 不存在返回 false。
//   exists : 存在返回 true。
//   value 与 key 均为二进制安全（可含 '\0'）。
class IStore {
public:
    using VisitFn = std::function<void(const std::string& key, const std::string& value)>;

    virtual ~IStore() = default;

    virtual void set(const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
    virtual bool del(const std::string& key) = 0;
    virtual bool exists(const std::string& key) const = 0;
    virtual std::size_t count() const = 0;

    // 遍历当前所有键值对（用于快照写盘）。
    virtual void for_each(const VisitFn& fn) const = 0;
};

// 数组命名空间：线性表语义。底层用 vector<pair> 复刻“顺序存储”的展示意图，
// 查找/删除为 O(n)，与原 kvs_array 一致。
class ArrayStore : public IStore {
public:
    void set(const std::string& key, const std::string& value) override;
    std::optional<std::string> get(const std::string& key) const override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) const override;
    std::size_t count() const override { return items_.size(); }
    void for_each(const VisitFn& fn) const override;

private:
    std::vector<std::pair<std::string, std::string>> items_;
};

// Hash 命名空间：std::unordered_map，平均 O(1)。
class HashStore : public IStore {
public:
    void set(const std::string& key, const std::string& value) override;
    std::optional<std::string> get(const std::string& key) const override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) const override;
    std::size_t count() const override { return map_.size(); }
    void for_each(const VisitFn& fn) const override;

private:
    std::unordered_map<std::string, std::string> map_;
};

// RBTree 命名空间：std::map（红黑树），按 key 字典序有序，O(log n)，
// 天然支持有序遍历，便于后续扩展范围查询。
class RBTreeStore : public IStore {
public:
    void set(const std::string& key, const std::string& value) override;
    std::optional<std::string> get(const std::string& key) const override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) const override;
    std::size_t count() const override { return map_.size(); }
    void for_each(const VisitFn& fn) const override;

private:
    std::map<std::string, std::string> map_;
};

}  // namespace tinykv

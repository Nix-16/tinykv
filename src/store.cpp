#include "tinykv/store.h"

#include <algorithm>

namespace tinykv {

// ---------------------------- ArrayStore ----------------------------

void ArrayStore::set(const std::string& key, const std::string& value) {
    for (auto& kv : items_) {
        if (kv.first == key) {
            kv.second = value;  // 覆盖
            return;
        }
    }
    items_.emplace_back(key, value);
}

std::optional<std::string> ArrayStore::get(const std::string& key) const {
    for (const auto& kv : items_) {
        if (kv.first == key) {
            return kv.second;
        }
    }
    return std::nullopt;
}

bool ArrayStore::del(const std::string& key) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->first == key) {
            items_.erase(it);
            return true;
        }
    }
    return false;
}

bool ArrayStore::exists(const std::string& key) const {
    return std::any_of(items_.begin(), items_.end(),
                       [&](const auto& kv) { return kv.first == key; });
}

void ArrayStore::for_each(const VisitFn& fn) const {
    for (const auto& kv : items_) {
        fn(kv.first, kv.second);
    }
}

// ---------------------------- HashStore ----------------------------

void HashStore::set(const std::string& key, const std::string& value) {
    map_[key] = value;
}

std::optional<std::string> HashStore::get(const std::string& key) const {
    auto it = map_.find(key);
    if (it == map_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool HashStore::del(const std::string& key) {
    return map_.erase(key) > 0;
}

bool HashStore::exists(const std::string& key) const {
    return map_.find(key) != map_.end();
}

void HashStore::for_each(const VisitFn& fn) const {
    for (const auto& kv : map_) {
        fn(kv.first, kv.second);
    }
}

// ---------------------------- RBTreeStore ----------------------------

void RBTreeStore::set(const std::string& key, const std::string& value) {
    map_[key] = value;
}

std::optional<std::string> RBTreeStore::get(const std::string& key) const {
    auto it = map_.find(key);
    if (it == map_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool RBTreeStore::del(const std::string& key) {
    return map_.erase(key) > 0;
}

bool RBTreeStore::exists(const std::string& key) const {
    return map_.find(key) != map_.end();
}

void RBTreeStore::for_each(const VisitFn& fn) const {
    for (const auto& kv : map_) {  // std::map 按 key 有序遍历
        fn(kv.first, kv.second);
    }
}

}  // namespace tinykv

#include "tinykv/store.h"

#include <memory>
#include <set>
#include <string>

#include "test_util.h"

using namespace tinykv;

// 对任意 IStore 后端跑同一套语义测试，确保三种实现行为一致。
static void test_crud(IStore& s) {
    CHECK(s.count() == 0);

    s.set("k1", "v1");
    CHECK(s.exists("k1"));
    CHECK(s.get("k1").value() == "v1");
    CHECK(s.count() == 1);

    // 覆盖语义。
    s.set("k1", "v2");
    CHECK(s.get("k1").value() == "v2");
    CHECK(s.count() == 1);

    // 未命中。
    CHECK(!s.get("nope").has_value());
    CHECK(!s.exists("nope"));

    // 删除存在 -> true；再删 -> false。
    CHECK(s.del("k1"));
    CHECK(!s.del("k1"));
    CHECK(!s.exists("k1"));
    CHECK(s.count() == 0);
}

static void test_batch(IStore& s) {
    for (int i = 0; i < 500; ++i) {
        s.set("k" + std::to_string(i), "v" + std::to_string(i));
    }
    CHECK(s.count() == 500);

    // 删一半。
    for (int i = 0; i < 500; i += 2) {
        CHECK(s.del("k" + std::to_string(i)));
    }
    CHECK(s.count() == 250);

    // 奇数还在，偶数没了。
    for (int i = 1; i < 500; i += 2) {
        CHECK(s.get("k" + std::to_string(i)).value() == "v" + std::to_string(i));
    }
    for (int i = 0; i < 500; i += 2) {
        CHECK(!s.exists("k" + std::to_string(i)));
    }
}

static void test_binary_safe(IStore& s) {
    std::string key("a\0b", 3);
    std::string val("x\0y\0z", 5);
    s.set(key, val);
    auto got = s.get(key);
    CHECK(got.has_value());
    CHECK(got->size() == 5);
    CHECK(*got == val);
}

static void test_foreach(IStore& s) {
    s.set("one", "1");
    s.set("two", "2");
    s.set("three", "3");
    std::set<std::string> seen;
    std::size_t n = 0;
    s.for_each([&](const std::string& k, const std::string&) {
        seen.insert(k);
        ++n;
    });
    CHECK(n == s.count());
    CHECK(seen.count("one") && seen.count("two") && seen.count("three"));
}

// RBTreeStore 的 for_each 必须按 key 字典序。
static void test_rbtree_ordered() {
    RBTreeStore s;
    s.set("banana", "1");
    s.set("apple", "2");
    s.set("cherry", "3");
    std::string prev;
    bool first = true;
    s.for_each([&](const std::string& k, const std::string&) {
        if (!first) {
            CHECK(prev < k);
        }
        prev = k;
        first = false;
    });
}

int main() {
    {
        ArrayStore s;
        test_crud(s);
    }
    {
        HashStore s;
        test_crud(s);
    }
    {
        RBTreeStore s;
        test_crud(s);
    }
    {
        ArrayStore s;
        test_batch(s);
    }
    {
        HashStore s;
        test_batch(s);
    }
    {
        RBTreeStore s;
        test_batch(s);
    }
    {
        ArrayStore s;
        test_binary_safe(s);
    }
    {
        HashStore s;
        test_binary_safe(s);
    }
    {
        RBTreeStore s;
        test_binary_safe(s);
    }
    {
        ArrayStore s;
        test_foreach(s);
    }
    {
        HashStore s;
        test_foreach(s);
    }
    {
        RBTreeStore s;
        test_foreach(s);
    }
    test_rbtree_ordered();

    std::puts("ALL store tests PASSED");
    return 0;
}

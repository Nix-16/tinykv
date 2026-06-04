#include "tinykv/database.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace tinykv {

namespace {

bool iequals(const std::string& a, const char* b) {
    std::size_t n = std::strlen(b);
    if (a.size() != n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string wrong_args(const char* name) {
    return std::string("wrong number of arguments for '") + name + "'";
}

}  // namespace

Database::Database(const Config& config)
    : config_(config), snapshot_(array_, hash_, rbtree_) {}

bool Database::load_persistence() {
    // 1) 先加载全量快照（启用时）。
    if (config_.snapshot_enabled && !config_.snapshot_file.empty()) {
        int rc = snapshot_.load(config_.snapshot_file);
        if (rc < 0) {
            std::fprintf(stderr, "snapshot load failed, rc=%d\n", rc);
            return false;
        }
    }

    // 2) 初始化并回放 AOF 增量。
    if (!aof_.init(config_.appendfilename, config_.appendonly, config_.appendfsync)) {
        std::fprintf(stderr, "aof init failed\n");
        return false;
    }
    if (!aof_.load([this](const RespCommand& cmd) { return apply_from_aof(cmd); })) {
        std::fprintf(stderr, "aof load failed\n");
        return false;
    }
    return true;
}

// 回放：直接套用与在线执行相同的写逻辑，但 aof_.is_loading() 为 true，
// 因此 maybe_append_aof 不会再次写盘。这里只接受写命令。
bool Database::apply_from_aof(const RespCommand& cmd) {
    if (cmd.empty()) {
        return true;
    }
    const std::string& op = cmd[0];

    auto set_into = [&](IStore& s, int argc) {
        if (static_cast<int>(cmd.size()) != argc) {
            return false;
        }
        s.set(cmd[1], cmd[2]);
        return true;
    };
    auto del_from = [&](IStore& s, int argc) {
        if (static_cast<int>(cmd.size()) != argc) {
            return false;
        }
        s.del(cmd[1]);  // 删除不存在的 key 视为可接受。
        return true;
    };

    if (iequals(op, "SET")) return set_into(array_, 3);
    if (iequals(op, "DEL")) return del_from(array_, 2);
    if (iequals(op, "HSET")) return set_into(hash_, 3);
    if (iequals(op, "HDEL")) return del_from(hash_, 2);
    if (iequals(op, "RSET")) return set_into(rbtree_, 3);
    if (iequals(op, "RDEL")) return del_from(rbtree_, 2);
    return false;  // 非写命令出现在 AOF：视为损坏。
}

bool Database::maybe_append_aof(const RespCommand& cmd) {
    return aof_.append_command(cmd);
}

bool Database::execute(const RespCommand& cmd, Buffer& out) {
    if (cmd.empty()) {
        resp_reply_error(out, "protocol error");
        return true;
    }
    const std::string& op = cmd[0];
    const int argc = static_cast<int>(cmd.size());

    // ---- SAVE ----
    if (iequals(op, "SAVE")) {
        if (argc != 1) {
            resp_reply_error(out, wrong_args("save"));
            return true;
        }
        if (snapshot_.save(config_.snapshot_file) != 0) {
            resp_reply_error(out, "snapshot save failed");
            return true;
        }
        if (!aof_.reset()) {
            resp_reply_error(out, "snapshot ok but aof reset failed");
            return true;
        }
        resp_reply_simple(out, "OK");
        return true;
    }

    // ---- PING [msg] ----
    if (iequals(op, "PING")) {
        if (argc == 1) {
            resp_reply_simple(out, "PONG");
        } else if (argc == 2) {
            resp_reply_bulk(out, cmd[1]);
        } else {
            resp_reply_error(out, wrong_args("ping"));
        }
        return true;
    }

    // 写命令（SET 族）：set -> 追加 AOF -> +OK
    auto do_set = [&](IStore& store, const char* name) {
        if (argc != 3) {
            resp_reply_error(out, wrong_args(name));
            return true;
        }
        store.set(cmd[1], cmd[2]);
        if (!maybe_append_aof(cmd)) {
            resp_reply_error(out, "aof append failed");
            return false;
        }
        resp_reply_simple(out, "OK");
        return true;
    };

    // 读命令（GET 族）：命中 bulk，未命中 nil
    auto do_get = [&](IStore& store, const char* name) {
        if (argc != 2) {
            resp_reply_error(out, wrong_args(name));
            return true;
        }
        auto v = store.get(cmd[1]);
        if (v) {
            resp_reply_bulk(out, *v);
        } else {
            resp_reply_nil(out);
        }
        return true;
    };

    // 删除命令（DEL 族）：真正删除才追加 AOF，回复删除条数 1/0
    auto do_del = [&](IStore& store, const char* name) {
        if (argc != 2) {
            resp_reply_error(out, wrong_args(name));
            return true;
        }
        bool removed = store.del(cmd[1]);
        if (removed && !maybe_append_aof(cmd)) {
            resp_reply_error(out, "aof append failed");
            return false;
        }
        resp_reply_integer(out, removed ? 1 : 0);
        return true;
    };

    // EXISTS 族
    auto do_exists = [&](IStore& store, const char* name) {
        if (argc != 2) {
            resp_reply_error(out, wrong_args(name));
            return true;
        }
        resp_reply_integer(out, store.exists(cmd[1]) ? 1 : 0);
        return true;
    };

    // ---- 默认命名空间（数组） ----
    if (iequals(op, "SET")) return do_set(array_, "set");
    if (iequals(op, "GET")) return do_get(array_, "get");
    if (iequals(op, "DEL")) return do_del(array_, "del");
    if (iequals(op, "EXISTS")) return do_exists(array_, "exists");

    // ---- Hash 命名空间 ----
    if (iequals(op, "HSET")) return do_set(hash_, "hset");
    if (iequals(op, "HGET")) return do_get(hash_, "hget");
    if (iequals(op, "HDEL")) return do_del(hash_, "hdel");
    if (iequals(op, "HEXISTS")) return do_exists(hash_, "hexists");

    // ---- RBTree 命名空间 ----
    if (iequals(op, "RSET")) return do_set(rbtree_, "rset");
    if (iequals(op, "RGET")) return do_get(rbtree_, "rget");
    if (iequals(op, "RDEL")) return do_del(rbtree_, "rdel");
    if (iequals(op, "REXISTS")) return do_exists(rbtree_, "rexists");

    resp_reply_error(out, "unknown command");
    return true;
}

void Database::shutdown() {
    if (config_.snapshot_enabled && !config_.snapshot_file.empty()) {
        if (snapshot_.save(config_.snapshot_file) == 0) {
            if (!aof_.reset()) {
                std::fprintf(stderr,
                             "graceful shutdown: snapshot ok, aof reset failed\n");
            }
        } else {
            std::fprintf(stderr,
                         "graceful shutdown: snapshot save failed, aof will be fsync'd\n");
        }
    }
    aof_.close();  // 内部 fsync 再 close。
}

}  // namespace tinykv

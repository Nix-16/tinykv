#include "tinykv/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace tinykv {

namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

}  // namespace

bool Config::load_file(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };

    std::ifstream in(path);
    if (!in) {
        return fail("cannot open config file: " + path);
    }

    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;

        // 去掉 '#' 之后的注释。
        auto hash = line.find('#');
        if (hash != std::string::npos) {
            line.erase(hash);
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        // 以首个空白切分 key / value。
        std::istringstream iss(line);
        std::string key;
        iss >> key;
        std::string rest;
        std::getline(iss, rest);
        std::string val = trim(rest);
        if (val.empty()) {
            continue;
        }

        auto bad = [&](const std::string& what) {
            return fail("invalid value for '" + what + "' at line " +
                        std::to_string(lineno) + ": " + val);
        };

        if (key == "bind") {
            bind_ip = val;
        } else if (key == "port") {
            try {
                port = std::stoi(val);
            } catch (...) {
                return bad("port");
            }
        } else if (key == "allocator") {
            if (val == "system") {
                allocator = AllocatorType::System;
            } else if (val == "jemalloc") {
                allocator = AllocatorType::Jemalloc;
            } else {
                return bad("allocator");
            }
        } else if (key == "network") {
            if (val == "reactor") {
                network = NetworkType::Reactor;
            } else if (val == "proactor") {
                network = NetworkType::Proactor;
            } else if (val == "ntyco") {
                network = NetworkType::Ntyco;
            } else {
                return bad("network");
            }
        } else if (key == "appendonly") {
            if (val == "yes") {
                appendonly = true;
            } else if (val == "no") {
                appendonly = false;
            } else {
                return bad("appendonly");
            }
        } else if (key == "appendfilename") {
            appendfilename = val;
        } else if (key == "appendfsync") {
            if (val == "no") {
                appendfsync = AofFsync::No;
            } else if (val == "always") {
                appendfsync = AofFsync::Always;
            } else if (val == "everysec") {
                appendfsync = AofFsync::Everysec;
            } else {
                return bad("appendfsync");
            }
        } else if (key == "snapshot_enabled") {
            if (val == "yes") {
                snapshot_enabled = true;
            } else if (val == "no") {
                snapshot_enabled = false;
            } else {
                return bad("snapshot_enabled");
            }
        } else if (key == "snapshot_file") {
            snapshot_file = val;
        }
        // 未识别项：忽略，与 C 版行为一致。
    }

    return true;
}

}  // namespace tinykv

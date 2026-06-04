#include "tinykv/snapshot.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace tinykv {

namespace {

constexpr uint32_t kVersion = 1;

struct Header {
    char magic[4];
    uint32_t version;
    uint32_t array_count;
    uint32_t hash_count;
    uint32_t rbtree_count;
};

bool write_one_kv(FILE* fp, const std::string& key, const std::string& value) {
    uint32_t klen = static_cast<uint32_t>(key.size());
    uint32_t vlen = static_cast<uint32_t>(value.size());
    if (std::fwrite(&klen, sizeof(klen), 1, fp) != 1) return false;
    if (std::fwrite(&vlen, sizeof(vlen), 1, fp) != 1) return false;
    if (klen && std::fwrite(key.data(), 1, klen, fp) != klen) return false;
    if (vlen && std::fwrite(value.data(), 1, vlen, fp) != vlen) return false;
    return true;
}

bool read_one_kv(FILE* fp, std::string& key, std::string& value) {
    uint32_t klen = 0, vlen = 0;
    if (std::fread(&klen, sizeof(klen), 1, fp) != 1) return false;
    if (std::fread(&vlen, sizeof(vlen), 1, fp) != 1) return false;

    key.resize(klen);
    value.resize(vlen);
    if (klen && std::fread(&key[0], 1, klen, fp) != klen) return false;
    if (vlen && std::fread(&value[0], 1, vlen, fp) != vlen) return false;
    return true;
}

}  // namespace

int Snapshot::save(const std::string& path) const {
    if (path.empty()) {
        return -1;
    }

    std::string tmp = path + ".tmp";
    FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) {
        return -2;
    }

    Header hdr;
    std::memcpy(hdr.magic, "KVS1", 4);
    hdr.version = kVersion;
    hdr.array_count = static_cast<uint32_t>(array_.count());
    hdr.hash_count = static_cast<uint32_t>(hash_.count());
    hdr.rbtree_count = static_cast<uint32_t>(rbtree_.count());

    if (std::fwrite(&hdr, sizeof(hdr), 1, fp) != 1) {
        std::fclose(fp);
        return -3;
    }

    bool ok = true;
    auto sink = [&](const std::string& k, const std::string& v) {
        if (ok && !write_one_kv(fp, k, v)) {
            ok = false;
        }
    };
    array_.for_each(sink);
    hash_.for_each(sink);
    rbtree_.for_each(sink);
    if (!ok) {
        std::fclose(fp);
        return -5;
    }

    std::fflush(fp);
    if (::fsync(::fileno(fp)) != 0) {
        std::fclose(fp);
        return -4;
    }
    if (std::fclose(fp) != 0) {
        return -4;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        return -4;
    }
    return 0;
}

int Snapshot::load(const std::string& path) {
    if (path.empty()) {
        return -1;
    }

    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        return 1;  // 文件不存在：视为正常。
    }

    Header hdr;
    if (std::fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        std::fclose(fp);
        return -3;
    }
    if (std::memcmp(hdr.magic, "KVS1", 4) != 0 || hdr.version != kVersion) {
        std::fclose(fp);
        return -3;
    }

    std::string key, value;
    auto load_into = [&](IStore& store, uint32_t n) -> int {
        for (uint32_t i = 0; i < n; ++i) {
            if (!read_one_kv(fp, key, value)) {
                return -4;
            }
            store.set(key, value);
        }
        return 0;
    };

    int rc;
    if ((rc = load_into(array_, hdr.array_count)) != 0 ||
        (rc = load_into(hash_, hdr.hash_count)) != 0 ||
        (rc = load_into(rbtree_, hdr.rbtree_count)) != 0) {
        std::fclose(fp);
        return rc;
    }

    std::fclose(fp);
    return 0;
}

}  // namespace tinykv

/*
 * 02_cpp — C++ RAII 封装：extern "C" 直接调用 libkvspace dispatch 前端
 *
 * 编译: g++ -std=c++17 02_cpp.cpp -lkvspace
 */

extern "C" {
    #include "kvspace/kvspace.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

struct KV {
    void *kv;
    std::string path;

    KV(const char *p) : path(p) {
        ::unlink(p);
        std::string dsn = "shm://" + path;
        kv = kvspaceConnect(dsn.c_str());
        if (!kv) throw std::runtime_error("connect failed");
    }
    ~KV() { kvspaceClose(kv); ::unlink(path.c_str()); }

    void set(const char *key, const uint8_t *val, uint32_t len) {
        const char *keys[1] = { key };
        const uint32_t lens[1] = { len };
        kvspaceSet(kv, keys, val, lens, 1, nullptr, 0);
    }
    bool get(const char *key, std::string &kind, std::string &raw) {
        uint8_t *out = nullptr; uint32_t len = 0;
        kvspaceGet(kv, key, &out, &len);
        if (!out || len == 0) return false;
        kvspaceHead_t h;
        kvspaceDecodeHead(out, len, &h);
        const char *k = (const char *)h.kindexpr;
        if (k[0] == '*' || k[0] == '@') k++;
        if (k[0] == '[') { const char *e = strchr(k, ']'); if (e) k = e + 1; }
        kind = k;
        raw.assign((const char *)(out + h.body_offset), h.body_len > 0 ? h.body_len : 0);
        kvspaceBytesFree(out, len);
        return true;
    }
    void del(const char *key) {
        const char *keys[1] = { key };
        kvspaceDel(kv, keys, 1, nullptr, 0);
    }
    void deltree(const char *p) { kvspaceDelTree(kv, p, nullptr, 0); }
    void mkindex(const char *p) { kvspaceMkindex(kv, p, nullptr, 0); }
    std::vector<std::string> list(const char *prefix) {
        uint8_t *out = nullptr; uint32_t len = 0;
        kvspaceList(kv, prefix, 0, 1, &out, &len);
        std::vector<std::string> r;
        if (out && len) {
            const char *s = (const char *)out;
            size_t i = 0;
            while (i < len) {
                const void *nl = memchr(s + i, '\n', len - i);
                size_t n = nl ? (size_t)((const char *)nl - (s + i)) : len - i;
                r.push_back(std::string(s + i, n));
                i += n + (nl ? 1 : 0);
            }
            kvspaceBytesFree(out, len);
        }
        return r;
    }
};

int main() {
    auto xv_int = [](int64_t v) {
        uint8_t *b; uint32_t n; kvspaceNewInt64(v, &b, &n);
        return std::pair<uint8_t *, uint32_t>{b, n};
    };
    auto xv_str = [](const char *s) {
        uint8_t *b; uint32_t n; kvspaceNewCharByte((const uint8_t *)s, (uint32_t)strlen(s), &b, &n);
        return std::pair<uint8_t *, uint32_t>{b, n};
    };

    KV kv("/tmp/kvspace_cpp.shm");
    kv.mkindex("/cpp/");

    auto [vi, li] = xv_int(42);
    kv.set("/cpp/a", vi, li);
    free(vi);

    auto [vs, ls] = xv_str("hello");
    kv.set("/cpp/b", vs, ls);
    free(vs);

    std::string kind, raw;
    if (kv.get("/cpp/a", kind, raw))
        printf("/cpp/a  kind=%s val=%ld\n", kind.c_str(), *(const int64_t *)raw.data());
    if (kv.get("/cpp/b", kind, raw))
        printf("/cpp/b  kind=%s val=%.*s\n", kind.c_str(), (int)raw.size(), raw.data());

    auto ns = kv.list("/cpp/");
    printf("list /cpp/:");
    for (auto &n : ns) printf(" %s", n.c_str());
    printf(" (count=%zu)\n", ns.size());

    kv.deltree("/cpp/");
    printf("deltree ok, get /cpp/a: %s\n", kv.get("/cpp/a", kind, raw) ? "EXISTS" : "NULL");

    printf("PASS cpp\n");
    return 0;
}

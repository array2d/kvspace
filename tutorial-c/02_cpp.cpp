/*
 * 02_cpp — C++ RAII 封装：extern "C" 直接调用 libkvspace dispatch 前端
 *
 * head 固定 16 字节 [bodylen:u64@0][attrslen:u16@8][padding:6@10-15]，body 偏移恒 16，
 * langtype 在 body 之后的 attr 区。bodylen 恒为有效字节数，容量归存储后端。
 *
 * 编译: g++ -std=c++17 02_cpp.cpp -lkvspace
 */

extern "C" {
    #include "kvspace/kvspace.h"
    #include <kvspace/xvalue_head.h>
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

    // Write into returned body pointer; WriteNewPlace writes langtype with the header/attr region.
    // 有效长度 = bodylen（head 里没有 layout，也没有 tag 2）；`ref` 由 langtype 的
    // `*`/`@` 前缀隐含，调用方不再单独给 head 位。
    void set(const char *key, const char *langtype, const uint8_t *val, uint32_t len) {
        if (!val || len < KVSPACE_XVALUE_HEAD_LEN) return;
        uint64_t effective = 0;
        if (kvspaceXvalueHeadEffectiveLen(val, len, &effective) != 0) return;
        if (effective > UINT32_MAX) return;
        const uint32_t bl = (uint32_t)effective;
        const uint8_t ref = (langtype[0] == '*')   ? 1U
                          : (langtype[0] == '@')   ? 2U
                                                   : 0U;
        uint8_t *dst = nullptr; char err[256];
        if (kvspaceWriteInPlace(kv, key, 1, bl, &dst, err, sizeof(err)) != 0)
            kvspaceWriteNewPlace(kv, key, ref, 0U, bl, bl, langtype, &dst, err,
                                 sizeof(err));
        if (bl && dst) memcpy(dst, val + KVSPACE_XVALUE_HEAD_LEN, (size_t)bl);
    }
    // Borrowed read: kind 与 body 长度都从**同一条 wire**解出，不经第二套头结构。
    // `kvspaceGet` 拿整条 wire；`xvalue_head.h` 的 inline 访问器读它。
    bool get(const char *key, std::string &kind, std::string &raw) {
        uint8_t *out = nullptr; uint32_t len = 0;
        kvspaceGet(kv, key, 0, &out, &len);
        if (!out || len < KVSPACE_XVALUE_HEAD_LEN) return false;
        const uint8_t *lt = nullptr;
        uint16_t lt_len = 0;
        uint64_t effective = 0;
        if (kvspaceXvalueHeadScan(out, len, &lt, &lt_len) != 0) return false;
        if (kvspaceXvalueHeadEffectiveLen(out, len, &effective) != 0) return false;
        const char *k = (const char *)(const void *)lt;
        uint16_t k_len = lt_len;
        if (k != nullptr && k_len != 0) {
            // 去掉 `*`/`@` 前缀与 `[dims]` 形状段，只留基种类。
            if (k[0] == '*' || k[0] == '@') { k++; k_len--; }
            if (k_len != 0 && k[0] == '[') {
                const char *close = (const char *)memchr(k, ']', k_len);
                if (close != nullptr) {
                    const size_t skip = (size_t)(close - k) + 1U;
                    k += skip;
                    k_len = (uint16_t)(k_len - skip);
                }
            }
        }
        kind.assign(k != nullptr ? k : "", k_len);
        if (effective > len - KVSPACE_XVALUE_HEAD_LEN) return false;
        raw.assign((const char *)(out + KVSPACE_XVALUE_HEAD_LEN),
                   (size_t)effective);
        return true;
    }
    void del(const char *key) {
        const char *keys[1] = { key };
        kvspaceDel(kv, keys, 1, nullptr, 0);
    }
    void deltree(const char *p) { kvspaceDelTree(kv, p, nullptr, 0); }
    void mkindex(const char *p) { kvspaceMkindex(kv, p, 0, nullptr, 0); }
    // 前缀枚举：ListLen 定计数 + 逐 idx ListAt 取名（自备缓冲，读出即拷入 vector）。
    std::vector<std::string> list(const char *prefix) {
        std::vector<std::string> r;
        int32_t count = 0;
        if (kvspaceListLen(kv, prefix, 0, 1, &count) != 0 || count <= 0) return r;
        for (int32_t i = 0; i < count; i++) {
            uint8_t nm[1024]; uint32_t nl = 0;
            if (kvspaceListAt(kv, prefix, 0, 1, i, nm, sizeof(nm), &nl) == 0 && nl > 0)
                r.push_back(std::string((const char *)nm, nl));
        }
        return r;
    }
};

int main() {
    auto xv_int = [](int64_t v) {
        uint8_t *b; uint32_t n; kvspaceNewInt64(v, "int64", &b, &n);
        return std::pair<uint8_t *, uint32_t>{b, n};
    };
    auto xv_str = [](const char *s) {
        uint8_t *b; uint32_t n;
        kvspaceNewChar((const uint8_t *)s, (uint32_t)strlen(s), "char/utf8", &b, &n);
        return std::pair<uint8_t *, uint32_t>{b, n};
    };

    KV kv("/tmp/kvspace_cpp.shm");
    kv.mkindex("/cpp/");

    auto [vi, li] = xv_int(42);
    kv.set("/cpp/a", "int64", vi, li);
    free(vi);

    auto [vs, ls] = xv_str("hello");
    kv.set("/cpp/b", "char/utf8", vs, ls);
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

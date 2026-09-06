/* frontend.c — KVSpace dispatch 前端。
 *
 * 导出与两个后端完全相同的 27 个 kvspace* 符号。运行期按 DSN scheme 用 dlopen
 * （RTLD_NOW | RTLD_LOCAL）装载后端，把 handle 包一层 {vtable, dl, backend}。
 * codec（TlvEncode、DecodeHead、New 等）无 handle，由前端静态实现，byte-identical。
 *
 * 后端装载名：
 *   shm://...           → libkvspace-c.so.1
 *   其余（redis/fs/s3） → libkvspace_durable.so.1
 * 目录由 KVSPACE_BACKEND_PATH 覆盖（默认走动态链接器搜索路径）。
 */

#include "kvspace/kvspace.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 后端 vtable（仅 handle 相关符号；codec 由前端静态实现） ─────────── */

typedef struct {
    void (*free)(void *h);
    int  (*disconnect)(void *h, char *err, uint32_t err_cap);
    int  (*get)(void *h, const char *key, int resolve, uint8_t **out, uint32_t *out_len);
    int  (*writeinplace)(void *h, const char *key, int resolve, uint32_t body_len,
                         uint8_t **body, char *err, uint32_t err_cap);
    int  (*writenewplace)(void *h, const char *key, const char *kindexpr, uint32_t body_len,
                          uint8_t **body, char *err, uint32_t err_cap);
    int  (*listlen)(void *h, const char *prefix, int expand_ext, int resolve, int32_t *out_count);
    int  (*listat)(void *h, const char *prefix, int expand_ext, int resolve, int32_t idx,
                   uint8_t *buf, uint32_t buf_cap, uint32_t *out_len);
    int  (*del)(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap);
    int  (*deltree)(void *h, const char *prefix, char *err, uint32_t err_cap);
    int  (*cp)(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
    int  (*cptree)(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
    int  (*cplist)(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
    int  (*mkindex)(void *h, const char *path, uint32_t capacity, char *err, uint32_t err_cap);
    int  (*mkindexext)(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap);
    int  (*rmindexext)(void *h, const char *path, char *err, uint32_t err_cap);
    int  (*clear)(void *h, char *err, uint32_t err_cap);
    int  (*watch)(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                  uint64_t tick_ns, uint8_t **out, uint32_t *out_len);
} kvspace_vt;

typedef struct {
    kvspace_vt *vt;
    void       *dl;
    void       *backend;
} kvspace_handle;

static kvspace_handle *H(void *h) { return (kvspace_handle *)h; }

/* ── 后端选择 ───────────────────────────────────────────────────────── */

static const char *backend_soname(const char *dsn) {
    return (dsn && strncmp(dsn, "shm://", 6) == 0)
        ? "libkvspace-c.so.1"
        : "libkvspace_durable.so.1";
}

static char *backend_path(const char *soname, char *buf, size_t cap) {
    const char *dir = getenv("KVSPACE_BACKEND_PATH");
    if (!dir || !dir[0]) dir = "/usr/lib/kvspace";
    snprintf(buf, cap, "%s/%s", dir, soname);
    return buf;
}

/* ── 常量查询（const.h 的 X 宏表生成查找表） ───────────────────────── */

#define KVSPACE_ENTRY(name) { #name, name },
static const struct { const char *key; const char *val; } kvspace_consts[] = {
    KVSPACE_KV(KVSPACE_ENTRY)
};
#undef KVSPACE_ENTRY

const char *kvspaceConst(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof kvspace_consts / sizeof kvspace_consts[0]; i++)
        if (strcmp(kvspace_consts[i].key, name) == 0)
            return kvspace_consts[i].val;
    return NULL;
}

/* ── 生命周期 ───────────────────────────────────────────────────────── */

void *kvspaceConnect(const char *dsn) {
    if (!dsn) return NULL;
    char path[4096];
    backend_path(backend_soname(dsn), path, sizeof path);

    void *dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) return NULL;

    kvspace_vt *vt = calloc(1, sizeof(*vt));
    if (!vt) { dlclose(dl); return NULL; }

    #define LOAD(field, name) do {                                  \
        *(void **)&(vt)->field = dlsym(dl, name);                   \
        if (!(vt)->field) { free(vt); dlclose(dl); return NULL; }   \
    } while (0)

    LOAD(free, "kvspaceClose");
    LOAD(disconnect, "kvspaceDisconnect");
    LOAD(get, "kvspaceGet");
    LOAD(writeinplace, "kvspaceWriteInPlace");
    LOAD(writenewplace, "kvspaceWriteNewPlace");
    LOAD(listlen, "kvspaceListLen");
    LOAD(listat, "kvspaceListAt");
    LOAD(del, "kvspaceDel");
    LOAD(deltree, "kvspaceDelTree");
    LOAD(cp, "kvspaceCp");
    LOAD(cptree, "kvspaceCpTree");
    LOAD(cplist, "kvspaceCpList");
    LOAD(mkindex, "kvspaceMkindex");
    LOAD(mkindexext, "kvspaceMkindexExt");
    LOAD(rmindexext, "kvspaceRmindexExt");
    LOAD(clear, "kvspaceClear");
    LOAD(watch, "kvspaceWatch");
    #undef LOAD

    void *(*connect)(const char *) = dlsym(dl, "kvspaceConnect");
    if (!connect) { free(vt); dlclose(dl); return NULL; }
    void *backend = connect(dsn);
    if (!backend) { free(vt); dlclose(dl); return NULL; }

    kvspace_handle *h = malloc(sizeof(*h));
    if (!h) { free(vt); dlclose(dl); return NULL; }
    h->vt = vt; h->dl = dl; h->backend = backend;
    return h;
}

void kvspaceClose(void *h) {
    if (!h) return;
    kvspace_handle *x = H(h);
    if (x->vt->free) x->vt->free(x->backend);
    if (x->dl) dlclose(x->dl);
    free(x->vt);
    free(x);
}

int kvspaceDisconnect(void *h, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->disconnect ? x->vt->disconnect(x->backend, err, err_cap) : 0;
}

/* ── 单点读写 / 目录（trampoline） ─────────────────────────────────── */

int kvspaceGet(void *h, const char *key, int resolve, uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->get(x->backend, key, resolve, out, out_len);
}

int kvspaceWriteInPlace(void *h, const char *key, int resolve, uint32_t body_len,
                        uint8_t **body, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->writeinplace(x->backend, key, resolve, body_len, body, err, err_cap);
}

int kvspaceWriteNewPlace(void *h, const char *key, const char *kindexpr, uint32_t body_len,
                         uint8_t **body, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->writenewplace(x->backend, key, kindexpr, body_len, body, err, err_cap);
}

int kvspaceListLen(void *h, const char *prefix, int expand_ext, int resolve, int32_t *out_count) {
    kvspace_handle *x = H(h);
    return x->vt->listlen(x->backend, prefix, expand_ext, resolve, out_count);
}

int kvspaceListAt(void *h, const char *prefix, int expand_ext, int resolve, int32_t idx,
                  uint8_t *buf, uint32_t buf_cap, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->listat(x->backend, prefix, expand_ext, resolve, idx, buf, buf_cap, out_len);
}

int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->del(x->backend, keys, nkeys, err, err_cap);
}

int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->deltree(x->backend, prefix, err, err_cap);
}

int kvspaceCp(void *h, const char *src, const char *dst, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->cp(x->backend, src, dst, err, err_cap);
}

int kvspaceCpTree(void *h, const char *src, const char *dst, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->cptree(x->backend, src, dst, err, err_cap);
}

int kvspaceCpList(void *h, const char *src, const char *dst, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->cplist(x->backend, src, dst, err, err_cap);
}

int kvspaceMkindex(void *h, const char *path, uint32_t capacity, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->mkindex(x->backend, path, capacity, err, err_cap);
}

int kvspaceMkindexExt(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->mkindexext(x->backend, path, ext_path, err, err_cap);
}

int kvspaceRmindexExt(void *h, const char *path, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->rmindexext(x->backend, path, err, err_cap);
}

int kvspaceClear(void *h, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->clear(x->backend, err, err_cap);
}

int kvspaceWatch(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                 uint64_t tick_ns, uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->watch(x->backend, key, target, target_len, tick_ns, out, out_len);
}

/* ── codec（静态，byte-identical 头格式） ──────────────────────────── */

static void wr_u32(uint8_t *d, uint32_t v) {
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}
static void wr_u64(uint8_t *d, uint64_t v) {
    for (int i = 0; i < 8; i++) d[i] = (uint8_t)(v >> (i * 8));
}
static uint32_t rd_u32(const uint8_t *d) {
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

static int build_kindexpr(char *buf, size_t cap, const char *kind, int ref,
                          const int32_t *dims, int ndim) {
    int o = 0;
    if (ref == 1) buf[o++] = '*';
    else if (ref == 2) buf[o++] = '@';
    if (ndim > 0) {
        buf[o++] = '[';
        for (int i = 0; i < ndim; i++) {
            if (i) buf[o++] = ',';
            o += snprintf(buf + o, cap - (size_t)o, "%d", dims[i]);
        }
        buf[o++] = ']';
    }
    size_t kl = strlen(kind);
    memcpy(buf + o, kind, kl);
    return o + (int)kl;
}

static int encode_head(const char *kind, int ref, int ro, uint32_t vid,
                       const int32_t *dims, int ndim,
                       const uint8_t *raw, uint32_t raw_len,
                       uint8_t **out, uint32_t *out_len) {
    char kx[256];
    int kxl = build_kindexpr(kx, sizeof kx, kind, ref, dims, ndim);
    int slot = kxl + 1;
    uint32_t total = 1u + (uint32_t)slot + 1u + 4u + 4u + raw_len;
    uint8_t *buf = malloc(total);
    if (!buf) return 1;
    buf[0] = (uint8_t)slot;
    memcpy(buf + 1, kx, (size_t)kxl);
    buf[1 + kxl] = 0;
    int o = 1 + slot;
    buf[o] = (uint8_t)(ro ? 1 : 0);
    wr_u32(buf + o + 1, vid);
    wr_u32(buf + o + 5, raw_len);
    if (raw_len && raw) memcpy(buf + o + 9, raw, raw_len);
    *out = buf; *out_len = total;
    return 0;
}

int kvspaceTlvEncode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                     const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len || !kind) return 1;
    if (ndim < 0) ndim = 0;
    if (ndim > 8) return 1;
    return encode_head(kind, 0, 0, 0, dims, ndim, raw, raw_len, out, out_len);
}

int kvspaceTlvEncodeMode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                         const int32_t *dims, int32_t ndim, int32_t ref, uint8_t ro, uint32_t vid,
                         uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len || !kind) return 1;
    if (ndim < 0) ndim = 0;
    if (ndim > 8) return 1;
    return encode_head(kind, ref, ro ? 1 : 0, vid, dims, ndim, raw, raw_len, out, out_len);
}

int kvspaceDecodeHead(const uint8_t *data, uint32_t data_len, kvspaceHead_t *out) {
    if (!out) return 1;
    memset(out, 0, sizeof(*out));
    if (!data || data_len < 1) return 1;
    int slot = data[0];
    int o = 1 + slot;
    if ((uint32_t)o + 9 > data_len) return 1;
    const uint8_t *kx = data + 1;
    int kxl = 0;
    while (kxl < slot && kx[kxl] != 0) kxl++;
    int n = kxl > 255 ? 255 : kxl;
    memcpy(out->kindexpr, kx, (size_t)n);
    out->kindexpr[n] = 0;
    out->ro = data[o] & 1;
    out->vid = rd_u32(data + o + 1);
    out->body_len = (int32_t)rd_u32(data + o + 5);
    out->body_offset = o + 9;
    return 0;
}

/* 指针（ref=1）：head kindexpr = "*" + target_kindexpr（目标完整 kindexpr，含其自身
 * 的引用/形状前缀），body = 目标 key 路径。指针恒标量，不派生 dims。 */
int kvspaceNewPtr(const char *target_kindexpr, const char *target,
                  uint8_t **out, uint32_t *out_len) {
    if (!target_kindexpr || !target || !out || !out_len) return 1;
    return encode_head(target_kindexpr, 1, 0, 0, NULL, 0,
                       (const uint8_t *)target, (uint32_t)strlen(target), out, out_len);
}

int kvspaceNewChar(const uint8_t *bytes, uint32_t len, uint8_t **out, uint32_t *out_len) {
    if (!bytes || !out || !out_len) return 1;
    int32_t d[1] = { (int32_t)len };
    return encode_head(KVSPACE_KIND_CHAR_UTF8, 0, 0, 0, d, 1, bytes, len, out, out_len);
}

int kvspaceNewBool(uint8_t v, uint8_t **out, uint32_t *out_len) {
    uint8_t b = v ? 1 : 0;
    return encode_head(KVSPACE_KIND_BOOL, 0, 0, 0, NULL, 0, &b, 1, out, out_len);
}

int kvspaceNewInt64(int64_t v, uint8_t **out, uint32_t *out_len) {
    uint8_t b[8]; wr_u64(b, (uint64_t)v);
    return encode_head(KVSPACE_KIND_INT64, 0, 0, 0, NULL, 0, b, 8, out, out_len);
}

int kvspaceNewFloat64(double v, uint8_t **out, uint32_t *out_len) {
    uint8_t b[8];
    uint64_t bits; memcpy(&bits, &v, 8);
    wr_u64(b, bits);
    return encode_head(KVSPACE_KIND_FLOAT64, 0, 0, 0, NULL, 0, b, 8, out, out_len);
}

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
    int  (*get)(void *h, const char *key, int resolve, uint8_t **out, uint32_t *out_len);
    void (*readreset)(void *h);
    int  (*getpart)(void *h, const char *key, uint32_t offset, uint32_t len,
                     uint8_t **out, uint32_t *out_len);
    int  (*setpart)(void *h, const char *key, uint32_t offset, const uint8_t *buf,
                     uint32_t buf_len, char *err, uint32_t err_cap);
    int  (*gethead)(void *h, const char *key, kvspaceHead_t *out);
    int  (*writeinplace)(void *h, const char *key, int resolve, uint32_t body_len,
                         uint8_t **body, char *err, uint32_t err_cap);
    int  (*writenewplace)(void *h, const char *key, uint8_t ref, uint8_t storetype,
                          uint8_t ro, uint32_t vid, const char *langtype, uint32_t body_len,
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
    LOAD(get, "kvspaceGet");
    LOAD(readreset, "kvspaceReadReset");
    LOAD(getpart, "kvspaceGetPart");
    LOAD(setpart, "kvspaceSetPart");
    LOAD(gethead, "kvspaceGetHead");
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

/* ── 单点读写 / 目录（trampoline） ─────────────────────────────────── */

int kvspaceGet(void *h, const char *key, int resolve, uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->get(x->backend, key, resolve, out, out_len);
}

void kvspaceReadReset(void *h) {
    kvspace_handle *x = H(h);
    if (x->vt->readreset) x->vt->readreset(x->backend);
}

int kvspaceGetPart(void *h, const char *key, uint32_t offset, uint32_t len,
                    uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->getpart(x->backend, key, offset, len, out, out_len);
}

int kvspaceSetPart(void *h, const char *key, uint32_t offset, const uint8_t *buf,
                    uint32_t buf_len, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->setpart(x->backend, key, offset, buf, buf_len, err, err_cap);
}

int kvspaceGetHead(void *h, const char *key, kvspaceHead_t *out) {
    kvspace_handle *x = H(h);
    return x->vt->gethead(x->backend, key, out);
}

int kvspaceWriteInPlace(void *h, const char *key, int resolve, uint32_t body_len,
                        uint8_t **body, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->writeinplace(x->backend, key, resolve, body_len, body, err, err_cap);
}

int kvspaceWriteNewPlace(void *h, const char *key, uint8_t ref, uint8_t storetype,
                         uint8_t ro, uint32_t vid, const char *langtype, uint32_t body_len,
                         uint8_t **body, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->writenewplace(x->backend, key, ref, storetype, ro, vid, langtype, body_len, body, err, err_cap);
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

/* ── codec（静态，byte-identical 三轴 wire）─────────────────────────
 *
 * head = [headlen u16 LE][ref u8][storetype u8][ro u8][vid u32 LE][body_len u32 LE]
 *        [storetype 物理字段][langtype kindexpr 串（占至 headlen）]
 * 固定前缀 HEAD_PREFIX = 13 字节。物理字段：ARRAYND / index / extindex 为 ndim u8 + dims[ndim] u32 LE
 * （index/extindex 的 dims=[len,cap,M]）；NONE / ATOM 无物理字段。langtype 无独立长度字段，占至 headlen。
 * 本前端只产 NONE/ATOM/ARRAYND/ptr；index/extindex 成员矩阵由后端 mkindex 构建，DecodeHead 统一解。
 */

#define HEAD_PREFIX 13u

static void wr_u16(uint8_t *d, uint16_t v) {
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
}
static void wr_u32(uint8_t *d, uint32_t v) {
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}
static void wr_u64(uint8_t *d, uint64_t v) {
    for (int i = 0; i < 8; i++) d[i] = (uint8_t)(v >> (i * 8));
}
static uint16_t rd_u16(const uint8_t *d) {
    return (uint16_t)((uint16_t)d[0] | ((uint16_t)d[1] << 8));
}
static uint32_t rd_u32(const uint8_t *d) {
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

/* ARRAYND / index / extindex 携带 ndim+dims 物理字段。 */
static int store_has_dims(uint8_t st) {
    return st == KVSPACE_STORETYPE_ARRAYND
        || st == KVSPACE_STORETYPE_INDEX
        || st == KVSPACE_STORETYPE_EXTINDEX;
}

/* langtype 串（ARRAYND 含 [dims]，其余为裸种类名/路径）。 */
static int build_langtype(char *buf, size_t cap, const char *kind, uint8_t storetype,
                          const int32_t *dims, int ndim) {
    int o = 0;
    if (storetype == KVSPACE_STORETYPE_ARRAYND && ndim > 0) {
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

/* 由 base 种类名（+ndim）推 storetype——便利编码函数用；WriteNewPlace 直接收 storetype。 */
/* map langtype：`{memitemkeylangtype}·{memitemvaluelangtype}`（见 spec [[map容器]]）。值容器物理
 * 布局恒 index（成员名索引落兄弟槽 `{key}·`）——`·` 之前的方括号是键类型，绝非维度。 */
static int is_map_langtype(const char *kind) {
    return kind && strstr(kind, KVSPACE_MEMBER_SEP) != NULL;
}

static int is_index_kind(const char *kind) {
    return strcmp(kind, KVSPACE_KIND_INDEX) == 0
        || strcmp(kind, KVSPACE_KIND_EXT_INDEX) == 0
        || strcmp(kind, KVSPACE_KIND_RWFUNC) == 0
        || strcmp(kind, KVSPACE_KIND_DEF_RWIR) == 0;
}
static uint8_t storetype_of(const char *kind, int ndim) {
    if (!kind || !kind[0]) return KVSPACE_STORETYPE_NONE;
    if (strcmp(kind, KVSPACE_KIND_EXT_INDEX) == 0) return KVSPACE_STORETYPE_EXTINDEX;
    if (is_index_kind(kind) || is_map_langtype(kind)) return KVSPACE_STORETYPE_INDEX;
    if (ndim > 0) return KVSPACE_STORETYPE_ARRAYND;
    return KVSPACE_STORETYPE_ATOM;
}

/* 指针 head 的 storetype = 目标语义 storetype（据目标完整 kindexpr 推；指针自身物理字段恒空）。 */
static uint8_t storetype_from_kindexpr(const char *kx) {
    if (!kx || !kx[0]) return KVSPACE_STORETYPE_NONE;
    const char *base = kx;
    int has_dims = 0;
    if (*base == '[') {
        const char *p = strchr(base, ']');
        if (p) { base = p + 1; has_dims = 1; }
    }
    if (strcmp(base, KVSPACE_KIND_EXT_INDEX) == 0) return KVSPACE_STORETYPE_EXTINDEX;
    if (is_index_kind(base) || base[0] == '/' || strstr(base, KVSPACE_MEMBER_SEP))
        return KVSPACE_STORETYPE_INDEX;
    if (has_dims) return KVSPACE_STORETYPE_ARRAYND;
    return KVSPACE_STORETYPE_ATOM;
}

/* 核心编码：写三轴 head + body。dims 仅在 store_has_dims 时落物理字段。 */
static int encode_head(uint8_t ref, uint8_t storetype, const char *langtype,
                       uint8_t ro, uint32_t vid, const int32_t *dims, int ndim,
                       const uint8_t *body, uint32_t body_len,
                       uint8_t **out, uint32_t *out_len) {
    if (!store_has_dims(storetype)) ndim = 0;
    if (ndim < 0) ndim = 0;
    if (ndim > 8) return 1;
    size_t lt_len = langtype ? strlen(langtype) : 0;
    size_t phys = store_has_dims(storetype) ? (1u + 4u * (size_t)ndim) : 0;
    size_t headlen = HEAD_PREFIX + phys + lt_len;
    if (headlen > 0xFFFF) return 1;
    uint8_t *buf = malloc(headlen + body_len);
    if (!buf) return 1;
    wr_u16(buf, (uint16_t)headlen);
    buf[2] = ref;
    buf[3] = storetype;
    buf[4] = (uint8_t)(ro ? 1 : 0);
    wr_u32(buf + 5, vid);
    wr_u32(buf + 9, body_len);
    size_t o = HEAD_PREFIX;
    if (store_has_dims(storetype)) {
        buf[o++] = (uint8_t)ndim;
        for (int i = 0; i < ndim; i++) { wr_u32(buf + o, (uint32_t)dims[i]); o += 4; }
    }
    if (lt_len) memcpy(buf + o, langtype, lt_len);
    if (body_len && body) memcpy(buf + headlen, body, body_len);
    *out = buf; *out_len = (uint32_t)(headlen + body_len);
    return 0;
}

int kvspaceTlvEncode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                     const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len) {
    return kvspaceTlvEncodeMode(kind, raw, raw_len, dims, ndim, 0, 0, 0, out, out_len);
}

int kvspaceTlvEncodeMode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                         const int32_t *dims, int32_t ndim, int32_t ref, uint8_t ro, uint32_t vid,
                         uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len || !kind) return 1;
    if (ndim < 0) ndim = 0;
    if (ndim > 8) return 1;
    uint8_t storetype = storetype_of(kind, ndim);
    char lt[256];
    int ltl = build_langtype(lt, sizeof lt, kind, storetype, dims, ndim);
    lt[ltl] = 0;
    return encode_head((uint8_t)ref, storetype, lt, ro ? 1 : 0, vid, dims, ndim,
                       raw, raw_len, out, out_len);
}

int kvspaceDecodeHead(const uint8_t *data, uint32_t data_len, kvspaceHead_t *out) {
    if (!out) return 1;
    memset(out, 0, sizeof(*out));
    if (!data || data_len < HEAD_PREFIX) return 1;
    uint32_t headlen = rd_u16(data);
    if (headlen < HEAD_PREFIX || headlen > data_len) return 1;
    out->headlen   = (uint16_t)headlen;
    out->ref       = data[2];
    out->storetype = data[3];
    out->ro        = data[4] & 1;
    out->vid       = rd_u32(data + 5);
    out->body_len  = (int32_t)rd_u32(data + 9);
    uint32_t o = HEAD_PREFIX;
    if (store_has_dims(out->storetype)) {
        if (o + 1 > headlen) return 1;
        int nd = data[o++];
        if (nd > 8 || o + 4u * (uint32_t)nd > headlen) return 1;
        out->ndim = nd;
        for (int i = 0; i < nd; i++) { out->dims[i] = (int32_t)rd_u32(data + o); o += 4; }
    }
    uint32_t lt_len = headlen - o;
    int n = lt_len > 255 ? 255 : (int)lt_len;
    memcpy(out->langtype, data + o, (size_t)n);
    out->langtype[n] = 0;
    out->langtype_len = n;
    out->body_offset = (int32_t)headlen;
    return 0;
}

/* 指针（ref=1）：langtype = target_kindexpr（目标完整 kindexpr）、storetype = 目标语义 storetype、
 * body = 目标 key 路径。指针自身物理字段恒空（ndim=0），不复制目标 dims。 */
int kvspaceNewPtr(const char *target_kindexpr, const char *target,
                  uint8_t **out, uint32_t *out_len) {
    if (!target_kindexpr || !target || !out || !out_len) return 1;
    uint8_t storetype = storetype_from_kindexpr(target_kindexpr);
    return encode_head(KVSPACE_REF_PTR, storetype, target_kindexpr, 0, 0, NULL, 0,
                       (const uint8_t *)target, (uint32_t)strlen(target), out, out_len);
}

int kvspaceNewChar(const uint8_t *bytes, uint32_t len, uint8_t **out, uint32_t *out_len) {
    if (!bytes || !out || !out_len) return 1;
    int32_t d[1] = { (int32_t)len };
    return kvspaceTlvEncode(KVSPACE_KIND_CHAR_UTF8, bytes, len, d, 1, out, out_len);
}

int kvspaceNewBool(uint8_t v, uint8_t **out, uint32_t *out_len) {
    uint8_t b = v ? 1 : 0;
    return kvspaceTlvEncode(KVSPACE_KIND_BOOL, &b, 1, NULL, 0, out, out_len);
}

int kvspaceNewInt64(int64_t v, uint8_t **out, uint32_t *out_len) {
    uint8_t b[8]; wr_u64(b, (uint64_t)v);
    return kvspaceTlvEncode(KVSPACE_KIND_INT64, b, 8, NULL, 0, out, out_len);
}

int kvspaceNewFloat64(double v, uint8_t **out, uint32_t *out_len) {
    uint8_t b[8];
    uint64_t bits; memcpy(&bits, &v, 8);
    wr_u64(b, bits);
    return kvspaceTlvEncode(KVSPACE_KIND_FLOAT64, b, 8, NULL, 0, out, out_len);
}

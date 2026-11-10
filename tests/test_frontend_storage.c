/* test_frontend_storage.c — 新存储 ABI 前端 dispatch：参数转发 + 缺符号行为。
 *
 * 两个 fake 后端（同一份 tests/fake_backend.c 编两次）：
 *   - `libfakebackend.so`       导出新 ABI 九个符号 → 验「参数原样转发」；
 *   - `libfakebackend_bare.so`  **不导出**它们      → 验「缺符号返回 KVSPACE_BACKEND」。
 * 判决只看**假后端记录下来的入参**，不看前端自报——前端说自己转了不算数。
 *
 * 本文件只 include storage.h：与旧 kvspace.h 有重名声明，同 TU 无法共存。 */

#include <kvspace/kvspace.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 前端必须 dlopen 进来，不能链接 ────────────────────────────────────
 *
 * 若本可执行文件直接链接 libkvspace，ld.so 就把它放进**全局作用域**。之后前端
 * 对 bare 后端做 `dlsym(dl, "kvspaceResolve")` 时，dlsym 会先查该 .so 及其依赖，
 * 再查全局作用域——于是命中**前端自己**导出的 kvspaceResolve，递归调用、当场段错。
 *
 * 那不是产品缺陷：真实部署里后端 .so 不链接前端，全局作用域里没有第二个
 * kvspaceResolve。这是「测试把自己也编进了被测对象的查找范围」造成的假象。
 * 所以这里用 RTLD_LOCAL dlopen 前端，让它的符号不进全局作用域，dlsym 才只看后端。 */

static struct {
    void *(*connect)(const char *);
    void (*close)(void *);
    uint8_t *(*data_base)(void *);
    int (*resolve)(void *, const char *, uint64_t, uint64_t *, char *, uint32_t);
    int (*realloc)(void *, const char *, uint64_t, uint64_t, uint64_t *, char *,
                   uint32_t);
    int (*free_)(void *, const char *, char *, uint32_t);
    int (*cp)(void *, const char *, const char *, char *, uint32_t);
    int (*mkindex)(void *, const char *, char *, uint32_t);
    int (*deltree)(void *, const char *, char *, uint32_t);
    int (*cpdir)(void *, const char *, const char *, char *, uint32_t);
    int (*listlen)(void *, const char *, int64_t *, uint32_t *, char *, uint32_t);
    int (*list)(void *, const char *, char *, uint32_t, uint32_t *, uint32_t *,
                uint64_t *, char *, uint32_t);
} api;

#define SYM(field, name)                                                       \
    do {                                                                       \
        *(void **)&api.field = dlsym(frontend, name);                           \
        if (!api.field) {                                                       \
            fprintf(stderr, "frontend missing %s\n", name);                     \
            return 2;                                                           \
        }                                                                       \
    } while (0)

static int bind_frontend(const char *path) {
    void *frontend = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!frontend) {
        fprintf(stderr, "dlopen frontend %s: %s\n", path, dlerror());
        return -1;
    }
    SYM(connect, "kvspaceConnect");
    SYM(close, "kvspaceClose");
    SYM(data_base, "kvspaceXvalueInProcessAddress");
    SYM(resolve, "kvspaceResolve");
    SYM(realloc, "kvspaceRealloc");
    SYM(free_, "kvspaceFree");
    SYM(cp, "kvspaceCp");
    SYM(mkindex, "kvspaceMkindex");
    SYM(deltree, "kvspaceDeltree");
    SYM(cpdir, "kvspaceCpdir");
    SYM(listlen, "kvspaceListlen");
    SYM(list, "kvspaceList");
#undef SYM
    return 0;
}

static int failures;

#define CHECK(condition, ...)                                                 \
    do {                                                                      \
        if (!(condition)) {                                                   \
            failures++;                                                       \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "\n");                                            \
        }                                                                     \
    } while (0)

/* 与 fake_backend.c 的 struct recorded 逐字段对应的影子副本。布局必须一致：
 * 两边都是「八个 int 计数 + key[256] + 三个 u64 + 一个 u32 + 一个指针」，
 * 这里用同样的顺序声明一遍。 */
struct recorded {
    int resolve_calls, realloc_calls, free_calls, makeindex_calls;
    int deltree_calls, cpdir_calls, listlen_calls, list_calls;
    char key[256];
    uint64_t anchor;
    uint64_t body_length, attr_length;
    uint32_t list_capacity;
    void *backend_seen;
};

/* 记录在假后端 .so 里，不是本可执行文件的符号：必须 dlsym 取地址。
 * 顺带钉住一件事——读到的必须是**前端真加载的那个**对象；若哪天前端换了加载
 * 路径（RTLD_LOCAL 的另一个副本），这里拿到的会是另一份记录，测试立刻显形。 */
static struct recorded *g_recorded_ptr;

static void bind_recorded(const char *soname) {
    void *dl = dlopen(soname, RTLD_NOW | RTLD_NOLOAD);
    if (!dl)
        return;
    g_recorded_ptr = (struct recorded *)dlsym(dl, "g_recorded");
    dlclose(dl);
}

#define g_recorded (*g_recorded_ptr)

#define FAKE_VALUE_OFFSET 0x1122334455667788ULL

static void reset(void) { memset(&g_recorded, 0, sizeof g_recorded); }

/* fake_backend 的 kvspaceConnect 返回这个值；前端会把它当 backend 传给每个调用，
 * 所以「假后端收到的 h」必须等于它，才算前端真把后端句柄转对了。 */
#define FAKE_BACKEND_HANDLE ((void *)(uintptr_t)0xBACC)

static void expect_key(const char *want, const char *who) {
    CHECK(strcmp(g_recorded.key, want) == 0, "%s: key 转成了 \"%s\"，应为 \"%s\"",
          who, g_recorded.key, want);
}

/* ── 一、满配后端：九个函数逐个验转发 ──────────────────────────────── */

static void test_forwarding(void *h) {
    uint64_t offset = 0;

    /* 基址：满配后端提供了该符号，前端必须把非 NULL 的基址转出来。
     * 若前端漏 dlsym 或错写成返回 NULL，调用方就永远读不到值。 */
    reset();
    CHECK(api.data_base(h) != NULL, "DataBase 没转出非 NULL 基址");

    reset();
    CHECK(api.resolve(h, "/a", 0xDEADBEEFULL, &offset, NULL, 0) == 0,
          "resolve 返回非 0");
    CHECK(g_recorded.resolve_calls == 1, "resolve 未被调用");
    expect_key("/a", "resolve");
    CHECK(g_recorded.anchor == 0xDEADBEEFULL,
          "resolve: anchor 转成 %llu", (unsigned long long)g_recorded.anchor);
    CHECK(offset == FAKE_VALUE_OFFSET,
          "resolve: out_value_offset 没转回来（%llx）",
          (unsigned long long)offset);
    CHECK(g_recorded.backend_seen == FAKE_BACKEND_HANDLE,
          "resolve: 后端句柄没转对");

    reset();
    CHECK(api.realloc(h, "/b", 64, 12, &offset, NULL, 0) == 0,
          "realloc 返回非 0");
    CHECK(g_recorded.realloc_calls == 1, "realloc 未被调用");
    expect_key("/b", "realloc");
    CHECK(g_recorded.body_length == 64 && g_recorded.attr_length == 12,
          "realloc: body/attr 长度转成 %llu/%llu",
          (unsigned long long)g_recorded.body_length,
          (unsigned long long)g_recorded.attr_length);

    reset();
    CHECK(api.free_(h, "/c", NULL, 0) == 0, "free 返回非 0");
    CHECK(g_recorded.free_calls == 1, "free 未被调用");
    expect_key("/c", "free");

    /* kvspaceCp 由旧 frontend.c 导出（新 ABI 复用同一符号），它走旧 vt，
     * 假后端里也有一份 kvspaceCp 占位，这里只验它能被调到、不崩。 */
    CHECK(api.cp(h, "/old", "/new", NULL, 0) == 0, "cp 返回非 0");

    reset();
    CHECK(api.mkindex(h, "/d/", NULL, 0) == 0, "makeindex 返回非 0");
    CHECK(g_recorded.makeindex_calls == 1, "makeindex 未被调用");
    expect_key("/d/", "makeindex");

    reset();
    CHECK(api.deltree(h, "/e/", NULL, 0) == 0, "deltree 返回非 0");
    CHECK(g_recorded.deltree_calls == 1, "deltree 未被调用");
    expect_key("/e/", "deltree");

    reset();
    CHECK(api.cpdir(h, "/src/", "/dst/", NULL, 0) == 0, "cpdir 返回非 0");
    CHECK(g_recorded.cpdir_calls == 1, "cpdir 未被调用");
    expect_key("/src/", "cpdir");

    int64_t count = -1;
    uint32_t max_name = 0;
    reset();
    CHECK(api.listlen(h, "/f/", &count, &max_name, NULL, 0) == 0,
          "listlen 返回非 0");
    CHECK(g_recorded.listlen_calls == 1, "listlen 未被调用");
    expect_key("/f/", "listlen");
    CHECK(count == 7 && max_name == 13, "listlen: N/M 转成 %lld/%u",
          (long long)count, max_name);

    char names[16] = {0};
    uint32_t length = 0, listed = 0;
    uint64_t generation = 0;
    reset();
    CHECK(api.list(h, "/g/", names, sizeof names, &length, &listed,
                      &generation, NULL, 0) == 0,
          "list 返回非 0");
    CHECK(g_recorded.list_calls == 1, "list 未被调用");
    expect_key("/g/", "list");
    CHECK(g_recorded.list_capacity == sizeof names,
          "list: out_capacity 转成 %u", g_recorded.list_capacity);
    CHECK(length == 3 && listed == 2 && generation == 42,
          "list: length/count/generation 转成 %u/%u/%llu", length, listed,
          (unsigned long long)generation);
    CHECK(memcmp(names, "a\0b", 3) == 0, "list: out_names 没转回来");
}

/* ── 二、裸后端（不导出新符号）：每个函数都必须 KVSPACE_BACKEND ─────── */

static void test_missing_symbols(void *h) {
    char err[256];
    uint64_t offset = 0;
    int64_t count = 0;
    uint32_t max_name = 0, length = 0, listed = 0;
    uint64_t generation = 0;
    char names[16] = {0};

    /* 地址访问器没有 err 通道：缺符号就是返回 NULL，不许拿假基址糊过去。
     *
     * 这里验的**正是兜底路径本身**，所以「返回 NULL」是期望结果。但光断言 NULL
     * 不够——NULL 也可能因为别的原因（vt 分配失败、backend 为空）而出现，那时
     * 测的就不是「缺符号兜底」了。所以先直接确认裸后端确实不导出该符号。 */
    void *bare_dl = dlopen("libkvspace_durable.so.1", RTLD_NOW | RTLD_NOLOAD);
    CHECK(bare_dl != NULL, "无法打开已加载的裸后端以核对符号缺失");
    if (bare_dl) {
        CHECK(dlsym(bare_dl, "kvspaceXvalueInProcessAddress") == NULL,
              "裸版 fake backend 不该导出 kvspaceXvalueInProcessAddress，否则本用例测错了对象");
        dlclose(bare_dl);
    }
    CHECK(api.data_base(h) == NULL, "DataBase 缺符号应返回 NULL");

    /* 每个用例都单独清 err：只验「返回值对」会漏掉「err 没写」，
     * 而调用方正是靠 err 知道该去升级哪个后端。 */
    memset(err, 0, sizeof err);
    CHECK(api.resolve(h, "/a", 0, &offset, err, sizeof err) == KVSPACE_BACKEND,
          "resolve 缺符号没返回 KVSPACE_BACKEND");
    CHECK(strstr(err, "kvspaceResolve") != NULL,
          "resolve 缺符号没写 err（err=\"%s\"）", err);

    memset(err, 0, sizeof err);
    CHECK(api.realloc(h, "/b", 1, 1, &offset, err, sizeof err) == KVSPACE_BACKEND,
          "realloc 缺符号没返回 KVSPACE_BACKEND");
    CHECK(strstr(err, "kvspaceRealloc") != NULL,
          "realloc 缺符号没写 err（err=\"%s\"）", err);

    memset(err, 0, sizeof err);
    CHECK(api.free_(h, "/c", err, sizeof err) == KVSPACE_BACKEND,
          "free 缺符号没返回 KVSPACE_BACKEND");
    CHECK(strstr(err, "kvspaceFree") != NULL,
          "free 缺符号没写 err（err=\"%s\"）", err);

    memset(err, 0, sizeof err);
    CHECK(api.mkindex(h, "/d/", err, sizeof err) == KVSPACE_BACKEND,
          "makeindex 缺符号没返回 KVSPACE_BACKEND");
    CHECK(strstr(err, "kvspaceMkindex") != NULL,
          "makeindex 缺符号没写 err（err=\"%s\"）", err);

    memset(err, 0, sizeof err);
    CHECK(api.deltree(h, "/e/", err, sizeof err) == KVSPACE_BACKEND,
          "deltree 缺符号没返回 KVSPACE_BACKEND");
    CHECK(strstr(err, "kvspaceDeltree") != NULL,
          "deltree 缺符号没写 err（err=\"%s\"）", err);

    memset(err, 0, sizeof err);
    CHECK(api.cpdir(h, "/s/", "/d/", err, sizeof err) == KVSPACE_BACKEND,
          "cpdir 缺符号没返回 KVSPACE_BACKEND");
    CHECK(strstr(err, "kvspaceCpdir") != NULL,
          "cpdir 缺符号没写 err（err=\"%s\"）", err);

    memset(err, 0, sizeof err);
    CHECK(api.listlen(h, "/f/", &count, &max_name, err, sizeof err) ==
              KVSPACE_BACKEND,
          "listlen 缺符号没返回 KVSPACE_BACKEND");
    CHECK(strstr(err, "kvspaceListlen") != NULL,
          "listlen 缺符号没写 err（err=\"%s\"）", err);

    memset(err, 0, sizeof err);
    CHECK(api.list(h, "/g/", names, sizeof names, &length, &listed,
                      &generation, err, sizeof err) == KVSPACE_BACKEND,
          "list 缺符号没返回 KVSPACE_BACKEND");
    CHECK(strstr(err, "kvspaceList") != NULL,
          "list 缺符号没写 err（err=\"%s\"）", err);
}

/* ── 三、Connect 成功与否不受新符号缺失影响（旧 ABI 完全无感） ──────── */

static void test_connect_unaffected(void *h) {
    CHECK(h != NULL, "裸后端下 Connect 失败：新符号缺失不该影响连接");
}

/* ── 四、探测闸：旧五参后端必须被拦下，而不是被当新四参调用 ──────────
 *
 * 后端导出了 `kvspaceMkindex`，但是**旧五参**签名，且没有任何新 ABI 独有符号。
 * 动态链接不查签名：没有闸时前端会把旧指针按新四参调用，参数错位后崩溃。
 *
 * 这个用例能跑完并断言 KVSPACE_BACKEND，就证明闸在调用之前生效了——如果闸失效，
 * 进程会在第一次 kvspaceMkindex 调用时直接段错，根本走不到 CHECK。 */
/* 旧后端的加载路径：main 从 argv[3] 拿到后存这里，供符号核对复用。 */
static const char *g_legacy_backend_path;

static void test_legacy_backend_gated(void *h) {
    char err[512];
    uint64_t offset = 0;
    int64_t count = 0;
    uint32_t max_name = 0, length = 0, listed = 0;
    uint64_t generation = 0;
    char names[16] = {0};

    /* 先确认这个后端确实导出了共名符号——否则本用例测的是「缺符号」，
     * 不是「闸拦住了旧签名」，两者结论完全不同。
     *
     * 这里**再 dlopen 一次**（不带 NOLOAD）：该库是前端按绝对路径加载的，
     * RTLD_NOLOAD 按 soname 匹配找不到它。多一次引用只增加计数，不影响被测
     * 行为——核对的是「这个文件里到底有什么符号」，与前端怎么加载无关。 */
    void *dl = dlopen(g_legacy_backend_path, RTLD_NOW | RTLD_LOCAL);
    CHECK(dl != NULL, "无法打开假旧后端以核对符号（%s）: %s",
          g_legacy_backend_path, dlerror());
    int *legacy_calls = NULL;
    if (dl) {
        CHECK(dlsym(dl, "kvspaceMkindex") != NULL,
              "假旧后端应导出共名的 kvspaceMkindex（本用例前提）");
        CHECK(dlsym(dl, "kvspaceResolve") == NULL,
              "假旧后端不该导出 kvspaceResolve（否则闸不会被触发）");
        legacy_calls = (int *)dlsym(dl, "g_legacy_mkindex_calls");
        CHECK(legacy_calls != NULL, "假旧后端缺少调用计数器，本用例无法断言");
        if (legacy_calls)
            *legacy_calls = 0;
        /* 不 dlclose：计数器地址要一直有效到断言完成。 */
    }

    memset(err, 0, sizeof err);
    CHECK(api.mkindex(h, "/gated/", err, sizeof err) == KVSPACE_BACKEND,
          "旧签名后端上 kvspaceMkindex 必须返回 KVSPACE_BACKEND（闸生效）");
    CHECK(strstr(err, "new storage ABI") != NULL,
          "闸拦下时 err 应指出后端没有新 ABI（err=\"%s\"）", err);
    if (legacy_calls)
        CHECK(*legacy_calls == 0,
              "闸失效：旧五参 kvspaceMkindex 被调用了 %d 次（应 0）",
              *legacy_calls);

    /* 其余新 ABI 函数同样必须被拦下。 */
    memset(err, 0, sizeof err);
    CHECK(api.resolve(h, "/a", 0, &offset, err, sizeof err) == KVSPACE_BACKEND,
          "旧后端上 resolve 必须返回 KVSPACE_BACKEND");
    memset(err, 0, sizeof err);
    CHECK(api.realloc(h, "/b", 1, 1, &offset, err, sizeof err) == KVSPACE_BACKEND,
          "旧后端上 realloc 必须返回 KVSPACE_BACKEND");
    memset(err, 0, sizeof err);
    CHECK(api.listlen(h, "/c/", &count, &max_name, err, sizeof err) ==
              KVSPACE_BACKEND,
          "旧后端上 listlen 必须返回 KVSPACE_BACKEND");
    memset(err, 0, sizeof err);
    CHECK(api.list(h, "/d/", names, sizeof names, &length, &listed,
                      &generation, err, sizeof err) == KVSPACE_BACKEND,
          "旧后端上 list 必须返回 KVSPACE_BACKEND");
}

/* DSN 的 scheme 决定加载哪个 soname（生产契约，测试不改它）：
 *   shm://…  → libkvspace-c.so.1     ← CMake 把 fake_backend(full) 装成这个名字
 *   其他     → libkvspace_durable.so.1 ← CMake 把 fake_backend(bare) 装成这个名字
 * 两者都在 libkvspace 同目录，前端走「候选 1」的正规路径找到它们。 */
/* 满配后端与裸后端**必须分进程**：两者都在全局链里时，前端对裸后端 dlsym 会命中
 * 满配后端那份 kvspaceResolve（dlsym 先查该 .so 与依赖，再查全局链），于是「缺符号
 * 走 KVSPACE_BACKEND」的路径根本测不到，反而调进满配实现。同一进程里两个同名导出
 * 只有一个能被 dlsym 拿到——这是 dlsym 语义，不是可以绕开的测试细节。
 *
 *   ./test_frontend_storage <libkvspace> forwarding   → 满配：验参数转发
 *   ./test_frontend_storage <libkvspace> missing      → 裸版：验缺符号
 * ctest 起两个用例，各一进程。 */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <libkvspace.so path> <forwarding|missing>\n",
                argv[0]);
        return 2;
    }
    if (bind_frontend(argv[1]) != 0)
        return 2;

    if (strcmp(argv[2], "forwarding") == 0) {
        void *full = api.connect("shm://frontend-storage-test");
        if (!full) {
            fprintf(stderr, "connect full backend (shm://) failed\n");
            return 2;
        }
        bind_recorded("libkvspace-c.so.1");
        if (!g_recorded_ptr) {
            fprintf(stderr, "cannot locate g_recorded in the loaded backend\n");
            return 2;
        }
        test_forwarding(full);
        api.close(full);
    } else if (strcmp(argv[2], "missing") == 0) {
        void *bare = api.connect("fs://frontend-storage-test");
        if (!bare) {
            fprintf(stderr, "connect bare backend (fs://) failed\n");
            return 2;
        }
        test_connect_unaffected(bare);
        test_missing_symbols(bare);
        api.close(bare);
    } else if (strcmp(argv[2], "legacy-mkindex") == 0) {
        /* 探测闸的回归用例：后端导出**旧五参** kvspaceMkindex、但没有任何新 ABI
         * 独有符号。闸若失效，前端会把这个指针按新四参调用 → 参数错位。
         *
         * 两个必须同时成立：
         *   1. 旧后端的 kvspaceMkindex **一次都没被调用**（计数器为 0）；
         *   2. 新 ABI 调用返回 KVSPACE_BACKEND 并写明「后端没有新 ABI」。
         * 只断言 2 不够：错调之后栈上剩什么全凭运气，返回值可能碰巧也是 BACKEND。 */
        g_legacy_backend_path = argv[3];
        /* DSN 用 fs://：它不是 shm，前端因此去找 libkvspace_durable.so.1——
         * CMake 已把旧五参假后端装成这个名字、放进 legacybackend/ 子目录，
         * 并把前端软链也放在那里。不能传裸路径：前端按 scheme 选 soname，
         * 裸路径会被当成 durable DSN 的路径部分。 */
        void *legacy = api.connect("fs://frontend-storage-legacy-gate");
        if (!legacy) {
            fprintf(stderr, "connect legacy backend failed\n");
            return 2;
        }
        test_legacy_backend_gated(legacy);
        api.close(legacy);
    } else {
        fprintf(stderr, "unknown mode: %s\n", argv[2]);
        return 2;
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}

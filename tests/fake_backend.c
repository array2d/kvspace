/* fake_backend.c — 测试用假后端：让前端能真 dlopen 到一个「什么都能答」的对象。
 *
 * 为什么要一个真 .so 而不是在测试里造个结构体：前端 `kvspaceConnect` 只用
 * dlopen/dlsym 拿后端，handle 布局是私有的。要在**不改产品代码**的前提下测到
 * dispatch，唯一的路径就是给前端一个真能 dlopen 的对象。
 *
 * 本文件编两次：
 *   - `-DWITH_STORAGE_ABI=1` → 导出新存储 ABI 九个符号，用于验「参数原样转发」；
 *   - `-DWITH_STORAGE_ABI=0` → **不导出**它们，用于验「缺符号 → KVSPACE_BACKEND」。
 * 同一份源码两种编译，缺符号那份天然不会是「另一套实现」，它就是少了一段。
 *
 * 旧 ABI 那 19 个必需符号一律给最小实现：它们的存在只为让 Connect 走通，
 * 本测试不校验它们的行为。
 *
 * 注意：本文件同时 include 新旧两份头是不行的（重名冲突），所以旧 ABI 的
 * 函数签名在这里**手写**——它们只作占位，且签名由前端 dlsym 的约定固定，
 * 不随 kvspace.h 演进而漂移。新 ABI 则直接 include storage.h 拿权威签名。
 *
 * 唯一的例外是 WITH_LEGACY_MKINDEX 那一份：它要导出**旧五参** kvspaceMkindex，
 * 而 storage.h 已声明四参版本，同 TU 直接冲突。所以那一份连 storage.h 都不
 * include，全靠手写——它本来也只演「一个未迁移的旧后端」，不需要新 ABI 签名。 */

#if !defined(WITH_LEGACY_MKINDEX) && !defined(WITH_PARTIAL_ABI)
#include <kvspace/kvspace.h>
#endif

#include <stdint.h>
#include <string.h>

/* ── 记录：转发是否正确，看这些值 ────────────────────────────────────
 * 只在 WITH_STORAGE_ABI=1 那份里存在。裸版若也定义一份，进程里就有两个同名
 * 对象，测试写一个、读另一个——那会造出「怎么改都看不到」的假绿。 */

#ifdef WITH_STORAGE_ABI

struct recorded {
    int resolve_calls, realloc_calls, free_calls, makeindex_calls;
    int deltree_calls, cpdir_calls, listlen_calls, list_calls;
    char key[256];
    uint64_t anchor;
    uint64_t body_length, attr_length;
    uint32_t list_capacity;
    void *backend_seen;
};

struct recorded g_recorded;

static void remember(const char *key) {
    if (!key) {
        g_recorded.key[0] = '\0';
        return;
    }
    strncpy(g_recorded.key, key, sizeof g_recorded.key - 1);
    g_recorded.key[sizeof g_recorded.key - 1] = '\0';
}

/* 假后端把 out_value_offset 写成可辨认常量，调用方据此确认值真转到了 out 参数。 */
#define FAKE_VALUE_OFFSET 0x1122334455667788ULL

#endif /* WITH_STORAGE_ABI —— 记录结构到此为止 */

/* ── 旧 ABI 占位已整块删除 ──────────────────────────────────────────
 *
 * 这里曾摆着 kvspaceGet / SetValue / ListLen / ListAt / Del / DelTree / CpTree /
 * CpList / MkindexExt / RmindexExt / Watch 等十几个空实现，理由是「前端 dlsym
 * 它们是必需符号」。前端重写后只加载 <kvspace/kvspace.h> 的十二个新符号，一个
 * 旧符号都不问——留着这些占位只会让人以为旧 ABI 仍在被使用。删。
 *
 * 唯一保留的旧式符号是下面 legacy / partial 两个变体里的 `kvspaceMkindex`：
 * 它们要复刻「后端还导出旧五参 mkindex」的形态，那是被测对象本身。 */

/* 生命周期与 cp 在新旧两代同名同签名，前端按新 ABI 加载它们。 */
void *kvspaceConnect(const char *dsn) { (void)dsn; return (void *)(uintptr_t)0xBACC; }
void kvspaceClose(void *h) { (void)h; }
int kvspaceCp(void *h, const char *s, const char *d, char *e, uint32_t c) {
    (void)h; (void)s; (void)d; (void)e; (void)c;
    return 0;
}

/* ── 第三变体：旧五参后端（WITH_LEGACY_MKINDEX=1）─────────────────────
 *
 * 它导出 `kvspaceMkindex`，但是**旧五参**签名，且没有任何新 ABI 独有符号。
 *
 * 为什么需要它：动态链接不查签名。没有探测闸时，前端 dlsym 拿到这个旧指针、
 * 按新四参调用，第 3 个参数（capacity）会顶掉 err、err 顶掉 err_cap——**多数
 * 情况下进程并不崩，只是静默算错**。实测：关掉闸后本用例仍「跑完」，因为
 * 错位后的参数恰好都落在了合法地址上。
 *
 * 所以这个后端必须能被观测：`g_legacy_mkindex_calls` 一旦非零，就说明旧实现
 * 真的被调用了——闸失效。只断言「返回 KVSPACE_BACKEND」是不够的：错调之后
 * 返回什么完全看栈上剩了什么，可能碰巧也是 BACKEND。 */
#ifdef WITH_LEGACY_MKINDEX
int g_legacy_mkindex_calls;

int kvspaceMkindex(void *h, const char *p, uint32_t capacity, char *e, uint32_t c) {
    (void)h; (void)p; (void)capacity; (void)e; (void)c;
    g_legacy_mkindex_calls++;
    return 0;
}
#endif


/* ── 半迁移变体（WITH_PARTIAL_ABI=1）─────────────────────────────────
 *
 * 真实踩到的形态：shm 曾导出 `kvspaceResolve`，但 `kvspaceMkindex` 还是旧五参、
 * `kvspaceXvalueInProcessAddress` 也没有。**析取判据**见前者就放行，然后按新
 * 四参调用旧指针 → 参数错位崩溃。
 *
 * 这份假后端就复刻那个形态，用来钉死「判据必须是合取」：
 *   WITH_PARTIAL_ABI=1  → 只导出 kvspaceResolve（一个）
 *   WITH_PARTIAL_ABI=2  → 只导出 kvspaceResolve + kvspaceXvalueInProcessAddress
 * 两种都必须被闸拦下。 */
#if defined(WITH_PARTIAL_ABI)
int g_partial_mkindex_calls;

/* 旧五参：闸若放行，这个会被按四参调用 */
int kvspaceMkindex(void *h, const char *p, uint32_t capacity, char *e, uint32_t c) {
    (void)h; (void)p; (void)capacity; (void)e; (void)c;
    g_partial_mkindex_calls++;
    return 0;
}

int kvspaceResolve(void *h, const char *key, uint64_t anchor,
                   uint64_t *out_offset, char *err, uint32_t err_cap) {
    (void)h; (void)key; (void)anchor; (void)err; (void)err_cap;
    if (out_offset)
        *out_offset = 0;
    return 0;
}

#if WITH_PARTIAL_ABI >= 2
uint8_t *kvspaceXvalueInProcessAddress(void *h) {
    (void)h;
    return NULL;
}
#endif
#endif /* WITH_PARTIAL_ABI */
int kvspaceClear(void *h, char *e, uint32_t c) {
    (void)h; (void)e; (void)c;
    return 0;
}

/* ── 新存储 ABI：只在 WITH_STORAGE_ABI=1 时导出 ──────────────────────── */

#ifdef WITH_STORAGE_ABI

/* 地址空间访问器：返回一块**真可读**的静态缓冲。
 * 前端若把 offset 与 base 拼错（比如漏加），测试里解引用会当场段错而不是
 * 悄悄读到别的内存——这正是要的：错就炸，不要「看起来成功」。 */
static uint8_t fake_data_region[4096];

uint8_t *kvspaceXvalueInProcessAddress(void *h) {
    (void)h;
    return fake_data_region;
}

int kvspaceResolve(void *h, const char *key, int64_t parent_key_anchor,
                   uint64_t *out_value_offset, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    g_recorded.resolve_calls++;
    g_recorded.backend_seen = h;
    remember(key);
    g_recorded.anchor = parent_key_anchor;
    if (out_value_offset) *out_value_offset = FAKE_VALUE_OFFSET;
    return 0;
}

int kvspaceRealloc(void *h, const char *key, uint64_t body_length,
                   uint64_t attr_length, uint64_t *out_value_offset, char *err,
                   uint32_t err_cap) {
    (void)err; (void)err_cap;
    g_recorded.realloc_calls++;
    g_recorded.backend_seen = h;
    remember(key);
    g_recorded.body_length = body_length;
    g_recorded.attr_length = attr_length;
    if (out_value_offset) *out_value_offset = FAKE_VALUE_OFFSET;
    return 0;
}

int kvspaceFree(void *h, const char *key, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    g_recorded.free_calls++;
    g_recorded.backend_seen = h;
    remember(key);
    return 0;
}

int kvspaceMkindex(void *h, const char *key, char *err, uint32_t err_cap) {
    (void)h; (void)err; (void)err_cap;
    g_recorded.makeindex_calls++;
    remember(key);
    return 0;
}

int kvspaceDeltree(void *h, const char *key, char *err, uint32_t err_cap) {
    (void)h; (void)err; (void)err_cap;
    g_recorded.deltree_calls++;
    remember(key);
    return 0;
}

int kvspaceCpdir(void *h, const char *source_key, const char *destination_key,
                 char *err, uint32_t err_cap) {
    (void)h; (void)destination_key; (void)err; (void)err_cap;
    g_recorded.cpdir_calls++;
    remember(source_key);
    return 0;
}

int kvspaceListlen(void *h, const char *key, int64_t *out_count,
                   uint32_t *out_max_name_length, char *err, uint32_t err_cap) {
    (void)h; (void)err; (void)err_cap;
    g_recorded.listlen_calls++;
    remember(key);
    if (out_count) *out_count = 7;
    if (out_max_name_length) *out_max_name_length = 13;
    return 0;
}

int kvspaceList(void *h, const char *key, char *out_names, uint32_t out_capacity,
                uint32_t *out_length, uint32_t *out_count,
                uint64_t *out_generation, char *err, uint32_t err_cap) {
    (void)h; (void)err; (void)err_cap;
    g_recorded.list_calls++;
    remember(key);
    g_recorded.list_capacity = out_capacity;
    if (out_names && out_capacity >= 3) memcpy(out_names, "a\0b", 3);
    if (out_length) *out_length = 3;
    if (out_count) *out_count = 2;
    if (out_generation) *out_generation = 42;
    return 0;
}

#endif /* WITH_STORAGE_ABI */

/* test_storage_durable.c — 新存储 ABI 对真 durable 后端的契约 smoke。
 *
 * durable（fs/redis/s3）没有进程内值区，所以 resolve / realloc 必须返回
 * KVSPACE_UNSUPPORTED 且 err 非空——**不是**静默返回 0 让调用方拿着垃圾偏移跑。
 * 其余 7 个 key 寻址操作正常。`kvspaceXvalueInProcessAddress` 必须**导出并返回 NULL**。
 *
 * --- 一条踩过的坑，写给后来的读者 ---
 * 前端对缺失符号的兜底是「返回 NULL」。于是 `kvspaceXvalueInProcessAddress(h) == NULL` 有两种
 * 来源，结果相同而含义相反：
 *   a) 后端导出了该符号并主动返回 NULL —— 契约兑现；
 *   b) 后端没导出，前端走了兜底      —— 契约没兑现。
 * 只断言 `== NULL` 会把 (b) 判成通过。本文件第一版就是这么假绿的。所以凡「缺失即
 * 某值」的兜底路径，**都要先直接向后端 dlsym 确认符号存在**，再断言那个值——
 * 否则测的是兜底，不是后端。
 * ---------------------------------
 *
 * DSN 用独立 fs:// 目录（带 pid），不碰共享 redis/6379，不 flush。
 * 本文件 include kvspace.h 与 kvspace_xvalue.h + tests/shm/test_wire.h
 * （与 storage.h 在 Connect/Close/Cp 上重名），故旧接口 kvspaceSetValue 的
 * 声明在此手写，仅用于构造 fixture（见下）。 */

#include <kvspace/kvspace.h>
#include <kvspace/kvspace_xvalue.h>
#include "shm/test_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>


static int failures;

#define CHECK(condition, ...)                                                  \
    do {                                                                       \
        if (!(condition)) {                                                    \
            failures++;                                                        \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

static char err[512];
static void clear_err(void) { memset(err, 0, sizeof err); }

/* ── resolve / realloc：durable 必须明确拒绝 ────────────────────────── */

static void test_offsets_unsupported(void *kv) {
    uint64_t offset = 0x5a5a5a5a5a5a5a5aULL;

    /* durable 没有进程内值区：基址必须是 NULL。返回任何非 NULL 值都会让调用方
     * 拿 base+offset 去解一段不属于它的地址——比测试失败严重得多。
     *
     * 但「前端返回 NULL」有两种来源，结果相同、含义完全不同：
     *   a) durable 导出了 kvspaceXvalueInProcessAddress 并主动返回 NULL —— 设计如此；
     *   b) durable 根本没导出该符号，前端按缺失处理返回 NULL —— 契约没兑现。
     * 只断言 `== NULL` 会把 (b) 当成通过（我第一版就是这么假绿的）。所以这里
     * 直接问后端 .so 要符号：**必须导出**，再由它自己返回 NULL。 */
    void *dl = dlopen("libkvspace_durable.so.1", RTLD_NOW | RTLD_NOLOAD);
    CHECK(dl != NULL, "无法打开已加载的 durable 后端以核对符号");
    if (dl) {
        CHECK(dlsym(dl, "kvspaceXvalueInProcessAddress") != NULL,
              "durable 未导出 kvspaceXvalueInProcessAddress：契约要求导出并返回 NULL，"
              "而非靠前端「缺符号即 NULL」蒙对");
        dlclose(dl);
    }
    CHECK(kvspaceXvalueInProcessAddress(kv) == NULL, "durable 的 kvspaceXvalueInProcessAddress 必须返回 NULL");

    clear_err();
    CHECK(kvspaceResolveOffset(kv, "/du/a", 0, &offset, err, sizeof err) ==
              KVSPACE_UNSUPPORTED,
          "durable 的 resolve 应返回 KVSPACE_UNSUPPORTED");
    CHECK(err[0] != '\0', "durable 的 resolve 拒绝时 err 不该为空");

    offset = 0x5a5a5a5a5a5a5a5aULL;
    clear_err();
    CHECK(kvspaceRealloc(kv, "/du/a", 8, 0, &offset, err, sizeof err) ==
              KVSPACE_UNSUPPORTED,
          "durable 的 realloc 应返回 KVSPACE_UNSUPPORTED");
    CHECK(err[0] != '\0', "durable 的 realloc 拒绝时 err 不该为空");
}

/* ── makeindex / free ───────────────────────────────────────────────── */

static void test_makeindex_free(void *kv) {
    clear_err();
    CHECK(kvspaceMkindex(kv, "/idx/", err, sizeof err) == KVSPACE_OK,
          "makeindex 失败: %s", err);
    /* 幂等：建两次都成功。 */
    clear_err();
    CHECK(kvspaceMkindex(kv, "/idx/", err, sizeof err) == KVSPACE_OK,
          "makeindex 第二次应幂等成功: %s", err);

    /* 空索引：free 成功。 */
    clear_err();
    CHECK(kvspaceFree(kv, "/idx/", err, sizeof err) == KVSPACE_OK,
          "free 空索引失败: %s", err);

    /* 非空索引：HAS_CHILDREN。durable 无法用 realloc 造成员（它 UNSUPPORTED），
     * 所以用 makeindex 造一个嵌套索引当成员——它是一个真实存在的子 key。 */
    CHECK(kvspaceMkindex(kv, "/idx2/", err, sizeof err) == KVSPACE_OK,
          "makeindex /idx2/ 失败: %s", err);
    clear_err();
    CHECK(kvspaceMkindex(kv, "/idx2/child/", err, sizeof err) == KVSPACE_OK,
          "建子索引失败: %s", err);
    clear_err();
    CHECK(kvspaceFree(kv, "/idx2/", err, sizeof err) == KVSPACE_HAS_CHILDREN,
          "free 非空索引应返回 HAS_CHILDREN");
}

/* ── listlen / list ────────────────────────────────────────────────── */

static void test_list(void *kv) {
    int64_t count = -1;
    uint32_t max_name = 0;

    CHECK(kvspaceMkindex(kv, "/ls/", err, sizeof err) == KVSPACE_OK,
          "makeindex /ls/ 失败: %s", err);

    clear_err();
    CHECK(kvspaceListlen(kv, "/ls/", &count, &max_name, err, sizeof err) ==
              KVSPACE_OK,
          "listlen 空索引失败: %s", err);
    CHECK(count == 0 && max_name == 0, "空索引 N/M 应为 0/0，实得 %lld/%u",
          (long long)count, max_name);

    /* 三个子索引，名字长度 2/3/1。 */
    const char *names[] = {"aa", "bbb", "c"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char key[64];
        snprintf(key, sizeof key, "/ls/%s/", names[i]);
        clear_err();
        CHECK(kvspaceMkindex(kv, key, err, sizeof err) == KVSPACE_OK,
              "建子索引 %s 失败: %s", key, err);
    }

    clear_err();
    CHECK(kvspaceListlen(kv, "/ls/", &count, &max_name, err, sizeof err) ==
              KVSPACE_OK,
          "listlen 失败: %s", err);
    CHECK(count == 3, "N 应为 3，实得 %lld", (long long)count);
    /* 子索引作为目录项要带尾 `/`，所以最长是 "bbb/" = 4 字节。 */
    CHECK(max_name == 4, "M 应为最长目录项字节数 4，实得 %u", max_name);

    uint32_t length = 0, listed = 0;
    uint64_t generation = 0;
    clear_err();
    CHECK(kvspaceList(kv, "/ls/", NULL, 0, &length, &listed, &generation, err,
                      sizeof err) == KVSPACE_OK,
          "list(NULL 探测) 失败: %s", err);
    /* "aa/" + "bbb/" + "c/" = 3 + 4 + 2 = 9（含各自 NUL 则 4+5+3=12）；
     * 这里只校验 count，length 的确切字节口径由后端定义，不在 smoke 里钉死。 */
    CHECK(listed == 3, "list 探测 count 应为 3，实得 %u", listed);

    uint32_t short_len = 0, short_count = 0;
    uint64_t short_gen = 0;
    char buf[4];
    clear_err();
    CHECK(kvspaceList(kv, "/ls/", buf, sizeof buf, &short_len, &short_count,
                      &short_gen, err, sizeof err) == KVSPACE_CAPACITY,
          "容量不足应返回 CAPACITY");
    CHECK(short_count == 3, "容量不足仍须回报 count=3，实得 %u", short_count);
}

/* ── 存在性三态（架构师裁决 A，shm/durable 同一套判据）─────────────────
 *
 *   不存在（尾带不带 `/` 都算）→ KVSPACE_MISSING
 *   存在但不是索引            → KVSPACE_NOT_INDEX
 *   存在且是空索引            → OK，N=M=0
 *
 * durable 侧不能靠 realloc 造普通值（它 UNSUPPORTED），所以「存在但不是索引」
 * 这条用**旧 ABI 的 SetValue** 造一个不带尾 `/` 的普通 key —— durable 的写路径
 * 在旧 ABI 上，新 ABI 只做寻址。这样三态在本后端上也都能实际到达。 */
static void test_existence_three_states(void *kv) {
    int64_t count = -1;
    uint32_t max_name = 0;
    uint32_t length = 0, listed = 0;
    uint64_t generation = 0;
    char names[64];

    /* 1) 从没建过：带尾 `/` 与不带，都必须 MISSING。 */
    clear_err();
    CHECK(kvspaceListlen(kv, "/never/", &count, &max_name, err, sizeof err) ==
              KVSPACE_MISSING,
          "从没建过的 /never/ 应返回 MISSING，而非 OK/0/0");
    clear_err();
    CHECK(kvspaceListlen(kv, "/never", &count, &max_name, err, sizeof err) ==
              KVSPACE_MISSING,
          "从没建过的 /never 应返回 MISSING");
    clear_err();
    CHECK(kvspaceList(kv, "/never/", names, sizeof names, &length, &listed,
                      &generation, err, sizeof err) == KVSPACE_MISSING,
          "从没建过的 /never/ 上 list 应返回 MISSING");
    clear_err();
    CHECK(kvspaceList(kv, "/never", names, sizeof names, &length, &listed,
                      &generation, err, sizeof err) == KVSPACE_MISSING,
          "从没建过的 /never 上 list 应返回 MISSING");

    /* 2) 存在且是空索引：OK，N=M=0。 */
    clear_err();
    CHECK(kvspaceMkindex(kv, "/empty/", err, sizeof err) == KVSPACE_OK,
          "建空索引 /empty/ 失败: %s", err);
    clear_err();
    CHECK(kvspaceListlen(kv, "/empty/", &count, &max_name, err, sizeof err) ==
              KVSPACE_OK,
          "已存在的空索引应返回 OK: %s", err);
    CHECK(count == 0 && max_name == 0,
          "已存在的空索引 N/M 应为 0/0，实得 %lld/%u", (long long)count,
          max_name);
    clear_err();
    CHECK(kvspaceList(kv, "/empty/", names, sizeof names, &length, &listed,
                      &generation, err, sizeof err) == KVSPACE_OK,
          "已存在的空索引上 list 应返回 OK: %s", err);
    CHECK(length == 0 && listed == 0,
          "已存在的空索引 list 应得 len=0 count=0，实得 %u/%u", length, listed);

    /* 3) 存在且非空的索引：N/M 正确。 */
    clear_err();
    CHECK(kvspaceMkindex(kv, "/full/", err, sizeof err) == KVSPACE_OK,
          "建 /full/ 失败: %s", err);
    clear_err();
    CHECK(kvspaceMkindex(kv, "/full/ab/", err, sizeof err) == KVSPACE_OK,
          "建 /full/ab/ 失败: %s", err);
    clear_err();
    CHECK(kvspaceListlen(kv, "/full/", &count, &max_name, err, sizeof err) ==
              KVSPACE_OK,
          "非空索引应返回 OK: %s", err);
    /* 目录项带尾 `/`：唯一子项 "ab/" → N=1, M=3。 */
    CHECK(count == 1 && max_name == 3,
          "非空索引应得 N=1 M=3，实得 %lld/%u", (long long)count, max_name);

    /* 3b) 有后代、但自己没有根值（架构师裁决 B）。
     *
     * `/desc` 与 `/desc/` 是**两个独立 key**，不是一个对象的两种写法：
     *   - `/desc/`（index 形态，尾 `/`）：key 是路径前缀，中间 index 可由后代
     *     自然产生，不必自带 XValue → 有后代即存在 → OK, N=1；
     *   - `/desc`（非 index 形态）：只认根值。它自己没有根值 → **MISSING**，
     *     不是 NOT_INDEX。
     *
     * 「有后代即存在」**只适用于 index 形态**；非 index 形态只认根值，只有
     * `/desc` 自己有根值时才 NOT_INDEX（存在但不是索引）。
     *
     * 这与「/never 与 /never/ 都给 MISSING」不矛盾：那条讲的是**两者都不存在
     * 时**同答 MISSING，不是任何输入都必须同答。
     *
     * 另注意 `resolve("/desc/")` 仍可 MISSING（问值在不在），
     * `listlen("/desc/")` OK（问子项有几个），两个问题不同。 */
    clear_err();
    CHECK(kvspaceMkindex(kv, "/desc/child/", err, sizeof err) == KVSPACE_OK,
          "建 /desc/child/ 失败: %s", err);
    clear_err();
    CHECK(kvspaceListlen(kv, "/desc/", &count, &max_name, err, sizeof err) ==
              KVSPACE_OK,
          "有后代但无根值的 /desc/ 应返回 OK（index 形态可由后代产生）: %s",
          err);
    /* 唯一子项 "child/" → N=1, M=6。 */
    CHECK(count == 1 && max_name == 6,
          "有后代无根值应得 N=1 M=6，实得 %lld/%u", (long long)count, max_name);
    clear_err();
    CHECK(kvspaceListlen(kv, "/desc", &count, &max_name, err, sizeof err) ==
              KVSPACE_MISSING,
          "非索引形态 /desc 自己没有根值 → MISSING（「有后代即存在」只管 index 形态）");

    /* 4) 「存在但不是索引」这一格**本后端已无法覆盖**。
     *
     * 造普通值需要写一个非 index 形态的 key，而 durable 的新 ABI 里：
     *   - kvspaceRealloc 返回 UNSUPPORTED（没有进程内值区）；
     *   - 旧 ABI 的 kvspaceSetValue 已随旧符号一起删除（P5）。
     * 没有第三条写路径，所以这一格在 durable 上测不了。
     *
     * 不拿 CLI 或别的东西旁路造：那会让「这条 key 是谁写的」变得说不清，而三态
     * 判据恰恰要求存在性来自被测后端本身。该格由 shm smoke 覆盖（那里 realloc
     * 可用，能直接造普通值）。
     *
     * 删掉而不是留着——留着会变成链接期 undefined reference，让人以为测试还在
     * 验它。 */
}

/* ── cp / cpdir / deltree ──────────────────────────────────────────── */

static void test_copy_and_delete(void *kv) {
    CHECK(kvspaceMkindex(kv, "/cd/src/", err, sizeof err) == KVSPACE_OK,
          "建 /cd/src/ 失败: %s", err);
    CHECK(kvspaceMkindex(kv, "/cd/src/m/", err, sizeof err) == KVSPACE_OK,
          "建 /cd/src/m/ 失败: %s", err);

    clear_err();
    CHECK(kvspaceCpdir(kv, "/cd/src/", "/cd/dst/", err, sizeof err) == KVSPACE_OK,
          "kvspaceCpdir 失败: %s", err);

    int64_t count = 0;
    uint32_t max_name = 0;
    clear_err();
    CHECK(kvspaceListlen(kv, "/cd/dst/", &count, &max_name, err, sizeof err) ==
              KVSPACE_OK,
          "cpdir 之后目标索引不可列出: %s", err);
    CHECK(count == 1, "cpdir 之后目标应有 1 个子项，实得 %lld",
          (long long)count);

    /* deltree = **连根删**（架构师裁决）：key 本身与全部后代一起消失。
     * 只验「子项没了」会漏掉「根还在」的实现——durable 早先正是这样：
     * 子项清空、/cd/src/ 仍可 listlen 出 n=0。所以这里必须**再对根查一次**。 */
    clear_err();
    CHECK(kvspaceDeltree(kv, "/cd/src/", err, sizeof err) == KVSPACE_OK,
          "kvspaceDeltree 失败: %s", err);
    clear_err();
    CHECK(kvspaceListlen(kv, "/cd/src/", &count, &max_name, err, sizeof err) ==
              KVSPACE_MISSING,
          "deltree 之后根索引仍存在（应为连根删）");
    clear_err();
    CHECK(kvspaceListlen(kv, "/cd/src/m/", &count, &max_name, err, sizeof err) ==
              KVSPACE_MISSING,
          "deltree 之后后代仍存在");

    /* key 不存在视为成功：删除是幂等的，第二次删同一棵树不该报错。 */
    clear_err();
    CHECK(kvspaceDeltree(kv, "/cd/src/", err, sizeof err) == KVSPACE_OK,
          "deltree 不存在的 key 应视为成功");

    /* cpdir 不动的兄弟子树必须还在：连根删只删目标这棵，别把同级一起带走。 */
    clear_err();
    CHECK(kvspaceListlen(kv, "/cd/dst/", &count, &max_name, err, sizeof err) ==
              KVSPACE_OK,
          "deltree 连带删掉了兄弟子树 /cd/dst/");
}

int main(void) {
    char dsn[256], path[256];
    snprintf(dsn, sizeof dsn, "fs:///tmp/kvspace_storage_durable_%ld",
             (long)getpid());
    snprintf(path, sizeof path, "/tmp/kvspace_storage_durable_%ld",
             (long)getpid());

    void *kv = kvspaceConnect(dsn);
    if (!kv) {
        fprintf(stderr, "connect %s failed（后端是否已导出新符号？）\n", dsn);
        return 2;
    }

    /* 探测：后端到底是不是新 ABI。只读、不改前端、不加导出符号——
     * 借一次必然失败的新 ABI 调用取 err，从中读前端的闸判定。
     *
     * 为什么值得打这行：闸的信息原本只藏在 err 字符串里，排障时要人眼看。
     * 但探测**不能**变成生产 API：那就等于给「后端是否迁移」造了第二个真相，
     * 而真相只有 vt 表本身。 */
    {
        clear_err();
        const uint8_t *langtype = NULL;
        uint16_t langtype_len = 0;
        (void)kvlangXvalueHeadLangtypeBounded(NULL, 0, &langtype);
        int64_t probe_count = 0;
        uint32_t probe_max = 0;
        const int probe_rc =
            kvspaceListlen(kv, "/__probe_never__/", &probe_count, &probe_max,
                           err, sizeof err);
        if (strstr(err, "has no new storage ABI") != NULL)
            printf("探测: 闸拦下: kvspaceResolveOffset/kvspaceXvalueInProcessAddress 均缺失\n");
        else
            printf("探测: 闸放行（后端已导出新 ABI 独有符号）\n");
        (void)probe_rc;
        (void)langtype_len;
    }

    test_offsets_unsupported(kv);
    test_makeindex_free(kv);
    test_list(kv);
    test_existence_three_states(kv);
    test_copy_and_delete(kv);

    kvspaceClose(kv);

    char cmd[320];
    snprintf(cmd, sizeof cmd, "rm -rf %s", path);
    if (system(cmd) != 0) {
        fprintf(stderr, "清理 %s 失败（不影响结论）\n", path);
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}

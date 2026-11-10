/* test_storage_shm.c — 新存储 ABI 对真 shm 后端的契约 smoke。
 *
 * 与 tests/test_frontend_storage.c 分工明确：那个用假后端验「前端转发对不对」，
 * 这个用真 libkvspace-c 验「后端语义对不对」。两者都要跑：前者能测到后者测不到的
 * （缺符号路径），后者能测到前者测不到的（真偏移、真 generation）。
 *
 * DSN 用本次进程独占的临时 shm 文件：不碰 /usr/lib 下的默认实例，不碰共享
 * redis/6379，不 flush。路径带 pid，并行跑多个 ctest 也不打架。
 *
 * 本文件只 include storage.h —— 与旧 kvspace.h 有重名声明，同 TU 不能共存。
 */

#include <kvspace/kvspace.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* 每个用例自备 err 缓冲：只验返回值会漏掉「失败但没说原因」，
 * 而调用方正是靠 err 判断要不要重试或换后端。 */
static char err[512];
static void clear_err(void) { memset(err, 0, sizeof err); }

/* ── resolve / realloc：真实 offset、原地、搬移 ─────────────────────── */

static void test_resolve_realloc(void *kv) {
    uint64_t first = 0, again = 0, grown = 0;

    /* 基址：shm 是映射后端，必须有非 NULL 基址；地址 = base + offset 可读。
     * 这是整套 ABI 的收口——offset 本身没用，必须能落到真内存上。 */
    uint8_t *base = kvspaceXvalueInProcessAddress(kv);
    CHECK(base != NULL, "shm 后端必须提供非 NULL 的 kvspaceXvalueInProcessAddress");

    clear_err();
    CHECK(kvspaceRealloc(kv, "/ct/a", 8, 0, &first, err, sizeof err) == KVSPACE_OK,
          "realloc 创建失败: %s", err);

    /* 真写读一次：往 base+offset 写 8 字节，再读回来。只验「offset 相等」证明
     * 不了 offset 可用——那可能是个自洽但落不到内存的编号。 */
    if (base) {
        uint64_t probe = 0x0123456789ABCDEFULL;
        memcpy(base + first, &probe, sizeof probe);
        uint64_t back = 0;
        memcpy(&back, base + first, sizeof back);
        CHECK(back == probe, "base+offset 写读回的字节不对");
    }

    clear_err();
    CHECK(kvspaceResolve(kv, "/ct/a", 0, &again, err, sizeof err) == KVSPACE_OK,
          "resolve 已存在的 key 失败: %s", err);
    CHECK(again == first,
          "resolve 的 offset(%llu) 与 realloc 返回(%llu) 不一致",
          (unsigned long long)again, (unsigned long long)first);

    clear_err();
    int rc = kvspaceResolve(kv, "/ct/never", 0, &again, err, sizeof err);
    CHECK(rc == KVSPACE_MISSING,
          "resolve 不存在的 key 应返回 MISSING，实得 %d (%s)", rc, err);

    /* 容量内原地：同一 offset 必须保持有效（这是「原地写只改 body」的地基）。 */
    clear_err();
    CHECK(kvspaceRealloc(kv, "/ct/a", 8, 0, &grown, err, sizeof err) == KVSPACE_OK,
          "同尺寸 realloc 失败: %s", err);
    CHECK(grown == first,
          "同尺寸 realloc 不该搬移：offset 从 %llu 变成 %llu",
          (unsigned long long)first, (unsigned long long)grown);
}

/* ── makeindex / free：空的与非空的索引 ─────────────────────────────── */

static void test_makeindex_free(void *kv) {
    uint64_t off = 0;

    clear_err();
    CHECK(kvspaceMkindex(kv, "/idx/", err, sizeof err) == KVSPACE_OK,
          "makeindex 失败: %s", err);
    /* 幂等：同一个索引建两次必须成功，不是 EXISTS。 */
    clear_err();
    CHECK(kvspaceMkindex(kv, "/idx/", err, sizeof err) == KVSPACE_OK,
          "makeindex 第二次应幂等成功: %s", err);

    /* 空的索引：直接 free 成功（连索引节点一起删）。 */
    clear_err();
    CHECK(kvspaceFree(kv, "/idx/", err, sizeof err) == KVSPACE_OK,
          "free 空索引失败: %s", err);

    /* 非空索引：free 必须返回 HAS_CHILDREN，且不能把成员删掉。 */
    clear_err();
    CHECK(kvspaceMkindex(kv, "/idx2/", err, sizeof err) == KVSPACE_OK,
          "makeindex /idx2/ 失败: %s", err);
    clear_err();
    CHECK(kvspaceRealloc(kv, "/idx2/member", 4, 0, &off, err, sizeof err) ==
              KVSPACE_OK,
          "在 /idx2/ 下建成员失败: %s", err);
    clear_err();
    CHECK(kvspaceFree(kv, "/idx2/", err, sizeof err) == KVSPACE_HAS_CHILDREN,
          "free 非空索引应返回 HAS_CHILDREN");
    clear_err();
    CHECK(kvspaceResolve(kv, "/idx2/member", 0, &off, err, sizeof err) ==
              KVSPACE_OK,
          "free 非空索引失败后，成员不该消失: %s", err);
}

/* ── listlen / list：N、M、复制、容量不足、generation ───────────────── */

static void test_list(void *kv) {
    uint64_t off = 0;
    int64_t count = -1;
    uint32_t max_name = 0;

    CHECK(kvspaceMkindex(kv, "/ls/", err, sizeof err) == KVSPACE_OK,
          "makeindex /ls/ 失败: %s", err);

    /* 空索引：N = M = 0。 */
    clear_err();
    CHECK(kvspaceListlen(kv, "/ls/", &count, &max_name, err, sizeof err) ==
              KVSPACE_OK,
          "listlen 空索引失败: %s", err);
    CHECK(count == 0 && max_name == 0, "空索引 N/M 应为 0/0，实得 %lld/%u",
          (long long)count, max_name);

    const char *names[] = {"aa", "bbb", "c"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char key[64];
        snprintf(key, sizeof key, "/ls/%s", names[i]);
        clear_err();
        CHECK(kvspaceRealloc(kv, key, 1, 0, &off, err, sizeof err) == KVSPACE_OK,
              "建成员 %s 失败: %s", key, err);
    }

    clear_err();
    CHECK(kvspaceListlen(kv, "/ls/", &count, &max_name, err, sizeof err) ==
              KVSPACE_OK,
          "listlen 失败: %s", err);
    CHECK(count == 3, "N 应为 3，实得 %lld", (long long)count);
    /* M 按 list 返回的同一字节串计；目录项带尾 `/`，普通成员不带。 */
    CHECK(max_name == 3, "M 应为最长子项字节数 3，实得 %u", max_name);

    uint32_t length = 0, listed = 0;
    uint64_t generation = 0;
    clear_err();
    CHECK(kvspaceList(kv, "/ls/", NULL, 0, &length, &listed, &generation, err,
                      sizeof err) == KVSPACE_OK,
          "list(NULL 只探测) 失败: %s", err);
    CHECK(length == 9 && listed == 3,
          "list 探测应得 length=9 count=3，实得 %u/%u", length, listed);

    char buf[32];
    uint32_t length2 = 0, listed2 = 0;
    uint64_t generation2 = 0;
    clear_err();
    CHECK(kvspaceList(kv, "/ls/", buf, sizeof buf, &length2, &listed2,
                      &generation2, err, sizeof err) == KVSPACE_OK,
          "list 复制失败: %s", err);
    CHECK(length2 == 9 && listed2 == 3,
          "list 复制应得 length=9 count=3，实得 %u/%u", length2, listed2);
    CHECK(memcmp(buf, "aa\0bbb\0c\0", 9) == 0,
          "list 输出的 NUL 分隔串不对（前 9 字节）");

    /* 容量不足：必须返回 CAPACITY，且**仍写出所需长度与个数**，不截断。 */
    uint32_t short_len = 0, short_count = 0;
    uint64_t short_gen = 0;
    clear_err();
    CHECK(kvspaceList(kv, "/ls/", buf, 4, &short_len, &short_count, &short_gen,
                      err, sizeof err) == KVSPACE_CAPACITY,
          "容量不足应返回 CAPACITY");
    CHECK(short_len == 9 && short_count == 3,
          "容量不足仍须回报所需 length=9 count=3，实得 %u/%u", short_len,
          short_count);

    /* generation 前进：key 集合变化时变，值修改不变。 */
    uint64_t gen_before = 0, gen_add = 0, gen_modify = 0;
    clear_err();
    CHECK(kvspaceList(kv, "/ls/", NULL, 0, &length, &listed, &gen_before, err,
                      sizeof err) == KVSPACE_OK,
          "取 generation 基线失败: %s", err);
    char key[64];
    snprintf(key, sizeof key, "/ls/%s", "dddd");
    CHECK(kvspaceRealloc(kv, key, 1, 0, &off, err, sizeof err) == KVSPACE_OK,
          "新增成员失败: %s", err);
    clear_err();
    CHECK(kvspaceList(kv, "/ls/", NULL, 0, &length, &listed, &gen_add, err,
                      sizeof err) == KVSPACE_OK,
          "取新增后 generation 失败: %s", err);
    CHECK(gen_add != gen_before, "新增成员后 generation 应前进，仍是 %llu",
          (unsigned long long)gen_add);

    /* 值修改（只改 body 长度、key 集合不变）不应推进 generation。 */
    CHECK(kvspaceRealloc(kv, key, 2, 0, &off, err, sizeof err) == KVSPACE_OK,
          "改成员内容失败: %s", err);
    clear_err();
    CHECK(kvspaceList(kv, "/ls/", NULL, 0, &length, &listed, &gen_modify, err,
                      sizeof err) == KVSPACE_OK,
          "取值修改后 generation 失败: %s", err);
    CHECK(gen_modify == gen_add,
          "只改值不该推进 generation，从 %llu 变成 %llu",
          (unsigned long long)gen_add, (unsigned long long)gen_modify);

    /* 非索引的 key：返回 NOT_INDEX，不能当成空目录。 */
    clear_err();
    CHECK(kvspaceListlen(kv, "/ct/a", &count, &max_name, err, sizeof err) ==
              KVSPACE_NOT_INDEX,
          "对非索引 listlen 应返回 NOT_INDEX");
}

/* ── 存在性三态（架构师裁决 A）───────────────────────────────────────
 *
 * listlen/list 必须**先判存在、再判形态**，三态互斥：
 *   不存在（尾带不带 `/` 都算）→ KVSPACE_MISSING
 *   存在但不是索引            → KVSPACE_NOT_INDEX
 *   存在且是空索引            → OK，N=M=0
 *
 * 为什么这条必须单独测：曾出现「带尾 `/` 一律答 OK/0/0」的实现——它只看形态、
 * 不查存在，于是**从没建过的 key** 和**删干净的空索引**给出同一个答案。那样
 * 调用方无法区分「删成功」与「本来就没有」，A 就不成立了。所以下面既有
 * 「从没建过」也「删过」，两者必须都 MISSING。 */
static void test_existence_three_states(void *kv) {
    int64_t count = -1;
    uint32_t max_name = 0;
    uint32_t length = 0, listed = 0;
    uint64_t generation = 0;
    char names[64];
    uint64_t off = 0;

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

    /* 2) 存在但不是索引：普通值（不带尾 `/`），返回 NOT_INDEX。 */
    clear_err();
    CHECK(kvspaceRealloc(kv, "/plain", 4, 0, &off, err, sizeof err) == KVSPACE_OK,
          "建普通值 /plain 失败: %s", err);
    clear_err();
    CHECK(kvspaceListlen(kv, "/plain", &count, &max_name, err, sizeof err) ==
              KVSPACE_NOT_INDEX,
          "已存在的普通值 /plain 应返回 NOT_INDEX");
    clear_err();
    CHECK(kvspaceList(kv, "/plain", names, sizeof names, &length, &listed,
                      &generation, err, sizeof err) == KVSPACE_NOT_INDEX,
          "已存在的普通值 /plain 上 list 应返回 NOT_INDEX");

    /* 3) 存在且是空索引：OK，N=M=0，list 给空列表。 */
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

    /* 4) 存在且非空的索引：N/M 正确（这一条 test_list 里也测，但三态要自洽）。 */
    clear_err();
    CHECK(kvspaceMkindex(kv, "/full/", err, sizeof err) == KVSPACE_OK,
          "建 /full/ 失败: %s", err);
    clear_err();
    CHECK(kvspaceRealloc(kv, "/full/ab", 1, 0, &off, err, sizeof err) == KVSPACE_OK,
          "建 /full/ab 失败: %s", err);
    clear_err();
    CHECK(kvspaceListlen(kv, "/full/", &count, &max_name, err, sizeof err) ==
              KVSPACE_OK,
          "非空索引应返回 OK: %s", err);
    CHECK(count == 1 && max_name == 2,
          "非空索引应得 N=1 M=2，实得 %lld/%u", (long long)count, max_name);

    /* 5) 有后代、但自己没有根值（架构师裁决 B）。
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
     * 这不与「/never 与 /never/ 都给 MISSING」矛盾：那条讲的是**两者都不存在
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

    /* 6) 同一形态下「补上根值」必须让结论翻转：MISSING → NOT_INDEX。
     *
     * 这一格是第 5 格的反面，也是整套判据最容易写错的地方：值形态的 key 在
     * **没有根值**时不存在（MISSING），一旦**有了根值**它就存在，只是不是索引
     * （NOT_INDEX）。两种答案只差「根值在不在」，与有没有后代无关。
     *
     * 不测这一格，实现可能把「值形态一律 MISSING」写成常量——上面第 5 格照样
     * 通过，但那是靠拒绝一切值形态蒙对的。 */
    clear_err();
    CHECK(kvspaceRealloc(kv, "/only/x", 4, 0, &off, err, sizeof err) == KVSPACE_OK,
          "建 /only/x 失败: %s", err);
    clear_err();
    CHECK(kvspaceListlen(kv, "/only", &count, &max_name, err, sizeof err) ==
              KVSPACE_MISSING,
          "只有后代的 /only（无根值）应返回 MISSING");
    clear_err();
    CHECK(kvspaceRealloc(kv, "/only", 4, 0, &off, err, sizeof err) == KVSPACE_OK,
          "给 /only 补根值失败: %s", err);
    clear_err();
    CHECK(kvspaceListlen(kv, "/only", &count, &max_name, err, sizeof err) ==
              KVSPACE_NOT_INDEX,
          "补上根值后 /only 应翻转为 NOT_INDEX（存在但不是索引）");
}

/* ── cp / cpdir / deltree ───────────────────────────────────────────── */

static void test_copy_and_delete(void *kv) {
    uint64_t off = 0;
    int64_t count_for_root = 0;
    uint32_t max_for_root = 0;

    CHECK(kvspaceRealloc(kv, "/cp/src", 4, 0, &off, err, sizeof err) == KVSPACE_OK,
          "建 /cp/src 失败: %s", err);
    clear_err();
    CHECK(kvspaceCp(kv, "/cp/src", "/cp/dst", err, sizeof err) == KVSPACE_OK,
          "kvspaceCp 失败: %s", err);
    clear_err();
    CHECK(kvspaceResolve(kv, "/cp/dst", 0, &off, err, sizeof err) == KVSPACE_OK,
          "cp 之后目标不存在: %s", err);

    /* cpdir：整棵子树复制。 */
    CHECK(kvspaceMkindex(kv, "/cd/src/", err, sizeof err) == KVSPACE_OK,
          "建 /cd/src/ 失败: %s", err);
    CHECK(kvspaceRealloc(kv, "/cd/src/m", 2, 0, &off, err, sizeof err) == KVSPACE_OK,
          "建 /cd/src/m 失败: %s", err);
    clear_err();
    CHECK(kvspaceCpdir(kv, "/cd/src/", "/cd/dst/", err, sizeof err) == KVSPACE_OK,
          "kvspaceCpdir 失败: %s", err);
    clear_err();
    CHECK(kvspaceResolve(kv, "/cd/dst/m", 0, &off, err, sizeof err) == KVSPACE_OK,
          "cpdir 之后子项不存在: %s", err);

    /* deltree = **连根删**：key 本身与全部后代一起消失（架构师裁决）。
     * 只验子项没了会漏掉「根还在」的实现，所以根必须单独查一次。 */
    clear_err();
    CHECK(kvspaceDeltree(kv, "/cd/src/", err, sizeof err) == KVSPACE_OK,
          "kvspaceDeltree 失败: %s", err);
    clear_err();
    CHECK(kvspaceResolve(kv, "/cd/src/m", 0, &off, err, sizeof err) ==
              KVSPACE_MISSING,
          "deltree 之后子项仍可 resolve");
    clear_err();
    CHECK(kvspaceListlen(kv, "/cd/src/", &count_for_root, &max_for_root, err,
                         sizeof err) == KVSPACE_MISSING,
          "deltree 之后根索引仍存在（应为连根删）");

    /* key 不存在视为成功：删除幂等，第二次删同一棵树不该报错。 */
    clear_err();
    CHECK(kvspaceDeltree(kv, "/cd/src/", err, sizeof err) == KVSPACE_OK,
          "deltree 不存在的 key 应视为成功");
    /* 兄弟子树不受牵连。 */
    clear_err();
    CHECK(kvspaceResolve(kv, "/cd/dst/m", 0, &off, err, sizeof err) == KVSPACE_OK,
          "deltree 连带删掉了兄弟子树 /cd/dst/");
}

int main(void) {
    /* 进程独占的 shm 路径：带 pid，绝不与别的用例或 /usr/lib 默认实例撞车。
     * 用完删掉；即便中途崩溃，也只是留下一个本次 pid 的临时文件。 */
    char dsn[256];
    snprintf(dsn, sizeof dsn, "shm:///tmp/kvspace_storage_shm_%ld.shm",
             (long)getpid());

    void *kv = kvspaceConnect(dsn);
    if (!kv) {
        fprintf(stderr, "connect %s failed（后端是否已导出新符号？）\n", dsn);
        return 2;
    }

    test_resolve_realloc(kv);
    test_makeindex_free(kv);
    test_list(kv);
    test_existence_three_states(kv);
    test_copy_and_delete(kv);

    kvspaceClose(kv);

    char path[256];
    snprintf(path, sizeof path, "/tmp/kvspace_storage_shm_%ld.shm",
             (long)getpid());
    unlink(path);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}

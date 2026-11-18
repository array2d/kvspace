#include <kvspace/kvspace.h>
#include <kvspace/kvspace_xvalue.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int failures;
#define CHECK(condition,...) \
    do { \
        if (!(condition)) { \
            failures++; \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } while (0)
static void test_resolve_realloc(void *kv) {
    uint64_t first = 0, again = 0, grown = 0;
    uint8_t *base = kvspaceXvalueInProcessAddress(kv);
    CHECK(base != NULL, "shm 后端必须提供非 NULL 的 kvspaceXvalueInProcessAddress");
    CHECK(kvspaceRealloc(kv, "/ct/a", 8, 0, &first) == KVSPACE_OK,"realloc 创建失败");
    if (base) {
        uint64_t probe = 0x0123456789ABCDEFULL;
        memcpy(base + first + KVSPACE_XVALUE_HEAD_LEN, &probe, sizeof probe);
        uint64_t back = 0;
        memcpy(&back, base + first + KVSPACE_XVALUE_HEAD_LEN, sizeof back);
        CHECK(back == probe, "base+offset 写读回的字节不对");
    }
    CHECK(kvspaceResolveOffset(kv, "/ct/a", NULL, &again) == KVSPACE_OK,"resolve 已存在的 key 失败");
    CHECK(again == first,
          "resolve 的 offset(%llu) 与 realloc 返回(%llu) 不一致",
          (unsigned long long)again, (unsigned long long)first);
    int rc = kvspaceResolveOffset(kv, "/ct/never", NULL, &again);
    CHECK(rc == KVSPACE_MISSING,"resolve 不存在的 key 应返回 MISSING，实得 %d ()", rc);
    CHECK(kvspaceRealloc(kv, "/ct/a", 8, 0, &grown) == KVSPACE_OK,"同尺寸 realloc 失败");
    CHECK(grown == first,
          "同尺寸 realloc 不该搬移：offset 从 %llu 变成 %llu",
          (unsigned long long)first, (unsigned long long)grown);
}
static void test_makeindex_free(void *kv) {
    uint64_t off = 0;
    CHECK(kvspaceMkindex(kv, "/idx/") == KVSPACE_OK,"makeindex 失败");
    CHECK(kvspaceMkindex(kv, "/idx/") == KVSPACE_OK,"makeindex 第二次应幂等成功");
    CHECK(kvspaceFree(kv, "/idx/") == KVSPACE_OK,"free 空索引失败");
    CHECK(kvspaceMkindex(kv, "/idx2/") == KVSPACE_OK,"makeindex /idx2/ 失败");
    CHECK(kvspaceRealloc(kv, "/idx2/member", 4, 0, &off) ==
              KVSPACE_OK,"在 /idx2/ 下建成员失败");
    CHECK(kvspaceFree(kv, "/idx2/") == KVSPACE_HAS_CHILDREN,
          "free 非空索引应返回 HAS_CHILDREN");
    CHECK(kvspaceResolveOffset(kv, "/idx2/member", NULL, &off) ==
              KVSPACE_OK,"free 非空索引失败后，成员不该消失");
}
static void test_list(void *kv) {
    uint64_t off = 0;
    int64_t count = -1;
    uint32_t max_name = 0;
    CHECK(kvspaceMkindex(kv, "/ls/") == KVSPACE_OK,"makeindex /ls/ 失败");
    CHECK(kvspaceListlen(kv, "/ls/", &count, &max_name) ==
              KVSPACE_OK,"listlen 空索引失败");
    CHECK(count == 0 && max_name == 0, "空索引 N/M 应为 0/0，实得 %lld/%u",
          (long long)count, max_name);
    const char *names[] = {"aa", "bbb", "c"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char key[64];
        snprintf(key, sizeof key, "/ls/%s", names[i]);
        CHECK(kvspaceRealloc(kv, key, 1, 0, &off) == KVSPACE_OK,"建成员 %s 失败", key);
    }
    CHECK(kvspaceListlen(kv, "/ls/", &count, &max_name) ==
              KVSPACE_OK,"listlen 失败");
    CHECK(count == 3, "N 应为 3，实得 %lld", (long long)count);
    CHECK(max_name == 3, "M 应为最长子项字节数 3，实得 %u", max_name);
    uint32_t length = 0, listed = 0;
    uint64_t generation = 0;
    CHECK(kvspaceList(kv, "/ls/", NULL, 0, &length, &listed, &generation) == KVSPACE_OK,"list(NULL 只探测) 失败");
    CHECK(length == 9 && listed == 3,
          "list 探测应得 length=9 count=3，实得 %u/%u", length, listed);
    char buf[32];
    uint32_t length2 = 0, listed2 = 0;
    uint64_t generation2 = 0;
    CHECK(kvspaceList(kv, "/ls/", buf, sizeof buf, &length2, &listed2,
                      &generation2) == KVSPACE_OK,"list 复制失败");
    CHECK(length2 == 9 && listed2 == 3,
          "list 复制应得 length=9 count=3，实得 %u/%u", length2, listed2);
    CHECK(memcmp(buf, "aa\0bbb\0c\0", 9) == 0,
          "list 输出的 NUL 分隔串不对（前 9 字节）");
    uint32_t short_len = 0, short_count = 0;
    uint64_t short_gen = 0;
    CHECK(kvspaceList(kv, "/ls/", buf, 4, &short_len, &short_count, &short_gen) == KVSPACE_CAPACITY,
          "容量不足应返回 CAPACITY");
    CHECK(short_len == 9 && short_count == 3,
          "容量不足仍须回报所需 length=9 count=3，实得 %u/%u", short_len,
          short_count);
    uint64_t gen_before = 0, gen_add = 0, gen_modify = 0;
    CHECK(kvspaceList(kv, "/ls/", NULL, 0, &length, &listed, &gen_before) == KVSPACE_OK,"取 generation 基线失败");
    char key[64];
    snprintf(key, sizeof key, "/ls/%s", "dddd");
    CHECK(kvspaceRealloc(kv, key, 1, 0, &off) == KVSPACE_OK,"新增成员失败");
    CHECK(kvspaceList(kv, "/ls/", NULL, 0, &length, &listed, &gen_add) == KVSPACE_OK,"取新增后 generation 失败");
    CHECK(gen_add != gen_before, "新增成员后 generation 应前进，仍是 %llu",
          (unsigned long long)gen_add);
    CHECK(kvspaceRealloc(kv, key, 2, 0, &off) == KVSPACE_OK,"改成员内容失败");
    CHECK(kvspaceList(kv, "/ls/", NULL, 0, &length, &listed, &gen_modify) == KVSPACE_OK,"取值修改后 generation 失败");
    CHECK(gen_modify == gen_add,
          "只改值不该推进 generation，从 %llu 变成 %llu",
          (unsigned long long)gen_add, (unsigned long long)gen_modify);
    CHECK(kvspaceListlen(kv, "/ct/a", &count, &max_name) ==
              KVSPACE_NOT_INDEX,
          "对非索引 listlen 应返回 NOT_INDEX");
}
static void test_existence_three_states(void *kv) {
    int64_t count = -1;
    uint32_t max_name = 0;
    uint32_t length = 0, listed = 0;
    uint64_t generation = 0;
    char names[64];
    uint64_t off = 0;
    CHECK(kvspaceListlen(kv, "/never/", &count, &max_name) ==
              KVSPACE_MISSING,
          "从没建过的 /never/ 应返回 MISSING，而非 OK/0/0");
    CHECK(kvspaceListlen(kv, "/never", &count, &max_name) ==
              KVSPACE_MISSING,
          "从没建过的 /never 应返回 MISSING");
    CHECK(kvspaceList(kv, "/never/", names, sizeof names, &length, &listed,
                      &generation) == KVSPACE_MISSING,
          "从没建过的 /never/ 上 list 应返回 MISSING");
    CHECK(kvspaceList(kv, "/never", names, sizeof names, &length, &listed,
                      &generation) == KVSPACE_MISSING,
          "从没建过的 /never 上 list 应返回 MISSING");
    CHECK(kvspaceRealloc(kv, "/plain", 4, 0, &off) == KVSPACE_OK,"建普通值 /plain 失败");
    CHECK(kvspaceListlen(kv, "/plain", &count, &max_name) ==
              KVSPACE_NOT_INDEX,
          "已存在的普通值 /plain 应返回 NOT_INDEX");
    CHECK(kvspaceList(kv, "/plain", names, sizeof names, &length, &listed,
                      &generation) == KVSPACE_NOT_INDEX,
          "已存在的普通值 /plain 上 list 应返回 NOT_INDEX");
    CHECK(kvspaceMkindex(kv, "/empty/") == KVSPACE_OK,"建空索引 /empty/ 失败");
    CHECK(kvspaceListlen(kv, "/empty/", &count, &max_name) ==
              KVSPACE_OK,"已存在的空索引应返回 OK");
    CHECK(count == 0 && max_name == 0,
          "已存在的空索引 N/M 应为 0/0，实得 %lld/%u", (long long)count,
          max_name);
    CHECK(kvspaceList(kv, "/empty/", names, sizeof names, &length, &listed,
                      &generation) == KVSPACE_OK,"已存在的空索引上 list 应返回 OK");
    CHECK(length == 0 && listed == 0,
          "已存在的空索引 list 应得 len=0 count=0，实得 %u/%u", length, listed);
    CHECK(kvspaceMkindex(kv, "/full/") == KVSPACE_OK,"建 /full/ 失败");
    CHECK(kvspaceRealloc(kv, "/full/ab", 1, 0, &off) == KVSPACE_OK,"建 /full/ab 失败");
    CHECK(kvspaceListlen(kv, "/full/", &count, &max_name) ==
              KVSPACE_OK,"非空索引应返回 OK");
    CHECK(count == 1 && max_name == 2,
          "非空索引应得 N=1 M=2，实得 %lld/%u", (long long)count, max_name);
    CHECK(kvspaceMkindex(kv, "/desc/child/") == KVSPACE_OK,"建 /desc/child/ 失败");
    CHECK(kvspaceListlen(kv, "/desc/", &count, &max_name) ==
              KVSPACE_OK,"有后代但无根值的 /desc/ 应返回 OK（index 形态可由后代产生）");
    CHECK(count == 1 && max_name == 6,
          "有后代无根值应得 N=1 M=6，实得 %lld/%u", (long long)count, max_name);
    CHECK(kvspaceListlen(kv, "/desc", &count, &max_name) ==
              KVSPACE_MISSING,
          "非索引形态 /desc 自己没有根值 → MISSING（「有后代即存在」只管 index 形态）");
    CHECK(kvspaceRealloc(kv, "/only/x", 4, 0, &off) == KVSPACE_OK,"建 /only/x 失败");
    CHECK(kvspaceListlen(kv, "/only", &count, &max_name) ==
              KVSPACE_MISSING,
          "只有后代的 /only（无根值）应返回 MISSING");
    CHECK(kvspaceRealloc(kv, "/only", 4, 0, &off) == KVSPACE_OK,"给 /only 补根值失败");
    CHECK(kvspaceListlen(kv, "/only", &count, &max_name) ==
              KVSPACE_NOT_INDEX,
          "补上根值后 /only 应翻转为 NOT_INDEX（存在但不是索引）");
}
static void test_copy_and_delete(void *kv) {
    uint64_t off = 0;
    int64_t count_for_root = 0;
    uint32_t max_for_root = 0;
    CHECK(kvspaceRealloc(kv, "/cp/src", 4, 0, &off) == KVSPACE_OK,"建 /cp/src 失败");
    CHECK(kvspaceCp(kv, "/cp/src", "/cp/dst") == KVSPACE_OK,"kvspaceCp 失败");
    CHECK(kvspaceResolveOffset(kv, "/cp/dst", NULL, &off) == KVSPACE_OK,"cp 之后目标不存在");
    CHECK(kvspaceMkindex(kv, "/cd/src/") == KVSPACE_OK,"建 /cd/src/ 失败");
    CHECK(kvspaceRealloc(kv, "/cd/src/m", 2, 0, &off) == KVSPACE_OK,"建 /cd/src/m 失败");
    CHECK(kvspaceCpdir(kv, "/cd/src/", "/cd/dst/") == KVSPACE_OK,"kvspaceCpdir 失败");
    CHECK(kvspaceResolveOffset(kv, "/cd/dst/m", NULL, &off) == KVSPACE_OK,"cpdir 之后子项不存在");
    CHECK(kvspaceDeltree(kv, "/cd/src/") == KVSPACE_OK,"kvspaceDeltree 失败");
    CHECK(kvspaceResolveOffset(kv, "/cd/src/m", NULL, &off) ==
              KVSPACE_MISSING,
          "deltree 之后子项仍可 resolve");
    CHECK(kvspaceListlen(kv, "/cd/src/", &count_for_root, &max_for_root) == KVSPACE_MISSING,
          "deltree 之后根索引仍存在（应为连根删）");
    CHECK(kvspaceDeltree(kv, "/cd/src/") == KVSPACE_OK,
          "deltree 不存在的 key 应视为成功");
    CHECK(kvspaceResolveOffset(kv, "/cd/dst/m", NULL, &off) == KVSPACE_OK,
          "deltree 连带删掉了兄弟子树 /cd/dst/");
}
int main(void) {
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
    static const char *const suffixes[] = {"", ".sbo.head", ".sbo.data", ".retirement"};
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        char path[256];
        snprintf(path, sizeof path, "/tmp/kvspace_storage_shm_%ld.shm%s",
                 (long)getpid(), suffixes[i]);
        unlink(path);
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}

#define _GNU_SOURCE
#include "kvspace/kvspace.h"
#include "kvspace/kvspace_xvalue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int failures;
#define CHECK(cond,...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
            ++failures; \
        } \
    } while (0)
int main(void) {
    char path[] = "/tmp/kvspace-anchor-hook-XXXXXX";
    char *dir = mkdtemp(path);
    if (!dir) {
        perror("mkdtemp");
        return 1;
    }
    char dsn[512];
    snprintf(dsn, sizeof dsn, "shm://%s/db", dir);
    void *h = kvspaceConnect(dsn);
    if (!h) {
        fprintf(stderr, "FAIL: connect %s\n", dsn);
        return 1;
    }
    int rc = kvspaceMkindex(h, "/d/");
    CHECK(rc == KVSPACE_OK,"mkindex /d/ rc=%d", rc);
    uint64_t off = 0;
    rc = kvspaceRealloc(h, "/d/x", 8, 0, &off);
    CHECK(rc == KVSPACE_OK,"realloc /d/x rc=%d", rc);
    uint8_t *base = kvspaceXvalueInProcessAddress(h);
    CHECK(base != NULL, "in-process address must not be NULL for shm");
    if (base) {
        for (int i = 0; i < 8; i++)
            base[off + KVSPACE_XVALUE_HEAD_LEN + (uint64_t)i] = (uint8_t)(i + 1);
    }
    kvspace_anchor_t anchor = {0};
    rc = kvspaceAnchorOf(h, "/d/", &anchor);
    CHECK(rc == KVSPACE_OK,"anchor_of /d/ rc=%d", rc);
    CHECK(anchor.node_offset > 0, "anchor 必须是正的 key 区偏移，实际 %llu",
          (unsigned long long)anchor.node_offset);
    CHECK(anchor.depth == 3, "anchor.depth 应是父 key 字节数 3，实际 %u",
          anchor.depth);
    CHECK(anchor.flags == KVSPACE_ANCHOR_FLAG_INDEX,
          "anchor.flags 应含 INDEX，实际 %u", anchor.flags);
    uint64_t child_off = 0;
    rc = kvspaceResolveOffset(h, "x", &anchor, &child_off);
    CHECK(rc == KVSPACE_OK,
          "用 anchor 解析相对后缀 x 必须成功（rc=%d）—— "
          "否则这个 anchor 就不是可用锚点",
          rc);
    CHECK(child_off == off,
          "经 anchor 解析出的偏移应与 Realloc 返回的一致：%llu vs %llu",
          (unsigned long long)child_off, (unsigned long long)off);
    uint64_t parent_off = 0;
    rc = kvspaceResolveOffset(h, "", &anchor, &parent_off);
    CHECK(rc == KVSPACE_OK,"空后缀解析到父节点自身 rc=%d", rc);
    static const char *const siblings[] = {"/d/a", "/d/b", "/d/c", "/d/d",
                                           "/d/e"};
    for (size_t i = 0; i < sizeof siblings / sizeof siblings[0]; i++) {
        uint64_t sibling_off = 0;
        rc = kvspaceRealloc(h, siblings[i], 1, 0, &sibling_off);
        CHECK(rc == KVSPACE_OK,"realloc %s rc=%d", siblings[i], rc);
    }
    int64_t child_count = 0;
    uint32_t max_name_length = 0;
    rc = kvspaceListlen(h, "/d/", &child_count, &max_name_length);
    CHECK(rc == KVSPACE_OK && child_count == 6,"/d/ 必须有 6 个子项（N4 上限 4，必然已增长）：rc=%d n=%lld",
          rc, (long long)child_count);
    uint64_t grown_off = 0;
    rc = kvspaceResolveOffset(h, "x", &anchor, &grown_off);
    CHECK(rc == KVSPACE_OK && grown_off == off,
          "节点增长（ART_MOVED）后旧 anchor 仍须解析到同一偏移：rc=%d "
          "off=%llu vs %llu",
          rc, (unsigned long long)grown_off, (unsigned long long)off);
    rc = kvspaceResolveOffset(h, "/d/x", &anchor, &child_off);
    CHECK(rc == KVSPACE_INVALID, "绝对后缀必须 INVALID（rc=%d）", rc);
    kvspace_anchor_t no_flag = anchor;
    no_flag.flags = 0;
    rc = kvspaceResolveOffset(h, "x", &no_flag, &child_off);
    CHECK(rc == KVSPACE_ANCHOR_INVALID,
          "flags 不含 INDEX 必须 ANCHOR_INVALID（rc=%d）", rc);
    kvspace_anchor_t untouched = {0};
    rc = kvspaceAnchorOf(h, "/d/x", &untouched);
    CHECK(rc == KVSPACE_NOT_INDEX, "值 key 不是 index，必须 NOT_INDEX（rc=%d）",
          rc);
    CHECK(untouched.node_offset == 0, "失败时不得写 out_anchor，实际 %llu",
          (unsigned long long)untouched.node_offset);
    rc = kvspaceAnchorOf(h, "/no-such-parent/", &untouched);
    CHECK(rc == KVSPACE_MISSING,
          "不存在的父节点必须 MISSING（rc=%d，期望 %d）", rc, KVSPACE_MISSING);
    CHECK(untouched.node_offset == 0, "失败时不得写 out_anchor，实际 %llu",
          (unsigned long long)untouched.node_offset);
    kvspaceClose(h);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: cleanup %s failed\n", dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("anchor_hook: all checks passed\n");
    return 0;
}

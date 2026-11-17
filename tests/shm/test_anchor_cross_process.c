#define _GNU_SOURCE
#include "kvspace/kvspace.h"
#include "kvspace/kvspace_xvalue.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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
static int gate_write(int fd, char value) {
    ssize_t n;
    do {
        n = write(fd, &value, 1);
    } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}
static int gate_read(int fd, char *value) {
    ssize_t n;
    do {
        n = read(fd, value, 1);
    } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}
static int child_main(const char *dsn, void *inherited,
                      const kvspace_anchor_t *anchor, uint64_t parent_offset,
                      const uint8_t *expected, int up_fd, int down_fd) {
    char gate = 0;
    int failures = 0;
    uint64_t offset = 0;
    kvspaceClose(inherited);
    void *h = kvspaceConnect(dsn);
    if (!h) {
        fprintf(stderr, "child: reconnect %s failed\n", dsn);
        return 1;
    }
    uint8_t *base = kvspaceXvalueInProcessAddress(h);
    if (!base) {
        fprintf(stderr, "child: in-process address is NULL\n");
        return 1;
    }
    int rc = kvspaceResolveOffset(h, "c", anchor, &offset);
    if (rc != KVSPACE_OK) {
        fprintf(stderr,"child: resolve c via inherited anchor rc=%d\n",
                rc);
        ++failures;
    } else if (offset != parent_offset) {
        fprintf(stderr,
                "child: resolved offset %llu differs from parent offset %llu\n",
                (unsigned long long)offset, (unsigned long long)parent_offset);
        ++failures;
    } else {
        const uint8_t *body = base + offset + KVSPACE_XVALUE_HEAD_LEN;
        if (memcmp(body, expected, 8) != 0) {
            fprintf(stderr, "child: value bytes differ across process\n");
            ++failures;
        }
    }
    kvspace_anchor_t bad = *anchor;
    bad.node_offset = 16;
    rc = kvspaceResolveOffset(h, "c", &bad, &offset);
    if (rc != KVSPACE_ANCHOR_INVALID) {
        fprintf(stderr, "child: misaligned anchor rc=%d, expected %d\n", rc,
                KVSPACE_ANCHOR_INVALID);
        ++failures;
    }
    bad = *anchor;
    bad.flags = 0;
    rc = kvspaceResolveOffset(h, "c", &bad, &offset);
    if (rc != KVSPACE_ANCHOR_INVALID) {
        fprintf(stderr, "child: anchor without INDEX flag rc=%d, expected %d\n",
                rc, KVSPACE_ANCHOR_INVALID);
        ++failures;
    }
    rc = kvspaceResolveOffset(h, "/p/c", anchor, &offset);
    if (rc != KVSPACE_INVALID) {
        fprintf(stderr, "child: absolute suffix rc=%d, expected %d\n", rc,
                KVSPACE_INVALID);
        ++failures;
    }
    if (gate_write(up_fd, failures ? 0 : 1) != 0 ||
        gate_read(down_fd, &gate) != 0 || gate != '2') {
        fprintf(stderr, "child: stage 2 gate failed\n");
        return 1;
    }
    rc = kvspaceResolveOffset(h, "c", anchor, &offset);
    if (rc != KVSPACE_OK || offset != parent_offset) {
        fprintf(stderr,"child: resolve after unrelated insert rc=%d offset=%llu\n",
                rc, (unsigned long long)offset);
        ++failures;
    } else if (memcmp(base + offset + KVSPACE_XVALUE_HEAD_LEN, expected, 8) !=
               0) {
        fprintf(stderr, "child: value bytes differ after unrelated insert\n");
        ++failures;
    }
    if (gate_write(up_fd, failures ? 0 : 1) != 0 ||
        gate_read(down_fd, &gate) != 0 || gate != '3') {
        fprintf(stderr, "child: stage 3 gate failed\n");
        return 1;
    }
    rc = kvspaceResolveOffset(h, "c", anchor, &offset);
    if (rc != KVSPACE_MISSING) {
        fprintf(stderr, "child: resolve freed child rc=%d, expected %d\n", rc,
                KVSPACE_MISSING);
        ++failures;
    }
    if (gate_write(up_fd, failures ? 0 : 1) != 0) {
        fprintf(stderr, "child: final gate write failed\n");
        return 1;
    }
    kvspaceClose(h);
    return failures ? 1 : 0;
}
static int child_resolve_relative(const char *dsn, void *inherited,
                                  const kvspace_anchor_t *anchor,
                                  const char *suffix, uint64_t expected_offset,
                                  const uint8_t *expected, uint32_t expected_len) {
    int failures = 0;
    uint64_t offset = 0;
    kvspaceClose(inherited);
    void *h = kvspaceConnect(dsn);
    if (!h) {
        fprintf(stderr, "child: reconnect %s failed\n", dsn);
        return 1;
    }
    uint8_t *base = kvspaceXvalueInProcessAddress(h);
    if (!base) {
        fprintf(stderr, "child: in-process address is NULL\n");
        kvspaceClose(h);
        return 1;
    }
    int rc = kvspaceResolveOffset(h, suffix, anchor, &offset);
    if (rc != KVSPACE_OK) {
        fprintf(stderr,"child: resolve \"%s\" rc=%d\n", suffix, rc);
        ++failures;
    } else if (offset != expected_offset) {
        fprintf(stderr,
                "child: resolve \"%s\" offset %llu differs from expected %llu\n",
                suffix, (unsigned long long)offset,
                (unsigned long long)expected_offset);
        ++failures;
    } else if (expected_len) {
        const kvspaceXvalueHead *head =
            (const kvspaceXvalueHead *)(const void *)(base + offset);
        uint64_t body = kvspaceXvalueHeadLen(head);
        if (body != expected_len) {
            fprintf(stderr, "child: resolve \"%s\" bodylen %llu != %u\n",
                    suffix, (unsigned long long)body, expected_len);
            ++failures;
        } else if (memcmp(base + offset + KVSPACE_XVALUE_HEAD_LEN, expected,
                          expected_len) != 0) {
            fprintf(stderr, "child: resolve \"%s\" body bytes differ\n", suffix);
            ++failures;
        }
    }
    kvspaceClose(h);
    return failures ? 1 : 0;
}
static int run_child_relative(const char *dsn, void *inherited,
                              const kvspace_anchor_t *anchor,
                              const char *suffix, uint64_t expected_offset,
                              const uint8_t *expected, uint32_t expected_len) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0)
        _exit(child_resolve_relative(dsn, inherited, anchor, suffix,
                                     expected_offset, expected, expected_len));
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        perror("waitpid");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child failed: status %d (exited=%d code=%d)\n", status,
                WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 1;
    }
    return 0;
}
static int run_child_after_parent_close(const char *dsn, void **handle,
                                        const kvspace_anchor_t *anchor,
                                        const char *suffix,
                                        uint64_t expected_offset,
                                        const uint8_t *expected,
                                        uint32_t expected_len) {
    void *inherited = *handle;
    int ready[2];
    int result[2];
    if (pipe(ready) != 0 || pipe(result) != 0) {
        perror("pipe");
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        char gate = 0;
        close(ready[1]);
        close(result[0]);
        if (gate_read(ready[0], &gate) != 0 || gate != 'x')
            _exit(1);
        int code = child_resolve_relative(dsn, inherited, anchor, suffix,
                                          expected_offset, expected,
                                          expected_len);
        if (gate_write(result[1], code ? 0 : 1) != 0)
            _exit(1);
        _exit(code);
    }
    close(ready[0]);
    close(result[1]);
    kvspaceClose(*handle);
    *handle = NULL;
    char report = 0;
    int ok = gate_write(ready[1], 'x') == 0 &&
             gate_read(result[0], &report) == 0 && report == 1;
    close(ready[1]);
    close(result[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        perror("waitpid");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child after parent close failed: status %d (exited=%d code=%d)\n",
                status, WIFEXITED(status),
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        ok = 0;
    }
    if (!ok)
        fprintf(stderr, "child after parent close did not report success\n");
    return ok ? 0 : 1;
}
int main(void) {
    char path[] = "/tmp/kvspace-anchor-xproc-XXXXXX";
    char *dir = mkdtemp(path);
    if (!dir) {
        perror("mkdtemp");
        return 1;
    }
    char dsn[520];
    snprintf(dsn, sizeof dsn, "shm://%s/db", dir);
    void *h = kvspaceConnect(dsn);
    if (!h) {
        fprintf(stderr, "FAIL: connect %s\n", dsn);
        return 1;
    }
    CHECK(kvspaceMkindex(h, "/p/") == KVSPACE_OK,"mkindex /p/");
    uint64_t parent_offset = 0;
    CHECK(kvspaceRealloc(h, "/p/c", 8, 0, &parent_offset) ==
              KVSPACE_OK,"realloc /p/c");
    uint8_t expected[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t *base = kvspaceXvalueInProcessAddress(h);
    CHECK(base != NULL, "in-process address is NULL");
    if (base)
        memcpy(base + parent_offset + KVSPACE_XVALUE_HEAD_LEN, expected, 8);
    kvspace_anchor_t anchor = {0};
    CHECK(kvspaceAnchorOf(h, "/p/", &anchor) == KVSPACE_OK,"anchor_of /p/");
    uint64_t resolved = 0;
    CHECK(kvspaceResolveOffset(h, "c", &anchor, &resolved) ==
                  KVSPACE_OK &&
              resolved == parent_offset,"parent relative resolve differs");
    int up[2], down[2];
    CHECK(pipe(up) == 0 && pipe(down) == 0, "pipe failed");
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        close(up[0]);
        close(down[1]);
        int code = child_main(dsn, h, &anchor, parent_offset, expected, up[1],
                              down[0]);
        _exit(code);
    }
    close(up[1]);
    close(down[0]);
    char gate = 0;
    CHECK(gate_read(up[0], &gate) == 0 && gate == 1,
          "child stage 1 failed (gate %d)", (int)gate);
    uint64_t other_offset = 0;
    CHECK(kvspaceRealloc(h, "/other/x", 4, 0, &other_offset) ==
              KVSPACE_OK,"realloc /other/x");
    kvspace_anchor_t after = {0};
    CHECK(kvspaceAnchorOf(h, "/p/", &after) == KVSPACE_OK,"anchor_of /p/ after insert");
    CHECK(after.epoch != anchor.epoch,
          "gen_epoch 应变化：before=%llu after=%llu",
          (unsigned long long)anchor.epoch, (unsigned long long)after.epoch);
    CHECK(gate_write(down[1], '2') == 0, "write gate 2 failed");
    CHECK(gate_read(up[0], &gate) == 0 && gate == 1,
          "child stage 2 failed (gate %d)", (int)gate);
    CHECK(kvspaceFree(h, "/p/c") == KVSPACE_OK,"free /p/c");
    CHECK(gate_write(down[1], '3') == 0, "write gate 3 failed");
    CHECK(gate_read(up[0], &gate) == 0 && gate == 1,
          "child stage 3 failed (gate %d)", (int)gate);
    close(up[0]);
    close(down[1]);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid, "waitpid failed");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child exit status %d (exited=%d code=%d)", status,
          WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    {
        uint8_t y[4] = {0x11, 0x22, 0x33, 0x44};
        uint64_t y_offset = 0;
        CHECK(kvspaceMkindex(h, "/ms/") == KVSPACE_OK,"mkindex /ms/");
        CHECK(kvspaceRealloc(h, "/ms/sub/y", sizeof y, 0, &y_offset) == KVSPACE_OK,"realloc /ms/sub/y");
        if (base)
            memcpy(base + y_offset + KVSPACE_XVALUE_HEAD_LEN, y, sizeof y);
        uint64_t absolute = 0;
        CHECK(kvspaceResolveOffset(h, "/ms/sub/y", NULL, &absolute) == KVSPACE_OK &&
                  absolute == y_offset,"parent absolute /ms/sub/y");
        kvspace_anchor_t anchor = {0};
        CHECK(kvspaceAnchorOf(h, "/ms/", &anchor) == KVSPACE_OK,"anchor_of /ms/");
        CHECK(run_child_relative(dsn, h, &anchor, "sub/y", y_offset, y,
                                 sizeof y) == 0,
              "multi-segment relative suffix failed across process");
    }
    {
        uint8_t body[8] = {9, 8, 7, 6, 5, 4, 3, 2};
        uint64_t c_offset = 0;
        CHECK(kvspaceMkindex(h, "/mv/") == KVSPACE_OK,"mkindex /mv/");
        CHECK(kvspaceRealloc(h, "/mv/c", sizeof body, 0, &c_offset) == KVSPACE_OK,"realloc /mv/c");
        if (base)
            memcpy(base + c_offset + KVSPACE_XVALUE_HEAD_LEN, body,
                   sizeof body);
        kvspace_anchor_t old_anchor = {0};
        CHECK(kvspaceAnchorOf(h, "/mv/", &old_anchor) ==
                  KVSPACE_OK,"anchor_of /mv/");
        static const char extra[] = "abdefghi";
        for (size_t i = 0; i < sizeof extra - 1; i++) {
            char key[32];
            uint64_t temporary = 0;
            snprintf(key, sizeof key, "/mv/%c", extra[i]);
            CHECK(kvspaceRealloc(h, key, 1, 0, &temporary) ==
                      KVSPACE_OK,"realloc %s", key);
        }
        kvspace_anchor_t moved = {0};
        CHECK(kvspaceAnchorOf(h, "/mv/", &moved) == KVSPACE_OK,"anchor_of /mv/ after growth");
        CHECK(moved.node_offset != old_anchor.node_offset,
              "anchor node did not move after growth (offset %llu)",
              (unsigned long long)old_anchor.node_offset);
        CHECK(moved.epoch != old_anchor.epoch,
              "epoch did not advance after growth (%llu)",
              (unsigned long long)old_anchor.epoch);
        CHECK(run_child_relative(dsn, h, &old_anchor, "c", c_offset, body,
                                 sizeof body) == 0,
              "MOVED old anchor relative resolve failed across process");
    }
    {
        uint8_t dbody[2] = {0x5a, 0x5b};
        uint64_t x_offset = 0;
        CHECK(kvspaceMkindex(h, "/m·") == KVSPACE_OK,"mkindex /m·");
        CHECK(kvspaceRealloc(h, "/m·x", sizeof dbody, 0, &x_offset) == KVSPACE_OK,"realloc /m·x");
        if (base)
            memcpy(base + x_offset + KVSPACE_XVALUE_HEAD_LEN, dbody,
                   sizeof dbody);
        kvspace_anchor_t anchor = {0};
        CHECK(kvspaceAnchorOf(h, "/m·", &anchor) == KVSPACE_OK,"anchor_of /m·");
        CHECK(run_child_relative(dsn, h, &anchor, "x", x_offset, dbody,
                                 sizeof dbody) == 0,
              "dot separator anchor relative resolve failed across process");
    }
    {
        uint8_t pbody[4] = {1, 2, 3, 4};
        uint64_t p_offset = 0;
        CHECK(kvspaceMkindex(h, "/pc/") == KVSPACE_OK,"mkindex /pc/");
        CHECK(kvspaceRealloc(h, "/pc/v", sizeof pbody, 0, &p_offset) == KVSPACE_OK,"realloc /pc/v");
        if (base)
            memcpy(base + p_offset + KVSPACE_XVALUE_HEAD_LEN, pbody,
                   sizeof pbody);
        kvspace_anchor_t anchor = {0};
        CHECK(kvspaceAnchorOf(h, "/pc/", &anchor) ==
                  KVSPACE_OK,"anchor_of /pc/");
        CHECK(run_child_after_parent_close(dsn, &h, &anchor, "v", p_offset,
                                           pbody, sizeof pbody) == 0,
              "anchor after parent close failed across process");
    }
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: cleanup %s failed\n", dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("anchor_cross_process: all checks passed\n");
    return 0;
}

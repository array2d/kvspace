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
    char err[256] = {0};
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
    int rc = kvspaceResolveOffset(h, "c", anchor, &offset, err, sizeof err);
    if (rc != KVSPACE_OK) {
        fprintf(stderr, "child: resolve c via inherited anchor rc=%d err=%s\n",
                rc, err);
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
    rc = kvspaceResolveOffset(h, "c", &bad, &offset, err, sizeof err);
    if (rc != KVSPACE_ANCHOR_INVALID) {
        fprintf(stderr, "child: misaligned anchor rc=%d, expected %d\n", rc,
                KVSPACE_ANCHOR_INVALID);
        ++failures;
    }
    bad = *anchor;
    bad.flags = 0;
    rc = kvspaceResolveOffset(h, "c", &bad, &offset, err, sizeof err);
    if (rc != KVSPACE_ANCHOR_INVALID) {
        fprintf(stderr, "child: anchor without INDEX flag rc=%d, expected %d\n",
                rc, KVSPACE_ANCHOR_INVALID);
        ++failures;
    }
    rc = kvspaceResolveOffset(h, "/p/c", anchor, &offset, err, sizeof err);
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
    rc = kvspaceResolveOffset(h, "c", anchor, &offset, err, sizeof err);
    if (rc != KVSPACE_OK || offset != parent_offset) {
        fprintf(stderr,
                "child: resolve after unrelated insert rc=%d offset=%llu err=%s\n",
                rc, (unsigned long long)offset, err);
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
    rc = kvspaceResolveOffset(h, "c", anchor, &offset, err, sizeof err);
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
    char err[256] = {0};
    CHECK(kvspaceMkindex(h, "/p/", err, sizeof err) == KVSPACE_OK,
          "mkindex /p/ err=%s", err);
    uint64_t parent_offset = 0;
    CHECK(kvspaceRealloc(h, "/p/c", 8, 0, &parent_offset, err, sizeof err) ==
              KVSPACE_OK,
          "realloc /p/c err=%s", err);
    uint8_t expected[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t *base = kvspaceXvalueInProcessAddress(h);
    CHECK(base != NULL, "in-process address is NULL");
    if (base)
        memcpy(base + parent_offset + KVSPACE_XVALUE_HEAD_LEN, expected, 8);
    kvspace_anchor_t anchor = {0};
    CHECK(kvspaceAnchorOf(h, "/p/", &anchor, err, sizeof err) == KVSPACE_OK,
          "anchor_of /p/ err=%s", err);
    uint64_t resolved = 0;
    CHECK(kvspaceResolveOffset(h, "c", &anchor, &resolved, err, sizeof err) ==
                  KVSPACE_OK &&
              resolved == parent_offset,
          "parent relative resolve differs err=%s", err);
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
    CHECK(kvspaceRealloc(h, "/other/x", 4, 0, &other_offset, err, sizeof err) ==
              KVSPACE_OK,
          "realloc /other/x err=%s", err);
    kvspace_anchor_t after = {0};
    CHECK(kvspaceAnchorOf(h, "/p/", &after, err, sizeof err) == KVSPACE_OK,
          "anchor_of /p/ after insert err=%s", err);
    CHECK(after.epoch != anchor.epoch,
          "gen_epoch 应变化：before=%llu after=%llu",
          (unsigned long long)anchor.epoch, (unsigned long long)after.epoch);
    CHECK(gate_write(down[1], '2') == 0, "write gate 2 failed");
    CHECK(gate_read(up[0], &gate) == 0 && gate == 1,
          "child stage 2 failed (gate %d)", (int)gate);
    CHECK(kvspaceFree(h, "/p/c", err, sizeof err) == KVSPACE_OK,
          "free /p/c err=%s", err);
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
    kvspaceClose(h);
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

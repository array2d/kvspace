#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "test_util.h"
#include "test_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_public_io.h"
#define SBO_DATA_SIZE (8UL * 64 * 64 * 64 * 64)
static int set_int32(kvspace_t *kv, const char *key, int32_t value) {
    uint8_t raw[4] = {(uint8_t)value, (uint8_t)(value >> 8),
                      (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    uint8_t *wire = NULL;
    uint64_t length = 0;
    if (kvspace_test_wire_scalar(raw, 4, "", &wire, &length) != 0)
        return -1;
    int status = t_set_wire(kv, key, wire, (int32_t)length);
    free(wire);
    return status;
}
static int32_t get_int32(kvspace_t *kv, const char *key) {
    int32_t length = 0;
    uint8_t *wire = t_get_wire(kv, key, 1, &length);
    if (!wire || length <= 0)
        return -1;
    kvspaceXvalueHead head;
    memcpy(&head, wire, sizeof head);
    if (kvspaceXvalueHeadLen(&head) != 4)
        return -1;
    const uint8_t *body = wire + KVSPACE_XVALUE_HEAD_LEN;
    return (int32_t)((uint32_t)body[0] | ((uint32_t)body[1] << 8) |
                     ((uint32_t)body[2] << 16) | ((uint32_t)body[3] << 24));
}
static int list_of(kvspace_t *kv, const char *prefix, char ***out_names,
                   int32_t *out_count) {
    return t_list(kv, prefix, false, 0, out_names, out_count);
}
static int has_name(char **names, int32_t count, const char *wanted) {
    for (int32_t index = 0; index < count; index++)
        if (strcmp(names[index], wanted) == 0)
            return 1;
    return 0;
}
static void free_names(char **names, int32_t count) {
    for (int32_t index = 0; index < count; index++)
        free(names[index]);
    free(names);
}
static uint64_t resolve_offset(kvspace_t *kv, const char *key) {
    uint64_t offset = 0;
    char error[128] = {0};
    if (kvspaceResolveOffset(kv, key, KVSPACE_ANCHOR_ROOT, &offset, error,
                             sizeof error) != KVSPACE_OK)
        return 0;
    return offset;
}
static void t_auto_create(const char *db) {
    kvspace_t *kv = t_open(db, SBO_DATA_SIZE);
    REQUIRE(kv != NULL, "open auto create");
    CHECK(set_int32(kv, "/a/b/c", 7) == 0, "set /a/b/c");
    CHECK(get_int32(kv, "/a/") == -1, "auto index /a/ holds an empty body");
    CHECK(resolve_offset(kv, "/a/") != 0, "auto index /a/ resolves");
    CHECK(resolve_offset(kv, "/a/b/") != 0, "auto index /a/b/ resolves");
    CHECK(get_int32(kv, "/a/b/c") == 7, "read /a/b/c");
    char **names = NULL;
    int32_t count = 0;
    REQUIRE(list_of(kv, "/a/", &names, &count) == 0, "list /a/");
    CHECK(count == 1 && has_name(names, count, "b/"), "list /a/ shows b/");
    free_names(names, count);
    names = NULL;
    count = 0;
    REQUIRE(list_of(kv, "/a/b/", &names, &count) == 0, "list /a/b/");
    CHECK(count == 1 && has_name(names, count, "c"), "list /a/b/ shows c");
    free_names(names, count);
    CHECK(set_int32(kv, "/a/b", 5) == 0, "set /a/b");
    names = NULL;
    count = 0;
    REQUIRE(list_of(kv, "/a/", &names, &count) == 0, "list /a/ with both forms");
    CHECK(count == 1 && has_name(names, count, "b/"),
          "list /a/ merges value and index forms into b/");
    free_names(names, count);
    CHECK(get_int32(kv, "/a/b") == 5, "read /a/b");
    CHECK(get_int32(kv, "/a/") == -1, "read /a/ still empty");
    CHECK(t_del(kv, "/a/b") == 0, "delete /a/b");
    names = NULL;
    count = 0;
    REQUIRE(list_of(kv, "/a/", &names, &count) == 0, "list /a/ after delete");
    CHECK(count == 1 && has_name(names, count, "b/"),
          "b/ survives value delete");
    free_names(names, count);
    CHECK(t_deltree(kv, "/a") == 0, "deltree /a");
    CHECK(resolve_offset(kv, "/a/") == 0, "/a/ gone after deltree");
    CHECK(resolve_offset(kv, "/a/b/c") == 0, "/a/b/c gone after deltree");
    t_close(kv);
}
static void t_three_forms(const char *db) {
    kvspace_t *kv = t_open(db, SBO_DATA_SIZE);
    REQUIRE(kv != NULL, "open three forms");
    CHECK(set_int32(kv, "/x/y", 1) == 0, "set /x/y");
    CHECK(set_int32(kv, "/x/y/", 2) == 0, "set /x/y/");
    CHECK(set_int32(kv, "/x/y·", 3) == 0, "set /x/y·");
    CHECK(get_int32(kv, "/x/y") == 1, "read /x/y");
    CHECK(get_int32(kv, "/x/y/") == 2, "read /x/y/");
    CHECK(get_int32(kv, "/x/y·") == 3, "read /x/y·");
    uint64_t plain = resolve_offset(kv, "/x/y");
    uint64_t slash = resolve_offset(kv, "/x/y/");
    uint64_t dot = resolve_offset(kv, "/x/y·");
    CHECK(plain != 0 && slash != 0 && dot != 0, "all three forms resolve");
    CHECK(plain != slash && plain != dot && slash != dot,
          "three forms are distinct keys");
    char **names = NULL;
    int32_t count = 0;
    REQUIRE(list_of(kv, "/x/", &names, &count) == 0, "list /x/");
    CHECK(count == 2 && has_name(names, count, "y/") &&
              has_name(names, count, "y·"),
          "list /x/ shows merged y/ and y·");
    free_names(names, count);
    CHECK(t_del(kv, "/x/y/") == 0, "delete /x/y/");
    CHECK(get_int32(kv, "/x/y") == 1 && get_int32(kv, "/x/y·") == 3,
          "sibling forms survive");
    t_close(kv);
}
static void t_anchor(const char *db) {
    kvspace_t *kv = t_open(db, SBO_DATA_SIZE);
    REQUIRE(kv != NULL, "open anchor");
    CHECK(set_int32(kv, "/d/e/f", 9) == 0, "set /d/e/f");
    uint64_t absolute = resolve_offset(kv, "/d/e/f");
    CHECK(absolute != 0, "resolve /d/e/f");
    int64_t anchor = 0;
    CHECK(kvspaceShmTestAnchorOf(kv, "/d/", &anchor) == KVSPACE_OK,
          "anchor of /d/");
    CHECK(anchor > 0, "anchor is a node offset");
    uint64_t relative = 0;
    char error[128] = {0};
    CHECK(kvspaceResolveOffset(kv, "e/f", (uint64_t)anchor, &relative, error,
                               sizeof error) == KVSPACE_OK,
          "relative resolve e/f");
    CHECK(relative == absolute, "relative resolve matches absolute");
    int64_t deep_anchor = 0;
    CHECK(kvspaceShmTestAnchorOf(kv, "/d/e/", &deep_anchor) == KVSPACE_OK,
          "anchor of /d/e/");
    relative = 0;
    CHECK(kvspaceResolveOffset(kv, "f", (uint64_t)deep_anchor, &relative, error,
                               sizeof error) == KVSPACE_OK,
          "relative resolve f");
    CHECK(relative == absolute, "nested relative resolve matches absolute");
    CHECK(kvspaceResolveOffset(kv, "/d/e/f", (uint64_t)anchor, &relative, error,
                               sizeof error) == KVSPACE_INVALID,
          "absolute key under non-zero anchor is INVALID");
    t_close(kv);
}
static void t_reopen(const char *db) {
    kvspace_t *kv = t_open(db, SBO_DATA_SIZE);
    REQUIRE(kv != NULL, "open reopen seed");
    CHECK(set_int32(kv, "/r/s/t", 42) == 0, "set /r/s/t");
    CHECK(t_mkindex(kv, "/r/empty/") == 0, "mkdir /r/empty/");
    t_close(kv);
    kv = t_open(db, SBO_DATA_SIZE);
    REQUIRE(kv != NULL, "reopen");
    CHECK(get_int32(kv, "/r/s/t") == 42, "read /r/s/t after reopen");
    char **names = NULL;
    int32_t count = 0;
    REQUIRE(list_of(kv, "/r/", &names, &count) == 0, "list /r/ after reopen");
    CHECK(count == 2 && has_name(names, count, "empty/") &&
              has_name(names, count, "s/"),
          "list /r/ after reopen");
    free_names(names, count);
    names = NULL;
    count = 0;
    REQUIRE(list_of(kv, "/r/s/", &names, &count) == 0, "list /r/s/ after reopen");
    CHECK(count == 1 && has_name(names, count, "t"), "list /r/s/ after reopen");
    free_names(names, count);
    int64_t anchor = 0;
    CHECK(kvspaceShmTestAnchorOf(kv, "/r/", &anchor) == KVSPACE_OK &&
              anchor > 0,
          "anchor survives reopen");
    CHECK(t_deltree(kv, "/r") == 0, "deltree /r");
    CHECK(resolve_offset(kv, "/r/s/t") == 0, "deltree after reopen");
    t_close(kv);
}
int main(void) {
    char template[] = "/tmp/kvspace-index-rules-XXXXXX";
    const char *directory = mkdtemp(template);
    if (!directory) {
        perror("mkdtemp");
        return 2;
    }
    char first[512], second[512], third[512], fourth[512];
    snprintf(first, sizeof first, "%s/auto", directory);
    snprintf(second, sizeof second, "%s/forms", directory);
    snprintf(third, sizeof third, "%s/anchor", directory);
    snprintf(fourth, sizeof fourth, "%s/reopen", directory);
    t_auto_create(first);
    t_three_forms(second);
    t_anchor(third);
    t_reopen(fourth);
    char command[700];
    snprintf(command, sizeof command, "rm -rf '%s'", directory);
    if (system(command) != 0)
        fprintf(stderr, "cleanup failed: %s\n", directory);
    if (failures) {
        fprintf(stderr, "FAILED (%d)\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}

#define _GNU_SOURCE
#include "test_public_io.h"
#include "test_wire.h"
#include <kvspace/kvspace.h>
#include <kvspace/kvspace_xvalue.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define SBO_DATA_SIZE (8UL * 64 * 64 * 64 * 64)
static int failures;
#define CHECK(cond,...) \
    do { \
        if (!(cond)) { \
            ++failures; \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } while (0)
static int resolve(void *kv, const char *key, uint64_t *offset) {
    return kvspaceResolveOffset(kv, key, NULL, offset);
}
static int wire_same(void *kv, const char *key, const uint8_t *expected,
                     uint32_t expected_len) {
    uint8_t *base = t_data_base(kv);
    uint64_t offset = 0;
    if (!base || resolve(kv, key, &offset) != KVSPACE_OK)
        return 0;
    const kvspaceXvalueHead *head =
        (const kvspaceXvalueHead *)(const void *)(base + offset);
    return kvspaceXvalueHeadLen(head) == expected_len &&
           memcmp(base + offset + KVSPACE_XVALUE_HEAD_LEN, expected,
                  expected_len) == 0;
}
static int attr_same(void *kv, const char *key, const uint8_t *expected,
                     uint16_t expected_len) {
    uint8_t *base = t_data_base(kv);
    uint64_t offset = 0;
    if (!base || resolve(kv, key, &offset) != KVSPACE_OK)
        return 0;
    const uint8_t *wire = base + offset;
    const kvspaceXvalueHead *head =
        (const kvspaceXvalueHead *)(const void *)wire;
    const uint8_t *data = NULL;
    uint16_t data_len = 0;
    if (kvspaceXvalueGetAttr(wire, kvspaceXvalueHeadTotalLen(head), 1, &data,
                             &data_len) != 0)
        return 0;
    return data_len == expected_len &&
           memcmp(data, expected, expected_len) == 0;
}
static int list_name_present(const char *buffer, uint32_t length,
                             const char *name) {
    uint32_t at = 0;
    while (at < length) {
        const char *current = buffer + at;
        size_t current_len = strlen(current);
        if (strcmp(current, name) == 0)
            return 1;
        at += (uint32_t)current_len + 1;
    }
    return 0;
}
static int install_scalar(void *kv, const char *key, const uint8_t *raw,
                          uint32_t raw_len, const char *langtype,
                          uint64_t *out_offset) {
    uint8_t *wire = NULL;
    uint64_t wire_len = 0;
    if (kvspace_test_wire_scalar(raw, raw_len, langtype, &wire, &wire_len) != 0)
        return -1;
    int rc = t_set_wire(kv, key, wire, (int32_t)wire_len);
    free(wire);
    if (rc != 0)
        return -1;
    return resolve(kv, key, out_offset) == KVSPACE_OK ? 0 : -1;
}
static int install_ptr(void *kv, const char *key, const char *target,
                       uint64_t *out_offset) {
    uint8_t *wire = NULL;
    uint64_t wire_len = 0;
    if (kvspace_test_wire_ptr((const uint8_t *)target,
                              (uint32_t)strlen(target), "int32", &wire,
                              &wire_len) != 0)
        return -1;
    int rc = t_set_wire(kv, key, wire, (int32_t)wire_len);
    free(wire);
    if (rc != 0)
        return -1;
    return resolve(kv, key, out_offset) == KVSPACE_OK ? 0 : -1;
}
static int install_ext(void *kv, const char *key, const char *locator,
                      uint64_t *out_offset) {
    uint8_t *wire = NULL;
    uint64_t wire_len = 0;
    if (kvspace_test_wire_ext((const uint8_t *)locator,
                              (uint32_t)strlen(locator), "int32", &wire,
                              &wire_len) != 0)
        return -1;
    int rc = t_set_wire(kv, key, wire, (int32_t)wire_len);
    free(wire);
    if (rc != 0)
        return -1;
    return resolve(kv, key, out_offset) == KVSPACE_OK ? 0 : -1;
}
int main(void) {
    char dir[] = "/tmp/kvspace-storage-boundary-XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    char path[180];
    snprintf(path, sizeof path, "%s/db", dir);
    void *kv = t_open(path, SBO_DATA_SIZE);
    CHECK(kv != NULL, "open %s", path);
    if (!kv)
        return 2;
    uint8_t *base = t_data_base(kv);
    CHECK(base != NULL, "in-process address is NULL");
    if (!base) {
        t_close(kv);
        return 2;
    }
    uint8_t target_body[4] = {0xaa, 0xbb, 0xcc, 0xdd};
    uint64_t target_offset = 0;
    CHECK(install_scalar(kv, "/target", target_body, sizeof target_body,
                         "int32", &target_offset) == 0,"install /target");
    uint64_t ptr_offset = 0;
    CHECK(install_ptr(kv, "/ptr", "/target", &ptr_offset) == 0,"install /ptr");
    CHECK(ptr_offset != target_offset,
          "resolve /ptr must return its own offset (%llu != %llu)",
          (unsigned long long)ptr_offset, (unsigned long long)target_offset);
    CHECK(wire_same(kv, "/ptr", (const uint8_t *)"/target", 7),
          "/ptr body must remain the target path text");
    CHECK(attr_same(kv, "/ptr", (const uint8_t *)"*int32", 6),
          "/ptr attr must carry the pointer prefix");
    uint64_t ext_offset = 0;
    CHECK(install_ext(kv, "/ext", "/target", &ext_offset) == 0,"install /ext");
    CHECK(ext_offset != target_offset,
          "resolve /ext must return its own offset (%llu != %llu)",
          (unsigned long long)ext_offset, (unsigned long long)target_offset);
    CHECK(wire_same(kv, "/ext", (const uint8_t *)"/target", 7),
          "/ext body must remain the locator text");
    CHECK(attr_same(kv, "/ext", (const uint8_t *)"@int32", 6),
          "/ext attr must carry the extension prefix");
    CHECK(kvspaceDeltree(kv, "/ptr") == KVSPACE_OK,"deltree /ptr");
    uint64_t ignored = 0;
    CHECK(resolve(kv, "/ptr", &ignored) == KVSPACE_MISSING,
          "/ptr must be gone after deltree");
    CHECK(resolve(kv, "/target", &ignored) == KVSPACE_OK &&
              ignored == target_offset,
          "/target must survive deltree of a pointer to it");
    CHECK(wire_same(kv, "/target", target_body, sizeof target_body),
          "/target body changed after deltree of /ptr");
    CHECK(kvspaceCp(kv, "/ext", "/ext2") == KVSPACE_OK,"cp /ext -> /ext2");
    uint64_t ext2_offset = 0;
    CHECK(resolve(kv, "/ext2", &ext2_offset) == KVSPACE_OK,
          "/ext2 missing after cp");
    CHECK(ext2_offset != target_offset,
          "cp must copy bytes, not follow the extension (%llu == %llu)",
          (unsigned long long)ext2_offset, (unsigned long long)target_offset);
    CHECK(wire_same(kv, "/ext2", (const uint8_t *)"/target", 7),
          "/ext2 body differs from locator");
    CHECK(attr_same(kv, "/ext2", (const uint8_t *)"@int32", 6),
          "/ext2 attr differs from extension prefix");
    CHECK(resolve(kv, "/ext", &ignored) == KVSPACE_OK && ignored == ext_offset,
          "cp must leave /ext untouched");
    CHECK(wire_same(kv, "/ext", (const uint8_t *)"/target", 7),
          "cp changed /ext body");
    CHECK(wire_same(kv, "/target", target_body, sizeof target_body),
          "cp changed /target body");
    {
        static const char *invalid_keys[] = {
            "abc",       "/a//b",       "/a··b",       "/a·/b",
            "/a/·b",     "/.kvspace-meta", "/.kvspace-meta/x",
        };
        for (size_t i = 0;
             i < sizeof invalid_keys / sizeof invalid_keys[0]; i++) {
            uint64_t offset = 777;
            int rc = kvspaceRealloc(kv, invalid_keys[i], 1, 0, &offset);
            CHECK(rc == KVSPACE_INVALID, "key \"%s\" rc=%d expected INVALID",
                  invalid_keys[i], rc);
            CHECK(offset == 0, "key \"%s\" must leave out_offset zero (got %llu)",
                  invalid_keys[i], (unsigned long long)offset);
        }
        uint64_t meta_offset = 0;
        CHECK(kvspaceRealloc(kv, "/.kvspace-meta·x", 1, 0, &meta_offset) == KVSPACE_OK,"/.kvspace-meta·x must be writable");
        CHECK(resolve(kv, "/.kvspace-meta·x", &ignored) == KVSPACE_OK,
              "/.kvspace-meta·x must resolve after write");
    }
    {
        uint64_t offset = 0;
        CHECK(kvspaceRealloc(kv, "/k/a·f", 1, 0, &offset) ==
                  KVSPACE_OK,"seed /k/a·f");
        CHECK(kvspaceRealloc(kv, "/k/z/q", 1, 0, &offset) ==
                  KVSPACE_OK,"seed /k/z/q");
        int64_t count = -1;
        uint32_t max_len = 0;
        CHECK(kvspaceListlen(kv, "/k/", &count, &max_len) ==
                  KVSPACE_OK,"listlen /k/");
        CHECK(count == 2, "listlen /k/ count %lld != 2", (long long)count);
        CHECK(max_len == 3, "listlen /k/ max_len %u != 3", (unsigned)max_len);
        uint32_t length = 0;
        uint32_t raw_count = 0;
        uint64_t generation = 0;
        CHECK(kvspaceList(kv, "/k/", NULL, 0, &length, &raw_count,
                          &generation) == KVSPACE_OK,"list probe /k/");
        CHECK(count == (int64_t)raw_count,
              "listlen count %lld != raw list count %u", (long long)count,
              (unsigned)raw_count);
        char *buffer = malloc(length ? length : 1);
        CHECK(buffer != NULL, "list buffer alloc");
        if (buffer) {
            CHECK(kvspaceList(kv, "/k/", buffer, length, &length, &raw_count,
                              &generation) == KVSPACE_OK,"list fetch /k/");
            CHECK(raw_count == 2, "raw list /k/ count %u != 2",
                  (unsigned)raw_count);
            CHECK(list_name_present(buffer, length, "a·"),
                  "list /k/ missing \"a·\"");
            CHECK(list_name_present(buffer, length, "z/"),
                  "list /k/ missing \"z/\"");
            free(buffer);
        }
        count = -1;
        max_len = 0;
        CHECK(kvspaceListlen(kv, "/k/a·", &count, &max_len) ==
                  KVSPACE_OK,"listlen /k/a·");
        CHECK(count == 1, "listlen /k/a· count %lld != 1", (long long)count);
        CHECK(max_len == 1, "listlen /k/a· max_len %u != 1", (unsigned)max_len);
        length = 0;
        raw_count = 0;
        generation = 0;
        CHECK(kvspaceList(kv, "/k/a·", NULL, 0, &length, &raw_count,
                          &generation) == KVSPACE_OK,"list probe /k/a·");
        CHECK(count == (int64_t)raw_count,
              "listlen /k/a· count %lld != raw list count %u",
              (long long)count, (unsigned)raw_count);
        buffer = malloc(length ? length : 1);
        CHECK(buffer != NULL, "list buffer alloc /k/a·");
        if (buffer) {
            CHECK(kvspaceList(kv, "/k/a·", buffer, length, &length, &raw_count,
                              &generation) == KVSPACE_OK,"list fetch /k/a·");
            CHECK(raw_count == 1, "raw list /k/a· count %u != 1",
                  (unsigned)raw_count);
            CHECK(list_name_present(buffer, length, "f"),
                  "list /k/a· missing \"f\"");
            free(buffer);
        }
    }
    t_close(kv);
    char command[256];
    snprintf(command, sizeof command, "rm -rf '%s'", dir);
    if (system(command) != 0)
        fprintf(stderr, "cleanup failed\n");
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("storage_boundary: all checks passed\n");
    return 0;
}

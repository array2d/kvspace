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
typedef struct {
    const char *langtype;
    uint32_t body_len;
    uint16_t attrs_len;
    uint64_t total;
} layout_case_t;
static const layout_case_t layout_cases[] = {
    {"bool", 1, 7, 24},    {"int8", 1, 7, 24},    {"int16", 2, 8, 26},
    {"int32", 4, 8, 28},   {"int64", 8, 8, 32},   {"uint8", 1, 8, 25},
    {"uint16", 2, 9, 27},  {"uint32", 4, 9, 29},  {"uint64", 8, 9, 33},
    {"float32", 4, 10, 30}, {"float64", 8, 10, 34},
};
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
    return kvspaceResolveOffset(kv, key, NULL, out_offset) ==
                   KVSPACE_OK
               ? 0
               : -1;
}
static void check_attr_record(const uint8_t *wire, uint64_t total,
                              const char *langtype) {
    const kvspaceXvalueHead *head =
        (const kvspaceXvalueHead *)(const void *)wire;
    uint64_t body_len = kvspaceXvalueHeadLen(head);
    uint64_t attrs_len = kvspaceXvalueHeadAttrsLen(head);
    const uint8_t *record = wire + KVSPACE_XVALUE_HEAD_LEN + body_len;
    CHECK(attrs_len >= 3, "attr region shorter than a record (%llu)",
          (unsigned long long)attrs_len);
    if (attrs_len < 3)
        return;
    uint16_t record_data_len =
        (uint16_t)((uint16_t)record[1] | ((uint16_t)record[2] << 8));
    size_t langtype_len = strlen(langtype);
    CHECK(record[0] == 1u, "attr tag %u != 1", (unsigned)record[0]);
    CHECK(record_data_len == langtype_len,
          "attr record length %u != langtype length %zu", record_data_len,
          langtype_len);
    CHECK((uint64_t)3 + record_data_len == attrs_len,
          "attr region holds more than one record (%llu != %llu)",
          (unsigned long long)(3 + record_data_len),
          (unsigned long long)attrs_len);
    CHECK(memcmp(record + 3, langtype, langtype_len) == 0,
          "attr data differs from langtype text");
    const uint8_t *data = NULL;
    uint16_t data_len = 0;
    CHECK(kvspaceXvalueGetAttr(wire, total, 1, &data, &data_len) == 0,
          "kvspaceXvalueGetAttr(tag=1) failed");
    if (data)
        CHECK(data_len == langtype_len &&
                  memcmp(data, langtype, langtype_len) == 0,
              "getAttr data differs from langtype text");
}
int main(void) {
    char dir[] = "/tmp/kvspace-xvalue-layout-XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    char path[160];
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
    for (size_t i = 0; i < sizeof layout_cases / sizeof layout_cases[0]; i++) {
        const layout_case_t *c = &layout_cases[i];
        uint8_t raw[8];
        for (uint32_t j = 0; j < c->body_len; j++)
            raw[j] = (uint8_t)(0x10u + i + j);
        char key[64];
        snprintf(key, sizeof key, "/layout/%s", c->langtype);
        uint64_t offset = 0;
        CHECK(install_scalar(kv, key, raw, c->body_len, c->langtype,
                             &offset) == 0,
              "%s: install failed", c->langtype);
        CHECK(offset % 8 == 0, "%s: offset %llu not 8-byte aligned",
              c->langtype, (unsigned long long)offset);
        if (offset % 8 != 0)
            continue;
        const uint8_t *wire = base + offset;
        const kvspaceXvalueHead *head =
            (const kvspaceXvalueHead *)(const void *)wire;
        CHECK(kvspaceXvalueHeadLen(head) == c->body_len,
              "%s: bodylen %llu != %u", c->langtype,
              (unsigned long long)kvspaceXvalueHeadLen(head), c->body_len);
        CHECK(kvspaceXvalueHeadAttrsLen(head) == c->attrs_len,
              "%s: attrslen %u != %u", c->langtype,
              (unsigned)kvspaceXvalueHeadAttrsLen(head), c->attrs_len);
        CHECK(kvspaceXvalueHeadTotalLen(head) == c->total,
              "%s: total %llu != %llu", c->langtype,
              (unsigned long long)kvspaceXvalueHeadTotalLen(head),
              (unsigned long long)c->total);
        CHECK(kvspaceXvalueHeadPaddingIsZero(head),
              "%s: head padding is not zero", c->langtype);
        CHECK(memcmp(wire + KVSPACE_XVALUE_HEAD_LEN, raw, c->body_len) == 0,
              "%s: body bytes differ after resolve", c->langtype);
        check_attr_record(wire, c->total, c->langtype);
    }
    {
        uint8_t raw16[2] = {0x21, 0x22};
        uint8_t raw32[4] = {0x31, 0x32, 0x33, 0x34};
        uint64_t first = 0;
        uint64_t second = 0;
        CHECK(install_scalar(kv, "/cap/inplace", raw16, sizeof raw16, "int16",
                             &first) == 0,
              "cap/inplace int16 install failed");
        CHECK(install_scalar(kv, "/cap/inplace", raw32, sizeof raw32, "int32",
                             &second) == 0,
              "cap/inplace int32 install failed");
        CHECK(first == second,
              "int16 -> int32 within capacity must keep offset (%llu != %llu)",
              (unsigned long long)first, (unsigned long long)second);
        const kvspaceXvalueHead *head =
            (const kvspaceXvalueHead *)(const void *)(base + second);
        CHECK(kvspaceXvalueHeadLen(head) == sizeof raw32,
              "inplace bodylen %llu", (unsigned long long)kvspaceXvalueHeadLen(head));
        CHECK(memcmp(base + second + KVSPACE_XVALUE_HEAD_LEN, raw32,
                     sizeof raw32) == 0,
              "inplace body bytes differ");
    }
    {
        uint8_t raw64[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        uint8_t raw15[15];
        uint8_t raw16[16];
        memset(raw15, 0x41, sizeof raw15);
        memset(raw16, 0x42, sizeof raw16);
        uint64_t int64_offset = 0;
        uint64_t uint64_offset = 0;
        CHECK(install_scalar(kv, "/cap/grow", raw64, sizeof raw64, "int64",
                             &int64_offset) == 0,
              "cap/grow int64 install failed");
        const kvspaceXvalueHead *head =
            (const kvspaceXvalueHead *)(const void *)(base + int64_offset);
        CHECK(kvspaceXvalueHeadTotalLen(head) == 32,
              "int64 total %llu != 32",
              (unsigned long long)kvspaceXvalueHeadTotalLen(head));
        CHECK(install_scalar(kv, "/cap/grow", raw64, sizeof raw64, "uint64",
                             &uint64_offset) == 0,
              "cap/grow uint64 install failed");
        CHECK(uint64_offset != int64_offset,
              "int64 -> uint64 must move (%llu == %llu)",
              (unsigned long long)int64_offset,
              (unsigned long long)uint64_offset);
        head = (const kvspaceXvalueHead *)(const void *)(base + uint64_offset);
        CHECK(kvspaceXvalueHeadTotalLen(head) == 33,
              "uint64 total %llu != 33",
              (unsigned long long)kvspaceXvalueHeadTotalLen(head));
        uint8_t *wire40 = NULL;
        uint64_t wire40_len = 0;
        CHECK(kvspace_test_wire_build(raw15, sizeof raw15, 0, "uint64",
                                      &wire40, &wire40_len) == 0 &&
                  wire40_len == 40,
              "40-byte wire build failed (len %llu)",
              (unsigned long long)wire40_len);
        uint64_t at40 = 0;
        CHECK(wire40 && t_set_wire(kv, "/cap/grow", wire40,
                                   (int32_t)wire40_len) == 0,
              "40-byte write failed");
        free(wire40);
        CHECK(kvspaceResolveOffset(kv, "/cap/grow", NULL, &at40) ==
                      KVSPACE_OK &&
                  at40 == uint64_offset,
              "40-byte write must stay at capacity boundary offset %llu",
              (unsigned long long)uint64_offset);
        uint8_t *wire41 = NULL;
        uint64_t wire41_len = 0;
        CHECK(kvspace_test_wire_build(raw16, sizeof raw16, 0, "uint64",
                                      &wire41, &wire41_len) == 0 &&
                  wire41_len == 41,
              "41-byte wire build failed (len %llu)",
              (unsigned long long)wire41_len);
        uint64_t at41 = 0;
        CHECK(wire41 && t_set_wire(kv, "/cap/grow", wire41,
                                   (int32_t)wire41_len) == 0,
              "41-byte write failed");
        free(wire41);
        CHECK(kvspaceResolveOffset(kv, "/cap/grow", NULL, &at41) ==
                      KVSPACE_OK &&
                  at41 != at40,
              "41-byte write must move past 40-byte capacity");
        uint64_t resolved = 0;
        CHECK(kvspaceResolveOffset(kv, "/cap/grow", NULL, &resolved) ==
                  KVSPACE_OK,
              "resolve /cap/grow failed");
        head = (const kvspaceXvalueHead *)(const void *)(base + resolved);
        CHECK(kvspaceXvalueHeadTotalLen(head) == 41,
              "final total %llu != 41",
              (unsigned long long)kvspaceXvalueHeadTotalLen(head));
    }
    {
        uint8_t retired_wire[64] = {0};
        kvspaceXvalueHead *retired_head = (kvspaceXvalueHead *)(void *)retired_wire;
        kvspaceXvalueHeadSetLen(retired_head, 0);
        kvspaceXvalueHeadSetAttrsLen(retired_head, 8);
        retired_wire[16] = 1u;
        retired_wire[17] = 1u;
        retired_wire[18] = 0u;
        retired_wire[19] = 'a';
        retired_wire[20] = KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN;
        retired_wire[21] = 1u;
        retired_wire[22] = 0u;
        retired_wire[23] = 'x';
        uint64_t new_length = 0;
        uint64_t effective = 0;
        const uint8_t *attr_ptr = NULL;
        uint16_t attr_len = 0;
        CHECK(kvspaceXvalueGetAttr(retired_wire, sizeof retired_wire, 1u,
                                   &attr_ptr, &attr_len) == -1,
              "GetAttr must reject wire containing retired tag 2");
        CHECK(kvspaceXvalueSetAttr(retired_wire, sizeof retired_wire, 1u,
                                   (const uint8_t *)"b", 1u, &new_length) == -1,
              "SetAttr must reject wire containing retired tag 2");
        CHECK(kvspaceXvalueHeadEffectiveLen(retired_wire, sizeof retired_wire,
                                            &effective) == -1,
              "EffectiveLen must reject wire containing retired tag 2");
        kvspaceXvalueHeadSetAttrsLen(retired_head, 5);
        CHECK(kvspaceXvalueGetAttr(retired_wire, sizeof retired_wire, 1u,
                                   &attr_ptr, &attr_len) == -1,
              "GetAttr must reject trailing truncated TLV bytes");
        CHECK(kvspaceXvalueSetAttr(retired_wire, sizeof retired_wire, 3u,
                                   (const uint8_t *)"c", 1u, &new_length) == -1,
              "SetAttr must reject trailing truncated TLV bytes");
        CHECK(kvspaceXvalueHeadEffectiveLen(retired_wire, sizeof retired_wire,
                                            &effective) == -1,
              "EffectiveLen must reject trailing truncated TLV bytes");
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
    printf("xvalue_layout: all checks passed\n");
    return 0;
}

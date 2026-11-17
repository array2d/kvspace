#include "trie.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "failed: %s (line %d)\n", #condition, __LINE__); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

#define CHILD_NAME_CAPACITY 32
#define CHILD_COUNT_CAPACITY 256

typedef struct {
    uint8_t *memory;
    uint64_t size;
    uint64_t used;
} bump_allocator_t;

typedef struct {
    bump_allocator_t allocator;
    trie_env_t env;
    uint64_t root;
} trie_fixture_t;

typedef struct {
    uint32_t count;
    uint8_t names[CHILD_COUNT_CAPACITY][CHILD_NAME_CAPACITY];
    uint16_t lengths[CHILD_COUNT_CAPACITY];
} collected_children_t;

static int bump_allocate(void *context, uint64_t bytes, uint64_t *out_offset) {
    bump_allocator_t *allocator = (bump_allocator_t *)context;
    uint64_t offset = (allocator->used + UINT64_C(7)) & ~UINT64_C(7);
    if (offset > allocator->size) {
        return -1;
    }
    if (allocator->size - offset < bytes) {
        return -1;
    }
    memset(allocator->memory + offset, 0, (size_t)bytes);
    allocator->used = offset + bytes;
    *out_offset = offset;
    return 0;
}

static int fixture_create(trie_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->allocator.size = UINT64_C(16) * 1024 * 1024;
    fixture->allocator.memory = (uint8_t *)malloc((size_t)fixture->allocator.size);
    if (fixture->allocator.memory == NULL) {
        return -1;
    }
    fixture->env.context = &fixture->allocator;
    fixture->env.alloc_bytes = bump_allocate;
    fixture->env.base = fixture->allocator.memory;
    fixture->env.slab_size = fixture->allocator.size;
    if (trie_root_create(&fixture->env, &fixture->root) != KVSPACE_OK) {
        return -1;
    }
    return 0;
}

static void fixture_destroy(trie_fixture_t *fixture) {
    free(fixture->allocator.memory);
    memset(fixture, 0, sizeof(*fixture));
}

static int header_of(trie_fixture_t *fixture, uint64_t node,
                     trie_hdr_t **out_header) {
    return trie_node_header(&fixture->env, node, out_header);
}

static int collect_child(void *context, const uint8_t *name,
                         uint16_t name_length, uint64_t child_offset) {
    collected_children_t *collection = (collected_children_t *)context;
    (void)child_offset;
    if (collection->count >= CHILD_COUNT_CAPACITY) {
        return -1;
    }
    if (name_length > CHILD_NAME_CAPACITY) {
        return -1;
    }
    memcpy(collection->names[collection->count], name, name_length);
    collection->lengths[collection->count] = name_length;
    collection->count++;
    return 0;
}

static int test_three_keys(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    uint64_t plain = 0;
    uint64_t slash = 0;
    uint64_t dot = 0;
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b", TRIE_VALUE_PACK(1, 10),
                     &plain) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b/",
                     TRIE_VALUE_PACK(1, 20), &slash) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b\xC2\xB7",
                     TRIE_VALUE_PACK(1, 30), &dot) == KVSPACE_OK);
    CHECK(plain != slash);
    CHECK(plain != dot);
    CHECK(slash != dot);
    uint64_t found = 0;
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b", &found) ==
          KVSPACE_OK);
    CHECK(found == plain);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b/", &found) ==
          KVSPACE_OK);
    CHECK(found == slash);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b\xC2\xB7",
                             &found) == KVSPACE_OK);
    CHECK(found == dot);
    trie_hdr_t *header = NULL;
    CHECK(header_of(&fixture, plain, &header) == KVSPACE_OK);
    CHECK(header->value == TRIE_VALUE_PACK(1, 10));
    CHECK(header_of(&fixture, slash, &header) == KVSPACE_OK);
    CHECK(header->value == TRIE_VALUE_PACK(1, 20));
    CHECK(header_of(&fixture, dot, &header) == KVSPACE_OK);
    CHECK(header->value == TRIE_VALUE_PACK(1, 30));
    collected_children_t collection;
    memset(&collection, 0, sizeof(collection));
    uint64_t parent = 0;
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/", &parent) ==
          KVSPACE_OK);
    CHECK(trie_list_children(&fixture.env, parent, collect_child, &collection,
                             NULL) == KVSPACE_OK);
    CHECK(collection.count == 3);
    CHECK(collection.lengths[0] == 1);
    CHECK(memcmp(collection.names[0], "b", 1) == 0);
    CHECK(collection.lengths[1] == 2);
    CHECK(memcmp(collection.names[1], "b/", 2) == 0);
    CHECK(collection.lengths[2] == 3);
    CHECK(memcmp(collection.names[2], "b\xC2\xB7", 3) == 0);
    fixture_destroy(&fixture);
    return 0;
}

static int test_auto_create(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b/c",
                     TRIE_VALUE_PACK(1, 7), NULL) == KVSPACE_OK);
    uint64_t directory_a = 0;
    uint64_t directory_b = 0;
    uint64_t value_c = 0;
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/", &directory_a) ==
          KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b/",
                             &directory_b) == KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b/c", &value_c) ==
          KVSPACE_OK);
    CHECK(directory_a != directory_b);
    CHECK(directory_b != value_c);
    uint32_t count = 0;
    CHECK(trie_child_count(&fixture.env, fixture.root, &count) == KVSPACE_OK);
    CHECK(count == 1);
    CHECK(trie_child_count(&fixture.env, directory_a, &count) == KVSPACE_OK);
    CHECK(count == 1);
    CHECK(trie_child_count(&fixture.env, directory_b, &count) == KVSPACE_OK);
    CHECK(count == 1);
    trie_hdr_t *header = NULL;
    CHECK(header_of(&fixture, directory_a, &header) == KVSPACE_OK);
    CHECK(header->name_length == 2);
    CHECK(memcmp(fixture.env.base + header->name_offset, "a/", 2) == 0);
    CHECK(header_of(&fixture, value_c, &header) == KVSPACE_OK);
    CHECK(header->value == TRIE_VALUE_PACK(1, 7));
    fixture_destroy(&fixture);
    return 0;
}

static int test_invalid_keys(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    trie_segment_t segments[TRIE_MAX_SEGMENTS];
    int count = 0;
    CHECK(trie_key_segments("", segments, TRIE_MAX_SEGMENTS, &count) ==
          KVSPACE_INVALID);
    CHECK(trie_key_segments("a/b", segments, TRIE_MAX_SEGMENTS, &count) ==
          KVSPACE_INVALID);
    CHECK(trie_key_segments("a//b", segments, TRIE_MAX_SEGMENTS, &count) ==
          KVSPACE_INVALID);
    CHECK(trie_key_segments("/a//b", segments, TRIE_MAX_SEGMENTS, &count) ==
          KVSPACE_INVALID);
    CHECK(trie_key_segments("//", segments, TRIE_MAX_SEGMENTS, &count) ==
          KVSPACE_INVALID);
    char long_key[1025];
    long_key[0] = '/';
    memset(long_key + 1, 'a', 1023);
    long_key[1024] = '\0';
    CHECK(strlen(long_key) == 1024);
    CHECK(trie_key_segments(long_key, segments, TRIE_MAX_SEGMENTS, &count) ==
          KVSPACE_INVALID);
    CHECK(trie_write(&fixture.env, fixture.root, "", TRIE_VALUE_ABSENT, NULL) ==
          KVSPACE_INVALID);
    CHECK(trie_write(&fixture.env, fixture.root, "a/b", TRIE_VALUE_ABSENT,
                     NULL) == KVSPACE_INVALID);
    CHECK(trie_write(&fixture.env, fixture.root, "/a//b", TRIE_VALUE_ABSENT,
                     NULL) == KVSPACE_INVALID);
    CHECK(trie_write(&fixture.env, fixture.root, long_key, TRIE_VALUE_ABSENT,
                     NULL) == KVSPACE_INVALID);
    CHECK(trie_find_relative(&fixture.env, fixture.root, "", &(uint64_t){0}) ==
          KVSPACE_INVALID);
    CHECK(trie_find_relative(&fixture.env, fixture.root, "/a",
                             &(uint64_t){0}) == KVSPACE_INVALID);
    CHECK(trie_child_count(&fixture.env, fixture.root, &(uint32_t){0}) ==
          KVSPACE_OK);
    fixture_destroy(&fixture);
    return 0;
}

static int test_listing(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/c",
                     TRIE_VALUE_PACK(1, 1), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/a",
                     TRIE_VALUE_PACK(1, 2), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b\xC2\xB7",
                     TRIE_VALUE_PACK(1, 3), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b",
                     TRIE_VALUE_PACK(1, 4), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/longname/",
                     TRIE_VALUE_PACK(1, 5), NULL) == KVSPACE_OK);
    uint64_t parent = 0;
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/", &parent) ==
          KVSPACE_OK);
    collected_children_t collection;
    memset(&collection, 0, sizeof(collection));
    uint32_t count = 0;
    CHECK(trie_list_children(&fixture.env, parent, collect_child, &collection,
                             &count) == KVSPACE_OK);
    CHECK(count == 5);
    CHECK(collection.count == 5);
    static const char *expected[] = {"a", "b", "b\xC2\xB7", "c", "longname/"};
    static const uint16_t expected_lengths[] = {1, 1, 3, 1, 9};
    for (uint32_t index = 0; index < 5; index++) {
        CHECK(collection.lengths[index] == expected_lengths[index]);
        CHECK(memcmp(collection.names[index], expected[index],
                     expected_lengths[index]) == 0);
    }
    uint16_t longest = 0;
    CHECK(trie_longest_child_name(&fixture.env, parent, &longest) ==
          KVSPACE_OK);
    CHECK(longest == 9);
    fixture_destroy(&fixture);
    return 0;
}

static int test_relative_lookup(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    uint64_t absolute = 0;
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b/c",
                     TRIE_VALUE_PACK(1, 9), &absolute) == KVSPACE_OK);
    uint64_t anchor = 0;
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/", &anchor) ==
          KVSPACE_OK);
    uint64_t relative = 0;
    CHECK(trie_find_relative(&fixture.env, anchor, "b/c", &relative) ==
          KVSPACE_OK);
    CHECK(relative == absolute);
    uint64_t directory = 0;
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b/", &directory) ==
          KVSPACE_OK);
    CHECK(trie_find_relative(&fixture.env, anchor, "b/", &relative) ==
          KVSPACE_OK);
    CHECK(relative == directory);
    CHECK(trie_find_relative(&fixture.env, fixture.root, "a/b/c", &relative) ==
          KVSPACE_OK);
    CHECK(relative == absolute);
    CHECK(trie_find_relative(&fixture.env, fixture.root + 3, "a",
                             &relative) == KVSPACE_ANCHOR_INVALID);
    CHECK(trie_find_relative(&fixture.env, fixture.env.slab_size + 8, "a",
                             &relative) == KVSPACE_ANCHOR_INVALID);
    CHECK(trie_find_relative(&fixture.env, 0, "a", &relative) ==
          KVSPACE_ANCHOR_INVALID);
    uint64_t dead = 0;
    CHECK(trie_write(&fixture.env, fixture.root, "/dead",
                     TRIE_VALUE_PACK(1, 1), &dead) == KVSPACE_OK);
    CHECK(trie_remove(&fixture.env, fixture.root, "/dead") == KVSPACE_OK);
    CHECK(trie_find_relative(&fixture.env, dead, "", &relative) ==
          KVSPACE_INVALID);
    CHECK(trie_find_relative(&fixture.env, dead, "x", &relative) ==
          KVSPACE_ANCHOR_INVALID);
    trie_hdr_t *header = NULL;
    CHECK(header_of(&fixture, dead, &header) == KVSPACE_OK);
    CHECK((header->flags & TRIE_FLAG_DEAD) != 0);
    CHECK(header->value == TRIE_VALUE_ABSENT);
    fixture_destroy(&fixture);
    return 0;
}

static int test_remove(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b/c",
                     TRIE_VALUE_PACK(1, 1), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b/d",
                     TRIE_VALUE_PACK(1, 2), NULL) == KVSPACE_OK);
    CHECK(trie_remove(&fixture.env, fixture.root, "/a/b/") ==
          TRIE_HAS_CHILDREN);
    uint64_t directory = 0;
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b/", &directory) ==
          KVSPACE_OK);
    uint32_t count = 0;
    CHECK(trie_child_count(&fixture.env, directory, &count) == KVSPACE_OK);
    CHECK(count == 2);
    CHECK(trie_remove(&fixture.env, fixture.root, "/a/b/c") == KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b/c",
                             &(uint64_t){0}) == KVSPACE_MISSING);
    CHECK(trie_remove(&fixture.env, fixture.root, "/a/b/c") == KVSPACE_MISSING);
    CHECK(trie_remove(&fixture.env, fixture.root, "/a/b/d") == KVSPACE_OK);
    CHECK(trie_remove(&fixture.env, fixture.root, "/a/b/") == KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b/",
                             &(uint64_t){0}) == KVSPACE_MISSING);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/",
                             &(uint64_t){0}) == KVSPACE_OK);
    CHECK(trie_remove(&fixture.env, fixture.root, "/") == TRIE_HAS_CHILDREN);
    CHECK(trie_remove(&fixture.env, fixture.root, "/a/") == KVSPACE_OK);
    CHECK(trie_remove(&fixture.env, fixture.root, "/") == KVSPACE_OK);
    uint64_t root = 0;
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/", &root) ==
          KVSPACE_OK);
    CHECK(root == fixture.root);
    fixture_destroy(&fixture);
    return 0;
}

static int test_expansion(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    for (int index = 0; index < 200; index++) {
        char key[32];
        snprintf(key, sizeof(key), "/node%03d/", index);
        CHECK(trie_write(&fixture.env, fixture.root, key,
                         TRIE_VALUE_PACK(1, (uint64_t)index), NULL) ==
              KVSPACE_OK);
    }
    trie_hdr_t *header = NULL;
    CHECK(header_of(&fixture, fixture.root, &header) == KVSPACE_OK);
    CHECK(header->count == 200);
    CHECK(header->capacity >= 200);
    uint32_t count = 0;
    CHECK(trie_child_count(&fixture.env, fixture.root, &count) == KVSPACE_OK);
    CHECK(count == 200);
    for (int index = 0; index < 200; index++) {
        char key[32];
        snprintf(key, sizeof(key), "/node%03d/", index);
        uint64_t node = 0;
        CHECK(trie_find_absolute(&fixture.env, fixture.root, key, &node) ==
              KVSPACE_OK);
        CHECK(header_of(&fixture, node, &header) == KVSPACE_OK);
        CHECK(header->value == TRIE_VALUE_PACK(1, (uint64_t)index));
    }
    collected_children_t collection;
    memset(&collection, 0, sizeof(collection));
    CHECK(trie_list_children(&fixture.env, fixture.root, collect_child,
                             &collection, &count) == KVSPACE_OK);
    CHECK(count == 200);
    for (uint32_t index = 0; index < 200; index++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "node%03d/", (int)index);
        CHECK(collection.lengths[index] == strlen(expected));
        CHECK(memcmp(collection.names[index], expected,
                     strlen(expected)) == 0);
    }
    fixture_destroy(&fixture);
    return 0;
}

static int test_deep_chain(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    uint64_t anchor = fixture.root;
    for (int index = 0; index < 1000; index++) {
        uint64_t child = 0;
        CHECK(trie_write_relative(&fixture.env, anchor, "a/",
                                  TRIE_VALUE_ABSENT, &child) == KVSPACE_OK);
        CHECK(child != anchor);
        anchor = child;
    }
    uint64_t walker = fixture.root;
    for (int index = 0; index < 1000; index++) {
        uint64_t child = 0;
        CHECK(trie_find_relative(&fixture.env, walker, "a/", &child) ==
              KVSPACE_OK);
        walker = child;
    }
    CHECK(walker == anchor);
    uint32_t count = 0;
    CHECK(trie_child_count(&fixture.env, anchor, &count) == KVSPACE_OK);
    CHECK(count == 0);
    fixture_destroy(&fixture);
    return 0;
}

static int test_generations(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    trie_hdr_t *root_header = NULL;
    CHECK(header_of(&fixture, fixture.root, &root_header) == KVSPACE_OK);
    CHECK(root_header->struct_gen == 0);
    CHECK(root_header->value_gen == 0);
    uint64_t directory = 0;
    CHECK(trie_write(&fixture.env, fixture.root, "/a/",
                     TRIE_VALUE_PACK(1, 5), &directory) == KVSPACE_OK);
    CHECK(root_header->struct_gen == 2);
    trie_hdr_t *directory_header = NULL;
    CHECK(header_of(&fixture, directory, &directory_header) == KVSPACE_OK);
    CHECK(directory_header->value_gen == 1);
    CHECK(directory_header->struct_gen == 0);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/",
                     TRIE_VALUE_PACK(1, 6), &directory) == KVSPACE_OK);
    CHECK(directory_header->value_gen == 2);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b/",
                     TRIE_VALUE_PACK(1, 1), NULL) == KVSPACE_OK);
    CHECK(directory_header->struct_gen == 2);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/c/",
                     TRIE_VALUE_PACK(1, 1), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/d/",
                     TRIE_VALUE_PACK(1, 1), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/e/",
                     TRIE_VALUE_PACK(1, 1), NULL) == KVSPACE_OK);
    CHECK(directory_header->struct_gen == 5);
    CHECK(directory_header->count == 4);
    CHECK(directory_header->capacity == 4);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/f/",
                     TRIE_VALUE_PACK(1, 1), NULL) == KVSPACE_OK);
    CHECK(directory_header->struct_gen == 7);
    CHECK(directory_header->count == 5);
    CHECK(directory_header->capacity > 4);
    uint64_t value_node = 0;
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b/",
                     TRIE_VALUE_PACK(1, 2), &value_node) == KVSPACE_OK);
    trie_hdr_t *value_header = NULL;
    CHECK(header_of(&fixture, value_node, &value_header) == KVSPACE_OK);
    CHECK(value_header->value_gen == 2);
    CHECK(trie_remove(&fixture.env, fixture.root, "/a/b/") == KVSPACE_OK);
    CHECK(value_header->value_gen == 3);
    CHECK((value_header->flags & TRIE_FLAG_DEAD) != 0);
    CHECK(directory_header->struct_gen == 8);
    CHECK(root_header->struct_gen == 2);
    fixture_destroy(&fixture);
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_three_keys();
    failures += test_auto_create();
    failures += test_invalid_keys();
    failures += test_listing();
    failures += test_relative_lookup();
    failures += test_remove();
    failures += test_expansion();
    failures += test_deep_chain();
    failures += test_generations();
    if (failures == 0) {
        printf("PASS trie_test\n");
        return 0;
    }
    fprintf(stderr, "FAILED %d test groups\n", failures);
    return 1;
}

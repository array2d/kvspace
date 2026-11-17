#include "trie.h"
#include <pthread.h>
#include <stdatomic.h>
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
#define SUBTREE_CAPACITY 16
#define SUBTREE_SUFFIX_CAPACITY 64
#define INDEX_VALUE_CAPACITY 8

typedef struct {
    uint8_t *memory;
    uint64_t size;
    uint64_t used;
} bump_allocator_t;

typedef struct {
    bump_allocator_t allocator;
    uint32_t index_value_calls;
    int32_t index_value_fail_at;
    uint64_t index_value_values[INDEX_VALUE_CAPACITY];
} fixture_context_t;

typedef struct {
    fixture_context_t context;
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
    fixture->context.allocator.size = UINT64_C(16) * 1024 * 1024;
    fixture->context.allocator.memory =
        (uint8_t *)malloc((size_t)fixture->context.allocator.size);
    if (fixture->context.allocator.memory == NULL) {
        return -1;
    }
    fixture->context.index_value_fail_at = -1;
    fixture->env.context = &fixture->context;
    fixture->env.alloc_bytes = bump_allocate;
    fixture->env.base = fixture->context.allocator.memory;
    fixture->env.slab_size = fixture->context.allocator.size;
    if (trie_root_create(&fixture->env, &fixture->root) != KVSPACE_OK) {
        return -1;
    }
    return 0;
}

static void fixture_destroy(trie_fixture_t *fixture) {
    free(fixture->context.allocator.memory);
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

typedef struct {
    uint32_t count;
    uint32_t stop_at;
    int stop_value;
    char suffixes[SUBTREE_CAPACITY][SUBTREE_SUFFIX_CAPACITY];
    uint16_t lengths[SUBTREE_CAPACITY];
    uint64_t nodes[SUBTREE_CAPACITY];
    uint64_t values[SUBTREE_CAPACITY];
} subtree_collection_t;

static int collect_subtree(void *context, const char *suffix,
                           uint64_t node_offset, uint64_t value) {
    subtree_collection_t *collection = (subtree_collection_t *)context;
    if (collection->stop_at != UINT32_MAX &&
        collection->count == collection->stop_at) {
        return collection->stop_value;
    }
    size_t length = strlen(suffix);
    if (collection->count >= SUBTREE_CAPACITY ||
        length >= SUBTREE_SUFFIX_CAPACITY) {
        return -1;
    }
    memcpy(collection->suffixes[collection->count], suffix, length + 1);
    collection->lengths[collection->count] = (uint16_t)length;
    collection->nodes[collection->count] = node_offset;
    collection->values[collection->count] = value;
    collection->count++;
    return 0;
}

typedef struct {
    uint32_t count;
    uint32_t stop_at;
    int stop_value;
} subtree_counter_t;

static int count_subtree(void *context, const char *suffix,
                         uint64_t node_offset, uint64_t value) {
    subtree_counter_t *counter = (subtree_counter_t *)context;
    (void)suffix;
    (void)node_offset;
    (void)value;
    if (counter->stop_at != UINT32_MAX &&
        counter->count == counter->stop_at) {
        return counter->stop_value;
    }
    counter->count++;
    return 0;
}

static int provide_index_value(void *context, uint64_t *out_value) {
    fixture_context_t *fixture_context = (fixture_context_t *)context;
    uint32_t call = fixture_context->index_value_calls;
    if (fixture_context->index_value_fail_at >= 0 &&
        call == (uint32_t)fixture_context->index_value_fail_at) {
        return -1;
    }
    uint64_t value = TRIE_VALUE_PACK(1, 100 + call);
    if (call < INDEX_VALUE_CAPACITY) {
        fixture_context->index_value_values[call] = value;
    }
    fixture_context->index_value_calls = call + 1;
    *out_value = value;
    return 0;
}

typedef struct {
    atomic_int inside;
    atomic_int violations;
} lock_shared_t;

typedef struct {
    trie_env_t *env;
    uint64_t node;
    int iterations;
    lock_shared_t *shared;
} lock_worker_t;

static void *lock_worker_run(void *context) {
    lock_worker_t *worker = (lock_worker_t *)context;
    for (int iteration = 0; iteration < worker->iterations; iteration++) {
        trie_node_lock(worker->env, worker->node);
        if (atomic_fetch_add_explicit(&worker->shared->inside, 1,
                                      memory_order_seq_cst) != 0) {
            atomic_fetch_add_explicit(&worker->shared->violations, 1,
                                      memory_order_seq_cst);
        }
        for (volatile int spin = 0; spin < 64; spin++) {
        }
        atomic_fetch_sub_explicit(&worker->shared->inside, 1,
                                  memory_order_seq_cst);
        trie_node_unlock(worker->env, worker->node);
    }
    return NULL;
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

static int test_subtree_visit(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b",
                     TRIE_VALUE_PACK(1, 1), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b/",
                     TRIE_VALUE_PACK(1, 2), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b\xC2\xB7",
                     TRIE_VALUE_PACK(1, 3), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b/c/",
                     TRIE_VALUE_PACK(1, 4), NULL) == KVSPACE_OK);
    uint64_t directory_a = 0;
    uint64_t node_b = 0;
    uint64_t node_b_slash = 0;
    uint64_t node_c = 0;
    uint64_t node_b_dot = 0;
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/", &directory_a) ==
          KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b", &node_b) ==
          KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b/",
                             &node_b_slash) == KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b/c/", &node_c) ==
          KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b\xC2\xB7",
                             &node_b_dot) == KVSPACE_OK);
    subtree_collection_t collection;
    memset(&collection, 0, sizeof(collection));
    collection.stop_at = UINT32_MAX;
    uint32_t count = 0;
    CHECK(trie_subtree_visit(&fixture.env, directory_a, collect_subtree,
                             &collection, &count) == KVSPACE_OK);
    CHECK(count == 5);
    CHECK(collection.count == 5);
    static const char *expected_suffixes[] = {"", "b", "b/", "b/c/",
                                              "b\xC2\xB7"};
    static const uint16_t expected_lengths[] = {0, 1, 2, 4, 3};
    static const uint64_t expected_values[] = {
        TRIE_VALUE_ABSENT, TRIE_VALUE_PACK(1, 1), TRIE_VALUE_PACK(1, 2),
        TRIE_VALUE_PACK(1, 4), TRIE_VALUE_PACK(1, 3)};
    const uint64_t expected_nodes[] = {directory_a, node_b, node_b_slash,
                                       node_c, node_b_dot};
    for (uint32_t index = 0; index < 5; index++) {
        CHECK(collection.lengths[index] == expected_lengths[index]);
        CHECK(memcmp(collection.suffixes[index], expected_suffixes[index],
                     expected_lengths[index]) == 0);
        CHECK(collection.values[index] == expected_values[index]);
        CHECK(collection.nodes[index] == expected_nodes[index]);
    }
    subtree_collection_t from_root;
    memset(&from_root, 0, sizeof(from_root));
    from_root.stop_at = UINT32_MAX;
    CHECK(trie_subtree_visit(&fixture.env, fixture.root, collect_subtree,
                             &from_root, &count) == KVSPACE_OK);
    CHECK(count == 6);
    CHECK(from_root.count == 6);
    static const char *root_suffixes[] = {"",    "a/",    "a/b",
                                          "a/b/", "a/b/c/", "a/b\xC2\xB7"};
    static const uint16_t root_lengths[] = {0, 2, 3, 4, 6, 5};
    for (uint32_t index = 0; index < 6; index++) {
        CHECK(from_root.lengths[index] == root_lengths[index]);
        CHECK(memcmp(from_root.suffixes[index], root_suffixes[index],
                     root_lengths[index]) == 0);
    }
    subtree_collection_t stopped;
    memset(&stopped, 0, sizeof(stopped));
    stopped.stop_at = 2;
    stopped.stop_value = 77;
    CHECK(trie_subtree_visit(&fixture.env, directory_a, collect_subtree,
                             &stopped, &count) == 77);
    CHECK(count == 2);
    CHECK(stopped.count == 2);
    CHECK(trie_subtree_visit(&fixture.env, directory_a, NULL, &stopped,
                             &count) == KVSPACE_INVALID);
    CHECK(trie_subtree_visit(&fixture.env, 0, collect_subtree, &stopped,
                             &count) == KVSPACE_ANCHOR_INVALID);
    fixture_destroy(&fixture);
    return 0;
}

static int test_subtree_length_limit(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    uint64_t anchor = fixture.root;
    for (int index = 0; index < 1000; index++) {
        uint64_t child = 0;
        CHECK(trie_write_relative(&fixture.env, anchor, "a/",
                                  TRIE_VALUE_ABSENT, &child) == KVSPACE_OK);
        anchor = child;
    }
    subtree_counter_t counter;
    memset(&counter, 0, sizeof(counter));
    counter.stop_at = UINT32_MAX;
    uint32_t count = 0;
    CHECK(trie_subtree_visit(&fixture.env, fixture.root, count_subtree,
                             &counter, &count) == KVSPACE_INVALID);
    CHECK(count == 512);
    fixture_destroy(&fixture);
    return 0;
}

static int test_child_at(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b",
                     TRIE_VALUE_PACK(1, 1), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b/",
                     TRIE_VALUE_PACK(1, 2), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b\xC2\xB7",
                     TRIE_VALUE_PACK(1, 3), NULL) == KVSPACE_OK);
    CHECK(trie_write(&fixture.env, fixture.root, "/a/c/",
                     TRIE_VALUE_PACK(1, 4), NULL) == KVSPACE_OK);
    uint64_t parent = 0;
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/", &parent) ==
          KVSPACE_OK);
    uint32_t count = 0;
    CHECK(trie_child_count(&fixture.env, parent, &count) == KVSPACE_OK);
    CHECK(count == 4);
    static const char *expected_names[] = {"b", "b/", "b\xC2\xB7", "c/"};
    static const uint16_t expected_lengths[] = {1, 2, 3, 2};
    static const char *expected_keys[] = {"/a/b", "/a/b/", "/a/b\xC2\xB7",
                                          "/a/c/"};
    for (uint32_t index = 0; index < 4; index++) {
        const uint8_t *name = NULL;
        uint16_t length = 0;
        uint64_t child = 0;
        CHECK(trie_child_at(&fixture.env, parent, index, &name, &length,
                            &child) == KVSPACE_OK);
        CHECK(length == expected_lengths[index]);
        CHECK(memcmp(name, expected_names[index], expected_lengths[index]) == 0);
        uint64_t found = 0;
        CHECK(trie_find_absolute(&fixture.env, fixture.root,
                                 expected_keys[index], &found) == KVSPACE_OK);
        CHECK(child == found);
    }
    const uint8_t *name = NULL;
    uint16_t length = 0;
    uint64_t child = 0;
    CHECK(trie_child_at(&fixture.env, parent, 4, &name, &length, &child) ==
          KVSPACE_INVALID);
    CHECK(trie_child_at(&fixture.env, parent, 0, NULL, &length, &child) ==
          KVSPACE_INVALID);
    CHECK(trie_child_at(&fixture.env, fixture.root + 3, 0, &name, &length,
                        &child) == KVSPACE_ANCHOR_INVALID);
    fixture_destroy(&fixture);
    return 0;
}

static int test_value_access(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    uint64_t node = 0;
    CHECK(trie_write(&fixture.env, fixture.root, "/v",
                     TRIE_VALUE_PACK(1, 5), &node) == KVSPACE_OK);
    uint64_t value = 0;
    CHECK(trie_value_get(&fixture.env, node, &value) == KVSPACE_OK);
    CHECK(value == TRIE_VALUE_PACK(1, 5));
    trie_hdr_t *header = NULL;
    CHECK(header_of(&fixture, node, &header) == KVSPACE_OK);
    CHECK(header->value_gen == 1);
    CHECK(trie_value_set(&fixture.env, node, TRIE_VALUE_PACK(1, 9)) ==
          KVSPACE_OK);
    CHECK(trie_value_get(&fixture.env, node, &value) == KVSPACE_OK);
    CHECK(value == TRIE_VALUE_PACK(1, 9));
    CHECK(header->value_gen == 2);
    CHECK(trie_value_get(&fixture.env, fixture.root, &value) == KVSPACE_OK);
    CHECK(value == TRIE_VALUE_ABSENT);
    uint64_t directory = 0;
    CHECK(trie_write(&fixture.env, fixture.root, "/x/y/",
                     TRIE_VALUE_PACK(1, 3), NULL) == KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/x/", &directory) ==
          KVSPACE_OK);
    CHECK(trie_value_get(&fixture.env, directory, &value) == KVSPACE_OK);
    CHECK(value == TRIE_VALUE_ABSENT);
    CHECK(trie_value_set(&fixture.env, directory, TRIE_VALUE_PACK(1, 42)) ==
          KVSPACE_OK);
    CHECK(trie_value_get(&fixture.env, directory, &value) == KVSPACE_OK);
    CHECK(value == TRIE_VALUE_PACK(1, 42));
    CHECK(trie_value_get(&fixture.env, fixture.root + 3, &value) ==
          KVSPACE_ANCHOR_INVALID);
    CHECK(trie_value_set(&fixture.env, fixture.root + 3, 0) ==
          KVSPACE_ANCHOR_INVALID);
    CHECK(trie_value_get(&fixture.env, node, NULL) == KVSPACE_INVALID);
    fixture_destroy(&fixture);
    return 0;
}

static int test_generations_api(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    uint64_t node = 0;
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b",
                     TRIE_VALUE_PACK(1, 5), &node) == KVSPACE_OK);
    uint32_t struct_gen = 0;
    uint32_t value_gen = 0;
    CHECK(trie_generations(&fixture.env, fixture.root, &struct_gen,
                           &value_gen) == KVSPACE_OK);
    CHECK(struct_gen == 2);
    CHECK(value_gen == 0);
    CHECK(trie_generations(&fixture.env, node, &struct_gen, &value_gen) ==
          KVSPACE_OK);
    CHECK(struct_gen == 0);
    CHECK(value_gen == 1);
    CHECK(trie_value_set(&fixture.env, node, TRIE_VALUE_PACK(1, 6)) ==
          KVSPACE_OK);
    CHECK(trie_generations(&fixture.env, node, &struct_gen, &value_gen) ==
          KVSPACE_OK);
    CHECK(struct_gen == 0);
    CHECK(value_gen == 2);
    CHECK(trie_generations(&fixture.env, node, NULL, &value_gen) ==
          KVSPACE_INVALID);
    CHECK(trie_generations(&fixture.env, fixture.root + 3, &struct_gen,
                           &value_gen) == KVSPACE_ANCHOR_INVALID);
    fixture_destroy(&fixture);
    return 0;
}

static int test_make_index_value(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    fixture.env.make_index_value = provide_index_value;
    uint64_t node_c = 0;
    CHECK(trie_write(&fixture.env, fixture.root, "/a/b/c",
                     TRIE_VALUE_PACK(1, 7), &node_c) == KVSPACE_OK);
    CHECK(fixture.context.index_value_calls == 2);
    uint64_t directory_a = 0;
    uint64_t directory_b = 0;
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/", &directory_a) ==
          KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/a/b/",
                             &directory_b) == KVSPACE_OK);
    uint64_t value = 0;
    CHECK(trie_value_get(&fixture.env, directory_a, &value) == KVSPACE_OK);
    CHECK(value == TRIE_VALUE_PACK(1, 100));
    CHECK(value == fixture.context.index_value_values[0]);
    CHECK(trie_value_get(&fixture.env, directory_b, &value) == KVSPACE_OK);
    CHECK(value == TRIE_VALUE_PACK(1, 101));
    CHECK(value == fixture.context.index_value_values[1]);
    trie_hdr_t *header = NULL;
    CHECK(header_of(&fixture, directory_a, &header) == KVSPACE_OK);
    CHECK(header->value_gen == 1);
    CHECK(trie_value_get(&fixture.env, node_c, &value) == KVSPACE_OK);
    CHECK(value == TRIE_VALUE_PACK(1, 7));
    CHECK(fixture.context.index_value_calls == 2);
    CHECK(trie_write(&fixture.env, fixture.root, "/q/",
                     TRIE_VALUE_PACK(1, 8), NULL) == KVSPACE_OK);
    CHECK(fixture.context.index_value_calls == 2);
    fixture.context.index_value_fail_at = 4;
    CHECK(trie_write(&fixture.env, fixture.root, "/x/y/z/w",
                     TRIE_VALUE_PACK(1, 9), NULL) == KVSPACE_BACKEND);
    CHECK(fixture.context.index_value_calls == 4);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/x/",
                             &(uint64_t){0}) == KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/x/y/",
                             &(uint64_t){0}) == KVSPACE_OK);
    CHECK(trie_find_absolute(&fixture.env, fixture.root, "/x/y/z/",
                             &(uint64_t){0}) == KVSPACE_MISSING);
    fixture_destroy(&fixture);
    return 0;
}

static int test_node_lock(void) {
    trie_fixture_t fixture;
    CHECK(fixture_create(&fixture) == 0);
    lock_shared_t shared;
    atomic_init(&shared.inside, 0);
    atomic_init(&shared.violations, 0);
    lock_worker_t worker_a;
    lock_worker_t worker_b;
    memset(&worker_a, 0, sizeof(worker_a));
    memset(&worker_b, 0, sizeof(worker_b));
    worker_a.env = &fixture.env;
    worker_a.node = fixture.root;
    worker_a.iterations = 50000;
    worker_a.shared = &shared;
    worker_b.env = &fixture.env;
    worker_b.node = fixture.root;
    worker_b.iterations = 50000;
    worker_b.shared = &shared;
    pthread_t thread_a;
    pthread_t thread_b;
    CHECK(pthread_create(&thread_a, NULL, lock_worker_run, &worker_a) == 0);
    CHECK(pthread_create(&thread_b, NULL, lock_worker_run, &worker_b) == 0);
    CHECK(pthread_join(thread_a, NULL) == 0);
    CHECK(pthread_join(thread_b, NULL) == 0);
    CHECK(atomic_load(&shared.inside) == 0);
    CHECK(atomic_load(&shared.violations) == 0);
    trie_node_lock(&fixture.env, fixture.root);
    trie_node_unlock(&fixture.env, fixture.root);
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
    failures += test_subtree_visit();
    failures += test_subtree_length_limit();
    failures += test_child_at();
    failures += test_value_access();
    failures += test_generations_api();
    failures += test_make_index_value();
    failures += test_node_lock();
    if (failures == 0) {
        printf("PASS trie_test\n");
        return 0;
    }
    fprintf(stderr, "FAILED %d test groups\n", failures);
    return 1;
}

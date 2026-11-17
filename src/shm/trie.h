#ifndef KVSPACE_TRIE_H
#define KVSPACE_TRIE_H
#include <kvspace/kvspace.h>
#include <stdint.h>

#define TRIE_FLAG_ROOT UINT32_C(1)
#define TRIE_FLAG_DEAD UINT32_C(2)

#define TRIE_VALUE_ABSENT UINT64_C(0)
#define TRIE_VALUE_HAS_VALUE(value) ((int)((value) & UINT64_C(1)))
#define TRIE_VALUE_OFFSET(value) ((uint64_t)((value) >> 1))
#define TRIE_VALUE_PACK(has_value, offset) \
    ((((uint64_t)(offset)) << 1) | ((has_value) ? UINT64_C(1) : UINT64_C(0)))

#define TRIE_HAS_CHILDREN KVSPACE_HAS_CHILDREN
#define TRIE_KEY_LIMIT 1024
#define TRIE_MAX_SEGMENTS 1024

typedef struct trie_hdr {
    uint64_t value;
    uint64_t table_offset;
    uint32_t count;
    uint32_t capacity;
    uint32_t lock;
    uint32_t flags;
    uint32_t struct_gen;
    uint32_t value_gen;
    uint64_t name_offset;
    uint16_t name_length;
    uint16_t pad;
} trie_hdr_t;

typedef struct trie_ent {
    uint64_t name_offset;
    uint16_t name_length;
    uint16_t pad;
    uint64_t child_offset;
} trie_ent_t;

typedef struct trie_segment {
    const uint8_t *pointer;
    uint16_t length;
} trie_segment_t;

typedef struct trie_env {
    void *context;
    int (*alloc_bytes)(void *context, uint64_t bytes, uint64_t *out_offset);
    uint8_t *base;
    uint64_t slab_size;
} trie_env_t;

typedef int (*trie_visit_t)(void *context, const uint8_t *name,
                            uint16_t name_length, uint64_t child_offset);

int trie_root_create(trie_env_t *env, uint64_t *out_root);
int trie_key_segments(const char *key, trie_segment_t *out_segments,
                      int max_segments, int *out_count);
int trie_find_absolute(trie_env_t *env, uint64_t root, const char *key,
                       uint64_t *out_node);
int trie_find_relative(trie_env_t *env, uint64_t anchor, const char *suffix,
                       uint64_t *out_node);
int trie_write(trie_env_t *env, uint64_t root, const char *key, uint64_t value,
               uint64_t *out_node);
int trie_write_relative(trie_env_t *env, uint64_t anchor, const char *suffix,
                        uint64_t value, uint64_t *out_node);
int trie_remove(trie_env_t *env, uint64_t root, const char *key);
int trie_node_header(trie_env_t *env, uint64_t offset,
                     trie_hdr_t **out_header);
int trie_child_count(trie_env_t *env, uint64_t node, uint32_t *out_count);
int trie_longest_child_name(trie_env_t *env, uint64_t node,
                            uint16_t *out_length);
int trie_list_children(trie_env_t *env, uint64_t node, trie_visit_t visit,
                       void *context, uint32_t *out_count);

#endif

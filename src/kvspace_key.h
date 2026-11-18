#ifndef KVSPACE_KEY_H
#define KVSPACE_KEY_H
#include "kvspace_internal.h"
#include <stdint.h>
enum separator_kind { SEPARATOR_SLASH = 0, SEPARATOR_DOT = 1, SEPARATOR_COUNT = 2, SEPARATOR_NONE = -1 };
typedef struct { const char bytes[3]; uint8_t length; } separator_entry_t;
extern const separator_entry_t separator_table[SEPARATOR_COUNT];
enum { ADAPTIVE_RADIX_TREE_N4 = 0,
       ADAPTIVE_RADIX_TREE_N16 = 1,
       ADAPTIVE_RADIX_TREE_N48 = 2,
       ADAPTIVE_RADIX_TREE_N256 = 3,
       ADAPTIVE_RADIX_TREE_MOVED = 255 };
typedef struct {
    uint8_t type, prefix[ADAPTIVE_RADIX_TREE_PREFIX_MAX], prefix_len;
    uint16_t count;
    uint16_t padding;
    uint64_t value;
} adaptive_radix_tree_header_t;
#define ADAPTIVE_RADIX_TREE_VALUE_HAS(value) ((int)((value) & 1u))
#define ADAPTIVE_RADIX_TREE_VALUE_OFFSET(value) ((uint64_t)((value) >> 1))
#define ADAPTIVE_RADIX_TREE_VALUE_MAKE(has,offset) ((((uint64_t)(offset)) << 1) | ((has) ? 1ull : 0ull))
static inline uint64_t adaptive_radix_tree_value_load(const adaptive_radix_tree_header_t *node_header) {
    return __atomic_load_n(&node_header->value, __ATOMIC_ACQUIRE);
}
static inline void adaptive_radix_tree_value_store(adaptive_radix_tree_header_t *node_header, uint64_t value) {
    __atomic_store_n(&node_header->value, value, __ATOMIC_RELEASE);
}
static inline bool adaptive_radix_tree_value_compare_and_swap(adaptive_radix_tree_header_t *node_header, uint64_t *expected, uint64_t new_value) {
    return __atomic_compare_exchange_n(&node_header->value, expected, new_value, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
typedef struct {
    adaptive_radix_tree_header_t header;
    uint8_t keys[4];
    int32_t children[4];
} adaptive_radix_tree_node4_t;
typedef struct {
    adaptive_radix_tree_header_t header;
    uint8_t keys[16];
    int32_t children[16];
} adaptive_radix_tree_node16_t;
typedef struct {
    adaptive_radix_tree_header_t header;
    uint8_t index[256];
    int32_t children[48];
} adaptive_radix_tree_node48_t;
typedef struct {
    adaptive_radix_tree_header_t header;
    int32_t children[256];
} adaptive_radix_tree_node256_t;
_Static_assert(sizeof(adaptive_radix_tree_node4_t) <= ADAPTIVE_RADIX_TREE_BLOCK_SIZE && sizeof(adaptive_radix_tree_node16_t) <= ADAPTIVE_RADIX_TREE_BLOCK_SIZE,
               "N4/N16 must fit one ART block");
typedef struct {
    char **keys;
    int32_t count;
    int32_t capacity;
} keyscan_t;

enum separator_kind separator_at(const char *text, size_t index, size_t length);
enum separator_kind ending_separator(const char *text, size_t length);
int separator_length(enum separator_kind kind);
int reserved_meta_path(const char *key);
int key_text_valid(const char *key);
char *path_join(const char *left, const char *right);
char *ending_directory(const char *path);
char *member_directory(const char *path);
int subtree_key(const char *base, size_t base_length, const char *key);
void keyscan_free(keyscan_t *scan);
int storage_has_children(kvspace_t *store, const char *key);
int storage_children(kvspace_t *store, const char *key, keyscan_t *out);
int storage_is_index_key(const char *key);
int32_t adaptive_radix_tree_id_of(kvspace_t *store, const void *pointer);
adaptive_radix_tree_header_t *adaptive_radix_tree_search(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length);
int32_t adaptive_radix_tree_insert(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length, int depth, uint64_t offset);
int32_t adaptive_radix_tree_delete(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length, int depth, bool *deleted, uint64_t *out_retired_offset);
int adaptive_radix_tree_scan_keys(kvspace_t *store, const char *prefix, int prefix_length, keyscan_t *scan);
int resolve_root(kvspace_t *store, const char *key, uint64_t *out_offset);
int resolve_anchor(kvspace_t *store, const char *key, const kvspace_anchor_t *anchor, uint64_t *out_offset);
int listlen_key(kvspace_t *store, const char *key, int64_t *out_count, uint32_t *out_max_name_length);
int list_names_raw(kvspace_t *store, const char *key, char *out_names, uint32_t out_capacity, uint32_t *out_length, uint32_t *out_count, uint64_t *out_generation);
#endif

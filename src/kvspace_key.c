#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kvspace_key.h"
#include "kvspace_recovery.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static adaptive_radix_tree_header_t *adaptive_radix_tree_header(kvspace_t *store, int32_t id);
static int shared_prefix_length(const uint8_t *left, int left_length, const uint8_t *right, int right_length);
static int32_t adaptive_radix_tree_child(kvspace_t *store, adaptive_radix_tree_header_t *node_header, uint8_t byte);
static int32_t adaptive_radix_tree_walk(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
                                        int depth, int32_t *out_parent, int *out_parent_depth);
static int32_t adaptive_radix_tree_follow(kvspace_t *store, int32_t id);
const separator_entry_t separator_table[SEPARATOR_COUNT] = {
    {"/", 1}, {"\xC2\xB7", 2}
};
void structure_seq_begin(kvspace_t *store) {
    __atomic_add_fetch(&store->header->structure_epoch, 1ull, __ATOMIC_ACQ_REL);
}
void structure_seq_end(kvspace_t *store) {
    __atomic_add_fetch(&store->header->structure_epoch, 1ull, __ATOMIC_RELEASE);
}
uint64_t structure_seq_read_begin(kvspace_t *store) {
    for (int spin = 0;; spin++) {
        uint64_t epoch = __atomic_load_n(&store->header->structure_epoch, __ATOMIC_ACQUIRE);
        if ((epoch & 1ull) == 0)
            return epoch;
        if (spin >= 4096) {
            recovery_repair_dead_writer(store);
            spin = 0;
        }
        kvspace_cpu_relax();
    }
}
bool structure_seq_read_retry(const kvspace_t *store, uint64_t before) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&store->header->structure_epoch, __ATOMIC_ACQUIRE) != before;
}
int separator_length(enum separator_kind kind) {
    if (kind < SEPARATOR_SLASH || kind >= SEPARATOR_COUNT)
        return 0;
    return separator_table[kind].length;
}
enum separator_kind separator_at(const char *text, size_t index, size_t length) {
    for (int kind = SEPARATOR_SLASH; kind < SEPARATOR_COUNT; kind++) {
        int count = separator_length((enum separator_kind)kind);
        if (count > 0 && index + (size_t)count <= length &&
            memcmp(text + index, separator_table[kind].bytes, (size_t)count) == 0)
            return (enum separator_kind)kind;
    }
    return SEPARATOR_NONE;
}
enum separator_kind ending_separator(const char *text, size_t length) {
    for (int kind = SEPARATOR_SLASH; kind < SEPARATOR_COUNT; kind++) {
        int count = separator_length((enum separator_kind)kind);
        if (count > 0 && (size_t)count <= length &&
            separator_at(text, length - (size_t)count, length) == (enum separator_kind)kind)
            return (enum separator_kind)kind;
    }
    return SEPARATOR_NONE;
}
static char *append_separator(const char *text, enum separator_kind kind) {
    size_t length = strlen(text);
    int count = separator_length(kind);
    char *result = malloc(length + (size_t)count + 1);
    if (!result)
        return NULL;
    memcpy(result, text, length);
    memcpy(result + length, separator_table[kind].bytes, (size_t)count);
    result[length + (size_t)count] = 0;
    return result;
}
static int separator_sequence_valid(const char *text, size_t length) {
    for (size_t index = 0; index < length;) {
        enum separator_kind kind = separator_at(text, index, length);
        if (kind == SEPARATOR_NONE) {
            index++;
            continue;
        }
        int count = separator_length(kind);
        if (separator_at(text, index + (size_t)count, length) != SEPARATOR_NONE)
            return 0;
        index += (size_t)count;
    }
    return 1;
}
int reserved_meta_path(const char *key) {
    static const char root[] = "/.kvspace-meta";
    if (strncmp(key, root, sizeof root - 1) != 0)
        return 0;
    size_t length = strlen(key);
    size_t index = sizeof root - 1;
    return index == length ||
           separator_at(key, index, length) == SEPARATOR_SLASH;
}
static int reserved_meta_relative(uint32_t anchor_depth, const char *suffix) {
    static const char root[] = "/.kvspace-meta";
    int slash_length = separator_length(SEPARATOR_SLASH);
    if (anchor_depth != (uint32_t)slash_length)
        return 0;
    const char *meta_name = root + slash_length;
    size_t meta_length = sizeof root - 1 - (size_t)slash_length;
    if (strncmp(suffix, meta_name, meta_length) != 0)
        return 0;
    size_t suffix_length = strlen(suffix);
    return suffix_length == meta_length ||
           separator_at(suffix, meta_length, suffix_length) == SEPARATOR_SLASH;
}
int key_text_valid(const char *key) {
    if (!key)
        return 0;
    size_t length = strlen(key);
    if (length == 0 || length >= 1024)
        return 0;
    if (reserved_meta_path(key) || !separator_sequence_valid(key, length))
        return 0;
    return separator_at(key, 0, length) == SEPARATOR_SLASH;
}
size_t subtree_base_length(const char *path, size_t length) {
    if (length == (size_t)separator_length(SEPARATOR_SLASH) &&
        separator_at(path, 0, length) == SEPARATOR_SLASH)
        return 0;
    enum separator_kind kind = ending_separator(path, length);
    if (kind != SEPARATOR_NONE)
        return length - (size_t)separator_length(kind);
    return length;
}
static int adaptive_radix_tree_node_size(int node_type) {
    return node_type == ADAPTIVE_RADIX_TREE_N4 ? (int)sizeof(adaptive_radix_tree_node4_t)
           : node_type == ADAPTIVE_RADIX_TREE_N16 ? (int)sizeof(adaptive_radix_tree_node16_t)
           : node_type == ADAPTIVE_RADIX_TREE_N48 ? (int)sizeof(adaptive_radix_tree_node48_t)
                          : (int)sizeof(adaptive_radix_tree_node256_t);
}
int32_t adaptive_radix_tree_id_of(kvspace_t *store, const void *pointer) {
    if (!pointer || !store->adaptive_radix_tree_data)
        return -1;
    return (int32_t)kvspace_block_id_by_data_offset(store->adaptive_radix_tree_meta,
                                         (uint64_t)((const uint8_t *)pointer - store->adaptive_radix_tree_data));
}
#ifdef RECOVERY_TEST_HOOKS
int64_t kvspaceTestArtAlignCheck(kvspace_t *store) {
    if (!store || !store->adaptive_radix_tree_meta)
        return -2;
    blocks_meta_t *meta = store->adaptive_radix_tree_meta;
    if (meta->sizeof_block_head != 8)
        return -2;
    uint64_t stride = (uint64_t)meta->sizeof_block_head + meta->block_size;
    if (stride % 8)
        return -2;
    uint64_t block_count = meta->total_size / stride;
    for (uint64_t id = 0; id < block_count; id++) {
        uint64_t offset = (uint64_t)kvspace_block_data_offset(meta, id);
        uint64_t value = offset + offsetof(adaptive_radix_tree_header_t, value);
        if (offset % 8 || value % 8 || (value & 63) > 56)
            return (int64_t)id;
    }
    return -1;
}
#endif
static int adaptive_radix_tree_slab_grow(kvspace_t *store) {
    size_t want = (size_t)store->header->adaptive_radix_tree_slab_size * 2;
    if (ADAPTIVE_RADIX_TREE_OFFSET + want > store->region_adaptive_radix_tree.reserve ||
        ftruncate(store->region_adaptive_radix_tree.file_descriptor, (off_t)(ADAPTIVE_RADIX_TREE_OFFSET + want)) != 0 ||
        region_map(&store->region_adaptive_radix_tree, ADAPTIVE_RADIX_TREE_OFFSET + want) != 0)
        return -1;
    store->adaptive_radix_tree_meta->total_size = want;
    __atomic_store_n(&store->header->adaptive_radix_tree_slab_size, want, __ATOMIC_RELEASE);
    return 0;
}
static void *adaptive_radix_tree_block_sized(kvspace_t *store, int32_t id, size_t needed_bytes) {
    if (id < 0 || !store->adaptive_radix_tree_meta)
        return NULL;
    uint64_t offset = (uint64_t)kvspace_block_data_offset(store->adaptive_radix_tree_meta, (uint64_t)id);
    size_t mapped = __atomic_load_n(&store->region_adaptive_radix_tree.mapped, __ATOMIC_ACQUIRE);
    if (offset > mapped || ADAPTIVE_RADIX_TREE_OFFSET + offset + needed_bytes > mapped)
        return NULL;
    return store->adaptive_radix_tree_data + offset;
}
static void *adaptive_radix_tree_block(kvspace_t *store, int32_t id) {
    return adaptive_radix_tree_block_sized(store, id, sizeof(adaptive_radix_tree_node4_t));
}
static bool adaptive_radix_tree_node_mapped(kvspace_t *store, const adaptive_radix_tree_header_t *node_header, uint8_t node_type) {
    if (!node_header || node_type > ADAPTIVE_RADIX_TREE_N256)
        return false;
    size_t offset = (size_t)((const uint8_t *)node_header - store->adaptive_radix_tree_data);
    size_t mapped = __atomic_load_n(&store->region_adaptive_radix_tree.mapped, __ATOMIC_ACQUIRE);
    size_t size = (size_t)adaptive_radix_tree_node_size(node_type);
    return ADAPTIVE_RADIX_TREE_OFFSET + offset + size <= mapped;
}
static int64_t adaptive_radix_tree_bump(kvspace_t *store, uint32_t block_count) {
    blocks_meta_t *meta = store->adaptive_radix_tree_meta;
    kvspace_block_meta_lock(meta);
    uint64_t id = KVSPACE_BLOCKMALLOC_MALLOC_BLOCKS(meta);
    if ((uint64_t)kvspace_block_offset(meta, id + block_count) > meta->total_size) {
        kvspace_block_meta_unlock(meta);
        return -1;
    }
    kvspace_block_mark_used(meta, store->adaptive_radix_tree_data + kvspace_block_offset(meta, id));
    kvspace_block_set_malloc_blocks(meta, id + block_count);
    __atomic_store_n(&meta->used_blocks, meta->used_blocks + block_count, __ATOMIC_RELEASE);
    kvspace_block_meta_unlock(meta);
    return (int64_t)id;
}
static int32_t adaptive_radix_tree_block_alloc(kvspace_t *store, int node_type) {
    const uint64_t head = KVSPACE_BLOCKMALLOC_HEAD_SIZE(store->adaptive_radix_tree_meta);
    const uint64_t stride = head + store->adaptive_radix_tree_meta->block_size;
    const uint32_t block_count = (uint32_t)(((uint64_t)adaptive_radix_tree_node_size(node_type) +
                                   head + stride - 1) /
                                  stride);
    int64_t id = adaptive_radix_tree_bump(store, block_count);
    while (id < 0) {
        if (adaptive_radix_tree_slab_grow(store) != 0)
            return -1;
        id = adaptive_radix_tree_bump(store, block_count);
    }
    return (int32_t)id;
}
static adaptive_radix_tree_header_t *adaptive_radix_tree_header(kvspace_t *store, int32_t id) {
    return (adaptive_radix_tree_header_t *)adaptive_radix_tree_block(store, id);
}
static int shared_prefix_length(const uint8_t *left, int left_length, const uint8_t *right, int right_length) {
    int common_length = left_length < right_length ? left_length : right_length;
    for (int i = 0; i < common_length; i++)
        if (left[i] != right[i])
            return i;
    return common_length;
}
static int32_t adaptive_radix_tree_child(kvspace_t *store, adaptive_radix_tree_header_t *node_header, uint8_t byte) {
    uint8_t node_type = __atomic_load_n(&node_header->type, __ATOMIC_ACQUIRE);
    if (!adaptive_radix_tree_node_mapped(store, node_header, node_type))
        return -1;
    uint16_t count = __atomic_load_n(&node_header->count, __ATOMIC_ACQUIRE);
    switch (node_type) {
    case ADAPTIVE_RADIX_TREE_N4: {
        if (count > 4)
            return -1;
        adaptive_radix_tree_node4_t *node_data = (adaptive_radix_tree_node4_t *)node_header;
        for (int i = 0; i < (int)count; i++)
            if (node_data->keys[i] == byte)
                return node_data->children[i];
        return -1;
    }
    case ADAPTIVE_RADIX_TREE_N16: {
        if (count > 16)
            return -1;
        adaptive_radix_tree_node16_t *node_data = (adaptive_radix_tree_node16_t *)node_header;
        int low = 0, high = (int)count - 1;
        while (low <= high) {
            int middle = (low + high) / 2;
            if (node_data->keys[middle] == byte)
                return node_data->children[middle];
            if (node_data->keys[middle] < byte)
                low = middle + 1;
            else
                high = middle - 1;
        }
        return -1;
    }
    case ADAPTIVE_RADIX_TREE_N48: {
        adaptive_radix_tree_node48_t *node_data = (adaptive_radix_tree_node48_t *)node_header;
        uint8_t index = node_data->index[byte];
        return index >= 48 ? -1 : node_data->children[index];
    }
    case ADAPTIVE_RADIX_TREE_N256: {
        return ((adaptive_radix_tree_node256_t *)node_header)->children[byte];
    }
    }
    return -1;
}
static int32_t adaptive_radix_tree_follow(kvspace_t *store, int32_t id) {
    for (int i = 0; i < 8 && id >= 0; i++) {
        adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, id);
        if (!node_header)
            return -1;
        if (__atomic_load_n(&node_header->type, __ATOMIC_ACQUIRE) != ADAPTIVE_RADIX_TREE_MOVED)
            return id;
        uint64_t moved_value = adaptive_radix_tree_value_load(node_header);
        if (ADAPTIVE_RADIX_TREE_VALUE_HAS(moved_value))
            return -1;
        uint64_t next_id = ADAPTIVE_RADIX_TREE_VALUE_OFFSET(moved_value);
        if (next_id > INT32_MAX)
            return -1;
        id = (int32_t)next_id;
    }
    return -1;
}
static int32_t adaptive_radix_tree_walk(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
                        int depth, int32_t *out_parent, int *out_parent_depth) {
    while (node_id >= 0 && depth <= key_length) {
        node_id = adaptive_radix_tree_follow(store, node_id);
        if (node_id < 0)
            return -1;
        adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
        if (!node_header)
            return -1;
        uint8_t prefix_length = __atomic_load_n(&node_header->prefix_len, __ATOMIC_ACQUIRE);
        if (prefix_length > ADAPTIVE_RADIX_TREE_PREFIX_MAX)
            return -1;
        if (prefix_length) {
            if (shared_prefix_length(node_header->prefix, prefix_length, key + depth, key_length - depth) !=
                prefix_length)
                return -1;
            depth += prefix_length;
        }
        if (depth == key_length)
            return ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node_header)) ? node_id : -1;
        if (out_parent)
            *out_parent = node_id;
        if (out_parent_depth)
            *out_parent_depth = depth;
        node_id = adaptive_radix_tree_child(store, node_header, key[depth]);
        depth++;
    }
    return -1;
}
static int32_t adaptive_radix_tree_find_with_parent(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
                         int32_t *out_parent, int *out_parent_depth) {
    if (node_id < 0 || !key)
        return -1;
    int32_t parent = -1;
    int parent_depth = 0;
    int32_t id = adaptive_radix_tree_walk(store, node_id, key, key_length, 0, &parent, &parent_depth);
    if (out_parent)
        *out_parent = parent;
    if (out_parent_depth)
        *out_parent_depth = parent_depth;
    return id;
}
static int32_t adaptive_radix_tree_find(kvspace_t *store, int32_t node_id, const uint8_t *key,
                        int key_length) {
    return adaptive_radix_tree_find_with_parent(store, node_id, key, key_length, NULL, NULL);
}
adaptive_radix_tree_header_t *adaptive_radix_tree_search(kvspace_t *store, int32_t node_id, const uint8_t *key,
                             int key_length) {
    int32_t id = adaptive_radix_tree_find(store, node_id, key, key_length);
    return id < 0 ? NULL : adaptive_radix_tree_header(store, id);
}
static int32_t adaptive_radix_tree_new_leaf(kvspace_t *store, uint64_t offset) {
    int32_t id = adaptive_radix_tree_block_alloc(store, ADAPTIVE_RADIX_TREE_N4);
    if (id < 0)
        return -1;
    adaptive_radix_tree_node4_t *node_data = adaptive_radix_tree_block_sized(store, id, sizeof(*node_data));
    if (!node_data)
        return -1;
    memset(node_data, 0, sizeof(*node_data));
    node_data->header.type = ADAPTIVE_RADIX_TREE_N4;
    adaptive_radix_tree_value_store(&node_data->header, ADAPTIVE_RADIX_TREE_VALUE_MAKE(1, offset));
    return id;
}
static int32_t adaptive_radix_tree_new_node(kvspace_t *store, int node_type) {
    int32_t id = adaptive_radix_tree_block_alloc(store, node_type);
    if (id < 0)
        return -1;
    int size = adaptive_radix_tree_node_size(node_type);
    void *node_data = adaptive_radix_tree_block_sized(store, id, (size_t)size);
    if (!node_data)
        return -1;
    memset(node_data, 0, (size_t)size);
    ((adaptive_radix_tree_header_t *)node_data)->type = (uint8_t)node_type;
    if (node_type == ADAPTIVE_RADIX_TREE_N48)
        memset(((adaptive_radix_tree_node48_t *)node_data)->index, 255, 256);
    if (node_type == ADAPTIVE_RADIX_TREE_N256) {
        adaptive_radix_tree_node256_t *node256 = node_data;
        for (int i = 0; i < 256; i++)
            node256->children[i] = -1;
    }
    return id;
}
static int32_t adaptive_radix_tree_grow(kvspace_t *store, void *old_node) {
    adaptive_radix_tree_header_t *old_header = old_node;
    int new_type;
    if (old_header->type == ADAPTIVE_RADIX_TREE_N4)
        new_type = ADAPTIVE_RADIX_TREE_N16;
    else if (old_header->type == ADAPTIVE_RADIX_TREE_N16)
        new_type = ADAPTIVE_RADIX_TREE_N48;
    else if (old_header->type == ADAPTIVE_RADIX_TREE_N48)
        new_type = ADAPTIVE_RADIX_TREE_N256;
    else
        return -1;
    int32_t node_id = adaptive_radix_tree_new_node(store, new_type);
    if (node_id < 0)
        return -1;
    adaptive_radix_tree_header_t *new_header = adaptive_radix_tree_block_sized(store, node_id, (size_t)adaptive_radix_tree_node_size(new_type));
    if (!new_header)
        return -1;
    new_header->prefix_len = old_header->prefix_len;
    memcpy(new_header->prefix, old_header->prefix, old_header->prefix_len);
    adaptive_radix_tree_value_store(new_header, adaptive_radix_tree_value_load(old_header));
    for (int i = 0; i < (int)old_header->count; i++) {
        uint8_t byte = 0;
        int32_t child_id = -1;
        switch (old_header->type) {
        case ADAPTIVE_RADIX_TREE_N4: {
            adaptive_radix_tree_node4_t *node_data = old_node;
            byte = node_data->keys[i];
            child_id = node_data->children[i];
            break;
        }
        case ADAPTIVE_RADIX_TREE_N16: {
            adaptive_radix_tree_node16_t *node_data = old_node;
            byte = node_data->keys[i];
            child_id = node_data->children[i];
            break;
        }
        case ADAPTIVE_RADIX_TREE_N48: {
            adaptive_radix_tree_node48_t *node_data = old_node;
            child_id = node_data->children[i];
            for (int j = 0; j < 256; j++)
                if (node_data->index[j] == i) {
                    byte = (uint8_t)j;
                    break;
                }
            break;
        }
        }
        switch (new_type) {
        case ADAPTIVE_RADIX_TREE_N16: {
            adaptive_radix_tree_node16_t *node_data = (adaptive_radix_tree_node16_t *)new_header;
            node_data->keys[i] = byte;
            node_data->children[i] = child_id;
            break;
        }
        case ADAPTIVE_RADIX_TREE_N48: {
            adaptive_radix_tree_node48_t *node_data = (adaptive_radix_tree_node48_t *)new_header;
            node_data->index[byte] = (uint8_t)i;
            node_data->children[i] = child_id;
            break;
        }
        case ADAPTIVE_RADIX_TREE_N256: {
            ((adaptive_radix_tree_node256_t *)new_header)->children[byte] = child_id;
            break;
        }
        }
    }
    new_header->count = old_header->count;
    if (new_type == ADAPTIVE_RADIX_TREE_N16) {
        adaptive_radix_tree_node16_t *node_data = (adaptive_radix_tree_node16_t *)new_header;
        for (int i = 0; i < (int)new_header->count - 1; i++)
            for (int j = i + 1; j < (int)new_header->count; j++)
                if (node_data->keys[i] > node_data->keys[j]) {
                    uint8_t temporary_key = node_data->keys[i];
                    node_data->keys[i] = node_data->keys[j];
                    node_data->keys[j] = temporary_key;
                    int32_t temporary_child = node_data->children[i];
                    node_data->children[i] = node_data->children[j];
                    node_data->children[j] = temporary_child;
                }
    }
    adaptive_radix_tree_value_store(old_header, ADAPTIVE_RADIX_TREE_VALUE_MAKE(0, (uint32_t)node_id));
    __atomic_store_n(&old_header->type, ADAPTIVE_RADIX_TREE_MOVED, __ATOMIC_RELEASE);
    return node_id;
}
static int adaptive_radix_tree_add(void *node, uint8_t byte, int32_t child_id) {
    adaptive_radix_tree_header_t *node_header = node;
    switch (node_header->type) {
    case ADAPTIVE_RADIX_TREE_N4: {
        if (node_header->count >= 4)
            return -1;
        adaptive_radix_tree_node4_t *node_data = node;
        node_data->keys[node_header->count] = byte;
        node_data->children[node_header->count] = child_id;
        __atomic_store_n(&node_header->count, (uint16_t)(node_header->count + 1), __ATOMIC_RELEASE);
        return 0;
    }
    case ADAPTIVE_RADIX_TREE_N16: {
        if (node_header->count >= 16)
            return -1;
        adaptive_radix_tree_node16_t *node_data = node;
        int position = (int)node_header->count;
        while (position > 0 && node_data->keys[position - 1] > byte) {
            node_data->keys[position] = node_data->keys[position - 1];
            node_data->children[position] = node_data->children[position - 1];
            position--;
        }
        node_data->keys[position] = byte;
        node_data->children[position] = child_id;
        __atomic_store_n(&node_header->count, (uint16_t)(node_header->count + 1), __ATOMIC_RELEASE);
        return 0;
    }
    case ADAPTIVE_RADIX_TREE_N48: {
        if (node_header->count >= 48)
            return -1;
        adaptive_radix_tree_node48_t *node_data = node;
        int slot = (int)node_header->count;
        node_data->children[slot] = child_id;
        node_data->index[byte] = (uint8_t)slot;
        __atomic_store_n(&node_header->count, (uint16_t)(node_header->count + 1), __ATOMIC_RELEASE);
        return 0;
    }
    case ADAPTIVE_RADIX_TREE_N256: {
        adaptive_radix_tree_node256_t *node_data = node;
        if (node_data->children[byte] >= 0)
            return -1;
        node_data->children[byte] = child_id;
        __atomic_store_n(&node_header->count, (uint16_t)(node_header->count + 1), __ATOMIC_RELEASE);
        return 0;
    }
    }
    return -1;
}
static void adaptive_radix_tree_replace_child(void *node, uint8_t byte, int32_t new_child) {
    adaptive_radix_tree_header_t *node_header = node;
    switch (node_header->type) {
    case ADAPTIVE_RADIX_TREE_N4: {
        adaptive_radix_tree_node4_t *node_data = node;
        for (int i = 0; i < (int)node_header->count; i++)
            if (node_data->keys[i] == byte) {
                __atomic_store_n(&node_data->children[i], new_child, __ATOMIC_RELEASE);
                return;
            }
        break;
    }
    case ADAPTIVE_RADIX_TREE_N16: {
        adaptive_radix_tree_node16_t *node_data = node;
        int low = 0, high = (int)node_header->count - 1;
        while (low <= high) {
            int middle = (low + high) / 2;
            if (node_data->keys[middle] == byte) {
                __atomic_store_n(&node_data->children[middle], new_child, __ATOMIC_RELEASE);
                return;
            }
            if (node_data->keys[middle] < byte)
                low = middle + 1;
            else
                high = middle - 1;
        }
        break;
    }
    case ADAPTIVE_RADIX_TREE_N48: {
        adaptive_radix_tree_node48_t *node_data = node;
        uint8_t index = node_data->index[byte];
        if (index < 48)
            __atomic_store_n(&node_data->children[index], new_child, __ATOMIC_RELEASE);
        break;
    }
    case ADAPTIVE_RADIX_TREE_N256: {
        adaptive_radix_tree_node256_t *node_data = node;
        __atomic_store_n(&node_data->children[byte], new_child, __ATOMIC_RELEASE);
        break;
    }
    }
}
static int32_t adaptive_radix_tree_leaf_chain(kvspace_t *store, const uint8_t *key, int key_length,
                              int depth, uint64_t offset) {
    int remaining = key_length - depth;
    if (remaining <= ADAPTIVE_RADIX_TREE_PREFIX_MAX) {
        int32_t id = adaptive_radix_tree_new_leaf(store, offset);
        if (id < 0)
            return -1;
        adaptive_radix_tree_node4_t *node_data = adaptive_radix_tree_block(store, id);
        if (!node_data)
            return -1;
        if (remaining > 0)
            memcpy(node_data->header.prefix, key + depth, (size_t)remaining);
        node_data->header.prefix_len = (uint8_t)remaining;
        return id;
    }
    int32_t id = adaptive_radix_tree_new_node(store, ADAPTIVE_RADIX_TREE_N4);
    if (id < 0)
        return -1;
    adaptive_radix_tree_header_t *node_data = adaptive_radix_tree_header(store, id);
    if (!node_data)
        return -1;
    memcpy(node_data->prefix, key + depth, ADAPTIVE_RADIX_TREE_PREFIX_MAX);
    node_data->prefix_len = ADAPTIVE_RADIX_TREE_PREFIX_MAX;
    int32_t subtree_node = adaptive_radix_tree_leaf_chain(store, key, key_length, depth + ADAPTIVE_RADIX_TREE_PREFIX_MAX + 1, offset);
    if (subtree_node < 0)
        return -1;
    adaptive_radix_tree_add(node_data, key[depth + ADAPTIVE_RADIX_TREE_PREFIX_MAX], subtree_node);
    return id;
}
int32_t adaptive_radix_tree_insert(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
                       int depth, uint64_t offset) {
    if (node_id < 0)
        return adaptive_radix_tree_leaf_chain(store, key, key_length, depth, offset);
    node_id = adaptive_radix_tree_follow(store, node_id);
    if (node_id < 0)
        return -1;
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
    if (!node_header)
        return -1;
    int shared = shared_prefix_length(node_header->prefix, node_header->prefix_len, key + depth, key_length - depth);
    int minimum_prefix_length = node_header->prefix_len < (key_length - depth) ? node_header->prefix_len : (key_length - depth);
    if (shared == minimum_prefix_length && (key_length - depth) < node_header->prefix_len) {
        int32_t new_node = adaptive_radix_tree_new_node(store, ADAPTIVE_RADIX_TREE_N4);
        if (new_node < 0)
            return -1;
        node_header = adaptive_radix_tree_header(store, node_id);
        adaptive_radix_tree_header_t *new_header = adaptive_radix_tree_header(store, new_node);
        if (!node_header || !new_header)
            return -1;
        new_header->prefix_len = (uint8_t)shared;
        if (shared > 0)
            memcpy(new_header->prefix, key + depth, (size_t)shared);
        uint8_t old_byte = node_header->prefix[shared];
        node_header->prefix_len -= (uint8_t)(shared + 1);
        if (node_header->prefix_len > 0)
            memmove(node_header->prefix, node_header->prefix + shared + 1, node_header->prefix_len);
        adaptive_radix_tree_add(new_header, old_byte, node_id);
        adaptive_radix_tree_value_store(new_header, ADAPTIVE_RADIX_TREE_VALUE_MAKE(1, offset));
        return new_node;
    }
    if (shared < minimum_prefix_length) {
        int32_t new_node = adaptive_radix_tree_new_node(store, ADAPTIVE_RADIX_TREE_N4);
        if (new_node < 0)
            return -1;
        int32_t leaf = adaptive_radix_tree_leaf_chain(store, key, key_length, depth + shared + 1, offset);
        if (leaf < 0)
            return -1;
        node_header = adaptive_radix_tree_header(store, node_id);
        adaptive_radix_tree_header_t *new_header = adaptive_radix_tree_header(store, new_node);
        if (!node_header || !new_header)
            return -1;
        new_header->prefix_len = (uint8_t)shared;
        if (shared > 0)
            memcpy(new_header->prefix, key + depth, (size_t)shared);
        uint8_t old_byte = node_header->prefix[shared];
        node_header->prefix_len -= (uint8_t)(shared + 1);
        if (node_header->prefix_len > 0)
            memmove(node_header->prefix, node_header->prefix + shared + 1, node_header->prefix_len);
        adaptive_radix_tree_add(new_header, old_byte, node_id);
        adaptive_radix_tree_add(new_header, key[depth + shared], leaf);
        return new_node;
    }
    depth += node_header->prefix_len;
    if (depth == key_length) {
        adaptive_radix_tree_value_store(node_header, ADAPTIVE_RADIX_TREE_VALUE_MAKE(1, offset));
        return node_id;
    }
    int32_t child_id = adaptive_radix_tree_child(store, node_header, key[depth]);
    if (child_id >= 0) {
        int32_t new_child = adaptive_radix_tree_insert(store, child_id, key, key_length, depth + 1, offset);
        if (new_child < 0)
            return -1;
        if (new_child != child_id) {
            node_header = adaptive_radix_tree_header(store, node_id);
            adaptive_radix_tree_replace_child(node_header, key[depth], new_child);
        }
        return node_id;
    }
    int32_t leaf = adaptive_radix_tree_leaf_chain(store, key, key_length, depth + 1, offset);
    if (leaf < 0)
        return -1;
    node_header = adaptive_radix_tree_header(store, node_id);
    if ((node_header->type == ADAPTIVE_RADIX_TREE_N4 && node_header->count >= 4) ||
        (node_header->type == ADAPTIVE_RADIX_TREE_N16 && node_header->count >= 16) ||
        (node_header->type == ADAPTIVE_RADIX_TREE_N48 && node_header->count >= 48)) {
        int32_t grown = adaptive_radix_tree_grow(store, node_header);
        if (grown < 0)
            return -1;
        adaptive_radix_tree_add(adaptive_radix_tree_block_sized(store, grown, (size_t)adaptive_radix_tree_node_size(adaptive_radix_tree_header(store, grown)->type)),
                                key[depth], leaf);
        return grown;
    }
    adaptive_radix_tree_add(node_header, key[depth], leaf);
    return node_id;
}
int32_t adaptive_radix_tree_delete(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
                       int depth, bool *deleted, uint64_t *out_retired_offset) {
    *deleted = false;
    *out_retired_offset = 0;
    if (node_id < 0)
        return -1;
    node_id = adaptive_radix_tree_follow(store, node_id);
    if (node_id < 0)
        return -1;
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
    if (!node_header)
        return -1;
    if (node_header->prefix_len) {
        if (shared_prefix_length(node_header->prefix, node_header->prefix_len, key + depth, key_length - depth) !=
            node_header->prefix_len)
            return node_id;
        depth += node_header->prefix_len;
    }
    if (depth == key_length) {
        uint64_t header_value = adaptive_radix_tree_value_load(node_header);
        if (!ADAPTIVE_RADIX_TREE_VALUE_HAS(header_value))
            return node_id;
        if (!adaptive_radix_tree_value_compare_and_swap(node_header, &header_value, 0))
            return node_id;
        *out_retired_offset = ADAPTIVE_RADIX_TREE_VALUE_OFFSET(header_value);
        *deleted = true;
    } else {
        int32_t child_id = adaptive_radix_tree_child(store, node_header, key[depth]);
        if (child_id >= 0) {
            int32_t return_code = adaptive_radix_tree_delete(store, child_id, key, key_length, depth + 1, deleted, out_retired_offset);
            if (return_code < 0)
                return return_code;
        }
    }
    return node_id;
}
static int32_t adaptive_radix_tree_cover(kvspace_t *store, int32_t node_id, const uint8_t *prefix,
                         int prefix_length, char *buffer, int buffer_capacity, int *buffer_position) {
    int depth = 0;
    if (node_id < 0 || !prefix || prefix_length < 0 || !buffer || !buffer_position)
        return -1;
    while (node_id >= 0 && depth <= prefix_length) {
        node_id = adaptive_radix_tree_follow(store, node_id);
        if (node_id < 0)
            return -1;
        adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
        if (!node_header)
            return -1;
        uint8_t prefix_len = __atomic_load_n(&node_header->prefix_len, __ATOMIC_ACQUIRE);
        if (prefix_len > ADAPTIVE_RADIX_TREE_PREFIX_MAX)
            return -1;
        int prefix_depth = depth, cover = 0;
        if (prefix_len) {
            int shared = shared_prefix_length(node_header->prefix, prefix_len, prefix + depth, prefix_length - depth);
            if (shared < prefix_len) {
                if (depth + shared < prefix_length)
                    return -1;
                cover = 1;
            } else
                depth += prefix_len;
        }
        if (cover || depth == prefix_length) {
            if (prefix_depth >= buffer_capacity)
                return -1;
            if (prefix_depth)
                memcpy(buffer, prefix, (size_t)prefix_depth);
            *buffer_position = prefix_depth;
            return node_id;
        }
        node_id = adaptive_radix_tree_child(store, node_header, prefix[depth]);
        if (node_id < 0)
            return -1;
        depth++;
    }
    return -1;
}
void keyscan_free(keyscan_t *scan) {
    for (int32_t i = 0; i < scan->count; i++)
        free(scan->keys[i]);
    free(scan->keys);
    scan->keys = NULL;
    scan->count = scan->capacity = 0;
}
static int keyscan_add_name(keyscan_t *scan, const char *key, size_t length) {
    if (scan->count == INT32_MAX)
        return -1;
    if (scan->count == scan->capacity) {
        int32_t capacity = scan->capacity > INT32_MAX / 2 ? INT32_MAX :
                      scan->capacity ? scan->capacity * 2 : 64;
        char **keys = realloc(scan->keys, (size_t)capacity * sizeof *keys);
        if (!keys)
            return -1;
        scan->keys = keys;
        scan->capacity = capacity;
    }
    char *copy = strndup(key, length);
    if (!copy)
        return -1;
    scan->keys[scan->count++] = copy;
    return 0;
}
static int child_name_len(const char *rest, int rest_length) {
    for (int i = 0; i < rest_length; i++) {
        enum separator_kind kind = separator_at(rest, (size_t)i, (size_t)rest_length);
        if (kind != SEPARATOR_NONE)
            return i + separator_length(kind);
    }
    return rest_length;
}
static int adaptive_radix_tree_has_live_value(kvspace_t *store, int32_t node_id, int depth) {
    if (node_id < 0 || depth > 1024)
        return 0;
    node_id = adaptive_radix_tree_follow(store, node_id);
    if (node_id < 0)
        return -1;
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
    if (!node_header)
        return -1;
    if (ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node_header)))
        return 1;
    uint8_t node_type = __atomic_load_n(&node_header->type, __ATOMIC_ACQUIRE);
    if (!adaptive_radix_tree_node_mapped(store, node_header, node_type))
        return -1;
    const int32_t *children;
    int count = __atomic_load_n(&node_header->count, __ATOMIC_ACQUIRE);
    switch (node_type) {
    case ADAPTIVE_RADIX_TREE_N4:
        if (count > 4)
            return -1;
        children = ((adaptive_radix_tree_node4_t *)node_header)->children;
        break;
    case ADAPTIVE_RADIX_TREE_N16:
        if (count > 16)
            return -1;
        children = ((adaptive_radix_tree_node16_t *)node_header)->children;
        break;
    case ADAPTIVE_RADIX_TREE_N48:
        if (count > 48)
            return -1;
        children = ((adaptive_radix_tree_node48_t *)node_header)->children;
        break;
    case ADAPTIVE_RADIX_TREE_N256:
        children = ((adaptive_radix_tree_node256_t *)node_header)->children;
        count = 256;
        break;
    default:
        return 0;
    }
    for (int i = 0; i < count; i++) {
        int live = adaptive_radix_tree_has_live_value(store, children[i], depth + 1);
        if (live != 0)
            return live;
    }
    return 0;
}
static int adaptive_radix_tree_scan_children(kvspace_t *store, int32_t node_id, char *buffer, int buffer_position, int buffer_capacity,
                             int prefix_length, keyscan_t *scan) {
    if (node_id < 0)
        return 0;
    node_id = adaptive_radix_tree_follow(store, node_id);
    if (node_id < 0)
        return -1;
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
    if (!node_header)
        return -1;
    uint8_t prefix_len = __atomic_load_n(&node_header->prefix_len, __ATOMIC_ACQUIRE);
    if (prefix_len > ADAPTIVE_RADIX_TREE_PREFIX_MAX || prefix_len > buffer_capacity - buffer_position - 1)
        return -1;
    for (int i = 0; i < prefix_len; i++)
        buffer[buffer_position++] = node_header->prefix[i];
    buffer[buffer_position] = 0;
    if (buffer_position > prefix_length) {
        const char *rest = buffer + prefix_length;
        int rest_length = buffer_position - prefix_length;
        int length = child_name_len(rest, rest_length);
        if (length < rest_length) {
            int live = adaptive_radix_tree_has_live_value(store, node_id, buffer_position);
            if (live <= 0)
                return live;
            return keyscan_add_name(scan, rest, (size_t)length);
        }
        if (ending_separator(rest, (size_t)rest_length) != SEPARATOR_NONE) {
            int live = adaptive_radix_tree_has_live_value(store, node_id, buffer_position);
            if (live <= 0)
                return live;
            return keyscan_add_name(scan, rest, (size_t)length);
        }
        if (ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node_header)) &&
            keyscan_add_name(scan, rest, (size_t)length) != 0)
            return -1;
    }
    uint8_t node_type = __atomic_load_n(&node_header->type, __ATOMIC_ACQUIRE);
    if (!adaptive_radix_tree_node_mapped(store, node_header, node_type))
        return -1;
    uint16_t count = __atomic_load_n(&node_header->count, __ATOMIC_ACQUIRE);
    switch (node_type) {
    case ADAPTIVE_RADIX_TREE_N4: {
        if (count > 4)
            return -1;
        adaptive_radix_tree_node4_t *node_data = (adaptive_radix_tree_node4_t *)node_header;
        for (int i = 0; i < (int)count; i++) {
            if (buffer_position + 1 >= buffer_capacity)
                return -1;
            buffer[buffer_position] = node_data->keys[i];
            if (adaptive_radix_tree_scan_children(store, node_data->children[i], buffer, buffer_position + 1, buffer_capacity,
                                  prefix_length, scan) != 0)
                return -1;
        }
        break;
    }
    case ADAPTIVE_RADIX_TREE_N16: {
        if (count > 16)
            return -1;
        adaptive_radix_tree_node16_t *node_data = (adaptive_radix_tree_node16_t *)node_header;
        for (int i = 0; i < (int)count; i++) {
            if (buffer_position + 1 >= buffer_capacity)
                return -1;
            buffer[buffer_position] = node_data->keys[i];
            if (adaptive_radix_tree_scan_children(store, node_data->children[i], buffer, buffer_position + 1, buffer_capacity,
                                  prefix_length, scan) != 0)
                return -1;
        }
        break;
    }
    case ADAPTIVE_RADIX_TREE_N48: {
        adaptive_radix_tree_node48_t *node_data = (adaptive_radix_tree_node48_t *)node_header;
        for (int i = 0; i < 256; i++) {
            uint8_t idx = node_data->index[i];
            if (idx == 255)
                continue;
            if (idx >= 48 || buffer_position + 1 >= buffer_capacity)
                return -1;
            buffer[buffer_position] = (uint8_t)i;
            if (adaptive_radix_tree_scan_children(store, node_data->children[idx], buffer, buffer_position + 1,
                                  buffer_capacity, prefix_length, scan) != 0)
                return -1;
        }
        break;
    }
    case ADAPTIVE_RADIX_TREE_N256: {
        adaptive_radix_tree_node256_t *node_data = (adaptive_radix_tree_node256_t *)node_header;
        for (int i = 0; i < 256; i++) {
            if (node_data->children[i] < 0)
                continue;
            if (buffer_position + 1 >= buffer_capacity)
                return -1;
            buffer[buffer_position] = (uint8_t)i;
            if (adaptive_radix_tree_scan_children(store, node_data->children[i], buffer, buffer_position + 1, buffer_capacity,
                                  prefix_length, scan) != 0)
                return -1;
        }
        break;
    }
    }
    return 0;
}
static int adaptive_radix_tree_scan_prefix(kvspace_t *store, const char *prefix, int prefix_length, char *buffer,
                        int buffer_capacity, keyscan_t *scan) {
    if (prefix_length >= buffer_capacity)
        return -1;
    int32_t root = __atomic_load_n(&store->header->adaptive_radix_tree_root, __ATOMIC_ACQUIRE);
    int buffer_position = 0;
    int32_t node_id = adaptive_radix_tree_cover(store, root, (const uint8_t *)prefix,
                            prefix_length, buffer, buffer_capacity, &buffer_position);
    if (node_id < 0)
        return 0;
    return adaptive_radix_tree_scan_children(store, node_id, buffer, buffer_position, buffer_capacity, prefix_length, scan);
}
static int adaptive_radix_tree_collect_keys(kvspace_t *store, int32_t node_id, char *buffer, int buffer_position,
                            int buffer_capacity, keyscan_t *scan) {
    if (node_id < 0)
        return 0;
    node_id = adaptive_radix_tree_follow(store, node_id);
    if (node_id < 0)
        return -1;
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
    if (!node_header)
        return -1;
    uint8_t prefix_len = __atomic_load_n(&node_header->prefix_len, __ATOMIC_ACQUIRE);
    if (prefix_len > ADAPTIVE_RADIX_TREE_PREFIX_MAX || prefix_len > buffer_capacity - buffer_position - 1)
        return -1;
    for (int i = 0; i < prefix_len; i++)
        buffer[buffer_position++] = node_header->prefix[i];
    if (ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node_header)) && keyscan_add_name(scan, buffer, (size_t)buffer_position) != 0)
        return -1;
    uint8_t node_type = __atomic_load_n(&node_header->type, __ATOMIC_ACQUIRE);
    if (!adaptive_radix_tree_node_mapped(store, node_header, node_type))
        return -1;
    const uint8_t *keys = NULL;
    const int32_t *children = NULL;
    int count = __atomic_load_n(&node_header->count, __ATOMIC_ACQUIRE);
    switch (node_type) {
    case ADAPTIVE_RADIX_TREE_N4:
        if (count > 4)
            return -1;
        keys = ((adaptive_radix_tree_node4_t *)node_header)->keys;
        children = ((adaptive_radix_tree_node4_t *)node_header)->children;
        break;
    case ADAPTIVE_RADIX_TREE_N16:
        if (count > 16)
            return -1;
        keys = ((adaptive_radix_tree_node16_t *)node_header)->keys;
        children = ((adaptive_radix_tree_node16_t *)node_header)->children;
        break;
    case ADAPTIVE_RADIX_TREE_N48: {
        adaptive_radix_tree_node48_t *node_data = (adaptive_radix_tree_node48_t *)node_header;
        for (int i = 0; i < 256; i++) {
            uint8_t idx = node_data->index[i];
            if (idx == 255)
                continue;
            if (idx >= 48 || buffer_position + 1 >= buffer_capacity)
                return -1;
            buffer[buffer_position] = (char)i;
            if (adaptive_radix_tree_collect_keys(store, node_data->children[idx], buffer, buffer_position + 1,
                                 buffer_capacity, scan) != 0)
                return -1;
        }
        return 0;
    }
    case ADAPTIVE_RADIX_TREE_N256: {
        adaptive_radix_tree_node256_t *node_data = (adaptive_radix_tree_node256_t *)node_header;
        for (int i = 0; i < 256; i++) {
            if (node_data->children[i] < 0)
                continue;
            if (buffer_position + 1 >= buffer_capacity)
                return -1;
            buffer[buffer_position] = (char)i;
            if (adaptive_radix_tree_collect_keys(store, node_data->children[i], buffer, buffer_position + 1, buffer_capacity,
                                 scan) != 0)
                return -1;
        }
        return 0;
    }
    default:
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (buffer_position + 1 >= buffer_capacity)
            return -1;
        buffer[buffer_position] = (char)keys[i];
        if (adaptive_radix_tree_collect_keys(store, children[i], buffer, buffer_position + 1, buffer_capacity, scan) != 0)
            return -1;
    }
    return 0;
}
int adaptive_radix_tree_scan_keys(kvspace_t *store, const char *prefix, int prefix_length,
                         keyscan_t *scan) {
    char buffer[4096];
    if (prefix_length >= (int)sizeof buffer)
        return -1;
    int32_t root = __atomic_load_n(&store->header->adaptive_radix_tree_root, __ATOMIC_ACQUIRE);
    int buffer_position = 0;
    int32_t node_id = adaptive_radix_tree_cover(store, root, (const uint8_t *)prefix, prefix_length,
                            buffer, (int)sizeof buffer, &buffer_position);
    if (node_id < 0)
        return 0;
    return adaptive_radix_tree_collect_keys(store, node_id, buffer, buffer_position, (int)sizeof buffer, scan);
}
static int storage_locate(kvspace_t *store, const char *key, int32_t *out_id,
                          uint64_t *out_offset) {
    int32_t root = __atomic_load_n(&store->header->adaptive_radix_tree_root, __ATOMIC_ACQUIRE);
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_search(store, root, (const uint8_t *)key,
                              (int)strlen(key));
    if (!node_header)
        return KVSPACE_MISSING;
    uint64_t packed_value = adaptive_radix_tree_value_load(node_header);
    if (!ADAPTIVE_RADIX_TREE_VALUE_HAS(packed_value))
        return KVSPACE_MISSING;
    *out_id = adaptive_radix_tree_id_of(store, node_header);
    *out_offset = ADAPTIVE_RADIX_TREE_VALUE_OFFSET(packed_value);
    return KVSPACE_OK;
}
static int anchor_offset_valid(kvspace_t *store, uint64_t offset,
                               int32_t *out_id) {
    blocks_meta_t *meta = store->adaptive_radix_tree_meta;
    if (offset < (uint64_t)meta->sizeof_block_head || offset >= meta->total_size)
        return 0;
    int64_t id = kvspace_block_id_by_data_offset(meta, offset);
    uint64_t used = __atomic_load_n(&meta->used_blocks, __ATOMIC_ACQUIRE);
    if (id < 0 || (uint64_t)id >= used)
        return 0;
    *out_id = (int32_t)id;
    return 1;
}
static int adaptive_radix_tree_resume(kvspace_t *store, int32_t node_id, const char *suffix,
                      int suffix_length, uint64_t *out_offset) {
    *out_offset = 0;
    int32_t id = adaptive_radix_tree_follow(store, node_id);
    if (id < 0)
        return KVSPACE_ANCHOR_INVALID;
    adaptive_radix_tree_header_t *node = adaptive_radix_tree_header(store, id);
    if (!node)
        return KVSPACE_ANCHOR_INVALID;
    uint8_t node_type = __atomic_load_n(&node->type, __ATOMIC_ACQUIRE);
    uint8_t prefix_len = __atomic_load_n(&node->prefix_len, __ATOMIC_ACQUIRE);
    if (node_type > ADAPTIVE_RADIX_TREE_N256 || prefix_len > ADAPTIVE_RADIX_TREE_PREFIX_MAX)
        return KVSPACE_ANCHOR_INVALID;
    uint64_t value = adaptive_radix_tree_value_load(node);
    if (suffix_length == 0)
        return ADAPTIVE_RADIX_TREE_VALUE_HAS(value) ? (*out_offset = ADAPTIVE_RADIX_TREE_VALUE_OFFSET(value), KVSPACE_OK)
                             : KVSPACE_MISSING;
    int32_t child = adaptive_radix_tree_child(store, node, (uint8_t)suffix[0]);
    if (child < 0)
        return KVSPACE_MISSING;
    int32_t found = adaptive_radix_tree_walk(store, child, (const uint8_t *)suffix, suffix_length, 1,
                             NULL, NULL);
    if (found < 0)
        return KVSPACE_MISSING;
    adaptive_radix_tree_header_t *found_header = adaptive_radix_tree_header(store, found);
    if (!found_header)
        return KVSPACE_MISSING;
    value = adaptive_radix_tree_value_load(found_header);
    if (!ADAPTIVE_RADIX_TREE_VALUE_HAS(value))
        return KVSPACE_MISSING;
    *out_offset = ADAPTIVE_RADIX_TREE_VALUE_OFFSET(value);
    return KVSPACE_OK;
}
int resolve_anchor(kvspace_t *store, const char *key,
                          const kvspace_anchor_t *anchor,
                          uint64_t *out_offset) {
    *out_offset = 0;
    if (!anchor || anchor->node_offset == 0)
        return resolve_root(store, key, out_offset);
    uint64_t suffix_length = (uint64_t)strlen(key);
    if (reserved_meta_relative(anchor->depth, key) ||
        separator_at(key, 0, (size_t)suffix_length) != SEPARATOR_NONE ||
        !separator_sequence_valid(key, (size_t)suffix_length) ||
        (uint64_t)anchor->depth + suffix_length >= 1024)
        return KVSPACE_INVALID;
    if (!(anchor->flags & KVSPACE_ANCHOR_FLAG_INDEX))
        return KVSPACE_ANCHOR_INVALID;
    for (int attempt = 0; attempt < 64; attempt++) {
        uint64_t before = structure_seq_read_begin(store);
        if (store_sync(store) != 0)
            return KVSPACE_BACKEND;
        int32_t node_id = -1;
        if (!anchor_offset_valid(store, anchor->node_offset, &node_id)) {
            if (!structure_seq_read_retry(store, before))
                return KVSPACE_ANCHOR_INVALID;
            kvspace_cpu_relax();
            continue;
        }
        int status = adaptive_radix_tree_resume(store, node_id, key, (int)suffix_length, out_offset);
        if (!structure_seq_read_retry(store, before)) {
            if (status == KVSPACE_OK &&
                *out_offset >= __atomic_load_n(&store->region_data.mapped, __ATOMIC_ACQUIRE) &&
                store_sync(store) != 0)
                return KVSPACE_BACKEND;
            return status;
        }
        *out_offset = 0;
        kvspace_cpu_relax();
    }
    *out_offset = 0;
    return KVSPACE_ANCHOR_INVALID;
}
int resolve_root_locked(kvspace_t *store, const char *key,
                        uint64_t *out_offset) {
    int32_t id = -1;
    *out_offset = 0;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    if (store_sync(store) != 0)
        return KVSPACE_BACKEND;
    return storage_locate(store, key, &id, out_offset);
}
int resolve_root(kvspace_t *store, const char *key,
                              uint64_t *out_offset) {
    *out_offset = 0;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    for (;;) {
        uint64_t before = structure_seq_read_begin(store);
        if (store_sync(store) != 0)
            return KVSPACE_BACKEND;
        int32_t id = -1;
        uint64_t candidate = 0;
        int status = storage_locate(store, key, &id, &candidate);
        if (!structure_seq_read_retry(store, before)) {
            if (status == KVSPACE_OK) {
                if (candidate >= __atomic_load_n(&store->region_data.mapped, __ATOMIC_ACQUIRE) &&
                    store_sync(store) != 0)
                    return KVSPACE_BACKEND;
                *out_offset = candidate;
            }
            return status;
        }
        kvspace_cpu_relax();
    }
}
int anchor_of_key(kvspace_t *store, const char *directory_key,
                  kvspace_anchor_t *out_anchor) {
    size_t key_len = strlen(directory_key);
    if (!key_text_valid(directory_key))
        return KVSPACE_INVALID;
    if (!storage_is_index_key(directory_key))
        return KVSPACE_NOT_INDEX;
    for (;;) {
        uint64_t before = structure_seq_read_begin(store);
        if (store_sync(store) != 0)
            return KVSPACE_BACKEND;
        int32_t root = __atomic_load_n(&store->header->adaptive_radix_tree_root, __ATOMIC_ACQUIRE);
        adaptive_radix_tree_header_t *node = adaptive_radix_tree_search(store, root,
                                     (const uint8_t *)directory_key,
                                     (int)key_len);
        if (!node || !ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node))) {
            if (!structure_seq_read_retry(store, before))
                return KVSPACE_MISSING;
            kvspace_cpu_relax();
            continue;
        }
        int32_t id = adaptive_radix_tree_id_of(store, node);
        uint64_t offset = id >= 0 ? (uint64_t)kvspace_block_data_offset(store->adaptive_radix_tree_meta, (uint64_t)id) : 0;
        if (!structure_seq_read_retry(store, before)) {
            if (id < 0 || offset == 0)
                return KVSPACE_BACKEND;
            out_anchor->node_offset = offset;
            out_anchor->epoch = before;
            out_anchor->depth = (uint32_t)key_len;
            out_anchor->flags = KVSPACE_ANCHOR_FLAG_INDEX;
            return KVSPACE_OK;
        }
        kvspace_cpu_relax();
    }
}
int storage_is_index_key(const char *key) {
    size_t length = strlen(key);
    return length > 0 && ending_separator(key, length) != SEPARATOR_NONE;
}
int storage_has_children(kvspace_t *store, const char *key) {
    keyscan_t children = {0};
    char buffer[4096];
    int return_code = 0;
    if (__atomic_load_n(&store->header->adaptive_radix_tree_root, __ATOMIC_ACQUIRE) < 0)
        return KVSPACE_OK;
    if (adaptive_radix_tree_scan_prefix(store, key, (int)strlen(key), buffer, (int)sizeof buffer,
                     &children) != 0) {
        keyscan_free(&children);
        return KVSPACE_BACKEND;
    }
    if (children.count > 0)
        return_code = KVSPACE_HAS_CHILDREN;
    keyscan_free(&children);
    return return_code;
}
int subtree_key(const char *base, size_t base_length, const char *key) {
    if (base_length == 0)
        return separator_at(key, 0, strlen(key)) == SEPARATOR_SLASH;
    if (strncmp(key, base, base_length) != 0)
        return 0;
    const char *rest = key + base_length;
    return rest[0] == 0 ||
           separator_at(rest, 0, strlen(rest)) != SEPARATOR_NONE;
}
static int storage_key_exists(kvspace_t *store, const char *key) {
    int32_t root = __atomic_load_n(&store->header->adaptive_radix_tree_root, __ATOMIC_ACQUIRE);
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_search(store, root, (const uint8_t *)key,
                              (int)strlen(key));
    if (node_header && ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node_header)))
        return 1;
    if (storage_is_index_key(key)) {
        keyscan_t children = {0};
        char buffer[4096];
        int return_code = 0;
        if (root >= 0)
            return_code = adaptive_radix_tree_scan_prefix(store, key, (int)strlen(key), buffer, (int)sizeof buffer,
                              &children);
        int has = return_code == 0 && children.count > 0;
        keyscan_free(&children);
        return has;
    }
    size_t length = strlen(key);
    if (length == 0)
        return 0;
    for (int kind = SEPARATOR_SLASH; kind < SEPARATOR_COUNT; kind++) {
        int sep_length = separator_length((enum separator_kind)kind);
        if (length + (size_t)sep_length >= 1024)
            continue;
        char *directory = append_separator(key, (enum separator_kind)kind);
        if (!directory)
            return 0;
        node_header = adaptive_radix_tree_search(store, root, (const uint8_t *)directory, (int)strlen(directory));
        int exists = node_header && ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node_header));
        free(directory);
        if (exists)
            return 1;
    }
    return 0;
}
static int storage_index_state(kvspace_t *store, const char *key) {
    if (!storage_is_index_key(key))
        return storage_key_exists(store, key) ? KVSPACE_NOT_INDEX : KVSPACE_MISSING;
    if (!storage_key_exists(store, key))
        return KVSPACE_MISSING;
    return KVSPACE_OK;
}
int storage_children(kvspace_t *store, const char *key, keyscan_t *out) {
    char buffer[4096];
    if (__atomic_load_n(&store->header->adaptive_radix_tree_root, __ATOMIC_ACQUIRE) >= 0 &&
        adaptive_radix_tree_scan_prefix(store, key, (int)strlen(key), buffer, (int)sizeof buffer, out) != 0) {
        keyscan_free(out);
        return KVSPACE_BACKEND;
    }
    if (out->count == 0 && !storage_key_exists(store, key))
        return KVSPACE_MISSING;
    return KVSPACE_OK;
}
static int storage_list_prefix(const char *key, char *out, size_t capacity) {
    size_t length = strlen(key);
    if (length == 0 || length >= capacity)
        return KVSPACE_INVALID;
    memcpy(out, key, length + 1);
    return KVSPACE_OK;
}
int listlen_key(kvspace_t *store, const char *key, int64_t *out_count,
                             uint32_t *out_max_name_length) {
    *out_count = 0;
    *out_max_name_length = 0;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    char prefix[1024];
    int return_code = storage_list_prefix(key, prefix, sizeof prefix);
    if (return_code != KVSPACE_OK)
        return return_code;
    for (;;) {
        uint64_t before = structure_seq_read_begin(store);
        if (store_sync(store) != 0)
            return KVSPACE_BACKEND;
        return_code = storage_index_state(store, key);
        if (return_code != KVSPACE_OK) {
            if (!structure_seq_read_retry(store, before))
                return return_code;
            kvspace_cpu_relax();
            continue;
        }
        keyscan_t children = {0};
        return_code = storage_children(store, prefix, &children);
        if (structure_seq_read_retry(store, before)) {
            keyscan_free(&children);
            kvspace_cpu_relax();
            continue;
        }
        if (return_code != KVSPACE_OK)
            return return_code;
        uint32_t max_length = 0;
        for (int32_t i = 0; i < children.count; i++) {
            size_t length = strlen(children.keys[i]);
            if (length > max_length)
                max_length = (uint32_t)length;
        }
        *out_count = children.count;
        *out_max_name_length = max_length;
        keyscan_free(&children);
        return KVSPACE_OK;
    }
}
int list_names_raw(kvspace_t *store, const char *key, char *out_names,
                          uint32_t out_capacity, uint32_t *out_length,
                          uint32_t *out_count, uint64_t *out_generation) {
    if (out_length)
        *out_length = 0;
    if (out_count)
        *out_count = 0;
    if (out_generation)
        *out_generation = 0;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    char prefix[1024];
    int return_code = storage_list_prefix(key, prefix, sizeof prefix);
    if (return_code != KVSPACE_OK)
        return return_code;
    for (;;) {
        uint64_t before = structure_seq_read_begin(store);
        if (store_sync(store) != 0)
            return KVSPACE_BACKEND;
        uint64_t generation = __atomic_load_n(&store->header->list_generation, __ATOMIC_ACQUIRE);
        return_code = storage_index_state(store, key);
        if (return_code != KVSPACE_OK) {
            if (!structure_seq_read_retry(store, before))
                return return_code;
            kvspace_cpu_relax();
            continue;
        }
        keyscan_t children = {0};
        return_code = storage_children(store, prefix, &children);
        if (structure_seq_read_retry(store, before)) {
            keyscan_free(&children);
            kvspace_cpu_relax();
            continue;
        }
        if (return_code != KVSPACE_OK)
            return return_code;
        uint64_t total = 0;
        for (int32_t i = 0; i < children.count; i++)
            total += (uint64_t)strlen(children.keys[i]) + 1;
        if (out_generation)
            *out_generation = generation;
        if (out_count)
            *out_count = (uint32_t)children.count;
        if (out_length)
            *out_length = (uint32_t)total;
        if (out_names != NULL) {
            if (total > out_capacity) {
                keyscan_free(&children);
                return KVSPACE_CAPACITY;
            }
            char *pointer = out_names;
            for (int32_t i = 0; i < children.count; i++) {
                size_t length = strlen(children.keys[i]) + 1;
                memcpy(pointer, children.keys[i], length);
                pointer += length;
            }
        }
        keyscan_free(&children);
        return KVSPACE_OK;
    }
}

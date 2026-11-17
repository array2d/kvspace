#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kvspace_key.h"
#include "kvspace_xvalue_internal.h"
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
static int32_t adaptive_radix_tree_child(kvspace_t *store, void *node, uint8_t byte);
static int32_t adaptive_radix_tree_walk(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
                                        int depth, int32_t *out_parent, int *out_parent_depth);
static int32_t adaptive_radix_tree_follow(kvspace_t *store, int32_t id);
const separator_entry_t separator_table[SEPARATOR_COUNT] = {
    {"/", 1}, {"\xC2\xB7", 2}
};
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
int key_text_valid(const char *key) {
    if (!key)
        return 0;
    size_t length = strlen(key);
    if (length == 0 || length >= 1024)
        return 0;
    if (reserved_meta_path(key) || !separator_sequence_valid(key, length))
        return 0;
    enum separator_kind first = SEPARATOR_NONE;
    for (size_t index = 0; index < length && first == SEPARATOR_NONE; index++)
        first = separator_at(key, index, length);
    return first == SEPARATOR_SLASH;
}
static int adaptive_radix_tree_node_size(int node_type) {
    return node_type == ADAPTIVE_RADIX_TREE_N4 ? sizeof(adaptive_radix_tree_node4_t)
           : node_type == ADAPTIVE_RADIX_TREE_N16 ? sizeof(adaptive_radix_tree_node16_t)
           : node_type == ADAPTIVE_RADIX_TREE_N48 ? sizeof(adaptive_radix_tree_node48_t)
                          : sizeof(adaptive_radix_tree_node256_t);
}
static uint64_t generation_capacity(const kvspace_t *store) {
    blocks_meta_t *meta = store->adaptive_radix_tree_meta;
    uint64_t head = (uint64_t)meta->sizeof_block_head;
    return meta->total_size / (head + meta->block_size);
}
int32_t adaptive_radix_tree_id_of(kvspace_t *store, const void *pointer) {
    if (!pointer || !store->adaptive_radix_tree_data)
        return -1;
    return (int32_t)kvspace_block_id_by_data_offset(store->adaptive_radix_tree_meta,
                                         (uint64_t)((const uint8_t *)pointer - store->adaptive_radix_tree_data));
}
generations_t *generations_at(kvspace_t *store, int32_t id) {
    if (id < 0 || !store->region_generations.base)
        return NULL;
    if ((uint64_t)id >= store->generation_count)
        return NULL;
    return (generations_t *)store->region_generations.base + id;
}
void generation_bump_struct(kvspace_t *store, int32_t id) {
    generations_t *generations = generations_at(store, id);
    if (generations)
        __atomic_add_fetch(&generations->struct_generation, 1u, __ATOMIC_ACQ_REL);
}
void generation_bump_value(kvspace_t *store, int32_t id) {
    generations_t *generations = generations_at(store, id);
    if (generations)
        __atomic_add_fetch(&generations->value_generation, 2u, __ATOMIC_ACQ_REL);
}
static int64_t generation_alloc_epoch(kvspace_t *store) {
    return (int64_t)__atomic_add_fetch(&store->header->generation_epoch, 1ull, __ATOMIC_ACQ_REL);
}
int32_t recovery_node_at(kvspace_t *store, const char *key) {
    int key_length = (int)strlen(key);
    int depth = 0;
    int32_t node_id = store->header->adaptive_radix_tree_root;
    while (node_id >= 0) {
        adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
        if (!node_header || node_header->type == ADAPTIVE_RADIX_TREE_MOVED)
            return -1;
        if (node_header->prefix_len) {
            int shared = shared_prefix_length(node_header->prefix, node_header->prefix_len,
                               (const uint8_t *)key + depth, key_length - depth);
            if (shared != node_header->prefix_len && depth + shared != key_length)
                return -1;
            depth += shared;
        }
        if (depth == key_length)
            return node_id;
        node_id = adaptive_radix_tree_child(store, node_header, (uint8_t)key[depth]);
        depth++;
    }
    return -1;
}
#define GENERATIONS_RESERVE (8ULL << 30)
int generations_grow(kvspace_t *store) {
    uint64_t want = generation_capacity(store) * sizeof(generations_t);
    if (want > GENERATIONS_RESERVE)
        return -1;
    store->generation_count = want / sizeof(generations_t);
    store->adaptive_radix_tree_total_seen = store->adaptive_radix_tree_meta->total_size;
    if (want <= store->region_generations.mapped)
        return 0;
    if (!store->region_generations.base) {
        void *mapping = mmap(NULL, GENERATIONS_RESERVE, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (mapping == MAP_FAILED)
            return -1;
        store->region_generations.base = mapping;
        store->region_generations.reserve = GENERATIONS_RESERVE;
    }
    if (ftruncate(store->region_generations.file_descriptor, (off_t)want) != 0)
        return -1;
    if (region_map(&store->region_generations, (size_t)want) != 0)
        return -1;
    return 0;
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
        uint64_t offset = kvspace_block_data_offset(meta, id);
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
    store->header->adaptive_radix_tree_slab_size = want;
    store->adaptive_radix_tree_meta->total_size = want;
    return 0;
}
static void *adaptive_radix_tree_block(kvspace_t *store, int32_t id) {
    if (id < 0)
        return NULL;
    return store->adaptive_radix_tree_data + kvspace_block_data_offset(store->adaptive_radix_tree_meta, (uint64_t)id);
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
    meta->used_blocks += block_count;
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
        if (adaptive_radix_tree_slab_grow(store) != 0 || generations_grow(store) != 0)
            return -1;
        id = adaptive_radix_tree_bump(store, block_count);
    }
    if (id >= 0) {
        generations_t *generations = generations_at(store, (int32_t)id);
        if (generations) {
            __atomic_store_n(&generations->struct_generation,
                             (uint32_t)generation_alloc_epoch(store), __ATOMIC_RELEASE);
            __atomic_store_n(&generations->value_generation, 0u, __ATOMIC_RELEASE);
        }
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
static int32_t adaptive_radix_tree_child(kvspace_t *store, void *node, uint8_t byte) {
    adaptive_radix_tree_header_t *node_header = (adaptive_radix_tree_header_t *)node;
    switch (node_header->type) {
    case ADAPTIVE_RADIX_TREE_N4: {
        adaptive_radix_tree_node4_t *node_data = node;
        for (int i = 0; i < (int)node_header->count; i++)
            if (node_data->keys[i] == byte)
                return node_data->children[i];
        return -1;
    }
    case ADAPTIVE_RADIX_TREE_N16: {
        adaptive_radix_tree_node16_t *node_data = node;
        int low = 0, high = (int)node_header->count - 1;
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
        adaptive_radix_tree_node48_t *node_data = node;
        uint8_t index = node_data->index[byte];
        return index == 255 ? -1 : node_data->children[index];
    }
    case ADAPTIVE_RADIX_TREE_N256: {
        return ((adaptive_radix_tree_node256_t *)node)->children[byte];
    }
    }
    return -1;
}
static int32_t adaptive_radix_tree_follow(kvspace_t *store, int32_t id);
static int32_t adaptive_radix_tree_walk(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
                        int depth, int32_t *out_parent, int *out_parent_depth) {
    while (node_id >= 0) {
        adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
        if (!node_header || node_header->type == ADAPTIVE_RADIX_TREE_MOVED)
            return -1;
        if (node_header->prefix_len) {
            if (shared_prefix_length(node_header->prefix, node_header->prefix_len, key + depth, key_length - depth) !=
                node_header->prefix_len)
                return -1;
            depth += node_header->prefix_len;
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
static int32_t adaptive_radix_tree_follow(kvspace_t *store, int32_t id) {
    for (int i = 0; i < 8 && id >= 0; i++) {
        adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, id);
        if (!node_header)
            return -1;
        if (node_header->type != ADAPTIVE_RADIX_TREE_MOVED)
            return id;
        id = (int32_t)ADAPTIVE_RADIX_TREE_VALUE_OFFSET(adaptive_radix_tree_value_load(node_header));
    }
    return -1;
}
static int32_t adaptive_radix_tree_new_leaf(kvspace_t *store, uint64_t offset) {
    int32_t id = adaptive_radix_tree_block_alloc(store, ADAPTIVE_RADIX_TREE_N4);
    if (id < 0)
        return -1;
    adaptive_radix_tree_node4_t *node_data = adaptive_radix_tree_block(store, id);
    memset(node_data, 0, sizeof(*node_data));
    node_data->header.type = ADAPTIVE_RADIX_TREE_N4;
    adaptive_radix_tree_value_store(&node_data->header, ADAPTIVE_RADIX_TREE_VALUE_MAKE(1, offset));
    return id;
}
static int32_t adaptive_radix_tree_new_node(kvspace_t *store, int node_type) {
    int32_t id = adaptive_radix_tree_block_alloc(store, node_type);
    if (id < 0)
        return -1;
    void *node_data = adaptive_radix_tree_block(store, id);
    int size = adaptive_radix_tree_node_size(node_type);
    memset(node_data, 0, size);
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
    adaptive_radix_tree_header_t *new_header = adaptive_radix_tree_header(store, node_id);
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
    {
        int32_t old_id = adaptive_radix_tree_id_of(store, old_header);
        generation_bump_struct(store, node_id);
        generation_bump_struct(store, old_id);
    }
    old_header->type = ADAPTIVE_RADIX_TREE_MOVED;
    adaptive_radix_tree_value_store(old_header, ADAPTIVE_RADIX_TREE_VALUE_MAKE(0, (uint32_t)node_id));
    return node_id;
}
static int adaptive_radix_tree_add(kvspace_t *store, void *node, uint8_t byte, int32_t child_id) {
    generation_bump_struct(store, adaptive_radix_tree_id_of(store, node));
    adaptive_radix_tree_header_t *node_header = node;
    switch (node_header->type) {
    case ADAPTIVE_RADIX_TREE_N4: {
        if (node_header->count >= 4)
            return -1;
        adaptive_radix_tree_node4_t *node_data = node;
        node_data->keys[node_header->count] = byte;
        node_data->children[node_header->count] = child_id;
        node_header->count++;
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
        node_header->count++;
        return 0;
    }
    case ADAPTIVE_RADIX_TREE_N48: {
        if (node_header->count >= 48)
            return -1;
        adaptive_radix_tree_node48_t *node_data = node;
        int slot = (int)node_header->count;
        node_data->index[byte] = (uint8_t)slot;
        node_data->children[slot] = child_id;
        node_header->count++;
        return 0;
    }
    case ADAPTIVE_RADIX_TREE_N256: {
        adaptive_radix_tree_node256_t *node_data = node;
        if (node_data->children[byte] >= 0)
            return -1;
        node_data->children[byte] = child_id;
        node_header->count++;
        return 0;
    }
    }
    return -1;
}
static void adaptive_radix_tree_remove(kvspace_t *store, void *node, uint8_t byte) {
    adaptive_radix_tree_header_t *node_header = node;
    switch (node_header->type) {
    case ADAPTIVE_RADIX_TREE_N4: {
        adaptive_radix_tree_node4_t *node_data = node;
        for (int i = 0; i < (int)node_header->count; i++)
            if (node_data->keys[i] == byte) {
                for (int j = i; j < (int)node_header->count - 1; j++) {
                    node_data->keys[j] = node_data->keys[j + 1];
                    node_data->children[j] = node_data->children[j + 1];
                }
                node_header->count--;
                return;
            }
        break;
    }
    case ADAPTIVE_RADIX_TREE_N16: {
        adaptive_radix_tree_node16_t *node_data = node;
        int low = 0, high = (int)node_header->count - 1, position = -1;
        while (low <= high) {
            int middle = (low + high) / 2;
            if (node_data->keys[middle] == byte) {
                position = middle;
                break;
            }
            if (node_data->keys[middle] < byte)
                low = middle + 1;
            else
                high = middle - 1;
        }
        if (position < 0)
            return;
        for (int j = position; j < (int)node_header->count - 1; j++) {
            node_data->keys[j] = node_data->keys[j + 1];
            node_data->children[j] = node_data->children[j + 1];
        }
        node_header->count--;
        break;
    }
    case ADAPTIVE_RADIX_TREE_N48: {
        adaptive_radix_tree_node48_t *node_data = node;
        uint8_t index = node_data->index[byte];
        if (index == 255)
            return;
        node_data->index[byte] = 255;
        int last = (int)node_header->count - 1;
        if (index != last) {
            node_data->children[index] = node_data->children[last];
            for (int j = 0; j < 256; j++)
                if (node_data->index[j] == last) {
                    node_data->index[j] = index;
                    break;
                }
        }
        node_header->count--;
        break;
    }
    case ADAPTIVE_RADIX_TREE_N256: {
        adaptive_radix_tree_node256_t *node_data = node;
        node_data->children[byte] = -1;
        node_header->count--;
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
        if (remaining > 0)
            memcpy(node_data->header.prefix, key + depth, (size_t)remaining);
        node_data->header.prefix_len = (uint8_t)remaining;
        return id;
    }
    int32_t id = adaptive_radix_tree_new_node(store, ADAPTIVE_RADIX_TREE_N4);
    if (id < 0)
        return -1;
    adaptive_radix_tree_header_t *node_data = adaptive_radix_tree_header(store, id);
    memcpy(node_data->prefix, key + depth, ADAPTIVE_RADIX_TREE_PREFIX_MAX);
    node_data->prefix_len = ADAPTIVE_RADIX_TREE_PREFIX_MAX;
    int32_t subtree_node = adaptive_radix_tree_leaf_chain(store, key, key_length, depth + ADAPTIVE_RADIX_TREE_PREFIX_MAX + 1, offset);
    if (subtree_node < 0)
        return -1;
    adaptive_radix_tree_add(store, node_data, key[depth + ADAPTIVE_RADIX_TREE_PREFIX_MAX], subtree_node);
    return id;
}
int32_t adaptive_radix_tree_insert(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
                       int depth, uint64_t offset) {
    if (node_id < 0)
        return adaptive_radix_tree_leaf_chain(store, key, key_length, depth, offset);
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
    if (!node_header)
        return -1;
    int shared = shared_prefix_length(node_header->prefix, node_header->prefix_len, key + depth, key_length - depth);
    int minimum_prefix_length = node_header->prefix_len < (key_length - depth) ? node_header->prefix_len : (key_length - depth);
    if (shared == minimum_prefix_length && (key_length - depth) < node_header->prefix_len) {
        int32_t new_node = adaptive_radix_tree_new_node(store, ADAPTIVE_RADIX_TREE_N4);
        if (new_node < 0)
            return -1;
        adaptive_radix_tree_header_t *new_header = adaptive_radix_tree_header(store, new_node);
        new_header->prefix_len = (uint8_t)shared;
        if (shared > 0)
            memcpy(new_header->prefix, key + depth, (size_t)shared);
        uint8_t old_byte = node_header->prefix[shared];
        node_header->prefix_len -= (uint8_t)(shared + 1);
        if (node_header->prefix_len > 0)
            memmove(node_header->prefix, node_header->prefix + shared + 1, node_header->prefix_len);
        adaptive_radix_tree_add(store, new_header, old_byte, node_id);
        adaptive_radix_tree_value_store(new_header, ADAPTIVE_RADIX_TREE_VALUE_MAKE(1, offset));
        return new_node;
    }
    if (shared < minimum_prefix_length) {
        int32_t new_node = adaptive_radix_tree_new_node(store, ADAPTIVE_RADIX_TREE_N4);
        if (new_node < 0)
            return -1;
        adaptive_radix_tree_header_t *new_header = adaptive_radix_tree_header(store, new_node);
        new_header->prefix_len = (uint8_t)shared;
        memcpy(new_header->prefix, key + depth, shared);
        uint8_t old_byte = node_header->prefix[shared];
        node_header->prefix_len -= (uint8_t)(shared + 1);
        if (node_header->prefix_len > 0)
            memmove(node_header->prefix, node_header->prefix + shared + 1, node_header->prefix_len);
        adaptive_radix_tree_add(store, new_header, old_byte, node_id);
        int32_t leaf = adaptive_radix_tree_leaf_chain(store, key, key_length, depth + shared + 1, offset);
        if (leaf < 0)
            return -1;
        adaptive_radix_tree_add(store, new_header, key[depth + shared], leaf);
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
            adaptive_radix_tree_remove(store, node_header, key[depth]);
            adaptive_radix_tree_add(store, node_header, key[depth], new_child);
        }
        return node_id;
    }
    int32_t leaf = adaptive_radix_tree_leaf_chain(store, key, key_length, depth + 1, offset);
    if (leaf < 0)
        return -1;
    if ((node_header->type == ADAPTIVE_RADIX_TREE_N4 && node_header->count >= 4) ||
        (node_header->type == ADAPTIVE_RADIX_TREE_N16 && node_header->count >= 16) ||
        (node_header->type == ADAPTIVE_RADIX_TREE_N48 && node_header->count >= 48)) {
        int32_t grown = adaptive_radix_tree_grow(store, node_header);
        if (grown < 0)
            return -1;
        adaptive_radix_tree_add(store, adaptive_radix_tree_header(store, grown), key[depth], leaf);
        return grown;
    }
    adaptive_radix_tree_add(store, node_header, key[depth], leaf);
    return node_id;
}
int32_t adaptive_radix_tree_delete(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
                       int depth, bool *deleted) {
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
        uint64_t garbage_index;
        if (garbage_reserve(store, &garbage_index) != 0)
            return -1;
        if (!adaptive_radix_tree_value_compare_and_swap(node_header, &header_value, 0)) {
            garbage_cancel(store, garbage_index);
            return node_id;
        }
        generation_bump_value(store, adaptive_radix_tree_id_of(store, node_header));
        generation_bump_struct(store, adaptive_radix_tree_id_of(store, node_header));
        garbage_commit(store, garbage_index, ADAPTIVE_RADIX_TREE_VALUE_OFFSET(header_value),
                    sbo_allocated_size(store->slots_box_meta, ADAPTIVE_RADIX_TREE_VALUE_OFFSET(header_value)));
        *deleted = true;
    } else {
        int32_t child_id = adaptive_radix_tree_child(store, node_header, key[depth]);
        if (child_id >= 0) {
            int32_t return_code = adaptive_radix_tree_delete(store, child_id, key, key_length, depth + 1, deleted);
            if (return_code < 0)
                return return_code;
        }
    }
    return node_id;
}
char *path_join(const char *left, const char *right) {
    size_t left_length = strlen(left), right_length = strlen(right);
    int separator = (left_length > 0 && ending_separator(left, left_length) != SEPARATOR_SLASH)
                  ? separator_length(SEPARATOR_SLASH) : 0;
    char *result = malloc(left_length + (size_t)separator + right_length + 1);
    memcpy(result, left, left_length);
    if (separator)
        memcpy(result + left_length, separator_table[SEPARATOR_SLASH].bytes, (size_t)separator);
    memcpy(result + left_length + (size_t)separator, right, right_length + 1);
    return result;
}
char *ending_directory(const char *path) {
    size_t length = strlen(path);
    if (ending_separator(path, length) == SEPARATOR_SLASH)
        return strdup(path);
    return append_separator(path, SEPARATOR_SLASH);
}
static int32_t adaptive_radix_tree_cover(kvspace_t *store, int32_t node_id, const uint8_t *prefix,
                         int prefix_length, char *buffer, int buffer_capacity, int *buffer_position) {
    int depth = 0;
    if (node_id < 0 || !prefix || prefix_length < 0 || !buffer || !buffer_position)
        return -1;
    while (node_id >= 0) {
        adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
        if (!node_header)
            return -1;
        int prefix_depth = depth, cover = 0;
        if (node_header->prefix_len) {
            int shared = shared_prefix_length(node_header->prefix, node_header->prefix_len, prefix + depth, prefix_length - depth);
            if (shared < node_header->prefix_len) {
                if (depth + shared < prefix_length)
                    return -1;
                cover = 1;
            } else
                depth += node_header->prefix_len;
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
static int adaptive_radix_tree_has_live_value(kvspace_t *store, int32_t node_id) {
    if (node_id < 0)
        return 0;
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
    if (!node_header)
        return -1;
    if (ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node_header)))
        return 1;
    const int32_t *children;
    int count = node_header->count;
    switch (node_header->type) {
    case ADAPTIVE_RADIX_TREE_N4:
        children = ((adaptive_radix_tree_node4_t *)node_header)->children;
        break;
    case ADAPTIVE_RADIX_TREE_N16:
        children = ((adaptive_radix_tree_node16_t *)node_header)->children;
        break;
    case ADAPTIVE_RADIX_TREE_N48:
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
        int live = adaptive_radix_tree_has_live_value(store, children[i]);
        if (live != 0)
            return live;
    }
    return 0;
}
static int adaptive_radix_tree_scan_children(kvspace_t *store, int32_t node_id, char *buffer, int buffer_position, int buffer_capacity,
                             int prefix_length, keyscan_t *scan) {
    if (node_id < 0)
        return 0;
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
    if (!node_header || node_header->prefix_len > buffer_capacity - buffer_position - 1)
        return -1;
    for (int i = 0; i < node_header->prefix_len; i++)
        buffer[buffer_position++] = node_header->prefix[i];
    buffer[buffer_position] = 0;
    if (buffer_position > prefix_length) {
        const char *rest = buffer + prefix_length;
        int rest_length = buffer_position - prefix_length;
        int length = child_name_len(rest, rest_length);
        if (length < rest_length) {
            int live = adaptive_radix_tree_has_live_value(store, node_id);
            if (live <= 0)
                return live;
            return keyscan_add_name(scan, rest, (size_t)length);
        }
        if (ending_separator(rest, (size_t)rest_length) != SEPARATOR_NONE) {
            int live = adaptive_radix_tree_has_live_value(store, node_id);
            if (live <= 0)
                return live;
            return keyscan_add_name(scan, rest, (size_t)length);
        }
        if (ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node_header)) &&
            keyscan_add_name(scan, rest, (size_t)length) != 0)
            return -1;
    }
    switch (node_header->type) {
    case ADAPTIVE_RADIX_TREE_N4: {
        adaptive_radix_tree_node4_t *node_data = (adaptive_radix_tree_node4_t *)node_header;
        for (int i = 0; i < (int)node_header->count; i++) {
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
        adaptive_radix_tree_node16_t *node_data = (adaptive_radix_tree_node16_t *)node_header;
        for (int i = 0; i < (int)node_header->count; i++) {
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
            if (node_data->index[i] == 255)
                continue;
            if (buffer_position + 1 >= buffer_capacity)
                return -1;
            buffer[buffer_position] = (uint8_t)i;
            if (adaptive_radix_tree_scan_children(store, node_data->children[node_data->index[i]], buffer, buffer_position + 1,
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
    int buffer_position = 0;
    int32_t node_id = adaptive_radix_tree_cover(store, store->header->adaptive_radix_tree_root, (const uint8_t *)prefix,
                            prefix_length, buffer, buffer_capacity, &buffer_position);
    if (node_id < 0)
        return 0;
    return adaptive_radix_tree_scan_children(store, node_id, buffer, buffer_position, buffer_capacity, prefix_length, scan);
}
static int adaptive_radix_tree_collect_keys(kvspace_t *store, int32_t node_id, char *buffer, int buffer_position,
                            int buffer_capacity, keyscan_t *scan) {
    if (node_id < 0)
        return 0;
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_header(store, node_id);
    if (!node_header || node_header->prefix_len > buffer_capacity - buffer_position - 1)
        return -1;
    for (int i = 0; i < node_header->prefix_len; i++)
        buffer[buffer_position++] = node_header->prefix[i];
    if (ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node_header)) && keyscan_add_name(scan, buffer, (size_t)buffer_position) != 0)
        return -1;
    const uint8_t *keys = NULL;
    const int32_t *children = NULL;
    int count = 0;
    switch (node_header->type) {
    case ADAPTIVE_RADIX_TREE_N4:
        keys = ((adaptive_radix_tree_node4_t *)node_header)->keys;
        children = ((adaptive_radix_tree_node4_t *)node_header)->children;
        count = node_header->count;
        break;
    case ADAPTIVE_RADIX_TREE_N16:
        keys = ((adaptive_radix_tree_node16_t *)node_header)->keys;
        children = ((adaptive_radix_tree_node16_t *)node_header)->children;
        count = node_header->count;
        break;
    case ADAPTIVE_RADIX_TREE_N48: {
        adaptive_radix_tree_node48_t *node_data = (adaptive_radix_tree_node48_t *)node_header;
        for (int i = 0; i < 256; i++) {
            if (node_data->index[i] == 255)
                continue;
            if (buffer_position + 1 >= buffer_capacity)
                return -1;
            buffer[buffer_position] = (char)i;
            if (adaptive_radix_tree_collect_keys(store, node_data->children[node_data->index[i]], buffer, buffer_position + 1,
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
    int buffer_position = 0;
    int32_t node_id = adaptive_radix_tree_cover(store, store->header->adaptive_radix_tree_root, (const uint8_t *)prefix, prefix_length,
                            buffer, (int)sizeof buffer, &buffer_position);
    if (node_id < 0)
        return 0;
    return adaptive_radix_tree_collect_keys(store, node_id, buffer, buffer_position, (int)sizeof buffer, scan);
}
static int storage_locate(kvspace_t *store, const char *key, int32_t *out_id,
                          uint64_t *out_offset) {
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_search(store, store->header->adaptive_radix_tree_root, (const uint8_t *)key,
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
    if (id < 0 || (uint64_t)id >= meta->used_blocks)
        return 0;
    *out_id = (int32_t)id;
    return 1;
}
static int adaptive_radix_tree_resume(kvspace_t *store, int32_t node_id, const char *suffix,
                      int suffix_length, uint64_t *out_offset, int follow_moved) {
    int32_t id = follow_moved ? adaptive_radix_tree_follow(store, node_id) : node_id;
    if (id < 0)
        return KVSPACE_ANCHOR_INVALID;
    adaptive_radix_tree_header_t *node = adaptive_radix_tree_header(store, id);
    if (!node || node->type > ADAPTIVE_RADIX_TREE_N256 || node->prefix_len > ADAPTIVE_RADIX_TREE_PREFIX_MAX)
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
    value = adaptive_radix_tree_value_load(adaptive_radix_tree_header(store, found));
    if (!ADAPTIVE_RADIX_TREE_VALUE_HAS(value))
        return KVSPACE_MISSING;
    *out_offset = ADAPTIVE_RADIX_TREE_VALUE_OFFSET(value);
    return KVSPACE_OK;
}
int resolve_anchor(kvspace_t *store, const char *key,
                          const kvspace_anchor_t *anchor,
                          uint64_t *out_offset) {
    if (!store || !key || !out_offset)
        return KVSPACE_INVALID;
    *out_offset = 0;
    if (!anchor || anchor->node_offset == 0)
        return resolve_root(store, key, out_offset);
    uint64_t suffix_length = (uint64_t)strlen(key);
    if (reserved_meta_path(key) ||
        separator_at(key, 0, (size_t)suffix_length) != SEPARATOR_NONE ||
        !separator_sequence_valid(key, (size_t)suffix_length) ||
        (uint64_t)anchor->depth + suffix_length >= 1024)
        return KVSPACE_INVALID;
    if (!(anchor->flags & KVSPACE_ANCHOR_FLAG_INDEX))
        return KVSPACE_ANCHOR_INVALID;
    if (store_sync(store) != 0)
        return KVSPACE_BACKEND;
    int32_t node_id = -1;
    if (!anchor_offset_valid(store, anchor->node_offset, &node_id))
        return KVSPACE_ANCHOR_INVALID;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64_t before =
            __atomic_load_n(&store->header->generation_epoch, __ATOMIC_ACQUIRE);
        int status = adaptive_radix_tree_resume(store, node_id, key, (int)suffix_length, out_offset,
                                anchor->epoch != before);
        uint64_t after =
            __atomic_load_n(&store->header->generation_epoch, __ATOMIC_ACQUIRE);
        if (before == after)
            return status;
    }
    return KVSPACE_ANCHOR_INVALID;
}
int resolve_root(kvspace_t *store, const char *key,
                             uint64_t *out_offset) {
    int32_t id = -1;
    if (!store || !key || !out_offset)
        return KVSPACE_INVALID;
    *out_offset = 0;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    if (store_sync(store) != 0)
        return KVSPACE_BACKEND;
    return storage_locate(store, key, &id, out_offset);
}
int storage_is_index_key(const char *key) {
    size_t length = strlen(key);
    return length > 0 && ending_separator(key, length) != SEPARATOR_NONE;
}
int storage_has_children(kvspace_t *store, const char *key) {
    keyscan_t children = {0};
    char buffer[4096];
    int return_code = 0;
    if (store->header->adaptive_radix_tree_root < 0)
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
char *member_directory(const char *path) {
    return append_separator(path, SEPARATOR_DOT);
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
    if (!store || !key)
        return 0;
    adaptive_radix_tree_header_t *node_header = adaptive_radix_tree_search(store, store->header->adaptive_radix_tree_root, (const uint8_t *)key,
                              (int)strlen(key));
    if (node_header && ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node_header)))
        return 1;
    if (storage_is_index_key(key)) {
        keyscan_t children = {0};
        char buffer[4096];
        int return_code = 0;
        if (store->header->adaptive_radix_tree_root >= 0)
            return_code = adaptive_radix_tree_scan_prefix(store, key, (int)strlen(key), buffer, (int)sizeof buffer,
                              &children);
        int has = return_code == 0 && children.count > 0;
        keyscan_free(&children);
        return has;
    }
    size_t length = strlen(key);
    if (length == 0 || length + 2 > 1024)
        return 0;
    char *dir = append_separator(key, SEPARATOR_SLASH);
    if (!dir)
        return 0;
    node_header = adaptive_radix_tree_search(store, store->header->adaptive_radix_tree_root, (const uint8_t *)dir, (int)strlen(dir));
    int exists = node_header && ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node_header));
    free(dir);
    return exists;
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
    if (store->header->adaptive_radix_tree_root >= 0 &&
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
    if (!store || !key || !out_count || !out_max_name_length)
        return KVSPACE_INVALID;
    *out_count = 0;
    *out_max_name_length = 0;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    if (store_sync(store) != 0)
        return KVSPACE_BACKEND;
    int return_code = storage_index_state(store, key);
    if (return_code != KVSPACE_OK)
        return return_code;
    char prefix[1024];
    return_code = storage_list_prefix(key, prefix, sizeof prefix);
    if (return_code != KVSPACE_OK)
        return return_code;
    keyscan_t children = {0};
    return_code = storage_children(store, prefix, &children);
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
int list_names_raw(kvspace_t *store, const char *key, char *out_names,
                          uint32_t out_capacity, uint32_t *out_length,
                          uint32_t *out_count, uint64_t *out_generation) {
    if (!store || !key)
        return KVSPACE_INVALID;
    if (out_length)
        *out_length = 0;
    if (out_count)
        *out_count = 0;
    if (out_generation)
        *out_generation = 0;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    if (store_sync(store) != 0)
        return KVSPACE_BACKEND;
    int return_code = storage_index_state(store, key);
    if (return_code != KVSPACE_OK)
        return return_code;
    char prefix[1024];
    return_code = storage_list_prefix(key, prefix, sizeof prefix);
    if (return_code != KVSPACE_OK)
        return return_code;
    keyscan_t children = {0};
    return_code = storage_children(store, prefix, &children);
    if (return_code != KVSPACE_OK)
        return return_code;
    uint64_t total = 0;
    for (int32_t i = 0; i < children.count; i++)
        total += (uint64_t)strlen(children.keys[i]) + 1;
    if (out_generation)
        *out_generation = __atomic_load_n(&store->header->list_generation, __ATOMIC_ACQUIRE);
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

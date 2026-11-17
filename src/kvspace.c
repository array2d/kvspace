#ifndef _GNU_SOURCE
#define _GNU_SOURCE 
#endif
#define BLOCKMALLOC_IMPLEMENTATION 
#include <blockmalloc/blockmalloc.h>
#define SBO_MIN_SHARD_BYTES (1ULL << 40)
#define SLOTSBOXMALLOC_IMPLEMENTATION 
#include "slotsboxmalloc/slotsboxobj.h"
#include <kvspace/kvspace.h>
#include <kvspace/kvspace_xvalue.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
typedef struct kvspace kvspace_t;
#define KVSPACE_MAGIC "kvspace-c"
#define ADAPTIVE_RADIX_TREE_PREFIX_MAX 10
#define ADAPTIVE_RADIX_TREE_BLOCK_SIZE 104
#define ADAPTIVE_RADIX_TREE_SLAB_INITIAL (64UL * 1024 * 1024)
#define SLOTS_BOX_HEAD_POOL_INITIAL (4UL * 1024 * 1024)
#define SHARED_MEMORY_REGION_RESERVE (1ULL << 38)
#define ADAPTIVE_RADIX_TREE_RESERVE (1ULL << 36)
#define DATA_RESERVE (1ULL << 40)
#define SLOTS_BOX_PUNCH_MIN (64UL * 1024)
enum separator_kind { SEPARATOR_SLASH = 0, SEPARATOR_DOT = 1, SEPARATOR_COUNT = 2, SEPARATOR_NONE = -1 };
static const struct { const char bytes[3]; uint8_t length; } separator_table[SEPARATOR_COUNT] = {
    {"/", 1}, {"\xC2\xB7", 2}
};
static int separator_length(enum separator_kind kind) {
    if (kind < SEPARATOR_SLASH || kind >= SEPARATOR_COUNT)
        return 0;
    return separator_table[kind].length;
}
static enum separator_kind separator_at(const char *text, size_t index, size_t length) {
    for (int kind = SEPARATOR_SLASH; kind < SEPARATOR_COUNT; kind++) {
        int count = separator_length((enum separator_kind)kind);
        if (count > 0 && index + (size_t)count <= length &&
            memcmp(text + index, separator_table[kind].bytes, (size_t)count) == 0)
            return (enum separator_kind)kind;
    }
    return SEPARATOR_NONE;
}
static enum separator_kind ending_separator(const char *text, size_t length) {
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
static int reserved_meta_path(const char *key) {
    static const char root[] = "/.kvspace-meta";
    if (strncmp(key, root, sizeof root - 1) != 0)
        return 0;
    size_t length = strlen(key);
    size_t index = sizeof root - 1;
    return index == length ||
           separator_at(key, index, length) == SEPARATOR_SLASH;
}
static int key_text_valid(const char *key) {
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
static int adaptive_radix_tree_node_size(int node_type) {
    return node_type == ADAPTIVE_RADIX_TREE_N4 ? sizeof(adaptive_radix_tree_node4_t)
           : node_type == ADAPTIVE_RADIX_TREE_N16 ? sizeof(adaptive_radix_tree_node16_t)
           : node_type == ADAPTIVE_RADIX_TREE_N48 ? sizeof(adaptive_radix_tree_node48_t)
                          : sizeof(adaptive_radix_tree_node256_t);
}
#define RECOVERY_SLOT_COUNT 64
enum { RECOVERY_SLOT_FREE = 0, RECOVERY_SLOT_MINE = 1, RECOVERY_SLOT_CLAIMING = 3 };
typedef struct {
    uint32_t process_id;
    uint32_t state;
    uint64_t start_time;
    uint64_t recovery_ack;
    uint32_t quiesced;
    uint32_t claim_id;
} recovery_slot_t;
#define GARBAGE_CAPACITY (1u << 21)
#define GARBAGE_THRESHOLD (64ull << 20)
#define RECOVERY_BARRIER_MILLISECONDS_DEFAULT 200
#define GARBAGE_MAX_CLAIM ((1u << 30) - 1u)
typedef struct {
    uint64_t offset;
    uint32_t epoch;
    uint32_t meta;
} garbage_entry_t;
enum { GARBAGE_ENTRY_EMPTY = 0, GARBAGE_ENTRY_RESERVED = 1, GARBAGE_ENTRY_COMMITTED = 2 };
#define GARBAGE_ENTRY_STATE(metadata) ((uint32_t)((metadata) & 3u))
#define GARBAGE_ENTRY_CLAIM(metadata) ((uint32_t)((metadata) >> 2))
#define GARBAGE_ENTRY_MAKE(state,claim_id) ((((uint32_t)(claim_id)) << 2) | (uint32_t)(state))
typedef struct {
    char magic[sizeof(KVSPACE_MAGIC)];
    uint64_t adaptive_radix_tree_slab_size, slots_box_head_size, slots_box_data_size;
    int32_t adaptive_radix_tree_root;
    uint64_t generation_epoch;
    uint32_t recovery_epoch;
    uint32_t ack_sequence;
    uint32_t barrier_state;
    uint64_t garbage_head, garbage_tail;
    uint64_t committed_bytes;
    uint64_t freed_bytes;
    uint64_t garbage_freeing;
    uint64_t list_generation;
    uint32_t claim_sequence;
    recovery_slot_t slots[RECOVERY_SLOT_COUNT];
} kvspace_header_t;
#define ADAPTIVE_RADIX_TREE_OFFSET (sizeof(kvspace_header_t) + sizeof(blocks_meta_t))
static size_t kvspace_data_size = 8ULL * 64 * 64 * 64 * 64;
static int delete_tree(kvspace_t *store, const char *prefix);
static int copy_key(kvspace_t *store, const char *source, const char *destination);
static int copy_tree(kvspace_t *store, const char *source, const char *destination);
static uint8_t *storage_data_base(kvspace_t *store);
static int resolve_root(kvspace_t *store, const char *key,
                             uint64_t *out_offset);
static int resolve_anchor(kvspace_t *store, const char *key,
                          const kvspace_anchor_t *anchor,
                          uint64_t *out_offset);
static int adaptive_radix_tree_resume(kvspace_t *store, int32_t node_id, const char *suffix,
                      int suffix_length, uint64_t *out_offset, int follow_moved);
static int realloc_key(kvspace_t *store, const char *key, uint64_t body_length,
                             uint64_t attribute_length, uint64_t *out_offset);
static int free_key(kvspace_t *store, const char *key);
static int makeindex_key(kvspace_t *store, const char *key);
static int listlen_key(kvspace_t *store, const char *key, int64_t *out_count,
                             uint32_t *out_max_name_length);
static int list_names_raw(kvspace_t *store, const char *key, char *out_names,
                          uint32_t out_capacity, uint32_t *out_length,
                          uint32_t *out_count, uint64_t *out_generation);
_Static_assert(ADAPTIVE_RADIX_TREE_SLAB_INITIAL / (ADAPTIVE_RADIX_TREE_BLOCK_SIZE + 2) > 32767ULL / 4,
               "ART_SLAB_INIT too small: blockmalloc would pick 14-bit ids");
_Static_assert(ADAPTIVE_RADIX_TREE_RESERVE / (ADAPTIVE_RADIX_TREE_BLOCK_SIZE + 8) <= (1ULL << 30),
               "ART_RESERVE too large for 30-bit block ids");
_Static_assert((ADAPTIVE_RADIX_TREE_BLOCK_SIZE + 8) % 8 == 0, "ART stride must keep 8B alignment");
_Static_assert(
    SLOTS_BOX_HEAD_POOL_INITIAL / (sizeof(sbo_box_t) + 2) > 32767ULL / 4,
    "SBO_HEAD_POOL_INIT too small: blockmalloc would pick 14-bit ids");
#define SLOTS_BOX_HEAD_FIXED \
    (sizeof(sbo_meta_t) + sizeof(blocks_meta_t) + sizeof(sbo_lock_t))
static blocks_meta_t *slots_box_pool(sbo_meta_t *meta) {
    return sbo_pool_meta(meta, 0);
}
static sbo_lock_t *slots_box_pool_lock(sbo_meta_t *meta) {
    return sbo_pool_lock(meta, 0);
}
static uint8_t *slots_box_pool_memory(sbo_meta_t *meta) {
    return sbo_pool_mem(meta, 0);
}
static sbo_box_t *slots_box_box(sbo_meta_t *meta, int32_t id) {
    return (sbo_box_t *)(slots_box_pool_memory(meta) + (size_t)id * meta->block_stride +
                         meta->box_off);
}
typedef struct {
    int file_descriptor;
    uint8_t *base;
    size_t mapped;
    size_t reserve;
} shared_memory_region_t;
struct kvspace {
    shared_memory_region_t region_adaptive_radix_tree, region_header, region_data, region_generations, region_garbage;
    garbage_entry_t *garbage;
    uint64_t generation_count;
    uint64_t adaptive_radix_tree_total_seen;
    kvspace_header_t *header;
    int recovery_slot;
    uint32_t process_id;
    uint32_t fork_generation;
    uint32_t quiesced;
    pthread_t maintenance_thread;
    uint32_t maintenance_sequence;
    uint32_t maintenance_started, maintenance_stop;
    uint32_t reclaim_unavailable;
    blocks_meta_t *adaptive_radix_tree_meta;
    uint8_t *adaptive_radix_tree_data;
    sbo_meta_t *slots_box_meta;
    uint8_t *slots_box_data;
};
static inline void list_generation_bump(kvspace_t *store) {
    __atomic_add_fetch(&store->header->list_generation, 1ull, __ATOMIC_RELEASE);
}
static int region_map(shared_memory_region_t *region, size_t size) {
    if (size > region->reserve)
        return -1;
    if (size <= region->mapped)
        return 0;
    if (mmap(region->base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
             region->file_descriptor, 0) == MAP_FAILED)
        return -1;
    region->mapped = size;
    return 0;
}
static int region_attach(shared_memory_region_t *region, bool create, size_t size,
                         size_t reserve) {
    struct stat stat_result;
    if (create ? ftruncate(region->file_descriptor, (off_t)size) != 0
               : fstat(region->file_descriptor, &stat_result) != 0 || stat_result.st_size < (off_t)size)
        return -1;
    region->reserve = size > reserve ? size : reserve;
    void *mapping = mmap(NULL, region->reserve, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mapping == MAP_FAILED)
        return -1;
    region->base = mapping;
    return region_map(region, size);
}
static void region_close(shared_memory_region_t *region) {
    if (region->base)
        munmap(region->base, region->reserve);
    if (region->file_descriptor >= 0)
        close(region->file_descriptor);
}
static void store_slots_box_free(kvspace_t *store, uint64_t offset);
static adaptive_radix_tree_header_t *adaptive_radix_tree_header(kvspace_t *store, int32_t id);
static int32_t adaptive_radix_tree_child(kvspace_t *store, void *node, uint8_t byte);
static int shared_prefix_length(const uint8_t *left, int left_length, const uint8_t *right, int right_length);
static int32_t adaptive_radix_tree_walk(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
                        int depth, int32_t *out_parent, int *out_parent_depth);
static int32_t adaptive_radix_tree_follow(kvspace_t *store, int32_t id);
typedef struct {
    uint32_t struct_generation;
    uint32_t value_generation;
} generations_t;
static uint64_t generation_capacity(const kvspace_t *store) {
    blocks_meta_t *meta = store->adaptive_radix_tree_meta;
    uint64_t head = (uint64_t)meta->sizeof_block_head;
    return meta->total_size / (head + meta->block_size);
}
static int32_t adaptive_radix_tree_id_of(kvspace_t *store, const void *pointer) {
    if (!pointer || !store->adaptive_radix_tree_data)
        return -1;
    return (int32_t)blockid_bydataoffset(store->adaptive_radix_tree_meta,
                                         (uint64_t)((const uint8_t *)pointer - store->adaptive_radix_tree_data));
}
static generations_t *generations_at(kvspace_t *store, int32_t id) {
    if (id < 0 || !store->region_generations.base)
        return NULL;
    if ((uint64_t)id >= store->generation_count)
        return NULL;
    return (generations_t *)store->region_generations.base + id;
}
static void generation_bump_struct(kvspace_t *store, int32_t id) {
    generations_t *generations = generations_at(store, id);
    if (generations)
        __atomic_add_fetch(&generations->struct_generation, 1u, __ATOMIC_ACQ_REL);
}
static void generation_bump_value(kvspace_t *store, int32_t id) {
    generations_t *generations = generations_at(store, id);
    if (generations)
        __atomic_add_fetch(&generations->value_generation, 2u, __ATOMIC_ACQ_REL);
}
static int64_t generation_alloc_epoch(kvspace_t *store) {
    return (int64_t)__atomic_add_fetch(&store->header->generation_epoch, 1ull, __ATOMIC_ACQ_REL);
}
static int recovery_maintenance_start(kvspace_t *store);
static bool recovery_claim_alive(kvspace_t *store, uint32_t claim);
static int store_sync(kvspace_t *store);
static uint64_t recovery_garbage_threshold(void) {
    return GARBAGE_THRESHOLD;
}
static void garbage_full_report(kvspace_t *store) {
    static bool once;
    if (once)
        return;
    once = true;
    uint64_t garbage_head_index = __atomic_load_n(&store->header->garbage_head, __ATOMIC_ACQUIRE);
    uint64_t garbage_tail_index = __atomic_load_n(&store->header->garbage_tail, __ATOMIC_ACQUIRE);
    uint32_t meta_state = __atomic_load_n(&store->garbage[garbage_head_index & (GARBAGE_CAPACITY - 1)].meta,
                                 __ATOMIC_ACQUIRE);
    fprintf(stderr,
            "kvspace: garbage ring full (cap=%llu, head=%llu, tail=%llu, "
            "bytes=%llu): head entry #%llu state=%u claim=%u is holding "
            "the drain (no reclaim round yet)\n",
            (unsigned long long)GARBAGE_CAPACITY, (unsigned long long)garbage_head_index,
            (unsigned long long)garbage_tail_index,
            (unsigned long long)(store->header->committed_bytes -
                                 store->header->freed_bytes),
            (unsigned long long)garbage_head_index, GARBAGE_ENTRY_STATE(meta_state), GARBAGE_ENTRY_CLAIM(meta_state));
}
static int garbage_reserve(kvspace_t *store, uint64_t *index) {
    uint32_t claim = store->recovery_slot >= 0 ? store->header->slots[store->recovery_slot].claim_id : 0;
    if (!claim)
        return -1;
    uint64_t garbage_head_index = __atomic_load_n(&store->header->garbage_head, __ATOMIC_ACQUIRE);
    uint64_t garbage_tail_index = __atomic_load_n(&store->header->garbage_tail, __ATOMIC_ACQUIRE);
    if (garbage_tail_index - garbage_head_index >= GARBAGE_CAPACITY) {
        garbage_full_report(store);
        return -1;
    }
    if (store->reclaim_unavailable &&
        GARBAGE_CAPACITY - (garbage_tail_index - garbage_head_index) < GARBAGE_CAPACITY / 4) {
        garbage_full_report(store);
        return -1;
    }
    garbage_entry_t *entry = &store->garbage[garbage_tail_index & (GARBAGE_CAPACITY - 1)];
    uint32_t meta_state = __atomic_load_n(&entry->meta, __ATOMIC_ACQUIRE);
    if (meta_state != GARBAGE_ENTRY_EMPTY) {
        if (GARBAGE_ENTRY_STATE(meta_state) != GARBAGE_ENTRY_RESERVED || recovery_claim_alive(store, GARBAGE_ENTRY_CLAIM(meta_state)))
            return -1;
        __atomic_store_n(&entry->meta, GARBAGE_ENTRY_EMPTY, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&entry->meta, GARBAGE_ENTRY_MAKE(GARBAGE_ENTRY_RESERVED, claim), __ATOMIC_RELEASE);
    __atomic_store_n(&store->header->garbage_tail, garbage_tail_index + 1, __ATOMIC_RELEASE);
    *index = garbage_tail_index;
    return 0;
}
static void garbage_commit(kvspace_t *store, uint64_t index, uint64_t offset,
                        uint64_t bytes_retired) {
    garbage_entry_t *entry = &store->garbage[index & (GARBAGE_CAPACITY - 1)];
    uint32_t claim = GARBAGE_ENTRY_CLAIM(__atomic_load_n(&entry->meta, __ATOMIC_ACQUIRE));
    entry->offset = offset;
    entry->epoch = __atomic_load_n(&store->header->recovery_epoch, __ATOMIC_ACQUIRE);
    __atomic_store_n(&entry->meta, GARBAGE_ENTRY_MAKE(GARBAGE_ENTRY_COMMITTED, claim), __ATOMIC_RELEASE);
    uint64_t bytes = store->header->committed_bytes + bytes_retired;
    __atomic_store_n(&store->header->committed_bytes, bytes, __ATOMIC_RELEASE);
    if (bytes - __atomic_load_n(&store->header->freed_bytes, __ATOMIC_ACQUIRE) >=
        recovery_garbage_threshold())
        (void)recovery_maintenance_start(store);
}
static void garbage_cancel(kvspace_t *store, uint64_t index) {
    __atomic_store_n(&store->garbage[index & (GARBAGE_CAPACITY - 1)].meta, GARBAGE_ENTRY_EMPTY,
                     __ATOMIC_RELEASE);
}
static int32_t recovery_node_at(kvspace_t *store, const char *key) {
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
static int recovery_process_start_time(uint32_t process_id, uint64_t *out) {
#if defined(__linux__)
    char path[64];
    if (snprintf(path, sizeof path, "/proc/%u/stat", process_id) >= (int)sizeof path)
        return 2;
    FILE *file = fopen(path, "r");
    if (!file)
        return errno == ENOENT ? 1 : 2;
    char buffer[1024];
    size_t bytes_read = fread(buffer, 1, sizeof buffer - 1, file);
    fclose(file);
    if (!bytes_read)
        return 2;
    buffer[bytes_read] = 0;
    char *closing_parenthesis = strrchr(buffer, ')');
    if (!closing_parenthesis)
        return 2;
    char *scan_pointer = closing_parenthesis + 1;
    int field = 3;
    while (*scan_pointer) {
        while (*scan_pointer == ' ')
            scan_pointer++;
        if (!*scan_pointer)
            break;
        if (field == 3 && (*scan_pointer == 'Z' || *scan_pointer == 'X'))
            return 1;
        if (field == 22) {
            *out = strtoull(scan_pointer, NULL, 10);
            return 0;
        }
        while (*scan_pointer && *scan_pointer != ' ')
            scan_pointer++;
        field++;
    }
    return 2;
#else
    (void)process_id;
    (void)out;
    return 2;
#endif
}
static bool recovery_slot_alive(const recovery_slot_t *slot) {
    uint64_t process_start_time = 0;
    uint32_t state = __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
    if (state == RECOVERY_SLOT_FREE)
        return false;
    if (state == RECOVERY_SLOT_CLAIMING)
        return true;
    int return_code = recovery_process_start_time(slot->process_id, &process_start_time);
    if (return_code == 1)
        return false;
    if (return_code != 0)
        return true;
    return process_start_time == slot->start_time;
}
static int recovery_register_claim(kvspace_t *store) {
    uint64_t process_start_time = 0;
    if (recovery_process_start_time((uint32_t)getpid(), &process_start_time) != 0)
        process_start_time = 0;
    uint32_t current_process_id = (uint32_t)getpid();
    for (int i = 0; i < RECOVERY_SLOT_COUNT; i++) {
        recovery_slot_t *slot = &store->header->slots[i];
        uint32_t state = __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
        if (state == RECOVERY_SLOT_CLAIMING && slot->process_id == 0) {
            __atomic_compare_exchange_n(&slot->state, &state, RECOVERY_SLOT_FREE, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            continue;
        }
        if (state != RECOVERY_SLOT_FREE && !recovery_slot_alive(slot))
            __atomic_compare_exchange_n(&slot->state, &state, RECOVERY_SLOT_FREE, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    for (int i = 0; i < RECOVERY_SLOT_COUNT; i++) {
        recovery_slot_t *slot = &store->header->slots[i];
        uint32_t expect = RECOVERY_SLOT_FREE;
        if (!__atomic_compare_exchange_n(&slot->state, &expect, RECOVERY_SLOT_CLAIMING, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        uint32_t claim_id = __atomic_add_fetch(&store->header->claim_sequence, 1u,
                                          __ATOMIC_ACQ_REL);
        if (claim_id > GARBAGE_MAX_CLAIM) {
            __atomic_store_n(&slot->state, RECOVERY_SLOT_FREE, __ATOMIC_RELEASE);
            return -3;
        }
        slot->process_id = current_process_id;
        slot->start_time = process_start_time;
        slot->quiesced = 1;
        slot->claim_id = claim_id;
        __atomic_store_n(&slot->recovery_ack,
                         __atomic_load_n(&store->header->recovery_epoch, __ATOMIC_ACQUIRE),
                         __ATOMIC_RELEASE);
        __atomic_store_n(&slot->state, RECOVERY_SLOT_MINE, __ATOMIC_RELEASE);
        return i;
    }
    return -2;
}
static void recovery_register_release(kvspace_t *store) {
    if (store->recovery_slot < 0)
        return;
    if (store->process_id != (uint32_t)getpid()) {
        store->recovery_slot = -1;
        return;
    }
    recovery_slot_t *slot = &store->header->slots[store->recovery_slot];
    uint32_t expect = RECOVERY_SLOT_MINE;
    __atomic_compare_exchange_n(&slot->state, &expect, RECOVERY_SLOT_FREE, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    store->recovery_slot = -1;
}
#define GENERATIONS_RESERVE (8ULL << 30)
static int generations_grow(kvspace_t *store) {
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
static uint32_t g_fork_generation;
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;
static void recovery_atfork_child(void) {
    __atomic_add_fetch(&g_fork_generation, 1u, __ATOMIC_ACQ_REL);
}
static void recovery_atfork_install(void) {
    (void)pthread_atfork(NULL, NULL, recovery_atfork_child);
}
static long long recovery_now_milliseconds(void) {
    struct timespec time_specification;
    clock_gettime(CLOCK_MONOTONIC, &time_specification);
    return (long long)time_specification.tv_sec * 1000 + time_specification.tv_nsec / 1000000;
}
static int recovery_barrier_milliseconds(void) {
    return RECOVERY_BARRIER_MILLISECONDS_DEFAULT;
}
#if defined(__linux__)
static void recovery_futex_wait(uint32_t *address, uint32_t value, int milliseconds) {
    struct timespec time_specification = {(time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000L};
    syscall(SYS_futex, address, FUTEX_WAIT, value, &time_specification, NULL, 0);
}
static void recovery_futex_wake(uint32_t *address) {
    syscall(SYS_futex, address, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}
static void recovery_futex_wait_private(uint32_t *address, uint32_t value, int milliseconds) {
    struct timespec time_specification = {(time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000L};
    syscall(SYS_futex, address, FUTEX_WAIT_PRIVATE, value, &time_specification, NULL, 0);
}
static void recovery_futex_wake_private(uint32_t *address) {
    syscall(SYS_futex, address, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}
#else
static void recovery_futex_wait(uint32_t *address, uint32_t value, int milliseconds) { (void)address; (void)value; usleep((useconds_t)milliseconds * 1000); }
static void recovery_futex_wake(uint32_t *address) { (void)address; }
static void recovery_futex_wait_private(uint32_t *address, uint32_t value, int milliseconds) { (void)address; (void)value; usleep((useconds_t)milliseconds * 1000); }
static void recovery_futex_wake_private(uint32_t *address) { (void)address; }
#endif
enum { RECOVERY_BARRIER_FREE = 0, RECOVERY_BARRIER_OWNED = 1 };
static uint32_t recovery_my_claim(kvspace_t *store) {
    return store->recovery_slot >= 0 ? store->header->slots[store->recovery_slot].claim_id : 0;
}
static bool recovery_claim_alive(kvspace_t *store, uint32_t claim) {
    if (!claim)
        return false;
    for (int i = 0; i < RECOVERY_SLOT_COUNT; i++) {
        recovery_slot_t *slot = &store->header->slots[i];
        if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != RECOVERY_SLOT_MINE)
            continue;
        if (__atomic_load_n(&slot->claim_id, __ATOMIC_ACQUIRE) != claim)
            continue;
        return recovery_slot_alive(slot);
    }
    return false;
}
static bool recovery_elect(kvspace_t *store) {
    uint32_t mine = recovery_my_claim(store);
    if (!mine)
        return false;
    uint32_t own = GARBAGE_ENTRY_MAKE(RECOVERY_BARRIER_OWNED, mine);
    for (;;) {
        uint32_t current = __atomic_load_n(&store->header->barrier_state, __ATOMIC_ACQUIRE);
        if (current == GARBAGE_ENTRY_MAKE(RECOVERY_BARRIER_FREE, 0)) {
            if (__atomic_compare_exchange_n(&store->header->barrier_state, &current, own,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                return true;
            continue;
        }
        if (GARBAGE_ENTRY_STATE(current) != RECOVERY_BARRIER_OWNED)
            return false;
        if (GARBAGE_ENTRY_CLAIM(current) == mine)
            return false;
        if (recovery_claim_alive(store, GARBAGE_ENTRY_CLAIM(current)))
            return false;
        if (__atomic_compare_exchange_n(&store->header->barrier_state, &current, own, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
}
static void recovery_release_busy(kvspace_t *store) {
    uint32_t mine = GARBAGE_ENTRY_MAKE(RECOVERY_BARRIER_OWNED, recovery_my_claim(store));
    __atomic_compare_exchange_n(&store->header->barrier_state, &mine,
                                GARBAGE_ENTRY_MAKE(RECOVERY_BARRIER_FREE, 0), false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static bool recovery_wait_acknowledgements(kvspace_t *store, uint32_t epoch, int deadline_milliseconds) {
    uint8_t dead[RECOVERY_SLOT_COUNT] = {0};
    long long start_milliseconds = recovery_now_milliseconds();
    for (;;) {
        uint32_t sequence = __atomic_load_n(&store->header->ack_sequence, __ATOMIC_ACQUIRE);
        bool missing = false;
        int first = -1;
        for (int i = 0; i < RECOVERY_SLOT_COUNT; i++) {
            recovery_slot_t *slot = &store->header->slots[i];
            if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != RECOVERY_SLOT_MINE)
                continue;
            if (__atomic_load_n(&slot->recovery_ack, __ATOMIC_ACQUIRE) == epoch)
                continue;
            if (__atomic_load_n(&slot->quiesced, __ATOMIC_ACQUIRE))
                continue;
            if (!dead[i]) {
                uint64_t process_start_time = 0;
                int return_code = recovery_process_start_time(slot->process_id, &process_start_time);
                if (return_code == 1 || (return_code == 0 && process_start_time != slot->start_time))
                    dead[i] = 1;
            }
            if (dead[i])
                continue;
            missing = true;
            if (first < 0)
                first = i;
        }
        if (!missing)
            return true;
        long long left = (long long)deadline_milliseconds - (recovery_now_milliseconds() - start_milliseconds);
        if (left <= 0 || __atomic_load_n(&store->maintenance_stop, __ATOMIC_ACQUIRE)) {
            if (first >= 0) {
                recovery_slot_t *slot = &store->header->slots[first];
                fprintf(stderr,
                        "kvspace: barrier round %u abandoned (waited %dms): "
                        "slot %d pid=%u start=%llu ack=%llu quiesced=%u\n",
                        epoch, deadline_milliseconds, first, slot->process_id,
                        (unsigned long long)slot->start_time,
                        (unsigned long long)__atomic_load_n(&slot->recovery_ack,
                                                            __ATOMIC_ACQUIRE),
                        __atomic_load_n(&slot->quiesced, __ATOMIC_ACQUIRE));
            }
            return false;
        }
        recovery_futex_wait(&store->header->ack_sequence, sequence, (int)(left < 5 ? left : 5));
    }
}
static int recovery_drain(kvspace_t *store, uint32_t epoch) {
    int freed = 0;
    uint64_t i = __atomic_load_n(&store->header->garbage_head, __ATOMIC_ACQUIRE);
    for (;;) {
        if (i >= __atomic_load_n(&store->header->garbage_tail, __ATOMIC_ACQUIRE))
            goto out;
        garbage_entry_t *entry = &store->garbage[i & (GARBAGE_CAPACITY - 1)];
        uint32_t meta_state = __atomic_load_n(&entry->meta, __ATOMIC_ACQUIRE);
        uint32_t garbage_state = GARBAGE_ENTRY_STATE(meta_state);
        if (garbage_state == GARBAGE_ENTRY_EMPTY) {
            __atomic_store_n(&store->header->garbage_head, i + 1, __ATOMIC_RELEASE);
            i++;
            continue;
        }
        if (garbage_state == GARBAGE_ENTRY_RESERVED) {
            if (recovery_claim_alive(store, GARBAGE_ENTRY_CLAIM(meta_state)))
                goto out;
            __atomic_store_n(&entry->meta, GARBAGE_ENTRY_EMPTY, __ATOMIC_RELEASE);
            fprintf(stderr,
                    "kvspace: garbage entry %llu reserved by dead claim %u: "
                    "leaked\n", (unsigned long long)i, GARBAGE_ENTRY_CLAIM(meta_state));
            __atomic_store_n(&store->header->garbage_head, i + 1, __ATOMIC_RELEASE);
            i++;
            continue;
        }
        if (garbage_state != GARBAGE_ENTRY_COMMITTED)
            goto out;
        if (!((int32_t)(epoch - __atomic_load_n(&entry->epoch, __ATOMIC_ACQUIRE)) > 0))
            goto out;
        uint64_t offset = __atomic_load_n(&entry->offset, __ATOMIC_ACQUIRE);
        uint64_t capacity = sbo_allocated_size(store->slots_box_meta, offset);
        __atomic_store_n(&store->header->garbage_freeing, offset, __ATOMIC_RELEASE);
        __atomic_store_n(&entry->meta, GARBAGE_ENTRY_EMPTY, __ATOMIC_RELEASE);
        __atomic_store_n(&store->header->garbage_head, i + 1, __ATOMIC_RELEASE);
        store_slots_box_free(store, offset);
        __atomic_store_n(&store->header->freed_bytes,
                         store->header->freed_bytes + capacity, __ATOMIC_RELEASE);
        i++;
        freed++;
    }
out:
    __atomic_store_n(&store->header->garbage_freeing, 0, __ATOMIC_RELEASE);
    return freed;
}
static int recovery_round(kvspace_t *store) {
#if !defined(__linux__)
    (void)store;
    return 0;
#else
    if (!store || store->header->adaptive_radix_tree_root < 0)
        return 0;
    if (store->process_id != (uint32_t)getpid())
        return -1;
    if (!recovery_elect(store))
        return -1;
    if (store_sync(store) != 0) {
        recovery_release_busy(store);
        return -1;
    }
    uint64_t pending = __atomic_exchange_n(&store->header->garbage_freeing, 0,
                                           __ATOMIC_ACQ_REL);
    if (pending) {
        fprintf(stderr, "kvspace: previous leader died freeing box %llu: "
                        "leaked\n", (unsigned long long)pending);
    }
    uint32_t epoch = __atomic_add_fetch(&store->header->recovery_epoch, 1u,
                                    __ATOMIC_ACQ_REL);
    if (!recovery_wait_acknowledgements(store, epoch, recovery_barrier_milliseconds())) {
        recovery_release_busy(store);
        return -2;
    }
    int freed = recovery_drain(store, epoch);
    for (int i = 0; i < RECOVERY_SLOT_COUNT; i++) {
        recovery_slot_t *slot = &store->header->slots[i];
        uint32_t state = RECOVERY_SLOT_MINE;
        if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != RECOVERY_SLOT_MINE)
            continue;
        if (recovery_slot_alive(slot))
            continue;
        __atomic_compare_exchange_n(&slot->state, &state, RECOVERY_SLOT_FREE, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    recovery_release_busy(store);
    return freed;
#endif
}
static void *recovery_maintenance(void *arg) {
    kvspace_t *store = arg;
    while (!__atomic_load_n(&store->maintenance_stop, __ATOMIC_ACQUIRE)) {
        uint32_t sequence = __atomic_load_n(&store->maintenance_sequence, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&store->maintenance_stop, __ATOMIC_ACQUIRE))
            break;
        recovery_futex_wait_private(&store->maintenance_sequence, sequence, 50);
        if (__atomic_load_n(&store->maintenance_stop, __ATOMIC_ACQUIRE))
            break;
        if (store->header->committed_bytes -
                __atomic_load_n(&store->header->freed_bytes, __ATOMIC_ACQUIRE) <
            recovery_garbage_threshold())
            continue;
        recovery_round(store);
    }
    return NULL;
}
static int recovery_maintenance_start(kvspace_t *store) {
#if !defined(__linux__)
    (void)store;
    return 0;
#else
    uint32_t started_state = 0;
    if (!__atomic_compare_exchange_n(&store->maintenance_started, &started_state, 1u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return started_state == 2u ? -1 : 0;
    if (store->process_id != (uint32_t)getpid()) {
        __atomic_store_n(&store->maintenance_started, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    if (pthread_create(&store->maintenance_thread, NULL, recovery_maintenance, store) != 0) {
        __atomic_store_n(&store->maintenance_started, 2u, __ATOMIC_RELEASE);
        store->reclaim_unavailable = 1;
        fprintf(stderr, "kvspace: maintenance thread create failed; reclaim "
                        "unavailable (writes hard-fail when the ring is 3/4 "
                        "full)\n");
        return -1;
    }
    return 0;
#endif
}
static void recovery_maintenance_stop(kvspace_t *store) {
    if (store->process_id != (uint32_t)getpid()) {
        __atomic_store_n(&store->maintenance_started, 0u, __ATOMIC_RELEASE);
        return;
    }
    if (__atomic_load_n(&store->maintenance_started, __ATOMIC_ACQUIRE) != 1u)
        return;
    __atomic_store_n(&store->maintenance_stop, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&store->maintenance_sequence, 1u, __ATOMIC_RELEASE);
    recovery_futex_wake_private(&store->maintenance_sequence);
    pthread_join(store->maintenance_thread, NULL);
    __atomic_store_n(&store->maintenance_started, 0u, __ATOMIC_RELEASE);
}
#ifdef RECOVERY_TEST_HOOKS
void kvspaceTestSetDataSize(size_t data_size) {
    kvspace_data_size = data_size;
}
#endif
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
        uint64_t offset = blockdata_offset(meta, id);
        uint64_t value = offset + offsetof(adaptive_radix_tree_header_t, value);
        if (offset % 8 || value % 8 || (value & 63) > 56)
            return (int64_t)id;
    }
    return -1;
}
#endif
static int store_sync(kvspace_t *store) {
    if (region_map(&store->region_adaptive_radix_tree, ADAPTIVE_RADIX_TREE_OFFSET + (size_t)store->header->adaptive_radix_tree_slab_size) != 0)
        return -1;
    if (store->adaptive_radix_tree_meta->total_size != store->adaptive_radix_tree_total_seen &&
        generations_grow(store) != 0)
        return -1;
    if (region_map(&store->region_header, (size_t)store->header->slots_box_head_size) != 0)
        return -1;
    return region_map(&store->region_data, (size_t)store->header->slots_box_data_size);
}
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
#ifdef BLOCKMALLOC_COUNT_MASK
#define BLOCKMALLOC_MALLOC_BLOCKS(metadata) blocks_malloc_count(metadata)
#define BLOCKMALLOC_HEAD_SIZE(metadata) blocks_head_size(metadata)
#else
#define BLOCKMALLOC_MALLOC_BLOCKS(metadata) ((uint64_t)(metadata)->malloc_blocks)
#define BLOCKMALLOC_HEAD_SIZE(metadata) ((uint64_t)(metadata)->sizeof_block_head)
#endif
static bool slots_box_pool_full(sbo_meta_t *meta) {
    blocks_meta_t *pool = slots_box_pool(meta);
    if (pool->free_next_id != -1)
        return false;
    uint64_t next_end = (uint64_t)block_offset(pool, BLOCKMALLOC_MALLOC_BLOCKS(pool)) +
                        BLOCKMALLOC_HEAD_SIZE(pool) + pool->block_size;
    return next_end > pool->total_size;
}
static int slots_box_head_grow(kvspace_t *store) {
    sbo_meta_t *meta = store->slots_box_meta;
    blocks_meta_t *pool = slots_box_pool(meta);
    uint64_t stride = pool->sizeof_block_head + pool->block_size;
    uint64_t capacity = (1ULL << (pool->sizeof_block_head == 2 ? 14 : 30)) * stride;
    uint64_t current = pool->total_size, want = current * 2;
    if (want > capacity)
        want = capacity;
    if (SLOTS_BOX_HEAD_FIXED + want > store->region_header.reserve)
        want = (store->region_header.reserve - SLOTS_BOX_HEAD_FIXED) & ~(uint64_t)7;
    if (want <= current ||
        ftruncate(store->region_header.file_descriptor, (off_t)(SLOTS_BOX_HEAD_FIXED + want)) != 0 ||
        region_map(&store->region_header, SLOTS_BOX_HEAD_FIXED + (size_t)want) != 0)
        return -1;
    pool->total_size = want;
    meta->per_slot_meta = want;
    meta->max_blocks = (uint32_t)(want / meta->block_stride);
    store->header->slots_box_head_size = SLOTS_BOX_HEAD_FIXED + want;
    return 0;
}
static int slots_box_data_grow(kvspace_t *store) {
    sbo_meta_t *meta = store->slots_box_meta;
    uint64_t current = meta->data_size, want = current * 64;
    if (want > store->region_data.reserve)
        return -1;
    if (slots_box_pool_full(meta) && slots_box_head_grow(store) != 0)
        return -1;
    if (ftruncate(store->region_data.file_descriptor, (off_t)want) != 0 ||
        region_map(&store->region_data, (size_t)want) != 0)
        return -1;
    int64_t node_id = blocks_alloc(slots_box_pool(meta), slots_box_pool_memory(meta));
    if (node_id < 0)
        return -1;
    sbo_box_t *root = slots_box_box(meta, 0), *box = slots_box_box(meta, (int32_t)node_id);
    memcpy(box, root, sizeof *box);
    box->parent = 0;
    box->parent_slot = 0;
    box->self_id = (int32_t)node_id;
    uint64_t child_mask = root->child_mask;
    while (child_mask) {
        int i = __builtin_ctzll(child_mask);
        child_mask &= child_mask - 1;
        slots_box_box(meta, root->children[i])->parent = (int32_t)node_id;
    }
    sbo_lock_t *lock = slots_box_pool_lock(meta);
    uint8_t old_level = box->objlevel;
    if (old_level >= 1 && old_level <= 3 && lock->hot_id1[old_level - 1] == 1)
        lock->hot_id1[old_level - 1] = (uint32_t)node_id + 1;
    uint8_t level = (uint8_t)(old_level + 1);
    sbo_box_format(root, level, -1, 255, 0);
    root->free_mask = ~1ULL;
    root->child_mask = 1ULL;
    root->states[0] = SBO_BOX;
    root->children[0] = (int32_t)node_id;
    uint16_t box_combined = sbo_combined(box);
    root->child_cap[0] = box_combined;
    root->child_max_enc = box_combined;
    root->child_max_count = 1;
    root->max_obj_cap = SBO_MULT_MAX;
    root->hint_child = 0;
    meta->data_size = want;
    meta->slot_shift = (uint8_t)__builtin_ctzll(want);
    store->header->slots_box_data_size = want;
    return 0;
}
static int store_slots_box_grow(kvspace_t *store, bool failed, uint64_t *seen_data) {
    sbo_meta_t *meta = store->slots_box_meta;
    sbo_lock(slots_box_pool_lock(meta));
    int return_code = 0;
    if (slots_box_pool_full(meta))
        return_code = slots_box_head_grow(store);
    else if (failed && meta->data_size == *seen_data)
        return_code = slots_box_data_grow(store);
    else if (failed)
        *seen_data = meta->data_size;
    sbo_unlock(slots_box_pool_lock(meta));
    return return_code;
}
static uint64_t store_slots_box_alloc(kvspace_t *store, size_t size) {
    sbo_meta_t *meta = store->slots_box_meta;
    uint64_t seen_data = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt && store_sync(store) != 0)
            return (uint64_t)-1;
        if (slots_box_pool_full(meta) && store_slots_box_grow(store, false, &seen_data) != 0)
            return (uint64_t)-1;
        uint64_t offset = sbo_alloc(meta, size);
        if (offset != (uint64_t)-1)
            return offset;
        if (store_slots_box_grow(store, true, &seen_data) != 0)
            return (uint64_t)-1;
    }
    return (uint64_t)-1;
}
static void store_slots_box_free(kvspace_t *store, uint64_t offset) {
#if defined(__linux__)
    uint64_t size = sbo_allocated_size(store->slots_box_meta, offset);
    if (size >= SLOTS_BOX_PUNCH_MIN) {
        uint64_t page_size = (uint64_t)sysconf(_SC_PAGESIZE);
        uint64_t page_start = (offset + page_size - 1) & ~(page_size - 1), page_end = (offset + size) & ~(page_size - 1);
        if (page_end > page_start)
            (void)fallocate(store->region_data.file_descriptor,
                            FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                            (off_t)page_start, (off_t)(page_end - page_start));
    }
#endif
    sbo_free(store->slots_box_meta, offset);
}
static void *adaptive_radix_tree_block(kvspace_t *store, int32_t id) {
    if (id < 0)
        return NULL;
    return store->adaptive_radix_tree_data + blockdata_offset(store->adaptive_radix_tree_meta, (uint64_t)id);
}
_Static_assert(offsetof(blocks_meta_t, used_blocks) ==
                   offsetof(blocks_meta_t, block_size) + 2 * sizeof(uint64_t),
               "blockmalloc: sizeof_block_head/malloc_blocks must share one u64 word");
#ifdef BLOCKMALLOC_COUNT_MASK
static inline void blockmalloc_set_malloc_blocks(blocks_meta_t *meta, uint64_t count) {
    blocks_set_malloc_count(meta, count);
}
#else
#define BLOCKMALLOC_WORD_OFFSET (offsetof(blocks_meta_t, block_size) + sizeof(uint64_t))
static unsigned blockmalloc_count_shift(void) {
    blocks_meta_t metadata;
    memset(&metadata, 0, sizeof metadata);
    metadata.malloc_blocks = 1;
    uint64_t word;
    memcpy(&word, (const char *)&metadata + BLOCKMALLOC_WORD_OFFSET, sizeof word);
    return word ? (unsigned)__builtin_ctzll(word) : 64u;
}
static void blockmalloc_set_malloc_blocks(blocks_meta_t *meta, uint64_t count) {
    uint64_t *word = (uint64_t *)((char *)meta + BLOCKMALLOC_WORD_OFFSET);
    const unsigned shift = blockmalloc_count_shift();
    const uint64_t mask = ((1ULL << 60) - 1) << shift;
    uint64_t old = __atomic_load_n(word, __ATOMIC_RELAXED);
    __atomic_store_n(word, (old & ~mask) | ((count << shift) & mask), __ATOMIC_RELEASE);
}
#endif
static bool blockmalloc_layout_ok(void) {
    blocks_meta_t metadata;
    memset(&metadata, 0, sizeof metadata);
    metadata.sizeof_block_head = 8;
    metadata.malloc_blocks = 5;
    metadata.used_blocks = 7;
    blockmalloc_set_malloc_blocks(&metadata, 0x0123456789ABCDEull);
    return metadata.sizeof_block_head == 8 && metadata.malloc_blocks == 0x0123456789ABCDEull &&
           metadata.block_size == 0 && metadata.used_blocks == 7;
}
static int64_t adaptive_radix_tree_bump(kvspace_t *store, uint32_t block_count) {
    blocks_meta_t *meta = store->adaptive_radix_tree_meta;
    spin_lock(&meta->lock);
    uint64_t id = BLOCKMALLOC_MALLOC_BLOCKS(meta);
    if ((uint64_t)block_offset(meta, id + block_count) > meta->total_size) {
        spin_unlock(&meta->lock);
        return -1;
    }
    setblock_t(meta, store->adaptive_radix_tree_data + block_offset(meta, id), 1, -1);
    blockmalloc_set_malloc_blocks(meta, id + block_count);
    meta->used_blocks += block_count;
    spin_unlock(&meta->lock);
    return (int64_t)id;
}
static int32_t adaptive_radix_tree_block_alloc(kvspace_t *store, int node_type) {
    const uint64_t head = BLOCKMALLOC_HEAD_SIZE(store->adaptive_radix_tree_meta);
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
static adaptive_radix_tree_header_t *adaptive_radix_tree_search(kvspace_t *store, int32_t node_id, const uint8_t *key,
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
static int32_t adaptive_radix_tree_insert(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
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
static int32_t adaptive_radix_tree_delete(kvspace_t *store, int32_t node_id, const uint8_t *key, int key_length,
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
static char *path_join(const char *left, const char *right) {
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
static char *ending_directory(const char *path) {
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
typedef struct {
    char **keys;
    int32_t count;
    int32_t capacity;
} keyscan_t;
static void keyscan_free(keyscan_t *scan) {
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
static int adaptive_radix_tree_scan_keys(kvspace_t *store, const char *prefix, int prefix_length,
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
static bool slots_box_data_size_ok(size_t data_size) {
    for (uint64_t step_size = 8; step_size <= (1ULL << 45); step_size *= 64)
        if (step_size == data_size)
            return true;
    return false;
}
static kvspace_t *open_shared_memory_store(const char *path, size_t data_size) {
    if (!path)
        return NULL;
    if (!blockmalloc_layout_ok()) {
        fprintf(stderr, "kvspace: unexpected blockmalloc blocks_meta_t layout\n");
        return NULL;
    }
    char head_path[PATH_MAX], data_path[PATH_MAX], generation_path[PATH_MAX],
        garbage_path[PATH_MAX];
    if (snprintf(head_path, sizeof head_path, "%s.sbo.head", path) >=
            (int)sizeof head_path ||
        snprintf(data_path, sizeof data_path, "%s.sbo.data", path) >=
            (int)sizeof data_path ||
        snprintf(generation_path, sizeof generation_path, "%s.gen", path) >=
            (int)sizeof generation_path ||
        snprintf(garbage_path, sizeof garbage_path, "%s.garb", path) >=
            (int)sizeof garbage_path)
        return NULL;
    kvspace_t *store = calloc(1, sizeof(*store));
    if (!store)
        return NULL;
    store->recovery_slot = -1;
    store->region_adaptive_radix_tree.file_descriptor = store->region_header.file_descriptor = store->region_data.file_descriptor = store->region_generations.file_descriptor =
        store->region_garbage.file_descriptor = -1;
    bool created = false;
    store->region_adaptive_radix_tree.file_descriptor = open(path, O_RDWR);
    if (store->region_adaptive_radix_tree.file_descriptor < 0) {
        if (!slots_box_data_size_ok(data_size))
            goto fail;
        store->region_adaptive_radix_tree.file_descriptor = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
        if (store->region_adaptive_radix_tree.file_descriptor < 0)
            goto fail;
        created = true;
    }
    size_t adaptive_radix_tree_slab, slots_box_head;
    if (created) {
        adaptive_radix_tree_slab = ADAPTIVE_RADIX_TREE_SLAB_INITIAL;
        slots_box_head = sbo_meta_size(data_size, SLOTS_BOX_HEAD_POOL_INITIAL);
    } else {
        kvspace_header_t temporary;
        if (pread(store->region_adaptive_radix_tree.file_descriptor, &temporary, sizeof temporary, 0) != (ssize_t)sizeof temporary ||
            memcmp(temporary.magic, KVSPACE_MAGIC, sizeof(KVSPACE_MAGIC) - 1) != 0)
            goto fail;
        adaptive_radix_tree_slab = (size_t)temporary.adaptive_radix_tree_slab_size;
        slots_box_head = (size_t)temporary.slots_box_head_size;
        data_size = (size_t)temporary.slots_box_data_size;
    }
    int open_flags = created ? O_RDWR | O_CREAT | O_TRUNC : O_RDWR;
    if (region_attach(&store->region_adaptive_radix_tree, created, ADAPTIVE_RADIX_TREE_OFFSET + adaptive_radix_tree_slab,
                      ADAPTIVE_RADIX_TREE_RESERVE) != 0 ||
        (store->region_header.file_descriptor = open(head_path, open_flags, 0644)) < 0 ||
        region_attach(&store->region_header, created, slots_box_head, SHARED_MEMORY_REGION_RESERVE) != 0 ||
        (store->region_data.file_descriptor = open(data_path, open_flags, 0644)) < 0 ||
        region_attach(&store->region_data, created, data_size, DATA_RESERVE) != 0 ||
        (store->region_generations.file_descriptor = open(generation_path, open_flags, 0644)) < 0 ||
        (store->region_garbage.file_descriptor = open(garbage_path, open_flags, 0644)) < 0 ||
        region_attach(&store->region_garbage, created,
                      (size_t)GARBAGE_CAPACITY * sizeof(garbage_entry_t),
                      (size_t)GARBAGE_CAPACITY * sizeof(garbage_entry_t)) != 0)
        goto fail;
    store->header = (kvspace_header_t *)store->region_adaptive_radix_tree.base;
    store->adaptive_radix_tree_meta = (blocks_meta_t *)(store->region_adaptive_radix_tree.base + sizeof(kvspace_header_t));
    store->adaptive_radix_tree_data = store->region_adaptive_radix_tree.base + ADAPTIVE_RADIX_TREE_OFFSET;
    store->slots_box_meta = (sbo_meta_t *)store->region_header.base;
    store->slots_box_data = store->region_data.base;
    store->garbage = (garbage_entry_t *)store->region_garbage.base;
    if (created) {
        memset(store->header, 0, sizeof(*store->header));
        store->header->adaptive_radix_tree_slab_size = adaptive_radix_tree_slab;
        store->header->slots_box_head_size = slots_box_head;
        store->header->slots_box_data_size = data_size;
        store->header->adaptive_radix_tree_root = -1;
        if (blocks_init(store->adaptive_radix_tree_meta, adaptive_radix_tree_slab, ADAPTIVE_RADIX_TREE_BLOCK_SIZE) != 0 ||
            sbo_init(store->slots_box_meta, slots_box_head, data_size) != 0)
            goto fail;
        store->adaptive_radix_tree_meta->sizeof_block_head = 8;
        memcpy(store->header->magic, KVSPACE_MAGIC, sizeof(KVSPACE_MAGIC) - 1);
    }
    if (generations_grow(store) != 0)
        goto fail;
    store->recovery_slot = recovery_register_claim(store);
    if (store->recovery_slot == -2) {
        fprintf(stderr, "kvspace: too many processes, N=%d\n", RECOVERY_SLOT_COUNT);
        goto fail;
    }
    if (store->recovery_slot == -3) {
        fprintf(stderr, "kvspace: claim_id exhausted (2^30 opens): "
                        "rebuild the store\n");
        goto fail;
    }
    if (store->recovery_slot < 0)
        goto fail;
    store->process_id = (uint32_t)getpid();
    (void)pthread_once(&g_atfork_once, recovery_atfork_install);
    store->fork_generation = __atomic_load_n(&g_fork_generation, __ATOMIC_ACQUIRE);
    store->quiesced = 1;
    if (store->header->committed_bytes -
            __atomic_load_n(&store->header->freed_bytes, __ATOMIC_ACQUIRE) >=
            recovery_garbage_threshold() &&
        recovery_maintenance_start(store) != 0)
        goto fail;
    return store;
fail:
    recovery_maintenance_stop(store);
    if (store->recovery_slot >= 0 && store->header)
        recovery_register_release(store);
    region_close(&store->region_adaptive_radix_tree);
    region_close(&store->region_header);
    region_close(&store->region_data);
    region_close(&store->region_generations);
    region_close(&store->region_garbage);
    free(store);
    if (created) {
        unlink(path);
        unlink(head_path);
        unlink(data_path);
        unlink(generation_path);
        unlink(garbage_path);
    }
    return NULL;
}
static bool recovery_other_alive(kvspace_t *store) {
    for (int i = 0; i < RECOVERY_SLOT_COUNT; i++) {
        if (i == store->recovery_slot)
            continue;
        recovery_slot_t *slot = &store->header->slots[i];
        if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != RECOVERY_SLOT_MINE)
            continue;
        if (recovery_slot_alive(slot))
            return true;
    }
    return false;
}
static void close_shared_memory_store(kvspace_t *store) {
    if (!store)
        return;
    if (store->process_id == (uint32_t)getpid() && store->recovery_slot >= 0 &&
        !recovery_other_alive(store)) {
#if defined(__linux__)
        __atomic_store_n(&store->header->slots[store->recovery_slot].quiesced, 1u,
                         __ATOMIC_RELEASE);
        (void)recovery_round(store);
#else
        uint64_t garbage_head_index = __atomic_load_n(&store->header->garbage_head, __ATOMIC_ACQUIRE);
        uint64_t garbage_tail_index = __atomic_load_n(&store->header->garbage_tail, __ATOMIC_ACQUIRE);
        for (; garbage_head_index < garbage_tail_index; garbage_head_index++) {
            garbage_entry_t *entry = &store->garbage[garbage_head_index & (GARBAGE_CAPACITY - 1)];
            uint32_t meta_state = __atomic_load_n(&entry->meta, __ATOMIC_ACQUIRE);
            if (GARBAGE_ENTRY_STATE(meta_state) == GARBAGE_ENTRY_COMMITTED) {
                uint64_t offset = __atomic_load_n(&entry->offset, __ATOMIC_ACQUIRE);
                uint64_t capacity = sbo_allocated_size(store->slots_box_meta, offset);
                __atomic_store_n(&entry->meta, GARBAGE_ENTRY_EMPTY, __ATOMIC_RELEASE);
                store_slots_box_free(store, offset);
                __atomic_store_n(&store->header->freed_bytes,
                                 store->header->freed_bytes + capacity, __ATOMIC_RELEASE);
            } else {
                __atomic_store_n(&entry->meta, GARBAGE_ENTRY_EMPTY, __ATOMIC_RELEASE);
            }
        }
        __atomic_store_n(&store->header->garbage_head, garbage_tail_index, __ATOMIC_RELEASE);
#endif
    }
    recovery_maintenance_stop(store);
    recovery_register_release(store);
    region_close(&store->region_adaptive_radix_tree);
    region_close(&store->region_header);
    region_close(&store->region_data);
    region_close(&store->region_generations);
    region_close(&store->region_garbage);
    free(store);
}
static int shared_memory_delete_key(kvspace_t *store, const char *key) {
    bool deleted = false;
    int32_t root = adaptive_radix_tree_delete(store, store->header->adaptive_radix_tree_root, (const uint8_t *)key,
                           (int)strlen(key), 0, &deleted);
    if (root < 0)
        return -1;
    store->header->adaptive_radix_tree_root = root;
    return deleted ? 1 : 0;
}
static char *member_directory(const char *path) {
    return append_separator(path, SEPARATOR_DOT);
}
static int subtree_key(const char *base, size_t base_length, const char *key) {
    if (base_length == 0)
        return separator_at(key, 0, strlen(key)) == SEPARATOR_SLASH;
    if (strncmp(key, base, base_length) != 0)
        return 0;
    const char *rest = key + base_length;
    return rest[0] == 0 ||
           separator_at(rest, 0, strlen(rest)) != SEPARATOR_NONE;
}
static int delete_tree(kvspace_t *store, const char *prefix) {
    if (!store || !prefix || reserved_meta_path(prefix))
        return -1;
    if (store_sync(store) != 0)
        return -1;
    size_t prefix_length = strlen(prefix);
    if (prefix_length >= 1024)
        return -1;
    char base[1024];
    memcpy(base, prefix, prefix_length + 1);
    size_t base_length = prefix_length;
    if (base_length == (size_t)separator_length(SEPARATOR_SLASH) &&
        separator_at(base, 0, base_length) == SEPARATOR_SLASH) {
        base_length = 0;
    } else {
        enum separator_kind kind = ending_separator(base, base_length);
        if (kind != SEPARATOR_NONE)
            base_length -= (size_t)separator_length(kind);
    }
    base[base_length] = 0;
    if (base_length && reserved_meta_path(base))
        return -1;
    keyscan_t keys = {0};
    if (adaptive_radix_tree_scan_keys(store, base_length ? base : separator_table[SEPARATOR_SLASH].bytes,
                      base_length ? (int)base_length : separator_length(SEPARATOR_SLASH),
                      &keys) != 0) {
        keyscan_free(&keys);
        return -1;
    }
    int return_code = 0, any = 0;
    for (int32_t i = 0; i < keys.count; i++) {
        const char *key = keys.keys[i];
        if (!subtree_key(base, base_length, key) || reserved_meta_path(key) ||
            strlen(key) >= 1024)
            continue;
        int deleted = shared_memory_delete_key(store, key);
        if (deleted < 0)
            return_code = -1;
        else if (deleted)
            any = 1;
    }
    keyscan_free(&keys);
    if (any)
        list_generation_bump(store);
    return return_code;
}
static int read_wire(kvspace_t *store, const char *key, const uint8_t **out,
                     uint64_t *out_length) {
    uint64_t offset = 0;
    if (resolve_root(store, key, &offset) != KVSPACE_OK)
        return -1;
    uint8_t *base = storage_data_base(store);
    if (!base)
        return -1;
    const uint8_t *wire = base + offset;
    const kvspaceXvalueHead *head = (const kvspaceXvalueHead *)(const void *)wire;
    uint64_t total = kvspaceXvalueHeadTotalLen(head);
    if (total < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    *out = wire;
    *out_length = total;
    return 0;
}
static int write_wire(kvspace_t *store, const char *key, const uint8_t *wire,
                      uint64_t length) {
    if (length < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    const kvspaceXvalueHead *head = (const kvspaceXvalueHead *)(const void *)wire;
    uint64_t body = kvspaceXvalueHeadLen(head);
    uint64_t attributes = kvspaceXvalueHeadAttrsLen(head);
    if (KVSPACE_XVALUE_HEAD_LEN + body + attributes != length)
        return -1;
    uint64_t offset = 0;
    if (realloc_key(store, key, body, attributes, &offset) != KVSPACE_OK)
        return -1;
    uint8_t *base = storage_data_base(store);
    if (!base)
        return -1;
    uint8_t *destination = base + offset;
    memcpy(destination + KVSPACE_XVALUE_HEAD_LEN, wire + KVSPACE_XVALUE_HEAD_LEN, body);
    if (attributes)
        memcpy(destination + KVSPACE_XVALUE_HEAD_LEN + body,
               wire + KVSPACE_XVALUE_HEAD_LEN + body, attributes);
    return 0;
}
static int copy_key(kvspace_t *store, const char *source, const char *destination) {
    if (!store || !source || !destination)
        return -1;
    const uint8_t *raw = NULL;
    uint64_t raw_length = 0;
    if (read_wire(store, source, &raw, &raw_length) != 0)
        return -1;
    uint8_t *temporary = malloc((size_t)raw_length);
    if (!temporary)
        return -1;
    memcpy(temporary, raw, (size_t)raw_length);
    int return_code = write_wire(store, destination, temporary, raw_length);
    free(temporary);
    return return_code;
}
static int storage_children(kvspace_t *store, const char *key, keyscan_t *out);
static int copy_tree_recursive(kvspace_t *store, const char *source, const char *destination) {
    {
        const uint8_t *raw = NULL;
        uint64_t raw_length = 0;
        if (read_wire(store, source, &raw, &raw_length) == 0) {
            if (copy_key(store, source, destination) != 0)
                return -1;
        }
    }
    if (store_sync(store) != 0)
        return -1;
    char *source_directory = ending_directory(source), *destination_directory = ending_directory(destination);
    char *member_source_directory = member_directory(source), *member_destination_directory = member_directory(destination);
    if (!source_directory || !destination_directory || !member_source_directory || !member_destination_directory) {
        free(source_directory);
        free(destination_directory);
        free(member_source_directory);
        free(member_destination_directory);
        return -1;
    }
    keyscan_t children = {0};
    keyscan_t members = {0};
    int return_code = 0;
    if (storage_children(store, source_directory, &children) == KVSPACE_BACKEND ||
        storage_children(store, member_source_directory, &members) == KVSPACE_BACKEND)
        return_code = -1;
    for (int32_t i = 0; return_code == 0 && i < children.count; i++) {
        char *child_source = path_join(source_directory, children.keys[i]), *child_destination = path_join(destination_directory, children.keys[i]);
        if (!child_source || !child_destination || copy_tree_recursive(store, child_source, child_destination) != 0)
            return_code = -1;
        free(child_source);
        free(child_destination);
    }
    for (int32_t i = 0; return_code == 0 && i < members.count; i++) {
        size_t member_source_length = strlen(member_source_directory), member_destination_length = strlen(member_destination_directory), name_length = strlen(members.keys[i]);
        char *child_source = malloc(member_source_length + name_length + 1);
        char *child_destination = malloc(member_destination_length + name_length + 1);
        if (!child_source || !child_destination) {
            return_code = -1;
        } else {
            memcpy(child_source, member_source_directory, member_source_length);
            memcpy(child_source + member_source_length, members.keys[i], name_length + 1);
            memcpy(child_destination, member_destination_directory, member_destination_length);
            memcpy(child_destination + member_destination_length, members.keys[i], name_length + 1);
            if (copy_tree_recursive(store, child_source, child_destination) != 0)
                return_code = -1;
        }
        free(child_source);
        free(child_destination);
    }
    keyscan_free(&children);
    keyscan_free(&members);
    free(source_directory);
    free(destination_directory);
    free(member_source_directory);
    free(member_destination_directory);
    return return_code;
}
static int copy_tree(kvspace_t *store, const char *source, const char *destination) {
    if (!store || !source || !destination || reserved_meta_path(source) ||
        reserved_meta_path(destination))
        return -1;
    size_t source_length = strlen(source);
    for (;;) {
        enum separator_kind kind = ending_separator(source, source_length);
        if (kind == SEPARATOR_NONE || source_length <= (size_t)separator_length(kind))
            break;
        source_length -= (size_t)separator_length(kind);
    }
    if (source_length == 1 && separator_at(source, 0, source_length) == SEPARATOR_SLASH &&
        separator_at(destination, 0, strlen(destination)) == SEPARATOR_SLASH)
        return strlen(destination) == 1 ? 0 : -1;
    if (strncmp(source, destination, source_length) == 0) {
        const char *rest = destination + source_length;
        size_t rest_length = strlen(rest);
        enum separator_kind kind = separator_at(rest, 0, rest_length);
        if (rest_length == 0 ||
            (kind != SEPARATOR_NONE &&
             rest_length == (size_t)separator_length(kind)))
            return 0;
        if (kind != SEPARATOR_NONE)
            return -1;
    }
    delete_tree(store, destination);
    return copy_tree_recursive(store, source, destination);
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
    int64_t id = blockid_bydataoffset(meta, offset);
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
static int resolve_anchor(kvspace_t *store, const char *key,
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
static uint8_t *storage_data_base(kvspace_t *store) {
    return store ? store->slots_box_data : NULL;
}
static int resolve_root(kvspace_t *store, const char *key,
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
static void storage_write_head(uint8_t *destination, uint64_t body_length,
                               uint64_t attribute_length) {
    kvspaceXvalueHead header;
    memset(&header, 0, sizeof header);
    kvspaceXvalueHeadSetLen(&header, body_length);
    kvspaceXvalueHeadSetAttrsLen(&header, (uint16_t)attribute_length);
    memcpy(destination, &header, sizeof header);
}
static int realloc_key(kvspace_t *store, const char *key,
                             uint64_t body_length, uint64_t attribute_length,
                             uint64_t *out_offset) {
    if (!store || !key || !out_offset)
        return KVSPACE_INVALID;
    *out_offset = 0;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    if (attribute_length > UINT16_MAX)
        return KVSPACE_INVALID;
    uint64_t need = KVSPACE_XVALUE_HEAD_LEN + body_length;
    if (need < body_length || need > UINT64_MAX - attribute_length)
        return KVSPACE_INVALID;
    need += attribute_length;
    if (need > INT32_MAX)
        return KVSPACE_INVALID;
    if (store_sync(store) != 0)
        return KVSPACE_BACKEND;
    adaptive_radix_tree_header_t *node = adaptive_radix_tree_search(store, store->header->adaptive_radix_tree_root, (const uint8_t *)key,
                                 (int)strlen(key));
    uint64_t old_header_value = node ? adaptive_radix_tree_value_load(node) : 0;
    bool retiring = node && ADAPTIVE_RADIX_TREE_VALUE_HAS(old_header_value);
    uint64_t garbage_index = 0;
    uint64_t old_capacity = 0;
    if (retiring) {
        uint64_t offset = ADAPTIVE_RADIX_TREE_VALUE_OFFSET(old_header_value);
        old_capacity = sbo_allocated_size(store->slots_box_meta, offset);
        if (need <= old_capacity) {
            generations_t *generations = generations_at(store, adaptive_radix_tree_id_of(store, node));
            if (generations)
                __atomic_add_fetch(&generations->value_generation, 1u, __ATOMIC_ACQ_REL);
            storage_write_head(store->slots_box_data + offset, body_length, attribute_length);
            if (generations)
                __atomic_add_fetch(&generations->value_generation, 1u, __ATOMIC_ACQ_REL);
            *out_offset = offset;
            return KVSPACE_OK;
        }
        if (garbage_reserve(store, &garbage_index) != 0)
            return KVSPACE_BACKEND;
    }
    uint64_t offset = store_slots_box_alloc(store, (size_t)need);
    if (offset == (uint64_t)-1) {
        if (retiring)
            garbage_cancel(store, garbage_index);
        return KVSPACE_BACKEND;
    }
    memset(store->slots_box_data + offset, 0, (size_t)need);
    storage_write_head(store->slots_box_data + offset, body_length, attribute_length);
    int32_t root = adaptive_radix_tree_insert(store, store->header->adaptive_radix_tree_root, (const uint8_t *)key,
                           (int)strlen(key), 0, offset);
    if (root < 0) {
        if (retiring)
            garbage_cancel(store, garbage_index);
        store_slots_box_free(store, offset);
        return KVSPACE_BACKEND;
    }
    store->header->adaptive_radix_tree_root = root;
    {
        int32_t node_id = recovery_node_at(store, key);
        generations_t *generations = generations_at(store, node_id);
        if (generations) {
            generation_bump_struct(store, node_id);
            generation_bump_value(store, node_id);
        }
    }
    if (retiring)
        garbage_commit(store, garbage_index, ADAPTIVE_RADIX_TREE_VALUE_OFFSET(old_header_value), old_capacity);
    else
        list_generation_bump(store);
    *out_offset = offset;
    return KVSPACE_OK;
}
static int storage_is_index_key(const char *key) {
    size_t length = strlen(key);
    return length > 0 && ending_separator(key, length) != SEPARATOR_NONE;
}
static int storage_has_children(kvspace_t *store, const char *key) {
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
static int free_key(kvspace_t *store, const char *key) {
    if (!store || !key)
        return KVSPACE_INVALID;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    if (store_sync(store) != 0)
        return KVSPACE_BACKEND;
    if (storage_is_index_key(key)) {
        char *directory = ending_directory(key);
        int child_return_code = storage_has_children(store, directory);
        free(directory);
        if (child_return_code != KVSPACE_OK)
            return child_return_code;
    }
    int return_code = shared_memory_delete_key(store, key);
    if (return_code < 0)
        return KVSPACE_BACKEND;
    if (return_code == 0)
        return KVSPACE_MISSING;
    list_generation_bump(store);
    return KVSPACE_OK;
}
static int makeindex_key(kvspace_t *store, const char *key) {
    if (!store || !key)
        return KVSPACE_INVALID;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    if (!storage_is_index_key(key))
        return KVSPACE_INVALID;
    uint64_t offset = 0;
    int return_code = resolve_root(store, key, &offset);
    if (return_code == KVSPACE_OK)
        return KVSPACE_OK;
    if (return_code != KVSPACE_MISSING)
        return return_code;
    return_code = realloc_key(store, key, 0, 0, &offset);
    return return_code;
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
static int storage_children(kvspace_t *store, const char *key, keyscan_t *out) {
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
static int listlen_key(kvspace_t *store, const char *key, int64_t *out_count,
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
static int list_names_raw(kvspace_t *store, const char *key, char *out_names,
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
static int kvspace_data_source_name_path(const char *data_source_name, char *out, size_t out_size) {
    if (!data_source_name)
        return -1;
    const char *separator = strstr(data_source_name, "://");
    if (!separator || separator - data_source_name != 3 || strncmp(data_source_name, "shm", 3) != 0)
        return -1;
    int written = snprintf(out, out_size, "%s", separator + 3);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}
static int kvspace_fail(int status, char *error, uint32_t error_capacity,
                        const char *operation, const char *key) {
    if (error && error_capacity)
        snprintf(error, error_capacity, "kvspace: %s failed at %s", operation,
                 key ? key : "(null)");
    return status;
}
KVSPACE_PUBLIC void *kvspaceConnect(const char *data_source_name) {
    int entry_errno = errno;
    char path[1024];
    if (kvspace_data_source_name_path(data_source_name, path, sizeof path) != 0) {
        errno = EINVAL;
        return NULL;
    }
    void *handle = (void *)open_shared_memory_store(path, kvspace_data_size);
    if (handle != NULL) {
        errno = entry_errno;
        return handle;
    }
    if (errno == 0)
        errno = EIO;
    return NULL;
}
KVSPACE_PUBLIC void kvspaceClose(void *handle) {
    if (handle)
        close_shared_memory_store((kvspace_t *)handle);
}
KVSPACE_PUBLIC uint8_t *kvspaceXvalueInProcessAddress(void *handle) {
    if (!handle)
        return NULL;
    return storage_data_base((kvspace_t *)handle);
}
KVSPACE_PUBLIC int kvspaceResolveOffset(void *handle, const char *key,
                                 const kvspace_anchor_t *anchor,
                                 uint64_t *out_value_offset, char *error,
                                 uint32_t error_capacity) {
    if (!handle || !key || !out_value_offset)
        return KVSPACE_INVALID;
    *out_value_offset = 0;
    int internal = resolve_anchor((kvspace_t *)handle, key, anchor,
                                  out_value_offset);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, error, error_capacity, "resolve", key);
}
KVSPACE_PUBLIC int kvspaceAnchorOf(void *handle, const char *directory_key,
                                   kvspace_anchor_t *out_anchor, char *error,
                                   uint32_t error_capacity) {
    if (!handle || !directory_key || !out_anchor)
        return KVSPACE_INVALID;
    size_t key_len = strlen(directory_key);
    if (!key_text_valid(directory_key))
        return kvspace_fail(KVSPACE_INVALID, error, error_capacity, "anchor",
                            directory_key);
    if (!storage_is_index_key(directory_key))
        return kvspace_fail(KVSPACE_NOT_INDEX, error, error_capacity, "anchor",
                            directory_key);
    kvspace_t *store = (kvspace_t *)handle;
    if (store_sync(store) != 0)
        return kvspace_fail(KVSPACE_BACKEND, error, error_capacity, "anchor",
                            directory_key);
    adaptive_radix_tree_header_t *node = adaptive_radix_tree_search(store, store->header->adaptive_radix_tree_root,
                                 (const uint8_t *)directory_key,
                                 (int)key_len);
    if (!node || !ADAPTIVE_RADIX_TREE_VALUE_HAS(adaptive_radix_tree_value_load(node)))
        return kvspace_fail(KVSPACE_MISSING, error, error_capacity, "anchor",
                            directory_key);
    int32_t id = adaptive_radix_tree_id_of(store, node);
    if (id < 0)
        return kvspace_fail(KVSPACE_BACKEND, error, error_capacity, "anchor",
                            directory_key);
    uint64_t offset = (uint64_t)blockdata_offset(store->adaptive_radix_tree_meta, (uint64_t)id);
    if (offset == 0)
        return kvspace_fail(KVSPACE_BACKEND, error, error_capacity, "anchor",
                            directory_key);
    out_anchor->node_offset = offset;
    out_anchor->epoch = __atomic_load_n(&store->header->generation_epoch, __ATOMIC_ACQUIRE);
    out_anchor->depth = (uint32_t)key_len;
    out_anchor->flags = KVSPACE_ANCHOR_FLAG_INDEX;
    return KVSPACE_OK;
}
KVSPACE_PUBLIC int kvspaceRealloc(void *handle, const char *key,
                                 uint64_t body_length, uint64_t attribute_length,
                                 uint64_t *out_value_offset, char *error,
                                 uint32_t error_capacity) {
    if (!handle || !key || !out_value_offset)
        return KVSPACE_INVALID;
    *out_value_offset = 0;
    int internal = realloc_key((kvspace_t *)handle, key,
                                            body_length, attribute_length,
                                            out_value_offset);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, error, error_capacity, "realloc",
                        key);
}
KVSPACE_PUBLIC int kvspaceFree(void *handle, const char *key, char *error,
                               uint32_t error_capacity) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    int internal = free_key((kvspace_t *)handle, key);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, error, error_capacity, "free", key);
}
KVSPACE_PUBLIC int kvspaceCp(void *handle, const char *old_key,
                             const char *new_key, char *error,
                             uint32_t error_capacity) {
    if (!handle || !old_key || !new_key)
        return KVSPACE_INVALID;
    if (copy_key((kvspace_t *)handle, old_key, new_key) != 0)
        return kvspace_fail(KVSPACE_BACKEND, error, error_capacity, "cp", old_key);
    return KVSPACE_OK;
}
KVSPACE_PUBLIC int kvspaceMkindex(void *handle, const char *key, char *error,
                                  uint32_t error_capacity) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    int internal = makeindex_key((kvspace_t *)handle, key);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, error, error_capacity, "mkindex",
                        key);
}
KVSPACE_PUBLIC int kvspaceDeltree(void *handle, const char *key, char *error,
                                  uint32_t error_capacity) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    if (delete_tree((kvspace_t *)handle, key) != 0)
        return kvspace_fail(KVSPACE_BACKEND, error, error_capacity, "deltree", key);
    return KVSPACE_OK;
}
KVSPACE_PUBLIC int kvspaceCpdir(void *handle, const char *source_key,
                                const char *destination_key, char *error,
                                uint32_t error_capacity) {
    if (!handle || !source_key || !destination_key)
        return KVSPACE_INVALID;
    if (copy_tree((kvspace_t *)handle, source_key, destination_key) != 0)
        return kvspace_fail(KVSPACE_BACKEND, error, error_capacity, "cpdir", source_key);
    return KVSPACE_OK;
}
KVSPACE_PUBLIC int kvspaceListlen(void *handle, const char *key,
                                  int64_t *out_count,
                                  uint32_t *out_max_name_length, char *error,
                                  uint32_t error_capacity) {
    if (!handle || !key || !out_count || !out_max_name_length)
        return KVSPACE_INVALID;
    *out_count = 0;
    *out_max_name_length = 0;
    int internal = listlen_key((kvspace_t *)handle, key, out_count,
                                            out_max_name_length);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, error, error_capacity, "listlen",
                        key);
}
KVSPACE_PUBLIC int kvspaceList(void *handle, const char *key, char *out_names,
                               uint32_t out_capacity, uint32_t *out_length,
                               uint32_t *out_count, uint64_t *out_generation,
                               char *error, uint32_t error_capacity) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    int internal = list_names_raw((kvspace_t *)handle, key, out_names,
                                         out_capacity, out_length, out_count,
                                         out_generation);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, error, error_capacity, "list", key);
}

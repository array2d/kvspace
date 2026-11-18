#ifndef KVSPACE_INTERNAL_H
#define KVSPACE_INTERNAL_H
#include <blockmalloc/blockmalloc.h>
#include <slotsboxmalloc/slotsboxobj.h>
#include <kvspace/kvspace.h>
#include <kvspace/kvspace_xvalue.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
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
#define RETIREMENT_CAPACITY (1u << 21)
#define RETIREMENT_THRESHOLD (64ull << 20)
#define RECOVERY_BARRIER_MILLISECONDS_DEFAULT 200
#define RETIREMENT_MAX_CLAIM ((1u << 30) - 1u)
typedef struct {
    uint64_t offset;
    uint32_t epoch;
    uint32_t meta;
} retirement_entry_t;
enum { RETIREMENT_ENTRY_EMPTY = 0, RETIREMENT_ENTRY_RESERVED = 1, RETIREMENT_ENTRY_COMMITTED = 2 };
#define RING_ENTRY_STATE(metadata) ((uint32_t)((metadata) & 3u))
#define RING_ENTRY_CLAIM(metadata) ((uint32_t)((metadata) >> 2))
#define RING_ENTRY_MAKE(state,claim_id) ((((uint32_t)(claim_id)) << 2) | (uint32_t)(state))
typedef struct {
    char magic[sizeof(KVSPACE_MAGIC)];
    uint64_t adaptive_radix_tree_slab_size, slots_box_head_size, slots_box_data_size;
    int32_t adaptive_radix_tree_root;
    uint64_t structure_epoch;
    uint32_t recovery_epoch;
    uint32_t ack_sequence;
    uint32_t barrier_state;
    uint64_t retirement_head, retirement_tail;
    uint64_t committed_bytes;
    uint64_t freed_bytes;
    uint64_t retirement_freeing;
    uint64_t list_generation;
    uint32_t claim_sequence;
    recovery_slot_t slots[RECOVERY_SLOT_COUNT];
} kvspace_header_t;
_Static_assert(ADAPTIVE_RADIX_TREE_SLAB_INITIAL /
                       (ADAPTIVE_RADIX_TREE_BLOCK_SIZE + 2) >
                   32767ULL / 4,
               "ADAPTIVE_RADIX_TREE_SLAB_INITIAL too small: blockmalloc would "
               "pick 14-bit ids");
_Static_assert(ADAPTIVE_RADIX_TREE_RESERVE /
                       (ADAPTIVE_RADIX_TREE_BLOCK_SIZE + 8) <=
                   (1ULL << 30),
               "ADAPTIVE_RADIX_TREE_RESERVE too large for 30-bit block ids");
_Static_assert((ADAPTIVE_RADIX_TREE_BLOCK_SIZE + 8) % 8 == 0,
               "ADAPTIVE_RADIX_TREE stride must keep 8B alignment");
#define ADAPTIVE_RADIX_TREE_OFFSET (sizeof(kvspace_header_t) + sizeof(blocks_meta_t))
#define SLOTS_BOX_HEAD_FIXED \
    (sizeof(sbo_meta_t) + sizeof(blocks_meta_t) + sizeof(sbo_lock_t))
typedef struct {
    int file_descriptor;
    uint8_t *base;
    size_t mapped;
    size_t reserve;
} shared_memory_region_t;
struct kvspace {
    shared_memory_region_t region_adaptive_radix_tree, region_header, region_data, region_retirement;
    retirement_entry_t *retirement;
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
_Static_assert(offsetof(blocks_meta_t, used_blocks) ==
                   offsetof(blocks_meta_t, block_size) + 2 * sizeof(uint64_t),
               "blockmalloc: sizeof_block_head/malloc_blocks must share one u64 word");
#ifdef BLOCKMALLOC_COUNT_MASK
static inline void kvspace_block_set_malloc_blocks(blocks_meta_t *meta, uint64_t count) {
    blocks_set_malloc_count(meta, count);
}
#else
#define KVSPACE_BLOCKMALLOC_WORD_OFFSET (offsetof(blocks_meta_t, block_size) + sizeof(uint64_t))
static inline unsigned kvspace_block_count_shift(void) {
    blocks_meta_t metadata;
    memset(&metadata, 0, sizeof metadata);
    metadata.malloc_blocks = 1;
    uint64_t word;
    memcpy(&word, (const char *)&metadata + KVSPACE_BLOCKMALLOC_WORD_OFFSET, sizeof word);
    return word ? (unsigned)__builtin_ctzll(word) : 64u;
}
static inline void kvspace_block_set_malloc_blocks(blocks_meta_t *meta, uint64_t count) {
    uint64_t *word = (uint64_t *)((char *)meta + KVSPACE_BLOCKMALLOC_WORD_OFFSET);
    const unsigned shift = kvspace_block_count_shift();
    const uint64_t mask = ((1ULL << 60) - 1) << shift;
    uint64_t old = __atomic_load_n(word, __ATOMIC_RELAXED);
    __atomic_store_n(word, (old & ~mask) | ((count << shift) & mask), __ATOMIC_RELEASE);
}
#endif

#ifdef BLOCKMALLOC_COUNT_MASK
#define KVSPACE_BLOCKMALLOC_MALLOC_BLOCKS(metadata) blocks_malloc_count(metadata)
#define KVSPACE_BLOCKMALLOC_HEAD_SIZE(metadata) blocks_head_size(metadata)
#else
#define KVSPACE_BLOCKMALLOC_MALLOC_BLOCKS(metadata) ((uint64_t)(metadata)->malloc_blocks)
#define KVSPACE_BLOCKMALLOC_HEAD_SIZE(metadata) ((uint64_t)(metadata)->sizeof_block_head)
#endif
static inline int64_t kvspace_block_offset(const blocks_meta_t *meta, uint64_t block_id) {
    return (int64_t)(block_id * (uint64_t)(meta->sizeof_block_head + meta->block_size));
}
static inline int64_t kvspace_block_data_offset(const blocks_meta_t *meta, uint64_t block_id) {
    return kvspace_block_offset(meta, block_id) + meta->sizeof_block_head;
}
static inline int64_t kvspace_block_id_by_data_offset(const blocks_meta_t *meta, uint64_t data_offset) {
    uint64_t block_size = (uint64_t)meta->sizeof_block_head + meta->block_size;
    uint64_t block_offset = data_offset - (uint64_t)meta->sizeof_block_head;
    if (block_offset % block_size != 0)
        return -1;
    return (int64_t)(block_offset / block_size);
}
static inline blocks_meta_t *kvspace_slots_box_pool_meta(sbo_meta_t *meta) {
    return (blocks_meta_t *)((uint8_t *)(meta + 1));
}
static inline sbo_lock_t *kvspace_slots_box_pool_lock(sbo_meta_t *meta) {
    return (sbo_lock_t *)((uint8_t *)(meta + 1) + meta->root_slots * sizeof(blocks_meta_t));
}
static inline uint8_t *kvspace_slots_box_pool_memory(sbo_meta_t *meta) {
    return (uint8_t *)(meta + 1) + meta->root_slots * (sizeof(blocks_meta_t) + sizeof(sbo_lock_t));
}
static inline sbo_box_t *kvspace_slots_box_box(sbo_meta_t *meta, int32_t id) {
    return (sbo_box_t *)(kvspace_slots_box_pool_memory(meta) + (size_t)id * meta->block_stride + meta->box_off);
}
static inline uint16_t kvspace_sbo_combined(const sbo_box_t *box) {
    uint16_t own = box->max_obj_cap ? sbo_enc(box->objlevel, box->max_obj_cap) : 0;
    return own > box->child_max_enc ? own : box->child_max_enc;
}
static inline void kvspace_sbo_box_format(sbo_box_t *box, uint8_t objlevel,
                                          int32_t parent, uint8_t parent_slot,
                                          int32_t self_id) {
    box->free_mask = ~0ULL;
    box->child_mask = 0;
    box->parent = parent;
    box->self_id = self_id;
    box->child_max_enc = 0;
    box->objlevel = objlevel;
    box->max_obj_cap = SBO_MULT_MAX;
    box->hint_child = 0;
    box->parent_slot = parent_slot;
    box->child_max_count = 0;
    memset(box->states, 0, sizeof(box->states));
}
int store_sync(kvspace_t *store);
int region_map(shared_memory_region_t *region, size_t size);
void kvspace_block_meta_lock(blocks_meta_t *meta);
void kvspace_block_meta_unlock(blocks_meta_t *meta);
void kvspace_block_mark_used(blocks_meta_t *meta, void *block_pointer);
#endif

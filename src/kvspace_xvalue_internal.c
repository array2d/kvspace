#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kvspace_xvalue_internal.h"
#include "kvspace_recovery.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
_Static_assert(
    SLOTS_BOX_HEAD_POOL_INITIAL / (sizeof(sbo_box_t) + 2) > 32767ULL / 4,
    "SLOTS_BOX_HEAD_POOL_INITIAL too small: blockmalloc would pick 14-bit ids");
static blocks_meta_t *slots_box_pool(sbo_meta_t *meta) {
    return kvspace_slots_box_pool_meta(meta);
}
static sbo_lock_t *slots_box_pool_lock(sbo_meta_t *meta) {
    return kvspace_slots_box_pool_lock(meta);
}
static uint8_t *slots_box_pool_memory(sbo_meta_t *meta) {
    return kvspace_slots_box_pool_memory(meta);
}
static sbo_box_t *slots_box_box(sbo_meta_t *meta, int32_t id) {
    return kvspace_slots_box_box(meta, id);
}
static bool slots_box_pool_full(sbo_meta_t *meta) {
    blocks_meta_t *pool = slots_box_pool(meta);
    if (pool->free_next_id != -1)
        return false;
    uint64_t next_end = (uint64_t)kvspace_block_offset(pool, KVSPACE_BLOCKMALLOC_MALLOC_BLOCKS(pool)) +
                        KVSPACE_BLOCKMALLOC_HEAD_SIZE(pool) + pool->block_size;
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
    __atomic_store_n(&store->header->slots_box_head_size, SLOTS_BOX_HEAD_FIXED + want, __ATOMIC_RELEASE);
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
    kvspace_sbo_box_format(root, level, -1, 255, 0);
    root->free_mask = ~1ULL;
    root->child_mask = 1ULL;
    root->states[0] = SBO_BOX;
    root->children[0] = (int32_t)node_id;
    uint16_t box_combined = kvspace_sbo_combined(box);
    root->child_cap[0] = box_combined;
    root->child_max_enc = box_combined;
    root->child_max_count = 1;
    root->max_obj_cap = SBO_MULT_MAX;
    root->hint_child = 0;
    meta->data_size = want;
    meta->slot_shift = (uint8_t)__builtin_ctzll(want);
    __atomic_store_n(&store->header->slots_box_data_size, want, __ATOMIC_RELEASE);
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
uint64_t store_slots_box_alloc(kvspace_t *store, size_t size) {
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
void store_slots_box_free(kvspace_t *store, uint64_t offset) {
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
uint64_t retirement_threshold(void) {
    return RETIREMENT_THRESHOLD;
}
static void retirement_full_report(kvspace_t *store) {
    static bool once;
    if (__atomic_exchange_n(&once, true, __ATOMIC_RELAXED))
        return;
    uint64_t retirement_head_index = __atomic_load_n(&store->header->retirement_head, __ATOMIC_ACQUIRE);
    uint64_t retirement_tail_index = __atomic_load_n(&store->header->retirement_tail, __ATOMIC_ACQUIRE);
    uint32_t meta_state = __atomic_load_n(&store->retirement[retirement_head_index & (RETIREMENT_CAPACITY - 1)].meta,
                                 __ATOMIC_ACQUIRE);
    uint64_t committed = __atomic_load_n(&store->header->committed_bytes, __ATOMIC_ACQUIRE);
    uint64_t freed = __atomic_load_n(&store->header->freed_bytes, __ATOMIC_ACQUIRE);
    fprintf(stderr,
            "kvspace: retirement ring full (cap=%llu, head=%llu, tail=%llu, "
            "bytes=%llu): head entry #%llu state=%u claim=%u is holding "
            "the drain (no reclaim round yet)\n",
            (unsigned long long)RETIREMENT_CAPACITY, (unsigned long long)retirement_head_index,
            (unsigned long long)retirement_tail_index,
            (unsigned long long)(committed > freed ? committed - freed : 0),
            (unsigned long long)retirement_head_index, RING_ENTRY_STATE(meta_state), RING_ENTRY_CLAIM(meta_state));
}
int retirement_reserve(kvspace_t *store, uint64_t *index) {
    uint32_t claim = store->recovery_slot >= 0 ? store->header->slots[store->recovery_slot].claim_id : 0;
    if (!claim)
        return -1;
    uint64_t retirement_head_index = __atomic_load_n(&store->header->retirement_head, __ATOMIC_ACQUIRE);
    uint64_t retirement_tail_index = __atomic_load_n(&store->header->retirement_tail, __ATOMIC_ACQUIRE);
    if (retirement_tail_index - retirement_head_index >= RETIREMENT_CAPACITY) {
        retirement_full_report(store);
        return -1;
    }
    if (store->reclaim_unavailable &&
        RETIREMENT_CAPACITY - (retirement_tail_index - retirement_head_index) < RETIREMENT_CAPACITY / 4) {
        retirement_full_report(store);
        return -1;
    }
    retirement_entry_t *entry = &store->retirement[retirement_tail_index & (RETIREMENT_CAPACITY - 1)];
    uint32_t meta_state = __atomic_load_n(&entry->meta, __ATOMIC_ACQUIRE);
    if (meta_state != RETIREMENT_ENTRY_EMPTY) {
        if (RING_ENTRY_STATE(meta_state) != RETIREMENT_ENTRY_RESERVED || recovery_claim_alive(store, RING_ENTRY_CLAIM(meta_state)))
            return -1;
        __atomic_store_n(&entry->meta, RETIREMENT_ENTRY_EMPTY, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&entry->meta, RING_ENTRY_MAKE(RETIREMENT_ENTRY_RESERVED, claim), __ATOMIC_RELEASE);
    __atomic_store_n(&store->header->retirement_tail, retirement_tail_index + 1, __ATOMIC_RELEASE);
    *index = retirement_tail_index;
    return 0;
}
void retirement_commit(kvspace_t *store, uint64_t index, uint64_t offset,
                        uint64_t bytes_retired) {
    retirement_entry_t *entry = &store->retirement[index & (RETIREMENT_CAPACITY - 1)];
    uint32_t claim = RING_ENTRY_CLAIM(__atomic_load_n(&entry->meta, __ATOMIC_ACQUIRE));
    __atomic_store_n(&entry->offset, offset, __ATOMIC_RELAXED);
    __atomic_store_n(&entry->epoch, __atomic_load_n(&store->header->recovery_epoch, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELAXED);
    uint64_t committed = __atomic_add_fetch(&store->header->committed_bytes, bytes_retired,
                                            __ATOMIC_ACQ_REL);
    __atomic_store_n(&entry->meta, RING_ENTRY_MAKE(RETIREMENT_ENTRY_COMMITTED, claim), __ATOMIC_RELEASE);
    uint64_t freed = __atomic_load_n(&store->header->freed_bytes, __ATOMIC_ACQUIRE);
    if (committed > freed && committed - freed >= retirement_threshold())
        (void)recovery_maintenance_start(store);
}
void retirement_cancel(kvspace_t *store, uint64_t index) {
    __atomic_store_n(&store->retirement[index & (RETIREMENT_CAPACITY - 1)].meta, RETIREMENT_ENTRY_EMPTY,
                     __ATOMIC_RELEASE);
}
int retirement_drain(kvspace_t *store, uint32_t epoch) {
    int freed = 0;
    uint64_t i = __atomic_load_n(&store->header->retirement_head, __ATOMIC_ACQUIRE);
    for (;;) {
        if (i >= __atomic_load_n(&store->header->retirement_tail, __ATOMIC_ACQUIRE))
            goto out;
        retirement_entry_t *entry = &store->retirement[i & (RETIREMENT_CAPACITY - 1)];
        uint32_t meta_state = __atomic_load_n(&entry->meta, __ATOMIC_ACQUIRE);
        uint32_t retirement_state = RING_ENTRY_STATE(meta_state);
        if (retirement_state == RETIREMENT_ENTRY_EMPTY) {
            __atomic_store_n(&store->header->retirement_head, i + 1, __ATOMIC_RELEASE);
            i++;
            continue;
        }
        if (retirement_state == RETIREMENT_ENTRY_RESERVED) {
            if (recovery_claim_alive(store, RING_ENTRY_CLAIM(meta_state)))
                goto out;
            __atomic_store_n(&entry->meta, RETIREMENT_ENTRY_EMPTY, __ATOMIC_RELEASE);
            fprintf(stderr,
                    "kvspace: retirement entry %llu reserved by dead claim %u: "
                    "leaked\n", (unsigned long long)i, RING_ENTRY_CLAIM(meta_state));
            __atomic_store_n(&store->header->retirement_head, i + 1, __ATOMIC_RELEASE);
            i++;
            continue;
        }
        if (retirement_state != RETIREMENT_ENTRY_COMMITTED)
            goto out;
        if (!((int32_t)(epoch - __atomic_load_n(&entry->epoch, __ATOMIC_ACQUIRE)) > 0))
            goto out;
        uint64_t offset = __atomic_load_n(&entry->offset, __ATOMIC_ACQUIRE);
        if (offset >= store->region_data.mapped && store_sync(store) != 0)
            goto out;
        uint64_t capacity = sbo_allocated_size(store->slots_box_meta, offset);
        __atomic_store_n(&store->header->retirement_freeing, offset, __ATOMIC_RELEASE);
        __atomic_store_n(&entry->meta, RETIREMENT_ENTRY_EMPTY, __ATOMIC_RELEASE);
        __atomic_store_n(&store->header->retirement_head, i + 1, __ATOMIC_RELEASE);
        store_slots_box_free(store, offset);
        __atomic_add_fetch(&store->header->freed_bytes, capacity, __ATOMIC_RELEASE);
        i++;
        freed++;
    }
out:
    __atomic_store_n(&store->header->retirement_freeing, 0, __ATOMIC_RELEASE);
    return freed;
}
void storage_write_head(uint8_t *destination, uint64_t body_length,
                               uint64_t attribute_length) {
    kvspaceXvalueHead header;
    memset(&header, 0, sizeof header);
    kvspaceXvalueHeadSetLen(&header, body_length);
    kvspaceXvalueHeadSetAttrsLen(&header, (uint16_t)attribute_length);
    memcpy(destination, &header, sizeof header);
}
void retirement_release_all(kvspace_t *store) {
    uint64_t retirement_head_index = __atomic_load_n(&store->header->retirement_head, __ATOMIC_ACQUIRE);
    uint64_t retirement_tail_index = __atomic_load_n(&store->header->retirement_tail, __ATOMIC_ACQUIRE);
    for (; retirement_head_index < retirement_tail_index; retirement_head_index++) {
        retirement_entry_t *entry = &store->retirement[retirement_head_index & (RETIREMENT_CAPACITY - 1)];
        uint32_t meta_state = __atomic_load_n(&entry->meta, __ATOMIC_ACQUIRE);
        if (RING_ENTRY_STATE(meta_state) == RETIREMENT_ENTRY_COMMITTED) {
            uint64_t offset = __atomic_load_n(&entry->offset, __ATOMIC_ACQUIRE);
            uint64_t capacity = sbo_allocated_size(store->slots_box_meta, offset);
            __atomic_store_n(&entry->meta, RETIREMENT_ENTRY_EMPTY, __ATOMIC_RELEASE);
            store_slots_box_free(store, offset);
            __atomic_add_fetch(&store->header->freed_bytes, capacity, __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&entry->meta, RETIREMENT_ENTRY_EMPTY, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&store->header->retirement_head, retirement_tail_index, __ATOMIC_RELEASE);
}

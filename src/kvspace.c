#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define BLOCKMALLOC_IMPLEMENTATION
#include <blockmalloc/blockmalloc.h>
#define SBO_MIN_SHARD_BYTES (1ULL << 40)
#define SLOTSBOXMALLOC_IMPLEMENTATION
#include "slotsboxmalloc/slotsboxobj.h"
#undef BLOCKMALLOC_IMPLEMENTATION
#undef SLOTSBOXMALLOC_IMPLEMENTATION
#include <kvspace/kvspace.h>
#include <kvspace/kvspace_xvalue.h>
#include "kvspace_internal.h"
#include "kvspace_key.h"
#include "kvspace_xvalue_internal.h"
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
static size_t kvspace_data_size = 8ULL * 64 * 64 * 64 * 64;
void kvspace_block_meta_lock(blocks_meta_t *meta) {
    spin_lock(&meta->lock);
}
void kvspace_block_meta_unlock(blocks_meta_t *meta) {
    spin_unlock(&meta->lock);
}
void kvspace_block_mark_used(blocks_meta_t *meta, void *block_pointer) {
    setblock_t(meta, block_pointer, 1, -1);
}
static uint8_t *storage_data_base(kvspace_t *store);
static int realloc_key(kvspace_t *store, const char *key, uint64_t body_length,
                       uint64_t attribute_length, uint64_t *out_offset);
static inline void list_generation_bump(kvspace_t *store) {
    __atomic_add_fetch(&store->header->list_generation, 1ull, __ATOMIC_RELEASE);
}
int region_map(shared_memory_region_t *region, size_t size) {
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
bool recovery_claim_alive(kvspace_t *store, uint32_t claim) {
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
    int freed = garbage_drain(store, epoch);
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
            garbage_threshold())
            continue;
        recovery_round(store);
    }
    return NULL;
}
int recovery_maintenance_start(kvspace_t *store) {
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
int store_sync(kvspace_t *store) {
    if (region_map(&store->region_adaptive_radix_tree, ADAPTIVE_RADIX_TREE_OFFSET + (size_t)store->header->adaptive_radix_tree_slab_size) != 0)
        return -1;
    if (store->adaptive_radix_tree_meta->total_size != store->adaptive_radix_tree_total_seen &&
        generations_grow(store) != 0)
        return -1;
    if (region_map(&store->region_header, (size_t)store->header->slots_box_head_size) != 0)
        return -1;
    return region_map(&store->region_data, (size_t)store->header->slots_box_data_size);
}
static bool blockmalloc_layout_ok(void) {
    blocks_meta_t metadata;
    memset(&metadata, 0, sizeof metadata);
    metadata.sizeof_block_head = 8;
    metadata.malloc_blocks = 5;
    metadata.used_blocks = 7;
    kvspace_block_set_malloc_blocks(&metadata, 0x0123456789ABCDEull);
    return metadata.sizeof_block_head == 8 && metadata.malloc_blocks == 0x0123456789ABCDEull &&
           metadata.block_size == 0 && metadata.used_blocks == 7;
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
            garbage_threshold() &&
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
        garbage_release_all(store);
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
static uint8_t *storage_data_base(kvspace_t *store) {
    return store ? store->slots_box_data : NULL;
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

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
#include "kvspace_recovery.h"
#include "kvspace_xvalue_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
static int realloc_key_locked(kvspace_t *store, const char *key, uint64_t body_length,
                              uint64_t attribute_length, uint64_t *out_offset);
static inline void list_generation_bump(kvspace_t *store) {
    __atomic_add_fetch(&store->header->list_generation, 1ull, __ATOMIC_RELEASE);
}
int region_map(shared_memory_region_t *region, size_t size) {
    if (size > region->reserve)
        return -1;
    if (size <= __atomic_load_n(&region->mapped, __ATOMIC_ACQUIRE))
        return 0;
    if (mmap(region->base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
             region->file_descriptor, 0) == MAP_FAILED)
        return -1;
    __atomic_store_n(&region->mapped, size, __ATOMIC_RELEASE);
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
#ifdef RECOVERY_TEST_HOOKS
void kvspaceTestSetDataSize(size_t data_size) {
    kvspace_data_size = data_size;
}
#endif
int store_sync(kvspace_t *store) {
    size_t art_size = ADAPTIVE_RADIX_TREE_OFFSET +
                      (size_t)__atomic_load_n(&store->header->adaptive_radix_tree_slab_size, __ATOMIC_ACQUIRE);
    size_t head_size = (size_t)__atomic_load_n(&store->header->slots_box_head_size, __ATOMIC_ACQUIRE);
    size_t data_size = (size_t)__atomic_load_n(&store->header->slots_box_data_size, __ATOMIC_ACQUIRE);
    if (art_size <= __atomic_load_n(&store->region_adaptive_radix_tree.mapped, __ATOMIC_ACQUIRE) &&
        head_size <= __atomic_load_n(&store->region_header.mapped, __ATOMIC_ACQUIRE) &&
        data_size <= __atomic_load_n(&store->region_data.mapped, __ATOMIC_ACQUIRE))
        return 0;
    pthread_mutex_lock(&store->map_lock);
    int return_code = 0;
    if (region_map(&store->region_adaptive_radix_tree, art_size) != 0 ||
        region_map(&store->region_header, head_size) != 0 ||
        region_map(&store->region_data, data_size) != 0)
        return_code = -1;
    pthread_mutex_unlock(&store->map_lock);
    return return_code;
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
    if (!blockmalloc_layout_ok()) {
        fprintf(stderr, "kvspace: unexpected blockmalloc blocks_meta_t layout\n");
        errno = EINVAL;
        return NULL;
    }
    char head_path[PATH_MAX], data_path[PATH_MAX], retirement_path[PATH_MAX];
    if (snprintf(head_path, sizeof head_path, "%s.sbo.head", path) >=
            (int)sizeof head_path ||
        snprintf(data_path, sizeof data_path, "%s.sbo.data", path) >=
            (int)sizeof data_path ||
        snprintf(retirement_path, sizeof retirement_path, "%s.retirement", path) >=
            (int)sizeof retirement_path) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    kvspace_t *store = calloc(1, sizeof(*store));
    if (!store)
        return NULL;
    if (pthread_mutex_init(&store->local_write_lock, NULL) != 0) {
        free(store);
        return NULL;
    }
    if (pthread_mutex_init(&store->map_lock, NULL) != 0) {
        pthread_mutex_destroy(&store->local_write_lock);
        free(store);
        return NULL;
    }
    store->locks_initialized = 1;
    store->recovery_slot = -1;
    store->region_adaptive_radix_tree.file_descriptor = store->region_header.file_descriptor = store->region_data.file_descriptor =
        store->region_retirement.file_descriptor = -1;
    bool created = false;
    store->region_adaptive_radix_tree.file_descriptor = open(path, O_RDWR);
    if (store->region_adaptive_radix_tree.file_descriptor < 0) {
        if (!slots_box_data_size_ok(data_size)) {
            errno = EINVAL;
            goto fail;
        }
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
            memcmp(temporary.magic, KVSPACE_MAGIC, sizeof(KVSPACE_MAGIC) - 1) != 0) {
            errno = EINVAL;
            goto fail;
        }
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
        (store->region_retirement.file_descriptor = open(retirement_path, open_flags, 0644)) < 0 ||
        region_attach(&store->region_retirement, created,
                      (size_t)RETIREMENT_CAPACITY * sizeof(retirement_entry_t),
                      (size_t)RETIREMENT_CAPACITY * sizeof(retirement_entry_t)) != 0)
        goto fail;
    store->header = (kvspace_header_t *)store->region_adaptive_radix_tree.base;
    store->adaptive_radix_tree_meta = (blocks_meta_t *)(store->region_adaptive_radix_tree.base + sizeof(kvspace_header_t));
    store->adaptive_radix_tree_data = store->region_adaptive_radix_tree.base + ADAPTIVE_RADIX_TREE_OFFSET;
    store->slots_box_meta = (sbo_meta_t *)store->region_header.base;
    store->slots_box_data = store->region_data.base;
    store->retirement = (retirement_entry_t *)store->region_retirement.base;
    if (created) {
        memset(store->header, 0, sizeof(*store->header));
        store->header->adaptive_radix_tree_slab_size = adaptive_radix_tree_slab;
        store->header->slots_box_head_size = slots_box_head;
        store->header->slots_box_data_size = data_size;
        store->header->adaptive_radix_tree_root = -1;
        if (blocks_init(store->adaptive_radix_tree_meta, adaptive_radix_tree_slab, ADAPTIVE_RADIX_TREE_BLOCK_SIZE) != 0 ||
            sbo_init(store->slots_box_meta, slots_box_head, data_size) != 0) {
            errno = EIO;
            goto fail;
        }
        store->adaptive_radix_tree_meta->sizeof_block_head = 8;
        memcpy(store->header->magic, KVSPACE_MAGIC, sizeof(KVSPACE_MAGIC) - 1);
    }
    store->recovery_slot = recovery_register_claim(store);
    if (store->recovery_slot == -2) {
        fprintf(stderr, "kvspace: too many processes, N=%d\n", RECOVERY_SLOT_COUNT);
        errno = EMFILE;
        goto fail;
    }
    if (store->recovery_slot == -3) {
        fprintf(stderr, "kvspace: claim_id exhausted (2^30 opens): "
                        "rebuild the store\n");
        errno = EOVERFLOW;
        goto fail;
    }
    if (store->recovery_slot < 0) {
        errno = EIO;
        goto fail;
    }
    store->process_id = (uint32_t)getpid();
    store->fork_generation = recovery_fork_generation();
    uint64_t committed = __atomic_load_n(&store->header->committed_bytes, __ATOMIC_ACQUIRE);
    uint64_t freed = __atomic_load_n(&store->header->freed_bytes, __ATOMIC_ACQUIRE);
    if (committed > freed && committed - freed >= retirement_threshold() &&
        recovery_maintenance_start(store) != 0) {
        errno = EAGAIN;
        goto fail;
    }
    return store;
fail:
    {
        int saved_errno = errno;
        recovery_maintenance_stop(store);
        if (store->recovery_slot >= 0 && store->header)
            recovery_register_release(store);
        region_close(&store->region_adaptive_radix_tree);
        region_close(&store->region_header);
        region_close(&store->region_data);
        region_close(&store->region_retirement);
        if (store->locks_initialized) {
            pthread_mutex_destroy(&store->map_lock);
            pthread_mutex_destroy(&store->local_write_lock);
        }
        free(store);
        if (created) {
            unlink(path);
            unlink(head_path);
            unlink(data_path);
            unlink(retirement_path);
        }
        errno = saved_errno ? saved_errno : EIO;
    }
    return NULL;
}
static void close_shared_memory_store(kvspace_t *store) {
    if (store->process_id == (uint32_t)getpid() && store->recovery_slot >= 0) {
        __atomic_store_n(&store->header->slots[store->recovery_slot].quiesced, 1u,
                         __ATOMIC_RELEASE);
        recovery_ack_progress(store);
        if (!recovery_other_alive(store)) {
#if defined(__linux__)
            (void)recovery_round(store);
#else
            retirement_release_all(store);
#endif
        }
    }
    recovery_maintenance_stop(store);
    recovery_register_release(store);
    region_close(&store->region_adaptive_radix_tree);
    region_close(&store->region_header);
    region_close(&store->region_data);
    region_close(&store->region_retirement);
    if (store->locks_initialized) {
        pthread_mutex_destroy(&store->map_lock);
        pthread_mutex_destroy(&store->local_write_lock);
    }
    free(store);
}
static int shared_memory_delete_key_locked(kvspace_t *store, const char *key) {
    uint64_t existing_offset = 0;
    int locate = resolve_root_locked(store, key, &existing_offset);
    if (locate != KVSPACE_OK)
        return locate == KVSPACE_MISSING ? 0 : -1;
    uint64_t retirement_index = 0;
    if (retirement_reserve(store, &retirement_index) != 0)
        return -1;
    bool deleted = false;
    uint64_t retired_offset = 0;
    int32_t current_root = __atomic_load_n(&store->header->adaptive_radix_tree_root, __ATOMIC_ACQUIRE);
    int32_t root = adaptive_radix_tree_delete(store, current_root, (const uint8_t *)key,
                           (int)strlen(key), 0, &deleted, &retired_offset);
    if (root < 0) {
        retirement_cancel(store, retirement_index);
        return -1;
    }
    if (!deleted) {
        retirement_cancel(store, retirement_index);
        return 0;
    }
    retirement_commit(store, retirement_index, retired_offset,
                      sbo_allocated_size(store->slots_box_meta, retired_offset));
    return 1;
}
static int delete_tree_locked(kvspace_t *store, const char *prefix) {
    if (!key_text_valid(prefix))
        return KVSPACE_INVALID;
    if (store_sync(store) != 0)
        return KVSPACE_BACKEND;
    size_t prefix_length = strlen(prefix);
    size_t base_length = subtree_base_length(prefix, prefix_length);
    char base[1024];
    if (base_length)
        memcpy(base, prefix, base_length);
    base[base_length] = 0;
    keyscan_t keys = {0};
    if (adaptive_radix_tree_scan_keys(store, base_length ? base : separator_table[SEPARATOR_SLASH].bytes,
                      base_length ? (int)base_length : separator_length(SEPARATOR_SLASH),
                      &keys) != 0) {
        keyscan_free(&keys);
        return KVSPACE_BACKEND;
    }
    int return_code = KVSPACE_OK, any = 0;
    for (int32_t i = 0; i < keys.count; i++) {
        const char *key = keys.keys[i];
        if (!subtree_key(base, base_length, key) || reserved_meta_path(key) ||
            strlen(key) >= 1024)
            continue;
        int deleted = shared_memory_delete_key_locked(store, key);
        if (deleted < 0)
            return_code = KVSPACE_BACKEND;
        else if (deleted)
            any = 1;
    }
    keyscan_free(&keys);
    if (any)
        list_generation_bump(store);
    return return_code;
}
static int read_wire_locked(kvspace_t *store, const char *key, const uint8_t **out,
                            uint64_t *out_length) {
    uint64_t offset = 0;
    int status = resolve_root_locked(store, key, &offset);
    if (status != KVSPACE_OK)
        return status;
    if (offset >= __atomic_load_n(&store->region_data.mapped, __ATOMIC_ACQUIRE) &&
        store_sync(store) != 0)
        return KVSPACE_BACKEND;
    uint8_t *base = storage_data_base(store);
    if (!base)
        return KVSPACE_BACKEND;
    const uint8_t *wire = base + offset;
    const kvspaceXvalueHead *head = (const kvspaceXvalueHead *)(const void *)wire;
    uint64_t total = kvspaceXvalueHeadTotalLen(head);
    if (total < KVSPACE_XVALUE_HEAD_LEN)
        return KVSPACE_BACKEND;
    *out = wire;
    *out_length = total;
    return KVSPACE_OK;
}
static int write_wire_locked(kvspace_t *store, const char *key, const uint8_t *wire,
                             uint64_t length) {
    if (length < KVSPACE_XVALUE_HEAD_LEN)
        return KVSPACE_INVALID;
    const kvspaceXvalueHead *head = (const kvspaceXvalueHead *)(const void *)wire;
    uint64_t body = kvspaceXvalueHeadLen(head);
    uint64_t attributes = kvspaceXvalueHeadAttrsLen(head);
    if (KVSPACE_XVALUE_HEAD_LEN + body + attributes != length)
        return KVSPACE_INVALID;
    uint64_t offset = 0;
    int status = realloc_key_locked(store, key, body, attributes, &offset);
    if (status != KVSPACE_OK)
        return status;
    uint8_t *base = storage_data_base(store);
    if (!base)
        return KVSPACE_BACKEND;
    uint8_t *destination = base + offset;
    memcpy(destination + KVSPACE_XVALUE_HEAD_LEN, wire + KVSPACE_XVALUE_HEAD_LEN, (size_t)body);
    if (attributes)
        memcpy(destination + KVSPACE_XVALUE_HEAD_LEN + body,
               wire + KVSPACE_XVALUE_HEAD_LEN + body, (size_t)attributes);
    return KVSPACE_OK;
}
static int copy_key_locked(kvspace_t *store, const char *source, const char *destination) {
    if (!key_text_valid(source) || !key_text_valid(destination))
        return KVSPACE_INVALID;
    const uint8_t *raw = NULL;
    uint64_t raw_length = 0;
    int status = read_wire_locked(store, source, &raw, &raw_length);
    if (status != KVSPACE_OK)
        return status;
    uint8_t *temporary = malloc((size_t)raw_length);
    if (!temporary)
        return KVSPACE_BACKEND;
    memcpy(temporary, raw, (size_t)raw_length);
    status = write_wire_locked(store, destination, temporary, raw_length);
    free(temporary);
    return status;
}
static int copy_tree_locked(kvspace_t *store, const char *source, const char *destination) {
    if (!key_text_valid(source) || !key_text_valid(destination))
        return KVSPACE_INVALID;
    if (store_sync(store) != 0)
        return KVSPACE_BACKEND;
    size_t source_length = strlen(source);
    size_t destination_length = strlen(destination);
    size_t source_base_length = subtree_base_length(source, source_length);
    size_t destination_base_length = subtree_base_length(destination, destination_length);
    char source_base[1024], destination_base[1024];
    if (source_base_length)
        memcpy(source_base, source, source_base_length);
    source_base[source_base_length] = 0;
    if (destination_base_length)
        memcpy(destination_base, destination, destination_base_length);
    destination_base[destination_base_length] = 0;
    if (source_base_length == destination_base_length &&
        memcmp(source_base, destination_base, source_base_length) == 0)
        return KVSPACE_OK;
    if (subtree_key(source_base, source_base_length, destination_base) ||
        subtree_key(destination_base, destination_base_length, source_base))
        return KVSPACE_INVALID;
    keyscan_t keys = {0};
    if (adaptive_radix_tree_scan_keys(store, source_base, (int)source_base_length, &keys) != 0) {
        keyscan_free(&keys);
        return KVSPACE_BACKEND;
    }
    char target[1024];
    for (int32_t i = 0; i < keys.count; i++) {
        const char *key = keys.keys[i];
        if (!subtree_key(source_base, source_base_length, key) || reserved_meta_path(key))
            continue;
        const char *suffix = key + source_base_length;
        size_t suffix_length = strlen(suffix);
        if (destination_base_length + suffix_length >= sizeof target) {
            keyscan_free(&keys);
            return KVSPACE_INVALID;
        }
        memcpy(target, destination_base, destination_base_length);
        memcpy(target + destination_base_length, suffix, suffix_length + 1);
        if (!key_text_valid(target)) {
            keyscan_free(&keys);
            return KVSPACE_INVALID;
        }
    }
    int status = delete_tree_locked(store, destination);
    if (status != KVSPACE_OK) {
        keyscan_free(&keys);
        return status;
    }
    for (int32_t i = 0; i < keys.count; i++) {
        const char *key = keys.keys[i];
        if (!subtree_key(source_base, source_base_length, key) || reserved_meta_path(key))
            continue;
        const char *suffix = key + source_base_length;
        size_t suffix_length = strlen(suffix);
        memcpy(target, destination_base, destination_base_length);
        memcpy(target + destination_base_length, suffix, suffix_length + 1);
        status = copy_key_locked(store, key, target);
        if (status != KVSPACE_OK) {
            keyscan_free(&keys);
            return status;
        }
    }
    keyscan_free(&keys);
    return KVSPACE_OK;
}
static uint8_t *storage_data_base(kvspace_t *store) {
    return store->slots_box_data;
}
static int realloc_key_locked(kvspace_t *store, const char *key,
                              uint64_t body_length, uint64_t attribute_length,
                              uint64_t *out_offset) {
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
    int32_t current_root = __atomic_load_n(&store->header->adaptive_radix_tree_root, __ATOMIC_ACQUIRE);
    adaptive_radix_tree_header_t *node = adaptive_radix_tree_search(store, current_root, (const uint8_t *)key,
                                 (int)strlen(key));
    uint64_t old_header_value = node ? adaptive_radix_tree_value_load(node) : 0;
    bool retiring = node && ADAPTIVE_RADIX_TREE_VALUE_HAS(old_header_value);
    uint64_t retirement_index = 0;
    uint64_t old_capacity = 0;
    if (retiring) {
        uint64_t offset = ADAPTIVE_RADIX_TREE_VALUE_OFFSET(old_header_value);
        if (offset >= __atomic_load_n(&store->region_data.mapped, __ATOMIC_ACQUIRE) &&
            store_sync(store) != 0)
            return KVSPACE_BACKEND;
        old_capacity = sbo_allocated_size(store->slots_box_meta, offset);
        if (need <= old_capacity) {
            storage_write_head(store->slots_box_data + offset, body_length, attribute_length);
            *out_offset = offset;
            return KVSPACE_OK;
        }
        if (retirement_reserve(store, &retirement_index) != 0)
            return KVSPACE_BACKEND;
    }
    uint64_t offset = store_slots_box_alloc(store, (size_t)need);
    if (offset == (uint64_t)-1) {
        if (retiring)
            retirement_cancel(store, retirement_index);
        return KVSPACE_BACKEND;
    }
    memset(store->slots_box_data + offset, 0, (size_t)need);
    storage_write_head(store->slots_box_data + offset, body_length, attribute_length);
    structure_seq_begin(store);
    current_root = __atomic_load_n(&store->header->adaptive_radix_tree_root, __ATOMIC_ACQUIRE);
    int32_t root = adaptive_radix_tree_insert(store, current_root, (const uint8_t *)key,
                           (int)strlen(key), 0, offset);
    if (root < 0) {
        structure_seq_end(store);
        if (retiring)
            retirement_cancel(store, retirement_index);
        store_slots_box_free(store, offset);
        return KVSPACE_BACKEND;
    }
    __atomic_store_n(&store->header->adaptive_radix_tree_root, root, __ATOMIC_RELEASE);
    structure_seq_end(store);
    if (retiring)
        retirement_commit(store, retirement_index, ADAPTIVE_RADIX_TREE_VALUE_OFFSET(old_header_value), old_capacity);
    else
        list_generation_bump(store);
    *out_offset = offset;
    return KVSPACE_OK;
}
static int free_key_locked(kvspace_t *store, const char *key) {
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    if (store_sync(store) != 0)
        return KVSPACE_BACKEND;
    if (storage_is_index_key(key)) {
        int child_return_code = storage_has_children(store, key);
        if (child_return_code != KVSPACE_OK)
            return child_return_code;
    }
    int return_code = shared_memory_delete_key_locked(store, key);
    if (return_code < 0)
        return KVSPACE_BACKEND;
    if (return_code == 0)
        return KVSPACE_MISSING;
    list_generation_bump(store);
    return KVSPACE_OK;
}
static int makeindex_key_locked(kvspace_t *store, const char *key) {
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    if (!storage_is_index_key(key))
        return KVSPACE_INVALID;
    uint64_t offset = 0;
    int return_code = resolve_root_locked(store, key, &offset);
    if (return_code == KVSPACE_OK)
        return KVSPACE_OK;
    if (return_code != KVSPACE_MISSING)
        return return_code;
    return realloc_key_locked(store, key, 0, 0, &offset);
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
KVSPACE_PUBLIC void *kvspaceConnect(const char *data_source_name) {
    int entry_errno = errno;
    char path[1024];
    if (kvspace_data_source_name_path(data_source_name, path, sizeof path) != 0) {
        errno = EINVAL;
        return NULL;
    }
    errno = 0;
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
    close_shared_memory_store((kvspace_t *)handle);
}
KVSPACE_PUBLIC uint8_t *kvspaceXvalueInProcessAddress(void *handle) {
    return storage_data_base((kvspace_t *)handle);
}
KVSPACE_PUBLIC int kvspaceResolveOffset(void *handle, const char *key,
                                 const kvspace_anchor_t *anchor,
                                 uint64_t *out_value_offset) {
    kvspace_t *store = (kvspace_t *)handle;
    *out_value_offset = 0;
    recovery_mark_active(store);
    return resolve_anchor(store, key, anchor, out_value_offset);
}
KVSPACE_PUBLIC int kvspaceAnchorOf(void *handle, const char *directory_key,
                                   kvspace_anchor_t *out_anchor) {
    kvspace_t *store = (kvspace_t *)handle;
    recovery_ack_progress(store);
    return anchor_of_key(store, directory_key, out_anchor);
}
KVSPACE_PUBLIC int kvspaceRealloc(void *handle, const char *key,
                                 uint64_t body_length, uint64_t attribute_length,
                                 uint64_t *out_value_offset) {
    kvspace_t *store = (kvspace_t *)handle;
    *out_value_offset = 0;
    if (!key_text_valid(key) || attribute_length > UINT16_MAX)
        return KVSPACE_INVALID;
    recovery_ack_progress(store);
    if (store_write_lock(store) != 0)
        return KVSPACE_BACKEND;
    int status = realloc_key_locked(store, key, body_length, attribute_length, out_value_offset);
    store_write_unlock(store);
    return status;
}
KVSPACE_PUBLIC int kvspaceFree(void *handle, const char *key) {
    kvspace_t *store = (kvspace_t *)handle;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    recovery_ack_progress(store);
    if (store_write_lock(store) != 0)
        return KVSPACE_BACKEND;
    int status = free_key_locked(store, key);
    store_write_unlock(store);
    return status;
}
KVSPACE_PUBLIC int kvspaceCp(void *handle, const char *old_key,
                             const char *new_key) {
    kvspace_t *store = (kvspace_t *)handle;
    if (!key_text_valid(old_key) || !key_text_valid(new_key))
        return KVSPACE_INVALID;
    recovery_ack_progress(store);
    if (store_write_lock(store) != 0)
        return KVSPACE_BACKEND;
    int status = copy_key_locked(store, old_key, new_key);
    store_write_unlock(store);
    return status;
}
KVSPACE_PUBLIC int kvspaceMkindex(void *handle, const char *key) {
    kvspace_t *store = (kvspace_t *)handle;
    if (!key_text_valid(key) || !storage_is_index_key(key))
        return KVSPACE_INVALID;
    recovery_ack_progress(store);
    if (store_write_lock(store) != 0)
        return KVSPACE_BACKEND;
    int status = makeindex_key_locked(store, key);
    store_write_unlock(store);
    return status;
}
KVSPACE_PUBLIC int kvspaceDeltree(void *handle, const char *key) {
    kvspace_t *store = (kvspace_t *)handle;
    if (!key_text_valid(key))
        return KVSPACE_INVALID;
    recovery_ack_progress(store);
    if (store_write_lock(store) != 0)
        return KVSPACE_BACKEND;
    int status = delete_tree_locked(store, key);
    store_write_unlock(store);
    return status;
}
KVSPACE_PUBLIC int kvspaceCpdir(void *handle, const char *source_key,
                                const char *destination_key) {
    kvspace_t *store = (kvspace_t *)handle;
    if (!key_text_valid(source_key) || !key_text_valid(destination_key))
        return KVSPACE_INVALID;
    recovery_ack_progress(store);
    if (store_write_lock(store) != 0)
        return KVSPACE_BACKEND;
    int status = copy_tree_locked(store, source_key, destination_key);
    store_write_unlock(store);
    return status;
}
KVSPACE_PUBLIC int kvspaceListlen(void *handle, const char *key,
                                  int64_t *out_count,
                                  uint32_t *out_max_name_length) {
    kvspace_t *store = (kvspace_t *)handle;
    *out_count = 0;
    *out_max_name_length = 0;
    recovery_ack_progress(store);
    return listlen_key(store, key, out_count, out_max_name_length);
}
KVSPACE_PUBLIC int kvspaceList(void *handle, const char *key, char *out_names,
                               uint32_t out_capacity, uint32_t *out_length,
                               uint32_t *out_count, uint64_t *out_generation) {
    kvspace_t *store = (kvspace_t *)handle;
    recovery_ack_progress(store);
    return list_names_raw(store, key, out_names, out_capacity,
                          out_length, out_count, out_generation);
}

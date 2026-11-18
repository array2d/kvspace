#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kvspace_recovery.h"
#include "kvspace_xvalue_internal.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static uint32_t g_fork_generation;
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;
static void recovery_atfork_child(void) {
    __atomic_add_fetch(&g_fork_generation, 1u, __ATOMIC_ACQ_REL);
}
static void recovery_atfork_install(void) {
    (void)pthread_atfork(NULL, NULL, recovery_atfork_child);
}
uint32_t recovery_fork_generation(void) {
    (void)pthread_once(&g_atfork_once, recovery_atfork_install);
    return __atomic_load_n(&g_fork_generation, __ATOMIC_ACQUIRE);
}
static inline bool recovery_fork_mismatch(const kvspace_t *store) {
    return store->fork_generation != __atomic_load_n(&g_fork_generation, __ATOMIC_ACQUIRE);
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
    (void)out;
    if (process_id == 0)
        return 1;
    if (kill((pid_t)process_id, 0) != 0 && errno == ESRCH)
        return 1;
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
int recovery_register_claim(kvspace_t *store) {
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
    recovery_repair_dead_writer(store);
    for (int i = 0; i < RECOVERY_SLOT_COUNT; i++) {
        recovery_slot_t *slot = &store->header->slots[i];
        uint32_t expect = RECOVERY_SLOT_FREE;
        if (!__atomic_compare_exchange_n(&slot->state, &expect, RECOVERY_SLOT_CLAIMING, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        uint32_t claim_id = __atomic_add_fetch(&store->header->claim_sequence, 1u,
                                          __ATOMIC_ACQ_REL);
        if (claim_id > RETIREMENT_MAX_CLAIM) {
            __atomic_store_n(&slot->state, RECOVERY_SLOT_FREE, __ATOMIC_RELEASE);
            return -3;
        }
        slot->process_id = current_process_id;
        slot->start_time = process_start_time;
        __atomic_store_n(&slot->quiesced, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&slot->claim_id, claim_id, __ATOMIC_RELEASE);
        __atomic_store_n(&slot->recovery_ack,
                         __atomic_load_n(&store->header->recovery_epoch, __ATOMIC_ACQUIRE),
                         __ATOMIC_RELEASE);
        __atomic_store_n(&slot->state, RECOVERY_SLOT_MINE, __ATOMIC_RELEASE);
        return i;
    }
    return -2;
}
void recovery_register_release(kvspace_t *store) {
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
static void recovery_publish_ack(kvspace_t *store, recovery_slot_t *slot, uint32_t epoch) {
    __atomic_store_n(&slot->recovery_ack, (uint64_t)epoch, __ATOMIC_RELEASE);
    __atomic_add_fetch(&store->header->ack_sequence, 1u, __ATOMIC_RELEASE);
    if (__atomic_load_n(&store->header->barrier_state, __ATOMIC_ACQUIRE))
        recovery_futex_wake(&store->header->ack_sequence);
}
void recovery_mark_active(kvspace_t *store) {
    if (store->recovery_slot < 0 || recovery_fork_mismatch(store))
        return;
    recovery_slot_t *slot = &store->header->slots[store->recovery_slot];
    uint32_t epoch = __atomic_load_n(&store->header->recovery_epoch, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&slot->quiesced, __ATOMIC_RELAXED)) {
        __atomic_store_n(&slot->recovery_ack, (uint64_t)epoch, __ATOMIC_RELEASE);
        __atomic_store_n(&slot->quiesced, 0u, __ATOMIC_SEQ_CST);
        epoch = __atomic_load_n(&store->header->recovery_epoch, __ATOMIC_SEQ_CST);
    }
    if ((uint32_t)__atomic_load_n(&slot->recovery_ack, __ATOMIC_RELAXED) != epoch)
        recovery_publish_ack(store, slot, epoch);
}
void recovery_ack_progress(kvspace_t *store) {
    if (store->recovery_slot < 0 || recovery_fork_mismatch(store))
        return;
    recovery_slot_t *slot = &store->header->slots[store->recovery_slot];
    if (__atomic_load_n(&slot->quiesced, __ATOMIC_RELAXED))
        return;
    uint32_t epoch = __atomic_load_n(&store->header->recovery_epoch, __ATOMIC_ACQUIRE);
    if ((uint32_t)__atomic_load_n(&slot->recovery_ack, __ATOMIC_RELAXED) != epoch)
        recovery_publish_ack(store, slot, epoch);
}
enum { RECOVERY_BARRIER_FREE = 0, RECOVERY_BARRIER_OWNED = 1 };
enum { WRITE_LOCK_FREE = 0, WRITE_LOCK_HELD = 1, WRITE_LOCK_CONTENDED = 2 };
static uint32_t recovery_my_claim(kvspace_t *store) {
    if (store->recovery_slot < 0 || recovery_fork_mismatch(store))
        return 0;
    return store->header->slots[store->recovery_slot].claim_id;
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
static void repair_odd_structure_epoch(kvspace_t *store) {
    uint64_t epoch = __atomic_load_n(&store->header->structure_epoch, __ATOMIC_ACQUIRE);
    if (epoch & 1ull)
        (void)__atomic_compare_exchange_n(&store->header->structure_epoch, &epoch, epoch + 1ull,
                                          false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
void recovery_repair_dead_writer(kvspace_t *store) {
    uint32_t current = __atomic_load_n(&store->header->write_lock, __ATOMIC_ACQUIRE);
    if (current == WRITE_LOCK_FREE) {
        repair_odd_structure_epoch(store);
        return;
    }
    uint32_t holder = RING_ENTRY_CLAIM(current);
    if (recovery_claim_alive(store, holder))
        return;
    repair_odd_structure_epoch(store);
    if (__atomic_compare_exchange_n(&store->header->write_lock, &current, WRITE_LOCK_FREE,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (RING_ENTRY_STATE(current) == WRITE_LOCK_CONTENDED)
            recovery_futex_wake(&store->header->write_lock);
    }
}
int store_write_lock(kvspace_t *store) {
    uint32_t mine = recovery_my_claim(store);
    if (!mine)
        return -1;
    pthread_mutex_lock(&store->local_write_lock);
    uint32_t expected = WRITE_LOCK_FREE;
    uint32_t held = RING_ENTRY_MAKE(WRITE_LOCK_HELD, mine);
    if (__atomic_compare_exchange_n(&store->header->write_lock, &expected, held, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;
    uint32_t desired = held;
    for (;;) {
        for (int spin = 0; spin < 128; spin++) {
            uint32_t current = __atomic_load_n(&store->header->write_lock, __ATOMIC_RELAXED);
            if (current == WRITE_LOCK_FREE) {
                expected = WRITE_LOCK_FREE;
                if (__atomic_compare_exchange_n(&store->header->write_lock, &expected, desired,
                                                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                    return 0;
            }
            kvspace_cpu_relax();
        }
        desired = RING_ENTRY_MAKE(WRITE_LOCK_CONTENDED, mine);
        uint32_t current = __atomic_load_n(&store->header->write_lock, __ATOMIC_ACQUIRE);
        if (current == WRITE_LOCK_FREE) {
            if (__atomic_compare_exchange_n(&store->header->write_lock, &current, desired,
                                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return 0;
            continue;
        }
        uint32_t holder = RING_ENTRY_CLAIM(current);
        if (holder == mine) {
            pthread_mutex_unlock(&store->local_write_lock);
            return -1;
        }
        if (!recovery_claim_alive(store, holder)) {
            if (__atomic_compare_exchange_n(&store->header->write_lock, &current, desired,
                                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                repair_odd_structure_epoch(store);
                return 0;
            }
            continue;
        }
        if (RING_ENTRY_STATE(current) == WRITE_LOCK_HELD) {
            uint32_t marked = RING_ENTRY_MAKE(WRITE_LOCK_CONTENDED, holder);
            if (!__atomic_compare_exchange_n(&store->header->write_lock, &current, marked,
                                             false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                continue;
            current = marked;
        }
        recovery_futex_wait(&store->header->write_lock, current, 2);
    }
}
void store_write_unlock(kvspace_t *store) {
    uint32_t previous = __atomic_exchange_n(&store->header->write_lock, WRITE_LOCK_FREE,
                                            __ATOMIC_RELEASE);
    if (RING_ENTRY_STATE(previous) == WRITE_LOCK_CONTENDED)
        recovery_futex_wake(&store->header->write_lock);
    pthread_mutex_unlock(&store->local_write_lock);
}
static bool recovery_elect(kvspace_t *store) {
    uint32_t mine = recovery_my_claim(store);
    if (!mine)
        return false;
    uint32_t own = RING_ENTRY_MAKE(RECOVERY_BARRIER_OWNED, mine);
    for (;;) {
        uint32_t current = __atomic_load_n(&store->header->barrier_state, __ATOMIC_ACQUIRE);
        if (current == RING_ENTRY_MAKE(RECOVERY_BARRIER_FREE, 0)) {
            if (__atomic_compare_exchange_n(&store->header->barrier_state, &current, own,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                return true;
            continue;
        }
        if (RING_ENTRY_STATE(current) != RECOVERY_BARRIER_OWNED)
            return false;
        if (RING_ENTRY_CLAIM(current) == mine)
            return false;
        if (recovery_claim_alive(store, RING_ENTRY_CLAIM(current)))
            return false;
        if (__atomic_compare_exchange_n(&store->header->barrier_state, &current, own, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
}
static void recovery_release_busy(kvspace_t *store) {
    uint32_t mine = RING_ENTRY_MAKE(RECOVERY_BARRIER_OWNED, recovery_my_claim(store));
    __atomic_compare_exchange_n(&store->header->barrier_state, &mine,
                                RING_ENTRY_MAKE(RECOVERY_BARRIER_FREE, 0), false,
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
            uint32_t acked = (uint32_t)__atomic_load_n(&slot->recovery_ack, __ATOMIC_ACQUIRE);
            if ((int32_t)(acked - epoch) >= 0)
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
            static bool once;
            if (first >= 0 && !__atomic_exchange_n(&once, true, __ATOMIC_RELAXED)) {
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
int recovery_round(kvspace_t *store) {
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
    uint64_t pending = __atomic_exchange_n(&store->header->retirement_freeing, 0,
                                           __ATOMIC_ACQ_REL);
    if (pending) {
        fprintf(stderr, "kvspace: previous leader died freeing box %llu: "
                        "leaked\n", (unsigned long long)pending);
    }
    uint32_t epoch = __atomic_add_fetch(&store->header->recovery_epoch, 1u,
                                    __ATOMIC_SEQ_CST);
    if (store->recovery_slot >= 0) {
        __atomic_store_n(&store->header->slots[store->recovery_slot].recovery_ack,
                         (uint64_t)epoch, __ATOMIC_RELEASE);
    }
    if (!recovery_wait_acknowledgements(store, epoch, recovery_barrier_milliseconds())) {
        recovery_release_busy(store);
        return -2;
    }
    int freed = retirement_drain(store, epoch);
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
        uint64_t committed = __atomic_load_n(&store->header->committed_bytes, __ATOMIC_ACQUIRE);
        uint64_t freed = __atomic_load_n(&store->header->freed_bytes, __ATOMIC_ACQUIRE);
        if (committed <= freed || committed - freed < retirement_threshold())
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
void recovery_maintenance_stop(kvspace_t *store) {
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
bool recovery_other_alive(kvspace_t *store) {
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 
#endif
#define BLOCKMALLOC_IMPLEMENTATION 
#include <blockmalloc/blockmalloc.h>
#define SBO_MIN_SHARD_BYTES (1ULL << 40)
#define SLOTSBOXMALLOC_IMPLEMENTATION 
#include "slotsboxmalloc/slotsboxobj.h"
#include "kvspace_shm.h"
#include "trie.h"
#include <kvspace/kvspace.h>
#include "xvalue_internal.h"
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
#define KVS_MAGIC "kvspace-c"
#define INDEX_BLOCK_SIZE 104
#define INDEX_SLAB_INIT (64UL * 1024 * 1024)
#define SBO_HEAD_POOL_INIT (4UL * 1024 * 1024)
#define REGION_RESERVE (1ULL << 38)
#define INDEX_RESERVE (1ULL << 36)
#define DATA_RESERVE (1ULL << 40)
#define SBO_PUNCH_MIN (64UL * 1024)
static int reserved_meta_path(const char *key) {
    static const char root[] = "/.kvspace-meta";
    return strncmp(key, root, sizeof root - 1) == 0 &&
           (key[sizeof root - 1] == 0 || key[sizeof root - 1] == '/');
}
#define P2_SLOT_N 64
enum { P2_SLOT_FREE = 0, P2_SLOT_MINE = 1, P2_SLOT_CLAIMING = 3 };
typedef struct {
    uint32_t pid;
    uint32_t state;
    uint64_t starttime;
    uint64_t recovery_ack;
    uint32_t quiesced;
    uint32_t claim_id;
} p2_slot_t;
#define GARB_CAP (1u << 21)
#define GARB_THRESHOLD (64ull << 20)
#define P2_BARRIER_MS_DEFAULT 200
#define GARB_MAX_CLAIM ((1u << 30) - 1u)
typedef struct {
    uint64_t off;
    uint32_t epoch;
    uint32_t meta;
} garb_ent_t;
enum { GE_EMPTY = 0, GE_RESERVED = 1, GE_COMMITTED = 2 };
#define GE_STATE(m) ((uint32_t)((m) & 3u))
#define GE_CLAIM(m) ((uint32_t)((m) >> 2))
#define GE_MAKE(st,id) ((((uint32_t)(id)) << 2) | (uint32_t)(st))
typedef struct {
    char magic[sizeof(KVS_MAGIC)];
    uint64_t index_slab_size, sbo_head_size, sbo_data_size;
    uint64_t trie_root;
    uint32_t recovery_epoch;
    uint32_t ack_seq;
    uint32_t barrier_state;
    uint64_t barrier_pid, barrier_starttime;
    uint64_t garb_head, garb_tail;
    uint64_t committed_bytes;
    uint64_t freed_bytes;
    uint64_t garb_freeing;
    uint64_t round_ok, round_aborted, leak_boxes;
    uint64_t list_gen;
    uint32_t claim_seq;
    p2_slot_t slots[P2_SLOT_N];
} kvspace_hdr_t;
#define INDEX_OFF (sizeof(kvspace_hdr_t) + sizeof(blocks_meta_t))
static size_t kvspace_data_size = 8ULL * 64 * 64 * 64 * 64;
static uint8_t *storage_data_base(kvspace_t *kv);
static int resolve_root(kvspace_t *kv, const char *key, uint64_t *out_offset);
static int realloc_key(kvspace_t *kv, const char *key, uint64_t body_len,
                       uint64_t attr_len, uint64_t *out_offset);
_Static_assert(INDEX_SLAB_INIT / (INDEX_BLOCK_SIZE + 2) > 32767ULL / 4,
               "index slab initial size too small for 30-bit block ids");
_Static_assert(INDEX_RESERVE / (INDEX_BLOCK_SIZE + 8) <= (1ULL << 30),
               "index reserve too large for 30-bit block ids");
_Static_assert((INDEX_BLOCK_SIZE + 8) % 8 == 0,
               "index stride must keep 8B alignment");
_Static_assert(
    SBO_HEAD_POOL_INIT / (sizeof(sbo_box_t) + 2) > 32767ULL / 4,
    "SBO_HEAD_POOL_INIT too small: blockmalloc would pick 14-bit ids");
#define SBO_HEAD_FIXED \
    (sizeof(sbo_meta_t) + sizeof(blocks_meta_t) + sizeof(sbo_lock_t))
static blocks_meta_t *sbo_pool(sbo_meta_t *m) {
    return sbo_pool_meta(m, 0);
}
static sbo_lock_t *sbo_plock(sbo_meta_t *m) {
    return sbo_pool_lock(m, 0);
}
static uint8_t *sbo_pmem(sbo_meta_t *m) {
    return sbo_pool_mem(m, 0);
}
static sbo_box_t *sbo_box(sbo_meta_t *m, int32_t id) {
    return (sbo_box_t *)(sbo_pmem(m) + (size_t)id * m->block_stride +
                         m->box_off);
}
typedef struct {
    int fd;
    uint8_t *base;
    size_t mapped;
    size_t reserve;
} shm_region_t;
struct kvspace {
    shm_region_t r_index, r_head, r_data, r_garb;
    garb_ent_t *garb;
    kvspace_hdr_t *hdr;
    uint64_t store_id;
    int p2_slot;
    uint32_t pid;
    uint32_t fork_gen;
    uint32_t quiesced;
    pthread_t maint_thread;
    uint32_t maint_seq;
    uint32_t maint_started, maint_stop;
    uint32_t reclaim_unavailable;
    blocks_meta_t *index_meta;
    uint8_t *index_data;
    sbo_meta_t *sbo_meta;
    uint8_t *sbo_data;
    trie_env_t trie_env;
};
static inline void list_gen_bump(kvspace_t *kv) {
    __atomic_add_fetch(&kv->hdr->list_gen, 1ull, __ATOMIC_RELEASE);
}
static int region_map(shm_region_t *r, size_t size) {
    if (size > r->reserve)
        return -1;
    if (size <= r->mapped)
        return 0;
    if (mmap(r->base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
             r->fd, 0) == MAP_FAILED)
        return -1;
    r->mapped = size;
    return 0;
}
static int region_attach(shm_region_t *r, bool create, size_t size,
                         size_t reserve) {
    struct stat st;
    if (create ? ftruncate(r->fd, (off_t)size) != 0
               : fstat(r->fd, &st) != 0 || st.st_size < (off_t)size)
        return -1;
    r->reserve = size > reserve ? size : reserve;
    void *p = mmap(NULL, r->reserve, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    r->base = p;
    return region_map(r, size);
}
static void region_close(shm_region_t *r) {
    if (r->base)
        munmap(r->base, r->reserve);
    if (r->fd >= 0)
        close(r->fd);
}
static void kv_sbo_free(kvspace_t *kv, uint64_t off);
static int p2_maint_start(kvspace_t *kv);
static bool p2_claim_alive(kvspace_t *kv, uint32_t claim);
static int kv_sync(kvspace_t *kv);
#ifdef P2_TEST_HOOKS
static struct {
    uint32_t garb_threshold_megabytes;
    int barrier_ms;
    uint32_t hold_drain_ms;
} g_p2_test_knobs;
#endif
static uint64_t p2_garb_threshold(void) {
#ifdef P2_TEST_HOOKS
    if (g_p2_test_knobs.garb_threshold_megabytes)
        return (uint64_t)g_p2_test_knobs.garb_threshold_megabytes << 20;
#endif
    return GARB_THRESHOLD;
}
static void garb_full_report(kvspace_t *kv) {
    static bool once;
    if (once)
        return;
    once = true;
    uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
    uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
    uint32_t m = __atomic_load_n(&kv->garb[h & (GARB_CAP - 1)].meta,
                                 __ATOMIC_ACQUIRE);
    fprintf(stderr,
            "kvspace: garbage ring full (cap=%llu, head=%llu, tail=%llu, "
            "bytes=%llu): head entry #%llu state=%u claim=%u is holding "
            "the drain (no reclaim round yet)\n",
            (unsigned long long)GARB_CAP, (unsigned long long)h,
            (unsigned long long)t,
            (unsigned long long)(kv->hdr->committed_bytes -
                                 kv->hdr->freed_bytes),
            (unsigned long long)h, GE_STATE(m), GE_CLAIM(m));
}
static int garb_reserve(kvspace_t *kv, uint64_t *idx) {
    uint32_t claim = kv->p2_slot >= 0 ? kv->hdr->slots[kv->p2_slot].claim_id : 0;
    if (!claim)
        return -1;
    uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
    uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
    if (t - h >= GARB_CAP) {
        garb_full_report(kv);
        return -1;
    }
    if (kv->reclaim_unavailable &&
        GARB_CAP - (t - h) < GARB_CAP / 4) {
        garb_full_report(kv);
        return -1;
    }
    garb_ent_t *e = &kv->garb[t & (GARB_CAP - 1)];
    uint32_t m = __atomic_load_n(&e->meta, __ATOMIC_ACQUIRE);
    if (m != GE_EMPTY) {
        if (GE_STATE(m) != GE_RESERVED || p2_claim_alive(kv, GE_CLAIM(m)))
            return -1;
        __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&e->meta, GE_MAKE(GE_RESERVED, claim), __ATOMIC_RELEASE);
    __atomic_store_n(&kv->hdr->garb_tail, t + 1, __ATOMIC_RELEASE);
    *idx = t;
    return 0;
}
static void garb_commit(kvspace_t *kv, uint64_t idx, uint64_t off,
                        uint64_t bytes_retired) {
    garb_ent_t *e = &kv->garb[idx & (GARB_CAP - 1)];
    uint32_t claim = GE_CLAIM(__atomic_load_n(&e->meta, __ATOMIC_ACQUIRE));
    e->off = off;
    e->epoch = __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE);
    __atomic_store_n(&e->meta, GE_MAKE(GE_COMMITTED, claim), __ATOMIC_RELEASE);
    uint64_t bytes = kv->hdr->committed_bytes + bytes_retired;
    __atomic_store_n(&kv->hdr->committed_bytes, bytes, __ATOMIC_RELEASE);
    if (bytes - __atomic_load_n(&kv->hdr->freed_bytes, __ATOMIC_ACQUIRE) >=
        p2_garb_threshold())
        (void)p2_maint_start(kv);
}
static void garb_cancel(kvspace_t *kv, uint64_t idx) {
    __atomic_store_n(&kv->garb[idx & (GARB_CAP - 1)].meta, GE_EMPTY,
                     __ATOMIC_RELEASE);
}
static int p2_proc_starttime(uint32_t pid, uint64_t *out) {
#if defined(__linux__)
    char p[64];
    if (snprintf(p, sizeof p, "/proc/%u/stat", pid) >= (int)sizeof p)
        return 2;
    FILE *f = fopen(p, "r");
    if (!f)
        return errno == ENOENT ? 1 : 2;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (!n)
        return 2;
    buf[n] = 0;
    char *rp = strrchr(buf, ')');
    if (!rp)
        return 2;
    char *s = rp + 1;
    int field = 3;
    while (*s) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        if (field == 3 && (*s == 'Z' || *s == 'X'))
            return 1;
        if (field == 22) {
            *out = strtoull(s, NULL, 10);
            return 0;
        }
        while (*s && *s != ' ')
            s++;
        field++;
    }
    return 2;
#else
    (void)pid;
    (void)out;
    return 2;
#endif
}
static bool p2_slot_alive(const p2_slot_t *s) {
    uint64_t st = 0;
    uint32_t state = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    if (state == P2_SLOT_FREE)
        return false;
    if (state == P2_SLOT_CLAIMING)
        return true;
    int rc = p2_proc_starttime(s->pid, &st);
    if (rc == 1)
        return false;
    if (rc != 0)
        return true;
    return st == s->starttime;
}
static int p2_reg_claim(kvspace_t *kv) {
    uint64_t st = 0;
    if (p2_proc_starttime((uint32_t)getpid(), &st) != 0)
        st = 0;
    uint32_t me = (uint32_t)getpid();
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        uint32_t v = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
        if (v == P2_SLOT_CLAIMING && s->pid == 0) {
            __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_FREE, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            continue;
        }
        if (v != P2_SLOT_FREE && !p2_slot_alive(s))
            __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_FREE, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        uint32_t expect = P2_SLOT_FREE;
        if (!__atomic_compare_exchange_n(&s->state, &expect, P2_SLOT_CLAIMING, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        uint32_t cid = __atomic_add_fetch(&kv->hdr->claim_seq, 1u,
                                          __ATOMIC_ACQ_REL);
        if (cid > GARB_MAX_CLAIM) {
            __atomic_store_n(&s->state, P2_SLOT_FREE, __ATOMIC_RELEASE);
            return -3;
        }
        s->pid = me;
        s->starttime = st;
        s->quiesced = 1;
        s->claim_id = cid;
        __atomic_store_n(&s->recovery_ack,
                         __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE),
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s->state, P2_SLOT_MINE, __ATOMIC_RELEASE);
        return i;
    }
    return -2;
}
static void p2_reg_release(kvspace_t *kv) {
    if (kv->p2_slot < 0)
        return;
    if (kv->pid != (uint32_t)getpid()) {
        kv->p2_slot = -1;
        return;
    }
    p2_slot_t *s = &kv->hdr->slots[kv->p2_slot];
    uint32_t expect = P2_SLOT_MINE;
    __atomic_compare_exchange_n(&s->state, &expect, P2_SLOT_FREE, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    kv->p2_slot = -1;
}
static void p2_ack_slot(kvspace_t *kv, uint32_t ep) {
    if (kv->p2_slot < 0)
        return;
    __atomic_store_n(&kv->hdr->slots[kv->p2_slot].recovery_ack, ep,
                     __ATOMIC_RELEASE);
    __atomic_add_fetch(&kv->hdr->ack_seq, 1u, __ATOMIC_RELEASE);
#if defined(__linux__)
    if (__atomic_load_n(&kv->hdr->barrier_state, __ATOMIC_ACQUIRE))
        syscall(SYS_futex, &kv->hdr->ack_seq, FUTEX_WAKE, INT_MAX, NULL, NULL,
                0);
#endif
}
static uint32_t g_fork_gen;
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;
static void p2_atfork_child(void) {
    __atomic_add_fetch(&g_fork_gen, 1u, __ATOMIC_ACQ_REL);
}
static void p2_atfork_install(void) {
    (void)pthread_atfork(NULL, NULL, p2_atfork_child);
}
#define P2_FORK_GUARD(kv) \
    ((kv)->fork_gen != __atomic_load_n(&g_fork_gen, __ATOMIC_ACQUIRE))
#define P2_VIEW_GUARD(kv) ((kv)->quiesced || P2_FORK_GUARD(kv))
static int head_of(const uint8_t *val, int32_t val_len, kvspaceXvalueHead *out) {
    if (!val || val_len < (int32_t)KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    memcpy(out, val, sizeof *out);
    const uint64_t total = kvspaceXvalueHeadTotalLen(out);
    if (total == 0 || total > (uint64_t)val_len)
        return -1;
    {
        const uint8_t *langtype = NULL;
        uint16_t langtype_len = 0;
        if (kvspace_c_scan_attrs(val, total, &langtype, &langtype_len) != 0)
            return -1;
    }
    return 0;
}
static long long p2_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int p2_barrier_ms(void) {
#ifdef P2_TEST_HOOKS
    int ms = g_p2_test_knobs.barrier_ms;
    if (ms == 0)
        ms = P2_BARRIER_MS_DEFAULT;
#else
    int ms = P2_BARRIER_MS_DEFAULT;
#endif
    return ms < 1 ? 1 : (ms > 5000 ? 5000 : ms);
}
#if defined(__linux__)
static void p2_futex_wait(uint32_t *addr, uint32_t val, int ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    syscall(SYS_futex, addr, FUTEX_WAIT, val, &ts, NULL, 0);
}
static void p2_futex_wake(uint32_t *addr) {
    syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}
static void p2_futex_wait_priv(uint32_t *addr, uint32_t val, int ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, &ts, NULL, 0);
}
static void p2_futex_wake_priv(uint32_t *addr) {
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}
#else
static void p2_futex_wait(uint32_t *a, uint32_t v, int ms) { (void)a; (void)v; usleep((useconds_t)ms * 1000); }
static void p2_futex_wake(uint32_t *a) { (void)a; }
static void p2_futex_wait_priv(uint32_t *a, uint32_t v, int ms) { (void)a; (void)v; usleep((useconds_t)ms * 1000); }
static void p2_futex_wake_priv(uint32_t *a) { (void)a; }
#endif
enum { P2_BARRIER_FREE = 0, P2_BARRIER_OWNED = 1 };
static uint32_t p2_my_claim(kvspace_t *kv) {
    return kv->p2_slot >= 0 ? kv->hdr->slots[kv->p2_slot].claim_id : 0;
}
static bool p2_claim_alive(kvspace_t *kv, uint32_t claim) {
    if (!claim)
        return false;
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
            continue;
        if (__atomic_load_n(&s->claim_id, __ATOMIC_ACQUIRE) != claim)
            continue;
        return p2_slot_alive(s);
    }
    return false;
}
static bool p2_elect(kvspace_t *kv) {
    uint32_t mine = p2_my_claim(kv);
    if (!mine)
        return false;
    uint32_t own = GE_MAKE(P2_BARRIER_OWNED, mine);
    for (;;) {
        uint32_t cur = __atomic_load_n(&kv->hdr->barrier_state, __ATOMIC_ACQUIRE);
        if (cur == GE_MAKE(P2_BARRIER_FREE, 0)) {
            if (__atomic_compare_exchange_n(&kv->hdr->barrier_state, &cur, own,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                return true;
            continue;
        }
        if (GE_STATE(cur) != P2_BARRIER_OWNED)
            return false;
        if (GE_CLAIM(cur) == mine)
            return false;
        if (p2_claim_alive(kv, GE_CLAIM(cur)))
            return false;
        if (__atomic_compare_exchange_n(&kv->hdr->barrier_state, &cur, own, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
}
static void p2_release_busy(kvspace_t *kv) {
    uint32_t mine = GE_MAKE(P2_BARRIER_OWNED, p2_my_claim(kv));
    __atomic_compare_exchange_n(&kv->hdr->barrier_state, &mine,
                                GE_MAKE(P2_BARRIER_FREE, 0), false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static bool p2_wait_acks(kvspace_t *kv, uint32_t r, int deadline_ms) {
    uint8_t dead[P2_SLOT_N] = {0};
    long long t0 = p2_now_ms();
    for (;;) {
        uint32_t seq = __atomic_load_n(&kv->hdr->ack_seq, __ATOMIC_ACQUIRE);
        bool missing = false;
        int first = -1;
        for (int i = 0; i < P2_SLOT_N; i++) {
            p2_slot_t *s = &kv->hdr->slots[i];
            if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
                continue;
            if (__atomic_load_n(&s->recovery_ack, __ATOMIC_ACQUIRE) == r)
                continue;
            if (__atomic_load_n(&s->quiesced, __ATOMIC_ACQUIRE))
                continue;
            if (!dead[i]) {
                uint64_t ts = 0;
                int rc = p2_proc_starttime(s->pid, &ts);
                if (rc == 1 || (rc == 0 && ts != s->starttime))
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
        long long left = (long long)deadline_ms - (p2_now_ms() - t0);
        if (left <= 0 || __atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE)) {
            if (first >= 0) {
                p2_slot_t *s = &kv->hdr->slots[first];
                fprintf(stderr,
                        "kvspace: barrier round %u abandoned (waited %dms): "
                        "slot %d pid=%u start=%llu ack=%llu quiesced=%u\n",
                        r, deadline_ms, first, s->pid,
                        (unsigned long long)s->starttime,
                        (unsigned long long)__atomic_load_n(&s->recovery_ack,
                                                            __ATOMIC_ACQUIRE),
                        __atomic_load_n(&s->quiesced, __ATOMIC_ACQUIRE));
            }
            return false;
        }
        p2_futex_wait(&kv->hdr->ack_seq, seq, (int)(left < 5 ? left : 5));
    }
}
static int p2_drain(kvspace_t *kv, uint32_t r) {
    int freed = 0;
    uint64_t i = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
    for (;;) {
        if (i >= __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE))
            goto out;
        garb_ent_t *e = &kv->garb[i & (GARB_CAP - 1)];
        uint32_t m = __atomic_load_n(&e->meta, __ATOMIC_ACQUIRE);
        uint32_t st = GE_STATE(m);
        if (st == GE_EMPTY) {
            __atomic_store_n(&kv->hdr->garb_head, i + 1, __ATOMIC_RELEASE);
            i++;
            continue;
        }
        if (st == GE_RESERVED) {
            if (p2_claim_alive(kv, GE_CLAIM(m)))
                goto out;
            __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
            __atomic_add_fetch(&kv->hdr->leak_boxes, 1, __ATOMIC_ACQ_REL);
            fprintf(stderr,
                    "kvspace: garbage entry %llu reserved by dead claim %u: "
                    "leaked\n", (unsigned long long)i, GE_CLAIM(m));
            __atomic_store_n(&kv->hdr->garb_head, i + 1, __ATOMIC_RELEASE);
            i++;
            continue;
        }
        if (st != GE_COMMITTED)
            goto out;
        if (!((int32_t)(r - __atomic_load_n(&e->epoch, __ATOMIC_ACQUIRE)) > 0))
            goto out;
        uint64_t off = __atomic_load_n(&e->off, __ATOMIC_ACQUIRE);
        uint64_t cap = sbo_allocated_size(kv->sbo_meta, off);
        __atomic_store_n(&kv->hdr->garb_freeing, off, __ATOMIC_RELEASE);
        __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
        __atomic_store_n(&kv->hdr->garb_head, i + 1, __ATOMIC_RELEASE);
#ifdef P2_TEST_HOOKS
        if (g_p2_test_knobs.hold_drain_ms)
            usleep((useconds_t)g_p2_test_knobs.hold_drain_ms * 1000);
#endif
        kv_sbo_free(kv, off);
        __atomic_store_n(&kv->hdr->freed_bytes,
                         kv->hdr->freed_bytes + cap, __ATOMIC_RELEASE);
        i++;
        freed++;
    }
out:
    __atomic_store_n(&kv->hdr->garb_freeing, 0, __ATOMIC_RELEASE);
    return freed;
}
static int p2_round(kvspace_t *kv) {
#if !defined(__linux__)
    (void)kv;
    return 0;
#else
    if (!kv || kv->hdr->trie_root == 0)
        return 0;
    if (kv->pid != (uint32_t)getpid())
        return -1;
    if (!p2_elect(kv))
        return -1;
    if (kv_sync(kv) != 0) {
        p2_release_busy(kv);
        return -1;
    }
    uint64_t pending = __atomic_exchange_n(&kv->hdr->garb_freeing, 0,
                                           __ATOMIC_ACQ_REL);
    if (pending) {
        __atomic_add_fetch(&kv->hdr->leak_boxes, 1, __ATOMIC_ACQ_REL);
        fprintf(stderr, "kvspace: previous leader died freeing box %llu: "
                        "leaked\n", (unsigned long long)pending);
    }
    uint32_t r = __atomic_add_fetch(&kv->hdr->recovery_epoch, 1u,
                                    __ATOMIC_ACQ_REL);
    if (!p2_wait_acks(kv, r, p2_barrier_ms())) {
        __atomic_add_fetch(&kv->hdr->round_aborted, 1, __ATOMIC_ACQ_REL);
        p2_release_busy(kv);
        return -2;
    }
    int freed = p2_drain(kv, r);
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        uint32_t v = P2_SLOT_MINE;
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
            continue;
        if (p2_slot_alive(s))
            continue;
        __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_FREE, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    __atomic_add_fetch(&kv->hdr->round_ok, 1, __ATOMIC_ACQ_REL);
    p2_release_busy(kv);
    return freed;
#endif
}
static void *p2_maint(void *arg) {
    kvspace_t *kv = arg;
    while (!__atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE)) {
        uint32_t seq = __atomic_load_n(&kv->maint_seq, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE))
            break;
        p2_futex_wait_priv(&kv->maint_seq, seq, 50);
        if (__atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE))
            break;
        if (kv->hdr->committed_bytes -
                __atomic_load_n(&kv->hdr->freed_bytes, __ATOMIC_ACQUIRE) <
            p2_garb_threshold())
            continue;
        p2_round(kv);
    }
    return NULL;
}
static int p2_maint_start(kvspace_t *kv) {
#if !defined(__linux__)
    (void)kv;
    return 0;
#else
    uint32_t st = 0;
    if (!__atomic_compare_exchange_n(&kv->maint_started, &st, 1u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return st == 2u ? -1 : 0;
    if (kv->pid != (uint32_t)getpid()) {
        __atomic_store_n(&kv->maint_started, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    if (pthread_create(&kv->maint_thread, NULL, p2_maint, kv) != 0) {
        __atomic_store_n(&kv->maint_started, 2u, __ATOMIC_RELEASE);
        kv->reclaim_unavailable = 1;
        fprintf(stderr, "kvspace: maintenance thread create failed; reclaim "
                        "unavailable (writes hard-fail when the ring is 3/4 "
                        "full)\n");
        return -1;
    }
    return 0;
#endif
}
static void p2_maint_stop(kvspace_t *kv) {
    if (kv->pid != (uint32_t)getpid()) {
        __atomic_store_n(&kv->maint_started, 0u, __ATOMIC_RELEASE);
        return;
    }
    if (__atomic_load_n(&kv->maint_started, __ATOMIC_ACQUIRE) != 1u)
        return;
    __atomic_store_n(&kv->maint_stop, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&kv->maint_seq, 1u, __ATOMIC_RELEASE);
    p2_futex_wake_priv(&kv->maint_seq);
    pthread_join(kv->maint_thread, NULL);
    __atomic_store_n(&kv->maint_started, 0u, __ATOMIC_RELEASE);
}
#ifdef P2_TEST_HOOKS
void kvspaceShmTestSetGarbageThresholdMegabytes(uint32_t megabytes) {
    g_p2_test_knobs.garb_threshold_megabytes = megabytes;
}
void kvspaceShmTestSetBarrierMilliseconds(int milliseconds) {
    g_p2_test_knobs.barrier_ms = milliseconds;
}
void kvspaceShmTestSetHoldDrainMilliseconds(uint32_t milliseconds) {
    g_p2_test_knobs.hold_drain_ms = milliseconds;
}
void kvspaceTestSetDataSize(size_t data_size) {
    kvspace_data_size = data_size;
}
#endif
static int kv_sync(kvspace_t *kv) {
    if (region_map(&kv->r_index,
                   INDEX_OFF + (size_t)kv->hdr->index_slab_size) != 0)
        return -1;
    if (region_map(&kv->r_head, (size_t)kv->hdr->sbo_head_size) != 0)
        return -1;
    if (region_map(&kv->r_data, (size_t)kv->hdr->sbo_data_size) != 0)
        return -1;
    kv->trie_env.base = kv->index_data;
    kv->trie_env.slab_size = kv->hdr->index_slab_size;
    return 0;
}
static int index_slab_grow(kvspace_t *kv) {
    size_t want = (size_t)kv->hdr->index_slab_size * 2;
    if (INDEX_OFF + want > kv->r_index.reserve ||
        ftruncate(kv->r_index.fd, (off_t)(INDEX_OFF + want)) != 0 ||
        region_map(&kv->r_index, INDEX_OFF + want) != 0)
        return -1;
    kv->hdr->index_slab_size = want;
    kv->index_meta->total_size = want;
    kv->trie_env.base = kv->index_data;
    kv->trie_env.slab_size = want;
    return 0;
}
#ifdef BLOCKMALLOC_COUNT_MASK
#define BM_MALLOC_BLOCKS(m) blocks_malloc_count(m)
#define BM_HEAD_SIZE(m) blocks_head_size(m)
#else
#define BM_MALLOC_BLOCKS(m) ((uint64_t)(m)->malloc_blocks)
#define BM_HEAD_SIZE(m) ((uint64_t)(m)->sizeof_block_head)
#endif
static bool sbo_pool_full(sbo_meta_t *m) {
    blocks_meta_t *p = sbo_pool(m);
    if (p->free_next_id != -1)
        return false;
    uint64_t next_end = (uint64_t)block_offset(p, BM_MALLOC_BLOCKS(p)) +
                        BM_HEAD_SIZE(p) + p->block_size;
    return next_end > p->total_size;
}
static int sbo_head_grow(kvspace_t *kv) {
    sbo_meta_t *m = kv->sbo_meta;
    blocks_meta_t *p = sbo_pool(m);
    uint64_t stride = p->sizeof_block_head + p->block_size;
    uint64_t cap = (1ULL << (p->sizeof_block_head == 2 ? 14 : 30)) * stride;
    uint64_t cur = p->total_size, want = cur * 2;
    if (want > cap)
        want = cap;
    if (SBO_HEAD_FIXED + want > kv->r_head.reserve)
        want = (kv->r_head.reserve - SBO_HEAD_FIXED) & ~(uint64_t)7;
    if (want <= cur ||
        ftruncate(kv->r_head.fd, (off_t)(SBO_HEAD_FIXED + want)) != 0 ||
        region_map(&kv->r_head, SBO_HEAD_FIXED + (size_t)want) != 0)
        return -1;
    p->total_size = want;
    m->per_slot_meta = want;
    m->max_blocks = (uint32_t)(want / m->block_stride);
    kv->hdr->sbo_head_size = SBO_HEAD_FIXED + want;
    return 0;
}
static int sbo_data_grow(kvspace_t *kv) {
    sbo_meta_t *m = kv->sbo_meta;
    uint64_t cur = m->data_size, want = cur * 64;
    if (want > kv->r_data.reserve)
        return -1;
    if (sbo_pool_full(m) && sbo_head_grow(kv) != 0)
        return -1;
    if (ftruncate(kv->r_data.fd, (off_t)want) != 0 ||
        region_map(&kv->r_data, (size_t)want) != 0)
        return -1;
    int64_t nid = blocks_alloc(sbo_pool(m), sbo_pmem(m));
    if (nid < 0)
        return -1;
    sbo_box_t *root = sbo_box(m, 0), *b = sbo_box(m, (int32_t)nid);
    memcpy(b, root, sizeof *b);
    b->parent = 0;
    b->parent_slot = 0;
    b->self_id = (int32_t)nid;
    uint64_t mm = root->child_mask;
    while (mm) {
        int i = __builtin_ctzll(mm);
        mm &= mm - 1;
        sbo_box(m, root->children[i])->parent = (int32_t)nid;
    }
    sbo_lock_t *lk = sbo_plock(m);
    uint8_t old_lvl = b->objlevel;
    if (old_lvl >= 1 && old_lvl <= 3 && lk->hot_id1[old_lvl - 1] == 1)
        lk->hot_id1[old_lvl - 1] = (uint32_t)nid + 1;
    uint8_t lvl = (uint8_t)(old_lvl + 1);
    sbo_box_format(root, lvl, -1, 255, 0);
    root->free_mask = ~1ULL;
    root->child_mask = 1ULL;
    root->states[0] = SBO_BOX;
    root->children[0] = (int32_t)nid;
    uint16_t b_comb = sbo_combined(b);
    root->child_cap[0] = b_comb;
    root->child_max_enc = b_comb;
    root->child_max_count = 1;
    root->max_obj_cap = SBO_MULT_MAX;
    root->hint_child = 0;
    m->data_size = want;
    m->slot_shift = (uint8_t)__builtin_ctzll(want);
    kv->hdr->sbo_data_size = want;
    return 0;
}
static int kv_sbo_grow(kvspace_t *kv, bool failed, uint64_t *seen_data) {
    sbo_meta_t *m = kv->sbo_meta;
    sbo_lock(sbo_plock(m));
    int rc = 0;
    if (sbo_pool_full(m))
        rc = sbo_head_grow(kv);
    else if (failed && m->data_size == *seen_data)
        rc = sbo_data_grow(kv);
    else if (failed)
        *seen_data = m->data_size;
    sbo_unlock(sbo_plock(m));
    return rc;
}
static uint64_t kv_sbo_alloc(kvspace_t *kv, size_t n) {
    sbo_meta_t *m = kv->sbo_meta;
    uint64_t seen_data = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt && kv_sync(kv) != 0)
            return (uint64_t)-1;
        if (sbo_pool_full(m) && kv_sbo_grow(kv, false, &seen_data) != 0)
            return (uint64_t)-1;
        uint64_t off = sbo_alloc(m, n);
        if (off != (uint64_t)-1)
            return off;
        if (kv_sbo_grow(kv, true, &seen_data) != 0)
            return (uint64_t)-1;
    }
    return (uint64_t)-1;
}
static void kv_sbo_free(kvspace_t *kv, uint64_t off) {
#if defined(__linux__)
    uint64_t sz = sbo_allocated_size(kv->sbo_meta, off);
    if (sz >= SBO_PUNCH_MIN) {
        uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE);
        uint64_t a = (off + pg - 1) & ~(pg - 1), b = (off + sz) & ~(pg - 1);
        if (b > a)
            (void)fallocate(kv->r_data.fd,
                            FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                            (off_t)a, (off_t)(b - a));
    }
#endif
    sbo_free(kv->sbo_meta, off);
}
_Static_assert(offsetof(blocks_meta_t, used_blocks) ==
                   offsetof(blocks_meta_t, block_size) + 2 * sizeof(uint64_t),
               "blockmalloc: sizeof_block_head/malloc_blocks must share one u64 word");
#ifdef BLOCKMALLOC_COUNT_MASK
static inline void bm_set_malloc_blocks(blocks_meta_t *m, uint64_t n) {
    blocks_set_malloc_count(m, n);
}
#else
#define BM_WORD_OFF (offsetof(blocks_meta_t, block_size) + sizeof(uint64_t))
static unsigned bm_count_shift(void) {
    blocks_meta_t t;
    memset(&t, 0, sizeof t);
    t.malloc_blocks = 1;
    uint64_t w;
    memcpy(&w, (const char *)&t + BM_WORD_OFF, sizeof w);
    return w ? (unsigned)__builtin_ctzll(w) : 64u;
}
static void bm_set_malloc_blocks(blocks_meta_t *m, uint64_t n) {
    uint64_t *w = (uint64_t *)((char *)m + BM_WORD_OFF);
    const unsigned sh = bm_count_shift();
    const uint64_t mask = ((1ULL << 60) - 1) << sh;
    uint64_t old = __atomic_load_n(w, __ATOMIC_RELAXED);
    __atomic_store_n(w, (old & ~mask) | ((n << sh) & mask), __ATOMIC_RELEASE);
}
#endif
static bool bm_layout_ok(void) {
    blocks_meta_t t;
    memset(&t, 0, sizeof t);
    t.sizeof_block_head = 8;
    t.malloc_blocks = 5;
    t.used_blocks = 7;
    bm_set_malloc_blocks(&t, 0x0123456789ABCDEull);
    return t.sizeof_block_head == 8 && t.malloc_blocks == 0x0123456789ABCDEull &&
           t.block_size == 0 && t.used_blocks == 7;
}
static int64_t index_bump(kvspace_t *kv, uint32_t blocks) {
    blocks_meta_t *meta = kv->index_meta;
    spin_lock(&meta->lock);
    uint64_t block_id = BM_MALLOC_BLOCKS(meta);
    if ((uint64_t)block_offset(meta, block_id + blocks) > meta->total_size) {
        spin_unlock(&meta->lock);
        return -1;
    }
    setblock_t(meta, kv->index_data + block_offset(meta, block_id), 1, -1);
    bm_set_malloc_blocks(meta, block_id + blocks);
    meta->used_blocks += blocks;
    spin_unlock(&meta->lock);
    return (int64_t)block_id;
}
static int trie_alloc_bytes(void *context, uint64_t bytes,
                            uint64_t *out_offset) {
    kvspace_t *kv = context;
    blocks_meta_t *meta = kv->index_meta;
    uint64_t stride = BM_HEAD_SIZE(meta) + meta->block_size;
    uint64_t needed = bytes + BM_HEAD_SIZE(meta);
    uint64_t blocks = (needed + stride - 1) / stride;
    if (blocks == 0)
        blocks = 1;
    if (blocks > UINT32_MAX)
        return -1;
    int64_t block_id = index_bump(kv, (uint32_t)blocks);
    while (block_id < 0) {
        if (index_slab_grow(kv) != 0)
            return -1;
        block_id = index_bump(kv, (uint32_t)blocks);
    }
    uint64_t offset = (uint64_t)blockdata_offset(meta, (uint64_t)block_id);
    memset(kv->index_data + offset, 0, (size_t)bytes);
    *out_offset = offset;
    return 0;
}
static int trie_make_index_value(void *context, uint64_t *out_value) {
    kvspace_t *kv = context;
    uint64_t offset = kv_sbo_alloc(kv, KVSPACE_XVALUE_HEAD_LEN);
    if (offset == (uint64_t)-1)
        return -1;
    kvspaceXvalueHead head;
    memset(&head, 0, sizeof head);
    kvspaceXvalueHeadSetLen(&head, 0);
    kvspaceXvalueHeadSetAttrsLen(&head, 0);
    memcpy(kv->sbo_data + offset, &head, sizeof head);
    *out_value = TRIE_VALUE_PACK(1, offset);
    return 0;
}
static int node_at(kvspace_t *kv, const char *key, uint64_t *out_node) {
    return trie_find_absolute(&kv->trie_env, kv->hdr->trie_root, key, out_node);
}
static int node_has_value(kvspace_t *kv, uint64_t node, uint64_t *out_offset) {
    uint64_t value = 0;
    int status = trie_value_get(&kv->trie_env, node, &value);
    if (status != KVSPACE_OK)
        return status;
    if (!TRIE_VALUE_HAS_VALUE(value))
        return KVSPACE_MISSING;
    if (out_offset != NULL)
        *out_offset = TRIE_VALUE_OFFSET(value);
    return KVSPACE_OK;
}
static int bad_dir_prefix(const char *p) {
    if (!p || !p[0])
        return 1;
    if (strcmp(p, "/") == 0)
        return 0;
    size_t l = strlen(p);
    if (p[l - 1] == '/')
        return 0;
    if (l >= 2 && (unsigned char)p[l - 2] == 0xC2 &&
        (unsigned char)p[l - 1] == 0xB7)
        return 0;
    return 1;
}
static char *edir(const char *p) {
    size_t l = strlen(p);
    if (l > 0 && p[l - 1] == '/')
        return strdup(p);
    char *r = malloc(l + 2);
    memcpy(r, p, l);
    r[l] = '/';
    r[l + 1] = '\0';
    return r;
}
typedef struct {
    char **keys;
    int32_t count;
    int32_t cap;
} keyscan_t;
static void keyscan_free(keyscan_t *scan) {
    for (int32_t i = 0; i < scan->count; i++)
        free(scan->keys[i]);
    free(scan->keys);
    scan->keys = NULL;
    scan->count = scan->cap = 0;
}
static int keyscan_add_n(keyscan_t *scan, const char *key, size_t len) {
    if (scan->count == INT32_MAX)
        return -1;
    if (scan->count == scan->cap) {
        int32_t cap = scan->cap > INT32_MAX / 2 ? INT32_MAX :
                      scan->cap ? scan->cap * 2 : 64;
        char **keys = realloc(scan->keys, (size_t)cap * sizeof *keys);
        if (!keys)
            return -1;
        scan->keys = keys;
        scan->cap = cap;
    }
    char *copy = strndup(key, len);
    if (!copy)
        return -1;
    scan->keys[scan->count++] = copy;
    return 0;
}
static bool sbo_data_size_ok(size_t n) {
    for (uint64_t s = 8; s <= (1ULL << 45); s *= 64)
        if (s == n)
            return true;
    return false;
}
static kvspace_t *open_shm_store(const char *path, size_t data_size) {
    if (!path)
        return NULL;
    if (!bm_layout_ok()) {
        fprintf(stderr, "kvspace: unexpected blockmalloc blocks_meta_t layout\n");
        return NULL;
    }
    char head_path[PATH_MAX], data_path[PATH_MAX], garb_path[PATH_MAX];
    if (snprintf(head_path, sizeof head_path, "%s.sbo.head", path) >=
            (int)sizeof head_path ||
        snprintf(data_path, sizeof data_path, "%s.sbo.data", path) >=
            (int)sizeof data_path ||
        snprintf(garb_path, sizeof garb_path, "%s.garb", path) >=
            (int)sizeof garb_path)
        return NULL;
    kvspace_t *kv = calloc(1, sizeof(*kv));
    if (!kv)
        return NULL;
    kv->p2_slot = -1;
    kv->r_index.fd = kv->r_head.fd = kv->r_data.fd = kv->r_garb.fd = -1;
    bool created = false;
    kv->r_index.fd = open(path, O_RDWR);
    if (kv->r_index.fd < 0) {
        if (!sbo_data_size_ok(data_size))
            goto fail;
        kv->r_index.fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
        if (kv->r_index.fd < 0)
            goto fail;
        created = true;
    }
    size_t index_slab, sbo_head;
    if (created) {
        index_slab = INDEX_SLAB_INIT;
        sbo_head = sbo_meta_size(data_size, SBO_HEAD_POOL_INIT);
    } else {
        kvspace_hdr_t stored;
        if (pread(kv->r_index.fd, &stored, sizeof stored, 0) !=
                (ssize_t)sizeof stored ||
            memcmp(stored.magic, KVS_MAGIC, sizeof(KVS_MAGIC) - 1) != 0)
            goto fail;
        index_slab = (size_t)stored.index_slab_size;
        sbo_head = (size_t)stored.sbo_head_size;
        data_size = (size_t)stored.sbo_data_size;
    }
    int flags = created ? O_RDWR | O_CREAT | O_TRUNC : O_RDWR;
    if (region_attach(&kv->r_index, created, INDEX_OFF + index_slab,
                      INDEX_RESERVE) != 0 ||
        (kv->r_head.fd = open(head_path, flags, 0644)) < 0 ||
        region_attach(&kv->r_head, created, sbo_head, REGION_RESERVE) != 0 ||
        (kv->r_data.fd = open(data_path, flags, 0644)) < 0 ||
        region_attach(&kv->r_data, created, data_size, DATA_RESERVE) != 0 ||
        (kv->r_garb.fd = open(garb_path, flags, 0644)) < 0 ||
        region_attach(&kv->r_garb, created,
                      (size_t)GARB_CAP * sizeof(garb_ent_t),
                      (size_t)GARB_CAP * sizeof(garb_ent_t)) != 0)
        goto fail;
    kv->hdr = (kvspace_hdr_t *)kv->r_index.base;
    kv->index_meta =
        (blocks_meta_t *)(kv->r_index.base + sizeof(kvspace_hdr_t));
    kv->index_data = kv->r_index.base + INDEX_OFF;
    kv->sbo_meta = (sbo_meta_t *)kv->r_head.base;
    kv->sbo_data = kv->r_data.base;
    kv->garb = (garb_ent_t *)kv->r_garb.base;
    kv->trie_env.context = kv;
    kv->trie_env.alloc_bytes = trie_alloc_bytes;
    kv->trie_env.make_index_value = trie_make_index_value;
    kv->trie_env.base = kv->index_data;
    kv->trie_env.slab_size = 0;
    {
        static uint32_t store_seq;
        kv->store_id = ((uint64_t)getpid() << 32) | (uint64_t)++store_seq;
    }
    if (created) {
        memset(kv->hdr, 0, sizeof(*kv->hdr));
        kv->hdr->index_slab_size = index_slab;
        kv->hdr->sbo_head_size = sbo_head;
        kv->hdr->sbo_data_size = data_size;
        kv->hdr->trie_root = 0;
        if (blocks_init(kv->index_meta, index_slab, INDEX_BLOCK_SIZE) != 0 ||
            sbo_init(kv->sbo_meta, sbo_head, data_size) != 0)
            goto fail;
        kv->index_meta->sizeof_block_head = 8;
        memcpy(kv->hdr->magic, KVS_MAGIC, sizeof(KVS_MAGIC) - 1);
        kv->trie_env.slab_size = index_slab;
        uint64_t root = 0;
        if (trie_root_create(&kv->trie_env, &root) != KVSPACE_OK)
            goto fail;
        kv->hdr->trie_root = root;
    }
    kv->trie_env.base = kv->index_data;
    kv->trie_env.slab_size = kv->hdr->index_slab_size;
    if (kv->hdr->trie_root == 0)
        goto fail;
    kv->p2_slot = p2_reg_claim(kv);
    if (kv->p2_slot == -2) {
        fprintf(stderr, "kvspace: too many processes, N=%d\n", P2_SLOT_N);
        goto fail;
    }
    if (kv->p2_slot == -3) {
        fprintf(stderr, "kvspace: claim_id exhausted (2^30 opens): "
                        "rebuild the store\n");
        goto fail;
    }
    if (kv->p2_slot < 0)
        goto fail;
    kv->pid = (uint32_t)getpid();
    (void)pthread_once(&g_atfork_once, p2_atfork_install);
    kv->fork_gen = __atomic_load_n(&g_fork_gen, __ATOMIC_ACQUIRE);
    kv->quiesced = 1;
    if (kv->hdr->committed_bytes -
            __atomic_load_n(&kv->hdr->freed_bytes, __ATOMIC_ACQUIRE) >=
            p2_garb_threshold() &&
        p2_maint_start(kv) != 0)
        goto fail;
    return kv;
fail:
    p2_maint_stop(kv);
    if (kv->p2_slot >= 0 && kv->hdr)
        p2_reg_release(kv);
    region_close(&kv->r_index);
    region_close(&kv->r_head);
    region_close(&kv->r_data);
    region_close(&kv->r_garb);
    free(kv);
    if (created) {
        unlink(path);
        unlink(head_path);
        unlink(data_path);
        unlink(garb_path);
    }
    return NULL;
}
static bool p2_other_alive(kvspace_t *kv) {
    for (int i = 0; i < P2_SLOT_N; i++) {
        if (i == kv->p2_slot)
            continue;
        p2_slot_t *s = &kv->hdr->slots[i];
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
            continue;
        if (p2_slot_alive(s))
            return true;
    }
    return false;
}
static void close_shm_store(kvspace_t *kv) {
    if (!kv)
        return;
    if (kv->pid == (uint32_t)getpid() && kv->p2_slot >= 0 &&
        !p2_other_alive(kv)) {
#if defined(__linux__)
        __atomic_store_n(&kv->hdr->slots[kv->p2_slot].quiesced, 1u,
                         __ATOMIC_RELEASE);
        (void)p2_round(kv);
#else
        uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
        uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
        for (; h < t; h++) {
            garb_ent_t *e = &kv->garb[h & (GARB_CAP - 1)];
            uint32_t m = __atomic_load_n(&e->meta, __ATOMIC_ACQUIRE);
            if (GE_STATE(m) == GE_COMMITTED) {
                uint64_t off = __atomic_load_n(&e->off, __ATOMIC_ACQUIRE);
                uint64_t cap = sbo_allocated_size(kv->sbo_meta, off);
                __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
                kv_sbo_free(kv, off);
                __atomic_store_n(&kv->hdr->freed_bytes,
                                 kv->hdr->freed_bytes + cap, __ATOMIC_RELEASE);
            } else {
                __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
            }
        }
        __atomic_store_n(&kv->hdr->garb_head, t, __ATOMIC_RELEASE);
#endif
    }
    p2_maint_stop(kv);
    p2_reg_release(kv);
    region_close(&kv->r_index);
    region_close(&kv->r_head);
    region_close(&kv->r_data);
    region_close(&kv->r_garb);
    free(kv);
}
static int parse_value(const uint8_t *s, uint64_t bound, int32_t *ol) {
    if (bound > INT32_MAX)
        bound = INT32_MAX;
    kvspaceXvalueHead wire;
    if (head_of(s, (int32_t)bound, &wire) != 0)
        return -1;
    const uint64_t total = kvspaceXvalueHeadTotalLen(&wire);
    if (total == 0 || total > bound)
        return -1;
    *ol = (int32_t)total;
    return 0;
}
static uint64_t ro_bound(const kvspace_t *kv, uint64_t off) {
    uint64_t m = kv->r_data.mapped;
    return off < m ? m - off : 0;
}
static int read_value_ro(kvspace_t *kv, uint64_t off, uint8_t **out,
                         int32_t *ol) {
    *out = kv->sbo_data + off;
    return parse_value(*out, ro_bound(kv, off), ol);
}
static int resolve_fetch(kvspace_t *kv, const char *path, char *out, int osz,
                         uint8_t **raw, int32_t *raw_length, int *fetched) {
    *fetched = 0;
    strncpy(out, path, osz - 1);
    out[osz - 1] = '\0';
    for (int depth = 0; depth < 16; depth++) {
        uint64_t node = 0;
        if (node_at(kv, out, &node) != KVSPACE_OK)
            return -1;
        uint64_t offset = 0;
        if (node_has_value(kv, node, &offset) != KVSPACE_OK)
            return -1;
        uint8_t *bytes = kv->sbo_data + offset;
        uint64_t size = ro_bound(kv, offset);
        kvspaceXvalueHead wire;
        if (head_of(bytes, size > INT32_MAX ? INT32_MAX : (int32_t)size,
                    &wire) == 0) {
            uint64_t length = kvspaceXvalueHeadLen(&wire);
            if (kvspace_c_reference_of(bytes, size) !=
                    KVSPACE_XVALUE_REFERENCE_PTR ||
                length >= (uint64_t)osz) {
                if (parse_value(bytes, size, raw_length) != 0)
                    return -1;
                *raw = bytes;
                *fetched = 1;
                return 0;
            }
            memcpy(out, bytes + KVSPACE_XVALUE_HEAD_LEN, (size_t)length);
            out[length] = 0;
            continue;
        }
        return -1;
    }
    return -1;
}
static void resolve_path(kvspace_t *kv, const char *path, char *out, int osz) {
    uint8_t *raw;
    int32_t raw_length;
    int fetched;
    (void)resolve_fetch(kv, path, out, osz, &raw, &raw_length, &fetched);
}
static int dir_ext_path(kvspace_t *kv, const char *directory, char *out,
                        int osz) {
    out[0] = 0;
    uint64_t node = 0;
    if (node_at(kv, directory, &node) != KVSPACE_OK)
        return 0;
    uint64_t offset = 0;
    if (node_has_value(kv, node, &offset) != KVSPACE_OK)
        return 0;
    uint8_t *raw;
    int32_t raw_length;
    if (read_value_ro(kv, offset, &raw, &raw_length) < 0)
        return 0;
    kvspaceXvalueHead wire;
    if (head_of(raw, raw_length, &wire) != 0 ||
        kvspace_c_reference_of(raw, (uint64_t)raw_length) !=
            KVSPACE_XVALUE_REFERENCE_EXT)
        return 0;
    uint64_t length = kvspaceXvalueHeadLen(&wire);
    if (length == 0 || length >= (uint64_t)osz)
        return 0;
    memcpy(out, raw + KVSPACE_XVALUE_HEAD_LEN, (size_t)length);
    out[length] = 0;
    return 1;
}
static int shm_del_key(kvspace_t *kv, const char *key) {
    uint64_t node = 0;
    int status = node_at(kv, key, &node);
    if (status == KVSPACE_MISSING)
        return 0;
    if (status != KVSPACE_OK)
        return -1;
    uint64_t value = 0;
    if (trie_value_get(&kv->trie_env, node, &value) != KVSPACE_OK)
        return -1;
    if (!TRIE_VALUE_HAS_VALUE(value))
        return 0;
    uint64_t garbage_index = 0;
    if (garb_reserve(kv, &garbage_index) != 0)
        return -1;
    uint64_t offset = TRIE_VALUE_OFFSET(value);
    uint64_t capacity = sbo_allocated_size(kv->sbo_meta, offset);
    status = trie_remove(&kv->trie_env, kv->hdr->trie_root, key);
    if (status == TRIE_HAS_CHILDREN) {
        garb_cancel(kv, garbage_index);
        return KVSPACE_HAS_CHILDREN;
    }
    if (status != KVSPACE_OK) {
        garb_cancel(kv, garbage_index);
        return -1;
    }
    garb_commit(kv, garbage_index, offset, capacity);
    return 1;
}
static char *memdir(const char *p) {
    size_t l = strlen(p);
    char *r = malloc(l + 3);
    memcpy(r, p, l);
    r[l] = (char)0xC2;
    r[l + 1] = (char)0xB7;
    r[l + 2] = 0;
    return r;
}
typedef struct {
    keyscan_t keys;
    const char *prefix;
} subtree_scan_t;
static int ends_with_middle_dot(const char *text, size_t length) {
    return length >= 2 && (unsigned char)text[length - 2] == 0xC2 &&
           (unsigned char)text[length - 1] == 0xB7;
}
static int subtree_collect(void *context, const char *suffix,
                           uint64_t node_offset, uint64_t value) {
    subtree_scan_t *scan = context;
    (void)node_offset;
    (void)value;
    size_t prefix_length = strlen(scan->prefix);
    size_t suffix_length = strlen(suffix);
    if (prefix_length + suffix_length >= TRIE_KEY_LIMIT)
        return -1;
    char *key = malloc(prefix_length + suffix_length + 1);
    if (key == NULL)
        return -1;
    memcpy(key, scan->prefix, prefix_length);
    memcpy(key + prefix_length, suffix, suffix_length + 1);
    int status = keyscan_add_n(&scan->keys, key, prefix_length + suffix_length);
    free(key);
    return status;
}
static int delete_subtree(kvspace_t *kv, uint64_t node, const char *key) {
    subtree_scan_t scan;
    memset(&scan, 0, sizeof scan);
    scan.prefix = key;
    uint32_t count = 0;
    if (trie_subtree_visit(&kv->trie_env, node, subtree_collect, &scan,
                           &count) != KVSPACE_OK) {
        keyscan_free(&scan.keys);
        return -1;
    }
    int status = 0;
    int deleted_any = 0;
    for (int32_t index = scan.keys.count - 1; index >= 0; index--) {
        int deleted = shm_del_key(kv, scan.keys.keys[index]);
        if (deleted == 1)
            deleted_any = 1;
        else if (deleted < 0)
            status = -1;
    }
    keyscan_free(&scan.keys);
    if (deleted_any)
        list_gen_bump(kv);
    return status;
}
static int delete_root_children(kvspace_t *kv) {
    subtree_scan_t scan;
    memset(&scan, 0, sizeof scan);
    scan.prefix = "/";
    uint32_t count = 0;
    if (trie_subtree_visit(&kv->trie_env, kv->hdr->trie_root, subtree_collect,
                           &scan, &count) != KVSPACE_OK) {
        keyscan_free(&scan.keys);
        return -1;
    }
    int status = 0;
    int deleted_any = 0;
    for (int32_t index = scan.keys.count - 1; index >= 0; index--) {
        const char *key = scan.keys.keys[index];
        if (key[0] == '/' && key[1] == 0)
            continue;
        int deleted = shm_del_key(kv, key);
        if (deleted == 1)
            deleted_any = 1;
        else if (deleted < 0)
            status = -1;
    }
    keyscan_free(&scan.keys);
    if (deleted_any)
        list_gen_bump(kv);
    return status;
}
static size_t last_segment_start(const char *base, size_t length) {
    size_t start = 1;
    for (size_t index = 1; index < length; index++) {
        if (base[index] == '/') {
            start = index + 1;
        } else if ((unsigned char)base[index] == 0xC2 &&
                   index + 1 < length &&
                   (unsigned char)base[index + 1] == 0xB7) {
            start = index + 2;
            index++;
        }
    }
    return start;
}
static int delete_tree(kvspace_t *kv, const char *prefix) {
    if (!kv || !prefix || reserved_meta_path(prefix))
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    size_t length = strlen(prefix);
    if (length >= TRIE_KEY_LIMIT)
        return -1;
    char base[TRIE_KEY_LIMIT];
    memcpy(base, prefix, length + 1);
    size_t base_length = length;
    while (base_length > 1 && base[base_length - 1] == '/')
        base_length--;
    if (ends_with_middle_dot(base, base_length))
        base_length -= 2;
    base[base_length] = 0;
    if (base_length == 0 || base[0] != '/')
        return 0;
    if (base_length > 1 && reserved_meta_path(base))
        return -1;
    if (base_length == 1)
        return delete_root_children(kv);
    size_t parent_length = last_segment_start(base, base_length);
    const char *segment = base + parent_length;
    size_t segment_length = base_length - parent_length;
    char parent_key[TRIE_KEY_LIMIT];
    memcpy(parent_key, base, parent_length);
    parent_key[parent_length] = 0;
    uint64_t parent = 0;
    int status = node_at(kv, parent_key, &parent);
    if (status == KVSPACE_MISSING)
        return 0;
    if (status != KVSPACE_OK)
        return -1;
    uint32_t count = 0;
    if (trie_child_count(&kv->trie_env, parent, &count) != KVSPACE_OK)
        return -1;
    keyscan_t matches;
    memset(&matches, 0, sizeof matches);
    for (uint32_t index = 0; index < count; index++) {
        const uint8_t *name = NULL;
        uint16_t name_length = 0;
        uint64_t child = 0;
        if (trie_child_at(&kv->trie_env, parent, index, &name, &name_length,
                          &child) != KVSPACE_OK) {
            keyscan_free(&matches);
            return -1;
        }
        (void)child;
        int matched =
            (name_length == segment_length &&
             memcmp(name, segment, segment_length) == 0) ||
            (name_length == segment_length + 1 &&
             memcmp(name, segment, segment_length) == 0 &&
             name[segment_length] == '/') ||
            (name_length == segment_length + 2 &&
             memcmp(name, segment, segment_length) == 0 &&
             name[segment_length] == 0xC2 &&
             name[segment_length + 1] == 0xB7);
        if (!matched)
            continue;
        char key[TRIE_KEY_LIMIT];
        if (parent_length + name_length >= TRIE_KEY_LIMIT) {
            keyscan_free(&matches);
            return -1;
        }
        memcpy(key, parent_key, parent_length);
        memcpy(key + parent_length, name, name_length);
        key[parent_length + name_length] = 0;
        if (keyscan_add_n(&matches, key, parent_length + name_length) != 0) {
            keyscan_free(&matches);
            return -1;
        }
    }
    status = 0;
    for (int32_t index = 0; index < matches.count; index++) {
        uint64_t child = 0;
        if (node_at(kv, matches.keys[index], &child) != KVSPACE_OK ||
            delete_subtree(kv, child, matches.keys[index]) != 0) {
            status = -1;
            break;
        }
    }
    keyscan_free(&matches);
    return status;
}
static int read_wire(kvspace_t *kv, const char *key, const uint8_t **out,
                     uint64_t *out_len) {
    uint64_t off = 0;
    if (resolve_root(kv, key, &off) != KVSPACE_OK)
        return -1;
    uint8_t *base = storage_data_base(kv);
    if (!base)
        return -1;
    const uint8_t *wire = base + off;
    const kvspaceXvalueHead *head = (const kvspaceXvalueHead *)(const void *)wire;
    uint64_t total = kvspaceXvalueHeadTotalLen(head);
    if (total < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    *out = wire;
    *out_len = total;
    return 0;
}
static int write_wire(kvspace_t *kv, const char *key, const uint8_t *wire,
                      uint64_t len) {
    if (len < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    const kvspaceXvalueHead *head = (const kvspaceXvalueHead *)(const void *)wire;
    uint64_t body = kvspaceXvalueHeadLen(head);
    uint64_t attrs = kvspaceXvalueHeadAttrsLen(head);
    if (KVSPACE_XVALUE_HEAD_LEN + body + attrs != len)
        return -1;
    uint64_t off = 0;
    if (realloc_key(kv, key, body, attrs, &off) != KVSPACE_OK)
        return -1;
    uint8_t *base = storage_data_base(kv);
    if (!base)
        return -1;
    uint8_t *dst = base + off;
    memcpy(dst + KVSPACE_XVALUE_HEAD_LEN, wire + KVSPACE_XVALUE_HEAD_LEN, body);
    if (attrs)
        memcpy(dst + KVSPACE_XVALUE_HEAD_LEN + body,
               wire + KVSPACE_XVALUE_HEAD_LEN + body, attrs);
    return 0;
}
static int copy_key(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst)
        return -1;
    const uint8_t *raw = NULL;
    uint64_t length = 0;
    if (read_wire(kv, src, &raw, &length) != 0)
        return -1;
    uint8_t *temporary = malloc((size_t)length);
    if (temporary == NULL)
        return -1;
    memcpy(temporary, raw, (size_t)length);
    int status = write_wire(kv, dst, temporary, length);
    free(temporary);
    return status;
}
static int ends_with_slash(const char *text) {
    size_t length = strlen(text);
    return length > 0 && text[length - 1] == '/';
}
static char *make_directory(const char *path, int middle_dot) {
    size_t length = strlen(path);
    if (middle_dot ? ends_with_middle_dot(path, length) : ends_with_slash(path))
        return strdup(path);
    char *result = malloc(length + (middle_dot ? 2u : 1u) + 1u);
    if (result == NULL)
        return NULL;
    memcpy(result, path, length);
    if (middle_dot) {
        result[length] = (char)0xC2;
        result[length + 1] = (char)0xB7;
        result[length + 2] = 0;
    } else {
        result[length] = '/';
        result[length + 1] = 0;
    }
    return result;
}
static char *join_child(const char *directory, const uint8_t *name,
                        uint16_t name_length) {
    size_t directory_length = strlen(directory);
    char *result = malloc(directory_length + name_length + 1);
    if (result == NULL)
        return NULL;
    memcpy(result, directory, directory_length);
    memcpy(result + directory_length, name, name_length);
    result[directory_length + name_length] = 0;
    return result;
}
static int copy_node(kvspace_t *kv, const char *src, const char *dst);
static int copy_children(kvspace_t *kv, const char *src_directory,
                         const char *dst_directory) {
    uint64_t node = 0;
    int status = node_at(kv, src_directory, &node);
    if (status == KVSPACE_MISSING)
        return 0;
    if (status != KVSPACE_OK)
        return -1;
    uint32_t count = 0;
    if (trie_child_count(&kv->trie_env, node, &count) != KVSPACE_OK)
        return -1;
    int result = 0;
    for (uint32_t index = 0; index < count; index++) {
        const uint8_t *name = NULL;
        uint16_t name_length = 0;
        uint64_t child = 0;
        if (trie_child_at(&kv->trie_env, node, index, &name, &name_length,
                          &child) != KVSPACE_OK) {
            result = -1;
            break;
        }
        (void)child;
        char *src_key = join_child(src_directory, name, name_length);
        char *dst_key = join_child(dst_directory, name, name_length);
        if (src_key == NULL || dst_key == NULL ||
            copy_node(kv, src_key, dst_key) != 0)
            result = -1;
        free(src_key);
        free(dst_key);
        if (result != 0)
            break;
    }
    return result;
}
static int copy_node(kvspace_t *kv, const char *src, const char *dst) {
    size_t src_length = strlen(src);
    int src_is_directory =
        ends_with_slash(src) || ends_with_middle_dot(src, src_length);
    char *dst_value = NULL;
    if (ends_with_slash(src))
        dst_value = make_directory(dst, 0);
    else if (ends_with_middle_dot(src, src_length))
        dst_value = make_directory(dst, 1);
    else
        dst_value = strdup(dst);
    if (dst_value == NULL)
        return -1;
    const uint8_t *raw = NULL;
    uint64_t length = 0;
    if (read_wire(kv, src, &raw, &length) == 0 &&
        copy_key(kv, src, dst_value) != 0) {
        free(dst_value);
        return -1;
    }
    int status = 0;
    if (src_is_directory) {
        status = copy_children(kv, src, dst_value);
    } else {
        char *src_directory = make_directory(src, 0);
        char *dst_directory = make_directory(dst_value, 0);
        if (src_directory == NULL || dst_directory == NULL) {
            free(src_directory);
            free(dst_directory);
            free(dst_value);
            return -1;
        }
        status = copy_children(kv, src_directory, dst_directory);
        free(src_directory);
        free(dst_directory);
        if (status == 0) {
            src_directory = make_directory(src, 1);
            dst_directory = make_directory(dst_value, 1);
            if (src_directory == NULL || dst_directory == NULL) {
                free(src_directory);
                free(dst_directory);
                status = -1;
            } else {
                status = copy_children(kv, src_directory, dst_directory);
                free(src_directory);
                free(dst_directory);
            }
        }
    }
    free(dst_value);
    return status;
}
static int copy_direct_children(kvspace_t *kv, const char *src_directory,
                                const char *dst_directory) {
    uint64_t node = 0;
    int status = node_at(kv, src_directory, &node);
    if (status == KVSPACE_MISSING)
        return 0;
    if (status != KVSPACE_OK)
        return -1;
    uint32_t count = 0;
    if (trie_child_count(&kv->trie_env, node, &count) != KVSPACE_OK)
        return -1;
    int result = 0;
    for (uint32_t index = 0; index < count; index++) {
        const uint8_t *name = NULL;
        uint16_t name_length = 0;
        uint64_t child = 0;
        if (trie_child_at(&kv->trie_env, node, index, &name, &name_length,
                          &child) != KVSPACE_OK) {
            result = -1;
            break;
        }
        (void)child;
        char *src_key = join_child(src_directory, name, name_length);
        char *dst_key = join_child(dst_directory, name, name_length);
        if (src_key == NULL || dst_key == NULL ||
            copy_key(kv, src_key, dst_key) != 0)
            result = -1;
        free(src_key);
        free(dst_key);
        if (result != 0)
            break;
    }
    return result;
}
static int copy_tree(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst || reserved_meta_path(src) ||
        reserved_meta_path(dst))
        return -1;
    size_t src_length = strlen(src);
    while (src_length > 1 && src[src_length - 1] == '/')
        src_length--;
    if (src_length == 1 && src[0] == '/' && dst[0] == '/')
        return dst[1] ? -1 : 0;
    if (strncmp(src, dst, src_length) == 0) {
        if (dst[src_length] == 0 ||
            (dst[src_length] == '/' && dst[src_length + 1] == 0))
            return 0;
        if (dst[src_length] == '/' ||
            ((unsigned char)dst[src_length] == 0xc2 &&
             (unsigned char)dst[src_length + 1] == 0xb7))
            return -1;
    }
    delete_tree(kv, dst);
    return copy_node(kv, src, dst);
}
int kvspaceShmCplist(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst || reserved_meta_path(src) ||
        reserved_meta_path(dst))
        return -1;
    if (strcmp(src, dst) == 0)
        return 0;
    delete_tree(kv, dst);
    const uint8_t *raw = NULL;
    uint64_t length = 0;
    if (read_wire(kv, src, &raw, &length) == 0 && copy_key(kv, src, dst) != 0)
        return -1;
    if (ends_with_slash(src) || ends_with_middle_dot(src, strlen(src)))
        return 0;
    char *src_directory = make_directory(src, 1);
    char *dst_directory = make_directory(dst, 1);
    if (src_directory == NULL || dst_directory == NULL) {
        free(src_directory);
        free(dst_directory);
        return -1;
    }
    int status = copy_direct_children(kv, src_directory, dst_directory);
    free(src_directory);
    free(dst_directory);
    return status;
}
static int same_child(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    if (na && a[na - 1] == '/') na--;
    if (nb && b[nb - 1] == '/') nb--;
    return na == nb && memcmp(a, b, na) == 0;
}
static int list_target(kvspace_t *kv, const char *prefix, bool expand_ext,
                       int resolve, char *target, size_t target_capacity,
                       char *ext, size_t ext_capacity) {
    ext[0] = 0;
    if (reserved_meta_path(prefix))
        return 1;
    if (kv_sync(kv) != 0 || bad_dir_prefix(prefix))
        return -1;
    if (resolve) {
        if (strlen(prefix) >= TRIE_KEY_LIMIT)
            return -1;
        resolve_path(kv, prefix, target, (int)target_capacity);
    } else {
        size_t length = strlen(prefix);
        if (length >= target_capacity)
            return -1;
        memcpy(target, prefix, length + 1);
    }
    if (reserved_meta_path(target))
        return 1;
    if (expand_ext) {
        char *directory = edir(target);
        if (directory == NULL)
            return -1;
        if (!dir_ext_path(kv, directory, ext, (int)ext_capacity))
            ext[0] = 0;
        free(directory);
    }
    return 0;
}
typedef struct {
    keyscan_t names;
    int hide_root_meta;
} children_scan_t;
static int name_contains_marker(const char *name, size_t length) {
    for (size_t index = 0; index + 2 < length; index++)
        if ((unsigned char)name[index] == 0xE2 &&
            (unsigned char)name[index + 1] == 0x80 &&
            (unsigned char)name[index + 2] == 0xA5)
            return 1;
    return 0;
}
static int single_segment_name(const char *name, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (name[index] == '/')
            return index + 1 == length;
        if ((unsigned char)name[index] == 0xC2 && index + 1 < length &&
            (unsigned char)name[index + 1] == 0xB7)
            return index + 2 == length;
    }
    return 1;
}
static int list_scan_visit(void *context, const char *suffix,
                           uint64_t node_offset, uint64_t value) {
    children_scan_t *scan = context;
    (void)node_offset;
    (void)value;
    size_t length = strlen(suffix);
    if (length == 0 || !single_segment_name(suffix, length))
        return 0;
    if (scan->hide_root_meta && length == sizeof(".kvspace-meta") - 1 &&
        memcmp(suffix, ".kvspace-meta", length) == 0)
        return 0;
    if (name_contains_marker(suffix, length))
        return 0;
    return keyscan_add_n(&scan->names, suffix, length) == 0 ? 0 : -1;
}
static int list_scan_directory(kvspace_t *kv, const char *directory,
                               children_scan_t *scan) {
    uint64_t node = 0;
    int status = node_at(kv, directory, &node);
    if (status == KVSPACE_MISSING)
        return 0;
    if (status != KVSPACE_OK)
        return -1;
    uint32_t count = 0;
    if (trie_subtree_visit(&kv->trie_env, node, list_scan_visit, scan,
                           &count) != KVSPACE_OK)
        return -1;
    return 0;
}
static int list_scan(kvspace_t *kv, const char *prefix, const char *ext,
                     char ***out_names, int32_t *out_count) {
    children_scan_t children;
    memset(&children, 0, sizeof children);
    children.hide_root_meta = strcmp(prefix, "/") == 0;
    if (list_scan_directory(kv, prefix, &children) != 0)
        goto fail;
    if (ext[0] && list_scan_directory(kv, ext, &children) != 0)
        goto fail;
    int32_t keep = 0;
    if (children.names.count > 1) {
        uint32_t buckets = 8u;
        while (buckets < (uint32_t)children.names.count * 2u)
            buckets <<= 1u;
        int32_t *table = malloc((size_t)buckets * sizeof *table);
        if (table == NULL)
            goto fail;
        int32_t *next = malloc((size_t)children.names.count * sizeof *next);
        if (next == NULL) {
            free(table);
            goto fail;
        }
        for (uint32_t bucket = 0; bucket < buckets; bucket++)
            table[bucket] = -1;
        for (int32_t index = 0; index < children.names.count; index++) {
            const char *name = children.names.keys[index];
            const size_t name_length = strlen(name);
            size_t base_length = name_length;
            if (base_length > 0 && name[base_length - 1] == '/')
                base_length--;
            uint32_t hash = 2166136261u;
            for (size_t at = 0; at < base_length; at++)
                hash = (hash ^ (unsigned char)name[at]) * 16777619u;
            uint32_t bucket = hash & (buckets - 1u);
            int32_t twin = -1;
            for (int32_t slot = table[bucket]; slot >= 0; slot = next[slot])
                if (same_child(children.names.keys[slot], name)) {
                    twin = slot;
                    break;
                }
            if (twin >= 0) {
                if (name[name_length - 1] == '/') {
                    free(children.names.keys[twin]);
                    children.names.keys[twin] = children.names.keys[index];
                } else {
                    free(children.names.keys[index]);
                }
                continue;
            }
            children.names.keys[keep] = children.names.keys[index];
            next[keep] = table[bucket];
            table[bucket] = keep;
            keep++;
        }
        free(next);
        free(table);
        children.names.count = keep;
        *out_names = children.names.keys;
        *out_count = children.names.count;
        return 0;
    }
    for (int32_t index = 0; index < children.names.count; index++)
        children.names.keys[keep++] = children.names.keys[index];
    children.names.count = keep;
    *out_names = children.names.keys;
    *out_count = children.names.count;
    return 0;
fail:
    keyscan_free(&children.names);
    return -1;
}
static int list_names(kvspace_t *kv, const char *prefix, bool expand_ext,
                      int resolve, char ***out_names, int32_t *out_count) {
    if (!kv || !prefix || !out_names || !out_count)
        return -1;
    *out_names = NULL;
    *out_count = 0;
    char target[4096], ext[1024];
    int status = list_target(kv, prefix, expand_ext, resolve, target,
                             sizeof target, ext, sizeof ext);
    if (status != 0)
        return status > 0 ? 0 : -1;
    return list_scan(kv, target, ext, out_names, out_count);
}
int kvspaceShmListLen(kvspace_t *kv, const char *prefix, bool expand_ext,
                      int resolve, int32_t *out_count) {
    if (!out_count)
        return -1;
    *out_count = 0;
    if (!kv || !prefix)
        return -1;
    char **names = NULL;
    int32_t count = 0;
    int status = list_names(kv, prefix, expand_ext, resolve, &names, &count);
    if (status != 0)
        return -1;
    for (int32_t index = 0; index < count; index++)
        free(names[index]);
    free(names);
    *out_count = count;
    return 0;
}
static int storage_locate(kvspace_t *kv, const char *key, uint64_t *out_node,
                          uint64_t *out_offset) {
    uint64_t node = 0;
    int status = node_at(kv, key, &node);
    if (status != KVSPACE_OK)
        return status;
    if (out_node != NULL)
        *out_node = node;
    return node_has_value(kv, node, out_offset);
}
static int storage_anchor_ok(kvspace_t *kv, uint64_t anchor) {
    trie_hdr_t *header = NULL;
    if (trie_node_header(&kv->trie_env, anchor, &header) != KVSPACE_OK)
        return 0;
    return (header->flags & TRIE_FLAG_DEAD) == 0;
}
static int resolve_anchor(kvspace_t *kv, const char *key, uint64_t anchor,
                          uint64_t *out_offset) {
    if (!kv || !key || !out_offset)
        return KVSPACE_INVALID;
    *out_offset = 0;
    if (reserved_meta_path(key))
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    if (!storage_anchor_ok(kv, anchor))
        return KVSPACE_ANCHOR_INVALID;
    uint64_t node = 0;
    int status = trie_find_relative(&kv->trie_env, anchor, key, &node);
    if (status != KVSPACE_OK)
        return status;
    return node_has_value(kv, node, out_offset);
}
static uint8_t *storage_data_base(kvspace_t *kv) {
    return kv ? kv->sbo_data : NULL;
}
static int resolve_root(kvspace_t *kv, const char *key, uint64_t *out_offset) {
    if (!kv || !key || !out_offset)
        return KVSPACE_INVALID;
    *out_offset = 0;
    if (reserved_meta_path(key))
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    return storage_locate(kv, key, NULL, out_offset);
}
#ifdef P2_TEST_HOOKS
int kvspaceShmTestAnchorOf(kvspace_t *kv, const char *parent_key,
                           int64_t *out_anchor) {
    if (!kv || !parent_key || !out_anchor)
        return KVSPACE_INVALID;
    if (reserved_meta_path(parent_key))
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    uint64_t node = 0;
    int status = node_at(kv, parent_key, &node);
    if (status != KVSPACE_OK)
        return status;
    if (node == 0)
        return KVSPACE_BACKEND;
    *out_anchor = (int64_t)node;
    return KVSPACE_OK;
}
#endif
static void storage_write_head(uint8_t *dst, uint64_t body_len,
                               uint64_t attr_len) {
    kvspaceXvalueHead h;
    memset(&h, 0, sizeof h);
    kvspaceXvalueHeadSetLen(&h, body_len);
    kvspaceXvalueHeadSetAttrsLen(&h, (uint16_t)attr_len);
    memcpy(dst, &h, sizeof h);
}
static int realloc_key(kvspace_t *kv, const char *key, uint64_t body_len,
                       uint64_t attr_len, uint64_t *out_offset) {
    if (!kv || !key || !out_offset)
        return KVSPACE_INVALID;
    *out_offset = 0;
    if (reserved_meta_path(key) || strstr(key, "//") ||
        strlen(key) >= TRIE_KEY_LIMIT)
        return KVSPACE_INVALID;
    if (attr_len > UINT16_MAX)
        return KVSPACE_INVALID;
    uint64_t need = KVSPACE_XVALUE_HEAD_LEN + body_len;
    if (need < body_len || need > UINT64_MAX - attr_len)
        return KVSPACE_INVALID;
    need += attr_len;
    if (need > INT32_MAX)
        return KVSPACE_INVALID;
    trie_segment_t segments[TRIE_MAX_SEGMENTS];
    int segment_count = 0;
    int parse = trie_key_segments(key, segments, TRIE_MAX_SEGMENTS,
                                  &segment_count);
    if (parse != KVSPACE_OK)
        return parse;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    uint64_t node = 0;
    int found = trie_find_absolute(&kv->trie_env, kv->hdr->trie_root, key,
                                   &node);
    if (found != KVSPACE_OK && found != KVSPACE_MISSING)
        return found == KVSPACE_INVALID ? KVSPACE_INVALID : KVSPACE_BACKEND;
    uint64_t old_value = 0;
    int has_old = 0;
    if (found == KVSPACE_OK) {
        if (trie_value_get(&kv->trie_env, node, &old_value) != KVSPACE_OK)
            return KVSPACE_BACKEND;
        has_old = TRIE_VALUE_HAS_VALUE(old_value);
    }
    uint64_t garbage_index = 0;
    uint64_t old_capacity = 0;
    if (has_old) {
        uint64_t old_offset = TRIE_VALUE_OFFSET(old_value);
        old_capacity = sbo_allocated_size(kv->sbo_meta, old_offset);
        if (need <= old_capacity) {
            (void)trie_value_set(&kv->trie_env, node,
                                 TRIE_VALUE_PACK(1, old_offset));
            storage_write_head(kv->sbo_data + old_offset, body_len, attr_len);
            (void)trie_value_set(&kv->trie_env, node,
                                 TRIE_VALUE_PACK(1, old_offset));
            *out_offset = old_offset;
            return KVSPACE_OK;
        }
        if (garb_reserve(kv, &garbage_index) != 0)
            return KVSPACE_BACKEND;
    }
    uint64_t offset = kv_sbo_alloc(kv, (size_t)need);
    if (offset == (uint64_t)-1) {
        if (has_old)
            garb_cancel(kv, garbage_index);
        return KVSPACE_BACKEND;
    }
    memset(kv->sbo_data + offset, 0, (size_t)need);
    storage_write_head(kv->sbo_data + offset, body_len, attr_len);
    uint64_t new_value = TRIE_VALUE_PACK(1, offset);
    int status;
    if (found == KVSPACE_OK)
        status = trie_value_set(&kv->trie_env, node, new_value);
    else
        status = trie_write(&kv->trie_env, kv->hdr->trie_root, key, new_value,
                            &node);
    if (status != KVSPACE_OK) {
        if (has_old)
            garb_cancel(kv, garbage_index);
        kv_sbo_free(kv, offset);
        return status == KVSPACE_INVALID ? KVSPACE_INVALID : KVSPACE_BACKEND;
    }
    if (has_old)
        garb_commit(kv, garbage_index, TRIE_VALUE_OFFSET(old_value),
                    old_capacity);
    else
        list_gen_bump(kv);
    *out_offset = offset;
    return KVSPACE_OK;
}
static int storage_is_index_key(const char *key) {
    size_t length = strlen(key);
    if (length == 0)
        return 0;
    if (key[length - 1] == '/')
        return 1;
    return length >= 2 && (unsigned char)key[length - 2] == 0xC2 &&
           (unsigned char)key[length - 1] == 0xB7;
}
static int storage_has_children(kvspace_t *kv, const char *key) {
    uint64_t node = 0;
    int status = node_at(kv, key, &node);
    if (status == KVSPACE_MISSING)
        return KVSPACE_OK;
    if (status != KVSPACE_OK)
        return KVSPACE_BACKEND;
    uint32_t count = 0;
    if (trie_child_count(&kv->trie_env, node, &count) != KVSPACE_OK)
        return KVSPACE_BACKEND;
    return count > 0 ? KVSPACE_HAS_CHILDREN : KVSPACE_OK;
}
static int free_key(kvspace_t *kv, const char *key) {
    if (!kv || !key)
        return KVSPACE_INVALID;
    if (reserved_meta_path(key) || strlen(key) >= TRIE_KEY_LIMIT)
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    if (storage_is_index_key(key)) {
        int child_status = storage_has_children(kv, key);
        if (child_status != KVSPACE_OK)
            return child_status;
    }
    int status = shm_del_key(kv, key);
    if (status == KVSPACE_HAS_CHILDREN)
        return KVSPACE_HAS_CHILDREN;
    if (status < 0)
        return KVSPACE_BACKEND;
    if (status == 0)
        return KVSPACE_MISSING;
    list_gen_bump(kv);
    return KVSPACE_OK;
}
static int makeindex_key(kvspace_t *kv, const char *key) {
    if (!kv || !key)
        return KVSPACE_INVALID;
    if (reserved_meta_path(key) || strlen(key) >= TRIE_KEY_LIMIT ||
        strstr(key, "//"))
        return KVSPACE_INVALID;
    if (!storage_is_index_key(key))
        return KVSPACE_INVALID;
    uint64_t offset = 0;
    int status = resolve_root(kv, key, &offset);
    if (status == KVSPACE_OK)
        return KVSPACE_OK;
    if (status != KVSPACE_MISSING)
        return status;
    return realloc_key(kv, key, 0, 0, &offset);
}
static int storage_index_state(kvspace_t *kv, const char *key) {
    uint64_t node = 0;
    int status = node_at(kv, key, &node);
    if (!storage_is_index_key(key))
        return status == KVSPACE_OK ? KVSPACE_NOT_INDEX : KVSPACE_MISSING;
    if (status != KVSPACE_OK)
        return KVSPACE_MISSING;
    return KVSPACE_OK;
}
static int storage_children(kvspace_t *kv, const char *key, keyscan_t *out) {
    uint64_t node = 0;
    int status = node_at(kv, key, &node);
    if (status == KVSPACE_MISSING)
        return KVSPACE_MISSING;
    if (status != KVSPACE_OK)
        return KVSPACE_BACKEND;
    uint32_t count = 0;
    if (trie_child_count(&kv->trie_env, node, &count) != KVSPACE_OK)
        return KVSPACE_BACKEND;
    for (uint32_t index = 0; index < count; index++) {
        const uint8_t *name = NULL;
        uint16_t name_length = 0;
        uint64_t child = 0;
        if (trie_child_at(&kv->trie_env, node, index, &name, &name_length,
                          &child) != KVSPACE_OK)
            return KVSPACE_BACKEND;
        if (keyscan_add_n(out, (const char *)name, name_length) != 0)
            return KVSPACE_BACKEND;
    }
    return KVSPACE_OK;
}
static int listlen_key(kvspace_t *kv, const char *key, int64_t *out_count,
                       uint32_t *out_max_name_length) {
    if (!kv || !key || !out_count || !out_max_name_length)
        return KVSPACE_INVALID;
    *out_count = 0;
    *out_max_name_length = 0;
    if (reserved_meta_path(key) || strlen(key) >= TRIE_KEY_LIMIT)
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    int status = storage_index_state(kv, key);
    if (status != KVSPACE_OK)
        return status;
    uint64_t node = 0;
    if (node_at(kv, key, &node) != KVSPACE_OK)
        return KVSPACE_BACKEND;
    uint32_t count = 0;
    if (trie_child_count(&kv->trie_env, node, &count) != KVSPACE_OK)
        return KVSPACE_BACKEND;
    uint16_t longest = 0;
    if (trie_longest_child_name(&kv->trie_env, node, &longest) != KVSPACE_OK)
        return KVSPACE_BACKEND;
    *out_count = count;
    *out_max_name_length = longest;
    return KVSPACE_OK;
}
static int list_names_raw(kvspace_t *kv, const char *key, char *out_names,
                          uint32_t out_capacity, uint32_t *out_length,
                          uint32_t *out_count, uint64_t *out_generation) {
    if (!kv || !key)
        return KVSPACE_INVALID;
    if (out_length)
        *out_length = 0;
    if (out_count)
        *out_count = 0;
    if (out_generation)
        *out_generation = 0;
    if (reserved_meta_path(key) || strlen(key) >= TRIE_KEY_LIMIT)
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    int status = storage_index_state(kv, key);
    if (status != KVSPACE_OK)
        return status;
    keyscan_t children;
    memset(&children, 0, sizeof children);
    status = storage_children(kv, key, &children);
    if (status != KVSPACE_OK) {
        keyscan_free(&children);
        return status;
    }
    uint64_t total = 0;
    for (int32_t index = 0; index < children.count; index++)
        total += (uint64_t)strlen(children.keys[index]) + 1;
    if (out_generation)
        *out_generation = __atomic_load_n(&kv->hdr->list_gen, __ATOMIC_ACQUIRE);
    if (out_count)
        *out_count = (uint32_t)children.count;
    if (out_length)
        *out_length = (uint32_t)total;
    if (out_names != NULL) {
        if (total > out_capacity) {
            keyscan_free(&children);
            return KVSPACE_CAPACITY;
        }
        char *cursor = out_names;
        for (int32_t index = 0; index < children.count; index++) {
            size_t name_length = strlen(children.keys[index]) + 1;
            memcpy(cursor, children.keys[index], name_length);
            cursor += name_length;
        }
    }
    keyscan_free(&children);
    return KVSPACE_OK;
}
#define KVSPACE_SHM_DEFAULT_SIZE (8ULL * 64 * 64 * 64 * 64)
#if defined(__GNUC__)
#define KVSPACE_PUBLIC __attribute__((visibility("default")))
#else
#define KVSPACE_PUBLIC 
#endif
static int kvspace_dsn_path(const char *dsn, char *out, size_t out_size) {
    if (!dsn)
        return -1;
    const char *separator = strstr(dsn, "://");
    if (!separator || separator - dsn != 3 || strncmp(dsn, "shm", 3) != 0)
        return -1;
    int written = snprintf(out, out_size, "%s", separator + 3);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}
static int kvspace_fail(int status, char *err, uint32_t err_cap,
                        const char *operation, const char *key) {
    if (err && err_cap)
        snprintf(err, err_cap, "kvspace: %s failed at %s", operation,
                 key ? key : "(null)");
    return status;
}
KVSPACE_PUBLIC void *kvspaceConnect(const char *dsn) {
    int entry_errno = errno;
    char path[1024];
    if (kvspace_dsn_path(dsn, path, sizeof path) != 0) {
        errno = EINVAL;
        return NULL;
    }
    void *handle = (void *)open_shm_store(path, kvspace_data_size);
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
        close_shm_store((kvspace_t *)handle);
}
KVSPACE_PUBLIC uint8_t *kvspaceXvalueInProcessAddress(void *handle) {
    if (!handle)
        return NULL;
    return storage_data_base((kvspace_t *)handle);
}
KVSPACE_PUBLIC int kvspaceResolveOffset(void *handle, const char *key,
                                 int64_t parent_key_anchor,
                                 uint64_t *out_value_offset, char *err,
                                 uint32_t err_cap) {
    if (!handle || !key || !out_value_offset)
        return KVSPACE_INVALID;
    *out_value_offset = 0;
    int internal;
    if (parent_key_anchor == (int64_t)KVSPACE_ANCHOR_ROOT) {
        internal = resolve_root((kvspace_t *)handle, key, out_value_offset);
    } else {
        internal = resolve_anchor((kvspace_t *)handle, key,
                                  (uint64_t)parent_key_anchor,
                                  out_value_offset);
    }
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "resolve", key);
}
KVSPACE_PUBLIC int kvspaceRealloc(void *handle, const char *key,
                                 uint64_t body_length, uint64_t attr_length,
                                 uint64_t *out_value_offset, char *err,
                                 uint32_t err_cap) {
    if (!handle || !key || !out_value_offset)
        return KVSPACE_INVALID;
    *out_value_offset = 0;
    int internal = realloc_key((kvspace_t *)handle, key,
                                            body_length, attr_length,
                                            out_value_offset);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "realloc",
                        key);
}
KVSPACE_PUBLIC int kvspaceFree(void *handle, const char *key, char *err,
                               uint32_t err_cap) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    int internal = free_key((kvspace_t *)handle, key);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "free", key);
}
KVSPACE_PUBLIC int kvspaceCp(void *handle, const char *old_key,
                             const char *new_key, char *err,
                             uint32_t err_cap) {
    if (!handle || !old_key || !new_key)
        return KVSPACE_INVALID;
    if (copy_key((kvspace_t *)handle, old_key, new_key) != 0)
        return kvspace_fail(KVSPACE_BACKEND, err, err_cap, "cp", old_key);
    return KVSPACE_OK;
}
KVSPACE_PUBLIC int kvspaceMkindex(void *handle, const char *key, char *err,
                                  uint32_t err_cap) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    int internal = makeindex_key((kvspace_t *)handle, key);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "mkindex",
                        key);
}
KVSPACE_PUBLIC int kvspaceDeltree(void *handle, const char *key, char *err,
                                  uint32_t err_cap) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    if (delete_tree((kvspace_t *)handle, key) != 0)
        return kvspace_fail(KVSPACE_BACKEND, err, err_cap, "deltree", key);
    return KVSPACE_OK;
}
KVSPACE_PUBLIC int kvspaceCpdir(void *handle, const char *source_key,
                                const char *destination_key, char *err,
                                uint32_t err_cap) {
    if (!handle || !source_key || !destination_key)
        return KVSPACE_INVALID;
    if (copy_tree((kvspace_t *)handle, source_key, destination_key) != 0)
        return kvspace_fail(KVSPACE_BACKEND, err, err_cap, "cpdir", source_key);
    return KVSPACE_OK;
}
KVSPACE_PUBLIC int kvspaceListlen(void *handle, const char *key,
                                  int64_t *out_count,
                                  uint32_t *out_max_name_length, char *err,
                                  uint32_t err_cap) {
    if (!handle || !key || !out_count || !out_max_name_length)
        return KVSPACE_INVALID;
    *out_count = 0;
    *out_max_name_length = 0;
    int internal = listlen_key((kvspace_t *)handle, key, out_count,
                                            out_max_name_length);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "listlen",
                        key);
}
KVSPACE_PUBLIC int kvspaceList(void *handle, const char *key, char *out_names,
                               uint32_t out_capacity, uint32_t *out_length,
                               uint32_t *out_count, uint64_t *out_generation,
                               char *err, uint32_t err_cap) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    int internal = list_names_raw((kvspace_t *)handle, key, out_names,
                                         out_capacity, out_length, out_count,
                                         out_generation);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "list", key);
}

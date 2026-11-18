#define _GNU_SOURCE 
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <slotsboxmalloc/slotsboxobj.h>
#ifndef BENCH_LINKED_ALLOCATOR
#define BENCH_LINKED_ALLOCATOR "glibc"
#endif
static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
typedef struct shim shim_t;
struct shim {
    const char *name;
    void *meta;
    uint8_t *base;
    void *(*alloc)(shim_t *s, size_t size);
    void (*free)(shim_t *s, void *p);
};
static void *slotsbox_alloc(shim_t *s, size_t size) {
    uint64_t off = sbo_alloc(s->meta, size);
    if (off == (uint64_t)-1)
        return NULL;
    return sbo_data_ptr(s->meta, s->base, off);
}
static void slotsbox_free(shim_t *s, void *p) {
    if (!p)
        return;
    sbo_free(s->meta, (uint64_t)((uint8_t *)p - s->base));
}
static void *glibc_alloc(shim_t *s, size_t size) {
    (void)s;
    return malloc(size);
}
static void glibc_free(shim_t *s, void *p) {
    (void)s;
    free(p);
}
static void *dl_alloc(shim_t *s, size_t size) {
    void *(*fn)(size_t) = (void *(*)(size_t))s->meta;
    return fn(size);
}
static void dl_free(shim_t *s, void *p) {
    void (*fn)(void *) = (void (*)(void *))(void *)s->base;
    fn(p);
}
#define LAT_CAP 2000000
typedef struct {
    double *v;
    size_t n;
} samples_t;
static int samples_init(samples_t *s, size_t cap) {
    s->v = calloc(cap, sizeof(double));
    s->n = 0;
    return s->v ? 0 : -1;
}
static void samples_reset(samples_t *s) { s->n = 0; }
static void samples_push(samples_t *s, double x) {
    if (s->n < LAT_CAP)
        s->v[s->n++] = x;
}
static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double percentile_ns(samples_t *s, double p) {
    if (s->n == 0)
        return 0.0;
    qsort(s->v, s->n, sizeof(double), cmp_double);
    size_t k = (size_t)(p * (double)(s->n - 1) + 0.5);
    if (k >= s->n)
        k = s->n - 1;
    return s->v[k] * 1e9;
}
typedef struct {
    void **slot;
    size_t cap;
    size_t count;
} live_t;
static int live_init(live_t *l, size_t cap) {
    l->slot = calloc(cap, sizeof(void *));
    l->cap = cap;
    l->count = 0;
    return l->slot ? 0 : -1;
}
static void live_reset(live_t *l) {
    for (size_t i = 0; i < l->count; i++)
        l->slot[i] = NULL;
    l->count = 0;
}
static uint64_t rng_state;
static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return rng_state = x;
}
static FILE *g_csv;
static void emit(const char *suite, const char *impl, const char *workload,
                 const char *metric, double value, const char *unit,
                 const char *version, const char *timestamp,
                 const char *notes, const char *allocator,
                 const char *size_class, const char *pattern,
                 const char *threads, const char *operation) {
    fprintf(g_csv,
            "%s,%s,%s,%s,%.6f,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
            suite, impl, workload, metric, value, unit, version, timestamp,
            notes && *notes ? notes : "-", allocator, size_class, pattern,
            threads, operation);
}
static void emit_str(const char *suite, const char *impl, const char *workload,
                     const char *metric, const char *value, const char *unit,
                     const char *version, const char *timestamp,
                     const char *notes, const char *allocator,
                     const char *size_class, const char *pattern,
                     const char *threads, const char *operation) {
    fprintf(g_csv, "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
            suite, impl, workload, metric, value, unit, version, timestamp,
            notes && *notes ? notes : "-", allocator, size_class, pattern,
            threads, operation);
}
typedef struct {
    const char *pattern;
    size_t size;
    size_t live_target;
    size_t ops;
} work_t;
static double run_workload(shim_t *sh, const work_t *w, live_t *live,
                           samples_t *lat, uint64_t *fails, uint64_t *alloced) {
    double t0 = now_seconds();
    *fails = 0;
    *alloced = 0;
    if (strcmp(w->pattern, "alloc-only") == 0) {
        for (size_t i = 0; i < w->ops && live->count < live->cap; i++) {
            double a = now_seconds();
            void *p = sh->alloc(sh, w->size);
            double b = now_seconds();
            samples_push(lat, b - a);
            (*alloced)++;
            if (!p) {
                (*fails)++;
                continue;
            }
            live->slot[live->count++] = p;
            *(volatile char *)p = (char)i;
        }
        return now_seconds() - t0;
    }
    if (strcmp(w->pattern, "lifo") == 0 || strcmp(w->pattern, "fifo") == 0) {
        bool lifo = (w->pattern[0] == 'l');
        size_t n = w->ops < live->cap ? w->ops : live->cap;
        for (size_t i = 0; i < n; i++) {
            double a = now_seconds();
            void *p = sh->alloc(sh, w->size);
            double b = now_seconds();
            samples_push(lat, b - a);
            (*alloced)++;
            if (!p) {
                (*fails)++;
                break;
            }
            live->slot[live->count++] = p;
            *(volatile char *)p = 1;
        }
        while (live->count > 0) {
            size_t k = lifo ? live->count - 1 : 0;
            void *p = live->slot[k];
            double a = now_seconds();
            sh->free(sh, p);
            double c = now_seconds();
            samples_push(lat, c - a);
            live->slot[k] = live->slot[live->count - 1];
            live->count--;
        }
        return now_seconds() - t0;
    }
    size_t target = w->live_target < live->cap ? w->live_target : live->cap;
    while (live->count < target) {
        void *p = sh->alloc(sh, w->size);
        (*alloced)++;
        if (!p) {
            (*fails)++;
            break;
        }
        live->slot[live->count++] = p;
        *(volatile char *)p = 1;
    }
    for (size_t i = 0; i < w->ops; i++) {
        size_t k = (size_t)(rng_next() % (live->count ? live->count : 1));
        if (live->count == 0)
            break;
        double a = now_seconds();
        sh->free(sh, live->slot[k]);
        void *p = sh->alloc(sh, w->size);
        double b = now_seconds();
        samples_push(lat, b - a);
        (*alloced)++;
        if (!p) {
            (*fails)++;
            live->slot[k] = live->slot[live->count - 1];
            live->count--;
            continue;
        }
        live->slot[k] = p;
        *(volatile char *)p = 1;
    }
    return now_seconds() - t0;
}
static long proc_status_kb(const char *field) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long val = -1;
    size_t flen = strlen(field);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, field, flen) == 0) {
            sscanf(line + flen, "%ld", &val);
            break;
        }
    }
    fclose(f);
    return val;
}
static size_t g_pool_header_bytes;
static int slotsbox_fragmentation(shim_t *sh, double *out_ratio,
                                  uint64_t *out_free, uint64_t *out_cont) {
    sbo_meta_t *m = (sbo_meta_t *)sh->meta;
    if (!m || m->root_slots != 1)
        return -1;
    uint64_t free_total = 0, longest = 0;
    uint32_t max_blocks = m->max_blocks;
    for (uint32_t id = 0; id < max_blocks; id++) {
        size_t pool_hdr = g_pool_header_bytes;
        uint8_t *pool = (uint8_t *)(m + 1) + (size_t)m->root_slots * pool_hdr;
        sbo_box_t *b = (sbo_box_t *)(pool + (size_t)id * m->block_stride +
                                     m->box_off);
        uint64_t fm = b->free_mask;
        if (!fm)
            continue;
        for (int i = 0; i < 64; i++)
            if (fm & (1ULL << i))
                free_total++;
        int run = 0;
        for (int i = 0; i < 64; i++) {
            if (fm & (1ULL << i)) {
                run++;
                if ((uint64_t)run > longest)
                    longest = (uint64_t)run;
            } else {
                run = 0;
            }
        }
    }
    (void)sh;
    *out_free = free_total;
    *out_cont = longest;
    *out_ratio = longest ? (double)free_total / (double)longest : 0.0;
    return 0;
}
static void print_header(FILE *f) {
    fprintf(f, "suite,implementation,workload,metric,value,unit,version,"
               "timestamp,notes,allocator,size_class,pattern,threads,"
               "operation\n");
}
#define ROUNDS 5
static void measure(shim_t *sh, const char *impl, const char *allocator,
                    const work_t *w, size_t threads, const char *version,
                    const char *ts, live_t *live, samples_t *lat) {
    double best_sec = 1e30;
    double p50_best = 0, p99_best = 0;
    uint64_t fails_best = 0, alloced_best = 0;
    for (int r = 0; r < ROUNDS; r++) {
        live_reset(live);
        samples_reset(lat);
        rng_state = 0x9E3779B97F4A7C15ULL ^ (uint64_t)r;
        uint64_t fails = 0, alloced = 0;
        double sec = run_workload(sh, w, live, lat, &fails, &alloced);
        if (sec < best_sec) {
            best_sec = sec;
            p50_best = percentile_ns(lat, 0.50);
            p99_best = percentile_ns(lat, 0.99);
            fails_best = fails;
            alloced_best = alloced;
        }
    }
    live_reset(live);
    char wl[128], sc[32], thr[16];
    snprintf(wl, sizeof wl, "%s/size=%zu", w->pattern, w->size);
    snprintf(sc, sizeof sc, "%zu", w->size);
    snprintf(thr, sizeof thr, "%zu", threads);
    char notes[128] = "";
    if (fails_best > 0)
        snprintf(notes, sizeof notes, "failures=%" PRIu64 "/%" PRIu64,
                 fails_best, alloced_best);
    double ops = alloced_best ? (double)alloced_best / best_sec : 0.0;
    emit("malloc", impl, wl, "ops_per_second", ops, "ops/s", version, ts,
         notes, allocator, sc, w->pattern, thr, "alloc");
    emit("malloc", impl, wl, "p50", p50_best, "ns", version, ts, notes,
         allocator, sc, w->pattern, thr, "alloc");
    emit("malloc", impl, wl, "p99", p99_best, "ns", version, ts, notes,
         allocator, sc, w->pattern, thr, "alloc");
    long rss = proc_status_kb("VmRSS:");
    long vm = proc_status_kb("VmSize:");
    if (rss >= 0)
        emit("malloc", impl, wl, "rss_bytes", (double)rss * 1024.0, "bytes",
             version, ts, notes, allocator, sc, w->pattern, thr, "alloc");
    if (vm >= 0)
        emit("malloc", impl, wl, "reserved_bytes", (double)vm * 1024.0, "bytes",
             version, ts, notes, allocator, sc, w->pattern, thr, "alloc");
    if (sh->meta) {
        double ratio = 0;
        uint64_t fr = 0, cont = 0;
        if (slotsbox_fragmentation(sh, &ratio, &fr, &cont) == 0) {
            char fn[128];
            snprintf(fn, sizeof fn, "free_slots=%" PRIu64 "; longest_run=%" PRIu64,
                     fr, cont);
            emit("malloc", impl, wl, "fragmentation_ratio", ratio, "ratio",
                 version, ts, fn, allocator, sc, w->pattern, thr, "alloc");
        }
    }
}
#define DATA_SIZE (8ULL * 64 * 64 * 64 * 64)
#define HEAD_POOL (256 * 1024)
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <out.csv> <meta.json> [version] [timestamp]\n",
                argv[0]);
        return 2;
    }
    const char *version = argc > 3 ? argv[3] : "unknown";
    const char *ts = argc > 4 ? argv[4] : "unknown";
    g_csv = fopen(argv[1], "w");
    if (!g_csv) {
        fprintf(stderr, "cannot open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    print_header(g_csv);
    size_t head_size = sbo_meta_size(DATA_SIZE, HEAD_POOL);
    {
        uint8_t shards = (DATA_SIZE / 64 >= (8ULL * 64 * 64 * 64)) ? 64 : 1;
        g_pool_header_bytes =
            (head_size - sizeof(sbo_meta_t)) / shards - HEAD_POOL;
    }
    void *meta = malloc(head_size);
    uint8_t *data = malloc(DATA_SIZE);
    if (!meta || !data) {
        fprintf(stderr, "cannot allocate slotsbox regions\n");
        return 1;
    }
    shim_t sb = {"slotsbox", meta, data, slotsbox_alloc, slotsbox_free};
    shim_t gc = {"glibc", NULL, NULL, glibc_alloc, glibc_free};
    {
        double a = now_seconds();
        int rc = sbo_init(meta, head_size, DATA_SIZE);
        double b = now_seconds();
        if (rc != 0) {
            fprintf(stderr, "sbo_init failed\n");
            return 1;
        }
        char wl[96], note[160];
        sbo_meta_t *mm = (sbo_meta_t *)meta;
        snprintf(wl, sizeof wl, "init/size=%llu", (unsigned long long)DATA_SIZE);
        snprintf(note, sizeof note,
                 "one-shot; excluded from per-op latency; shards=%u "
                 "SBO_MIN_SHARD_BYTES=%llu",
                 (unsigned)mm->root_slots,
                 (unsigned long long)SBO_MIN_SHARD_BYTES);
        emit("malloc", "slotsbox", wl, "init_seconds", b - a, "s", version, ts,
             note, "slotsbox", "-", "init", "1", "alloc");
    }
    const size_t sizes[] = {8, 64, 512, 4096, 32768, 2 * 1024 * 1024};
    const char *patterns[] = {"alloc-only", "lifo", "fifo", "random", "churn"};
    double scale = 1.0;
    {
        const char *e = getenv("BENCH_SCALE");
        if (e && atof(e) > 0)
            scale = atof(e);
    }
    const size_t ops_base[] = {2000000, 2000000, 1000000, 500000, 200000, 20000};
    const size_t live_base[] = {500000, 500000, 200000, 100000, 50000, 2000};
    live_t live;
    samples_t lat;
    if (live_init(&live, 600000) != 0 || samples_init(&lat, LAT_CAP) != 0) {
        fprintf(stderr, "cannot allocate tracking structures\n");
        return 1;
    }
    shim_t *impls[] = {&sb, &gc};
    for (size_t ii = 0; ii < 2; ii++) {
        shim_t *sh = impls[ii];
        if (sh == &sb) {
            for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
                memset(meta, 0, head_size);
                if (sbo_init(meta, head_size, DATA_SIZE) != 0) {
                    fprintf(stderr, "sbo_init failed for size %zu\n", sizes[si]);
                    return 1;
                }
                for (size_t pi = 0; pi < 5; pi++) {
                    work_t w = {patterns[pi], sizes[si],
                                (size_t)((double)live_base[si] * scale),
                                (size_t)((double)ops_base[si] * scale)};
                    measure(sh, "slotsbox", "slotsbox", &w, 1, version, ts,
                            &live, &lat);
                }
            }
        } else {
            for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
                for (size_t pi = 0; pi < 5; pi++) {
                    work_t w = {patterns[pi], sizes[si],
                                (size_t)((double)live_base[si] * scale),
                                (size_t)((double)ops_base[si] * scale)};
                    measure(sh, BENCH_LINKED_ALLOCATOR, BENCH_LINKED_ALLOCATOR, &w, 1,
                            version, ts, &live, &lat);
                }
            }
        }
    }
    const char *sonames[] = {"libjemalloc.so.2", "libmimalloc.so.2",
                             "libtcmalloc.so.4"};
    const char *names[] = {"jemalloc", "mimalloc", "tcmalloc"};
    for (size_t i = 0; i < 3; i++) {
        if (strcmp(names[i], BENCH_LINKED_ALLOCATOR) == 0) {
            continue;
        }
        void *library = dlopen(sonames[i], RTLD_NOW | RTLD_LOCAL);
        if (library == NULL) {
            char note[160];
            snprintf(note, sizeof note, "skipped: %s not installed (%s)",
                     names[i], dlerror());
            emit_str("malloc", names[i], "-", "ops_per_second", "-", "ops/s",
                     "n/a", ts, note, names[i], "-", "-", "-", "-");
            continue;
        }
        void *(*alloc_fn)(size_t) =
            (void *(*)(size_t))(void *)dlsym(library, "malloc");
        void (*free_fn)(void *) = (void (*)(void *))(void *)dlsym(library, "free");
        if (alloc_fn == NULL || free_fn == NULL) {
            char note[160];
            snprintf(note, sizeof note,
                     "skipped: %s lacks malloc/free exports", names[i]);
            emit_str("malloc", names[i], "-", "ops_per_second", "-", "ops/s",
                     "n/a", ts, note, names[i], "-", "-", "-", "-");
            dlclose(library);
            continue;
        }
        shim_t dl = {names[i], (void *)alloc_fn, (uint8_t *)(void *)free_fn,
                     dl_alloc, dl_free};
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            for (size_t pi = 0; pi < 5; pi++) {
                work_t w = {patterns[pi], sizes[si],
                            (size_t)((double)live_base[si] * scale),
                            (size_t)((double)ops_base[si] * scale)};
                measure(&dl, names[i], names[i], &w, 1, version, ts, &live, &lat);
            }
        }
        dlclose(library);
    }
    fclose(g_csv);
    free(live.slot);
    free(lat.v);
    free(meta);
    free(data);
    return 0;
}

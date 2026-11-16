#define _POSIX_C_SOURCE 200809L
#include <kvspace/kvspace.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static FILE *g_out;
static const char *g_version = "unknown";
static const char *g_timestamp = "";
static const char *g_dsn = "shm:///tmp/kvspace-bench.shms";
static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
static void emit(const char *impl, const char *workload, const char *metric,
                 const char *value, const char *unit, const char *notes,
                 const char *value_size, const char *key_depth,
                 const char *threads, const char *percentile) {
    fprintf(g_out, "kvspace,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
            impl, workload, metric, value, unit, g_version, g_timestamp,
            notes, value_size, key_depth, threads, percentile);
}
static void emit_u64(const char *impl, const char *workload, const char *metric,
                     uint64_t v, const char *unit, const char *value_size,
                     const char *key_depth, const char *threads,
                     const char *percentile) {
    char buf[32];
    snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
    emit(impl, workload, metric, buf, unit, "", value_size, key_depth,
         threads, percentile);
}
static void emit_skipped(const char *impl, const char *workload,
                         const char *metric, const char *reason) {
    char notes[160];
    snprintf(notes, sizeof notes, "skipped: %s", reason);
    emit(impl, workload, metric, "", "", notes, "", "", "", "-");
}
static uint64_t pct(const double *sorted, size_t n, double q) {
    if (n == 0)
        return 0;
    size_t at = (size_t)(q * (double)(n - 1) + 0.5);
    if (at >= n)
        at = n - 1;
    return (uint64_t)sorted[at];
}
static void store_clear(const char *dsn) {
    static const char *const suffixes[] = {
        "", ".sbo.data", ".sbo.head", ".garb", ".gen",
    };
    if (strncmp(dsn, "shm://", 6) != 0)
        return;
    const char *path = dsn + 6;
    char buf[512];
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        snprintf(buf, sizeof buf, "%s%s", path, suffixes[i]);
        unlink(buf);
    }
}
static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
#define Q1_ITERS 200000
#define Q1_WARMUP 2000
static void suite_q1(void) {
    void *h = kvspaceConnect(g_dsn);
    if (!h) {
        emit_skipped("kvspace", "write-8B", "p50", "connect failed");
        return;
    }
    char err[256];
    uint64_t off = 0;
    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    err[0] = 0;
    if (kvspaceRealloc(h, "/bench/k", 8, 0, &off, err, sizeof err) != 0) {
        emit_skipped("kvspace", "write-8B", "p50", "realloc failed");
        kvspaceClose(h);
        return;
    }
    double *samples = malloc(sizeof(double) * Q1_ITERS);
    if (!samples) {
        emit_skipped("kvspace", "write-8B", "p50", "oom");
        kvspaceClose(h);
        return;
    }
    uint8_t *base = kvspaceXvalueInProcessAddress(h);
    for (int i = 0; i < Q1_WARMUP; i++) {
        uint64_t o = 0;
        kvspaceResolveOffset(h, "/bench/k", 0, &o, err, sizeof err);
    }
    for (int i = 0; i < Q1_ITERS; i++) {
        double t0 = now_ns();
        uint64_t o = 0;
        if (kvspaceResolveOffset(h, "/bench/k", 0, &o, err, sizeof err) == 0 &&
            base) {
            memcpy(base + o, payload, 8);
        }
        samples[i] = now_ns() - t0;
    }
    qsort(samples, Q1_ITERS, sizeof(double), cmp_double);
    emit_u64("kvspace", "write-8B", "p50", pct(samples, Q1_ITERS, 0.50),
             "ns", "8", "1", "1", "50");
    emit_u64("kvspace", "write-8B", "p99", pct(samples, Q1_ITERS, 0.99),
             "ns", "8", "1", "1", "99");
    free(samples);
    kvspaceClose(h);
    store_clear(g_dsn);
}
#define Q2_FILL_KEYS 4096
#define Q2_FILL_BYTES 256
static void suite_q2(void) {
    void *h = kvspaceConnect(g_dsn);
    if (!h) {
        emit_skipped("kvspace", "address-stable-after-grow", "correct",
                     "connect failed");
        return;
    }
    char err[256], key[64];
    uint64_t off_before = 0;
    err[0] = 0;
    if (kvspaceRealloc(h, "/bench/probe", 8, 0, &off_before, err,
                       sizeof err) != 0) {
        emit_skipped("kvspace", "address-stable-after-grow", "correct",
                     "probe realloc failed");
        kvspaceClose(h);
        return;
    }
    uint8_t *base0 = kvspaceXvalueInProcessAddress(h);
    if (base0)
        memcpy(base0 + off_before, "PROBE!!", 8);
    for (int i = 0; i < Q2_FILL_KEYS; i++) {
        uint64_t o = 0;
        snprintf(key, sizeof key, "/bench/fill%06d", i);
        err[0] = 0;
        if (kvspaceRealloc(h, key, Q2_FILL_BYTES, 0, &o, err, sizeof err) != 0)
            break;
        uint8_t *b = kvspaceXvalueInProcessAddress(h);
        if (b)
            memset(b + o, (uint8_t)(i & 0xFF), Q2_FILL_BYTES);
    }
    uint64_t off_after = 0;
    err[0] = 0;
    int resolved = kvspaceResolveOffset(h, "/bench/probe", 0, &off_after, err,
                                        sizeof err);
    uint8_t *base1 = kvspaceXvalueInProcessAddress(h);
    int same = 0;
    if (resolved == 0 && base1) {
        same = memcmp(base1 + off_after, "PROBE!!", 8) == 0;
    }
    emit_u64("kvspace", "address-stable-after-grow", "correct",
             (uint64_t)same, "bool", "", "", "", "-");
    kvspaceClose(h);
    store_clear(g_dsn);
}
typedef struct {
    int thread_id;
    int dirs;
    int iters;
    const char *dsn;
    uint64_t elapsed_ns;
} q3_worker_t;
static void *q3_thread(void *arg) {
    q3_worker_t *w = arg;
    void *h = kvspaceConnect(w->dsn);
    if (!h)
        return NULL;
    char err[256], key[96], dir[64];
    snprintf(dir, sizeof dir, "/bench/dir%d/", w->thread_id % w->dirs);
    double t0 = now_ns();
    for (int i = 0; i < w->iters; i++) {
        uint64_t off = 0;
        snprintf(key, sizeof key, "%se%06d", dir, i);
        err[0] = 0;
        if (kvspaceRealloc(h, key, 64, 0, &off, err, sizeof err) == 0)
            kvspaceFree(h, key, err, sizeof err);
    }
    w->elapsed_ns = (uint64_t)(now_ns() - t0);
    kvspaceClose(h);
    return NULL;
}
#define Q3_ITERS 20000
static const int Q3_THREADS[] = {1, 4, 16};
static const int Q3_DIRS[] = {1, 8};
static void suite_q3(void) {
    for (size_t ti = 0; ti < sizeof Q3_THREADS / sizeof Q3_THREADS[0]; ti++) {
        for (size_t di = 0; di < sizeof Q3_DIRS / sizeof Q3_DIRS[0]; di++) {
            int nt = Q3_THREADS[ti], nd = Q3_DIRS[di];
            pthread_t th[16];
            q3_worker_t work[16];
            char workload[96];
            store_clear(g_dsn);
            for (int i = 0; i < nt; i++) {
                work[i] = (q3_worker_t){.thread_id = i, .dirs = nd,
                                        .iters = Q3_ITERS, .dsn = g_dsn,
                                        .elapsed_ns = 0};
                pthread_create(&th[i], NULL, q3_thread, &work[i]);
            }
            uint64_t worst = 0, total_ops = 0;
            for (int i = 0; i < nt; i++) {
                pthread_join(th[i], NULL);
                if (work[i].elapsed_ns > worst)
                    worst = work[i].elapsed_ns;
                total_ops += (uint64_t)Q3_ITERS * 2;
            }
            snprintf(workload, sizeof workload, "create-delete/t=%d/d=%d", nt, nd);
            uint64_t ops = worst ? (uint64_t)((double)total_ops * 1e9 /
                                              (double)worst) : 0;
            char ts[16], ds[16];
            snprintf(ts, sizeof ts, "%d", nt);
            snprintf(ds, sizeof ds, "%d", nd);
            emit_u64("kvspace", workload, "ops_per_second", ops, "ops/s",
                     "64", ds, ts, "-");
        }
    }
    store_clear(g_dsn);
}
#define Q4_KEYS 512
static void suite_q4(void) {
    pid_t pid = fork();
    if (pid == 0) {
        void *h = kvspaceConnect(g_dsn);
        if (!h)
            _exit(2);
        char err[256], key[64];
        for (int i = 0; i < Q4_KEYS; i++) {
            uint64_t off = 0;
            snprintf(key, sizeof key, "/bench/recover%04d", i);
            err[0] = 0;
            if (kvspaceRealloc(h, key, 8, 0, &off, err, sizeof err) != 0)
                _exit(3);
            uint8_t *b = kvspaceXvalueInProcessAddress(h);
            int64_t marker = i;
            if (b)
                memcpy(b + off, &marker, sizeof marker);
        }
        raise(SIGKILL);
        _exit(0);
    }
    if (pid < 0) {
        emit_skipped("kvspace", "kill-9-then-reopen", "correct", "fork failed");
        return;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    void *h = kvspaceConnect(g_dsn);
    if (!h) {
        emit_skipped("kvspace", "kill-9-then-reopen", "correct",
                     "reopen failed");
        return;
    }
    char err[256], key[64];
    uint64_t correct = 0, missing = 0, unreadable = 0;
    for (int i = 0; i < Q4_KEYS; i++) {
        uint64_t off = 0;
        snprintf(key, sizeof key, "/bench/recover%04d", i);
        err[0] = 0;
        if (kvspaceResolveOffset(h, key, 0, &off, err, sizeof err) != 0) {
            missing++;
            continue;
        }
        uint8_t *b = kvspaceXvalueInProcessAddress(h);
        int64_t v = -1;
        if (!b) {
            unreadable++;
            continue;
        }
        memcpy(&v, b + off, sizeof v);
        if (v == (int64_t)i)
            correct++;
        else
            unreadable++;
    }
    emit_u64("kvspace", "kill-9-then-reopen", "correct", correct, "count",
             "", "", "", "-");
    emit_u64("kvspace", "kill-9-then-reopen", "missing", missing, "count",
             "", "", "", "-");
    emit_u64("kvspace", "kill-9-then-reopen", "unreadable", unreadable, "count",
             "", "", "", "-");
    kvspaceClose(h);
    store_clear(g_dsn);
}
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <out.csv> <q1|q2|q3|q4|all> [dsn]\n", argv[0]);
        return 2;
    }
    if (argc > 3)
        g_dsn = argv[3];
    g_out = fopen(argv[1], "w");
    if (!g_out) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    const char *suite = argv[2];
    int all = strcmp(suite, "all") == 0;
    if (all || strcmp(suite, "q1") == 0) suite_q1();
    if (all || strcmp(suite, "q2") == 0) suite_q2();
    if (all || strcmp(suite, "q3") == 0) suite_q3();
    if (all || strcmp(suite, "q4") == 0) suite_q4();
    fclose(g_out);
    return 0;
}

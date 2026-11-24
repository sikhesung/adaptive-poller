#define _POSIX_C_SOURCE 200809L

#include "poller.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void print_row(const char *mode, const poller_stats_t *st,
                      double total_ms, uint64_t *lats_ns, int iters) {
    qsort(lats_ns, (size_t)iters, sizeof(uint64_t), cmp_u64);
    double p50 = (double)lats_ns[(size_t)(iters * 0.50)] / 1000.0;
    double p99 = (double)lats_ns[(size_t)(iters * 0.99)] / 1000.0;
    double ops = (double)iters / (total_ms / 1000.0);
    printf("  %-8s | %-6s | %7.2f ms | %9.0f op/s | p50=%5.1fus | p99=%6.1fus | ctl=%5llu (elided=%4llu) | sw=%llu\n",
           mode, st->active_backend, total_ms, ops, p50, p99,
           (unsigned long long)st->ctl_calls,
           (unsigned long long)st->ctl_elided,
           (unsigned long long)st->switches);
}

/* Workload A: Small N=32 + Frequent interest toggling & redundant mod calls */
static void bench_churn(poller_mode_t mode) {
    const int N = 32;
    const int ITERS = 10000;
    poller_t *p = poller_create(mode, N);
    int sp[32][2];
    for (int i = 0; i < N; i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp[i]) == 0);
        set_nonblock(sp[i][0]);
        set_nonblock(sp[i][1]);
        assert(poller_add(p, sp[i][0], POLLER_IN, NULL) == 0);
    }

    uint64_t *lats = malloc((size_t)ITERS * sizeof(uint64_t));
    poller_event_t ev[32];
    uint64_t t_all = now_ns();
    for (int it = 0; it < ITERS; it++) {
        uint64_t t0 = now_ns();
        int idx = it % N;
        /* Alternate genuine toggle & redundant re-arm to exercise elision */
        uint32_t mask = ((it / 2) & 1) ? (POLLER_IN | POLLER_OUT) : POLLER_IN;
        poller_mod(p, sp[idx][0], mask, NULL);
        char b = 'a';
        ssize_t w = write(sp[idx][1], &b, 1);
        (void)w;
        int n = poller_wait(p, ev, 32, 10);
        for (int j = 0; j < n; j++) {
            char tmp[16];
            ssize_t r = read(ev[j].fd, tmp, sizeof(tmp));
            (void)r;
        }
        lats[it] = now_ns() - t0;
    }
    double total_ms = (double)(now_ns() - t_all) / 1000000.0;

    poller_stats_t st;
    poller_get_stats(p, &st);
    print_row(poller_mode_name(mode), &st, total_ms, lats, ITERS);

    free(lats);
    for (int i = 0; i < N; i++) {
        poller_del(p, sp[i][0]);
        close(sp[i][0]);
        close(sp[i][1]);
    }
    poller_destroy(p);
}

/* Workload B: Large N=2,000 Idle Keep-Alive + 8 Active */
static void bench_idle_scale(poller_mode_t mode) {
    const int N = 2000;
    const int ACTIVE = 8;
    const int ITERS = 4000;
    poller_t *p = poller_create(mode, N);
    int (*sp)[2] = malloc((size_t)N * sizeof(int[2]));
    uint64_t *lats = malloc((size_t)ITERS * sizeof(uint64_t));
    assert(sp && lats);

    for (int i = 0; i < N; i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp[i]) == 0);
        set_nonblock(sp[i][0]);
        set_nonblock(sp[i][1]);
        assert(poller_add(p, sp[i][0], POLLER_IN, NULL) == 0);
    }

    poller_event_t ev[32];
    uint64_t t_all = now_ns();
    for (int it = 0; it < ITERS; it++) {
        uint64_t t0 = now_ns();
        int idx = it % ACTIVE;
        char b = 'z';
        ssize_t w = write(sp[idx][1], &b, 1);
        (void)w;
        int n = poller_wait(p, ev, 32, 10);
        for (int j = 0; j < n; j++) {
            char tmp[16];
            ssize_t r = read(ev[j].fd, tmp, sizeof(tmp));
            (void)r;
        }
        lats[it] = now_ns() - t0;
    }
    double total_ms = (double)(now_ns() - t_all) / 1000000.0;

    poller_stats_t st;
    poller_get_stats(p, &st);
    print_row(poller_mode_name(mode), &st, total_ms, lats, ITERS);

    free(lats);
    for (int i = 0; i < N; i++) {
        poller_del(p, sp[i][0]);
        close(sp[i][0]);
        close(sp[i][1]);
    }
    free(sp);
    poller_destroy(p);
}

/* Workload C: Traffic Spike (24 -> 600 -> 24 fds) measuring adaptive stability */
static void bench_spike(poller_mode_t mode) {
    const int BASE = 24;
    const int PEAK = 600;
    const int PHASE_ITERS = 1500;
    const int TOTAL = PHASE_ITERS * 3;
    poller_t *p = poller_create(mode, BASE);
    int (*sp)[2] = malloc((size_t)PEAK * sizeof(int[2]));
    uint64_t *lats = malloc((size_t)TOTAL * sizeof(uint64_t));
    assert(sp && lats);

    for (int i = 0; i < BASE; i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp[i]) == 0);
        set_nonblock(sp[i][0]);
        set_nonblock(sp[i][1]);
        assert(poller_add(p, sp[i][0], POLLER_IN, NULL) == 0);
    }

    poller_event_t ev[32];
    int k = 0;
    uint64_t t_all = now_ns();

    /* Phase 1: Small traffic (24 fds) */
    for (int it = 0; it < PHASE_ITERS; it++, k++) {
        uint64_t t0 = now_ns();
        int idx = it % 4;
        char b = '1';
        ssize_t w = write(sp[idx][1], &b, 1); (void)w;
        int n = poller_wait(p, ev, 32, 10);
        for (int j = 0; j < n; j++) { char t[8]; ssize_t r = read(ev[j].fd, t, sizeof(t)); (void)r; }
        lats[k] = now_ns() - t0;
    }

    /* Spike up to 600 fds */
    for (int i = BASE; i < PEAK; i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp[i]) == 0);
        set_nonblock(sp[i][0]);
        set_nonblock(sp[i][1]);
        assert(poller_add(p, sp[i][0], POLLER_IN, NULL) == 0);
    }

    /* Phase 2: Peak traffic (600 fds) */
    for (int it = 0; it < PHASE_ITERS; it++, k++) {
        uint64_t t0 = now_ns();
        int idx = it % 4;
        char b = '2';
        ssize_t w = write(sp[idx][1], &b, 1); (void)w;
        int n = poller_wait(p, ev, 32, 10);
        for (int j = 0; j < n; j++) { char t[8]; ssize_t r = read(ev[j].fd, t, sizeof(t)); (void)r; }
        lats[k] = now_ns() - t0;
    }

    /* Drop back to 24 fds */
    for (int i = BASE; i < PEAK; i++) {
        poller_del(p, sp[i][0]);
        close(sp[i][0]);
        close(sp[i][1]);
    }

    /* Phase 3: Post-spike calm (24 fds) */
    for (int it = 0; it < PHASE_ITERS; it++, k++) {
        uint64_t t0 = now_ns();
        int idx = it % 4;
        char b = '3';
        ssize_t w = write(sp[idx][1], &b, 1); (void)w;
        int n = poller_wait(p, ev, 32, 10);
        for (int j = 0; j < n; j++) { char t[8]; ssize_t r = read(ev[j].fd, t, sizeof(t)); (void)r; }
        lats[k] = now_ns() - t0;
    }

    double total_ms = (double)(now_ns() - t_all) / 1000000.0;
    poller_stats_t st;
    poller_get_stats(p, &st);
    print_row(poller_mode_name(mode), &st, total_ms, lats, TOTAL);

    free(lats);
    for (int i = 0; i < BASE; i++) {
        poller_del(p, sp[i][0]);
        close(sp[i][0]);
        close(sp[i][1]);
    }
    free(sp);
    poller_destroy(p);
}

int main(void) {
    printf("=== Workload A: Small N=32 + Frequent Churn & Redundant Mod (10,000 iters) ===\n");
    bench_churn(POLLER_POLL);
    bench_churn(POLLER_FAST);
    bench_churn(POLLER_AUTO);
    bench_churn(POLLER_ADAPTIVE);

    printf("\n=== Workload B: Large N=2,000 Idle Keep-Alive + 8 Active (4,000 iters) ===\n");
    bench_idle_scale(POLLER_POLL);
    bench_idle_scale(POLLER_FAST);
    bench_idle_scale(POLLER_AUTO);
    bench_idle_scale(POLLER_ADAPTIVE);

    printf("\n=== Workload C: Dynamic Traffic Spike 24 -> 600 -> 24 fds (4,500 iters) ===\n");
    bench_spike(POLLER_POLL);
    bench_spike(POLLER_FAST);
    bench_spike(POLLER_AUTO);
    bench_spike(POLLER_ADAPTIVE);
    return 0;
}

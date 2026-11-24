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

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Unit tests: verify poll, epoll, auto, and adaptive hysteresis */
static void run_unit_tests(void) {
    poller_mode_t modes[] = { POLLER_POLL, POLLER_EPOLL, POLLER_AUTO, POLLER_ADAPTIVE };
    for (size_t m = 0; m < 4; m++) {
        poller_t *p = poller_create(modes[m], 128);
        assert(p != NULL);
        int sp[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
        set_nonblock(sp[0]);
        set_nonblock(sp[1]);

        assert(poller_add(p, sp[0], POLLER_IN, (void *)(uintptr_t)42) == 0);
        char msg = 'x';
        assert(write(sp[1], &msg, 1) == 1);

        poller_event_t ev[4];
        int n = poller_wait(p, ev, 4, 100);
        assert(n == 1);
        assert(ev[0].fd == sp[0]);
        assert((ev[0].events & POLLER_IN) != 0);
        assert((uintptr_t)ev[0].udata == 42);

        char got = 0;
        assert(read(sp[0], &got, 1) == 1 && got == 'x');
        assert(poller_mod(p, sp[0], POLLER_IN | POLLER_OUT, (void *)(uintptr_t)99) == 0);
        assert(poller_del(p, sp[0]) == 0);

        close(sp[0]);
        close(sp[1]);
        poller_destroy(p);
    }

    /* Verify POLLER_AUTO zero-cost selection */
    poller_t *p_small = poller_create(POLLER_AUTO, 64);
    poller_t *p_large = poller_create(POLLER_AUTO, 2048);
    poller_stats_t s1, s2;
    poller_get_stats(p_small, &s1);
    poller_get_stats(p_large, &s2);
    assert(s1.using_epoll == 0 && s1.switches == 0);
#ifdef __linux__
    assert(s2.using_epoll == 1 && s2.switches == 0);
#endif
    poller_destroy(p_small);
    poller_destroy(p_large);

    /* Verify POLLER_ADAPTIVE hysteresis promotion (>= 256) without thrashing */
    poller_t *pa = poller_create(POLLER_ADAPTIVE, 64);
    int fds[300][2];
    for (int i = 0; i < 300; i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]) == 0);
        assert(poller_add(pa, fds[i][0], POLLER_IN, NULL) == 0);
    }
    poller_stats_t sa;
    poller_get_stats(pa, &sa);
#ifdef __linux__
    assert(sa.using_epoll == 1);
    assert(sa.switches == 1);
#endif
    /* Drop to 200 fds (still above LOW_WATER=64) -> must NOT thrash back to poll */
    for (int i = 200; i < 300; i++) {
        assert(poller_del(pa, fds[i][0]) == 0);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    poller_get_stats(pa, &sa);
#ifdef __linux__
    assert(sa.using_epoll == 1);
    assert(sa.switches == 1);
#endif
    for (int i = 0; i < 200; i++) {
        assert(poller_del(pa, fds[i][0]) == 0);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    poller_destroy(pa);
    printf("[OK] All unit tests passed (poll / epoll / auto / adaptive hysteresis).\n");
}

/* Workload A: Small N=32 with heavy add/mod/del churn */
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

    poller_event_t ev[32];
    uint64_t t0 = now_us();
    for (int it = 0; it < ITERS; it++) {
        int idx = it % N;
        /* Simulate frequent interest toggling (IN <-> IN|OUT) */
        poller_mod(p, sp[idx][0], (it & 1) ? (POLLER_IN | POLLER_OUT) : POLLER_IN, NULL);
        char b = 'a';
        ssize_t w = write(sp[idx][1], &b, 1);
        (void)w;
        int n = poller_wait(p, ev, 32, 10);
        for (int j = 0; j < n; j++) {
            char tmp[16];
            ssize_t r = read(ev[j].fd, tmp, sizeof(tmp));
            (void)r;
        }
    }
    uint64_t dt = now_us() - t0;

    poller_stats_t st;
    poller_get_stats(p, &st);
    printf("  %-9s | backend=%-5s | %6.2f ms | epoll_ctl=%6llu | switches=%llu\n",
           poller_mode_name(mode), st.using_epoll ? "epoll" : "poll",
           (double)dt / 1000.0,
           (unsigned long long)st.epoll_ctl_calls,
           (unsigned long long)st.switches);

    for (int i = 0; i < N; i++) {
        poller_del(p, sp[i][0]);
        close(sp[i][0]);
        close(sp[i][1]);
    }
    poller_destroy(p);
}

/* Workload B: C10K-style 2,000 mostly-idle sockets + 8 active sockets */
static void bench_idle_scale(poller_mode_t mode) {
    const int N = 2000;
    const int ACTIVE = 8;
    const int ITERS = 4000;
    poller_t *p = poller_create(mode, N);
    int (*sp)[2] = malloc((size_t)N * sizeof(int[2]));
    assert(sp != NULL);

    for (int i = 0; i < N; i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp[i]) == 0);
        set_nonblock(sp[i][0]);
        set_nonblock(sp[i][1]);
        assert(poller_add(p, sp[i][0], POLLER_IN, NULL) == 0);
    }

    poller_event_t ev[32];
    uint64_t t0 = now_us();
    for (int it = 0; it < ITERS; it++) {
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
    }
    uint64_t dt = now_us() - t0;

    poller_stats_t st;
    poller_get_stats(p, &st);
    printf("  %-9s | backend=%-5s | %6.2f ms | epoll_ctl=%6llu | switches=%llu\n",
           poller_mode_name(mode), st.using_epoll ? "epoll" : "poll",
           (double)dt / 1000.0,
           (unsigned long long)st.epoll_ctl_calls,
           (unsigned long long)st.switches);

    for (int i = 0; i < N; i++) {
        poller_del(p, sp[i][0]);
        close(sp[i][0]);
        close(sp[i][1]);
    }
    free(sp);
    poller_destroy(p);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        run_unit_tests();
        return 0;
    }

    run_unit_tests();
    printf("\n=== Workload A: Small N=32 + Frequent State Churn (10,000 iters) ===\n");
    bench_churn(POLLER_POLL);
    bench_churn(POLLER_EPOLL);
    bench_churn(POLLER_AUTO);
    bench_churn(POLLER_ADAPTIVE);

    printf("\n=== Workload B: Large N=2,000 Idle Sockets + 8 Active (4,000 iters) ===\n");
    bench_idle_scale(POLLER_POLL);
    bench_idle_scale(POLLER_EPOLL);
    bench_idle_scale(POLLER_AUTO);
    bench_idle_scale(POLLER_ADAPTIVE);
    return 0;
}

#define _POSIX_C_SOURCE 200809L

#include "poller.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void test_basic_event_delivery(void) {
    poller_mode_t modes[] = { POLLER_POLL, POLLER_FAST, POLLER_AUTO, POLLER_ADAPTIVE };
    for (size_t i = 0; i < 4; i++) {
        poller_t *p = poller_create(modes[i], 128);
        assert(p != NULL);

        /* Invalid fd and duplicate add checks */
        assert(poller_add(p, -1, POLLER_IN, NULL) < 0);
        assert(poller_del(p, 999) < 0);

        int sp[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
        set_nonblock(sp[0]);
        set_nonblock(sp[1]);

        assert(poller_add(p, sp[0], POLLER_IN, (void *)(uintptr_t)0xBEEF) == 0);
        assert(poller_add(p, sp[0], POLLER_IN, NULL) < 0); /* duplicate rejected */

        char msg = 'K';
        assert(write(sp[1], &msg, 1) == 1);

        poller_event_t ev[4];
        int n = poller_wait(p, ev, 4, 100);
        assert(n == 1);
        assert(ev[0].fd == sp[0]);
        assert((ev[0].events & POLLER_IN) != 0);
        assert((uintptr_t)ev[0].udata == 0xBEEF);

        char got = 0;
        assert(read(sp[0], &got, 1) == 1 && got == 'K');
        assert(poller_del(p, sp[0]) == 0);
        assert(poller_del(p, sp[0]) < 0); /* double del rejected */

        close(sp[0]);
        close(sp[1]);
        poller_destroy(p);
    }
}

static void test_mod_elision(void) {
    poller_t *p = poller_create(POLLER_FAST, 128);
    assert(p != NULL);

    int sp[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
    assert(poller_add(p, sp[0], POLLER_IN, NULL) == 0);

    poller_stats_t s0, s1;
    poller_get_stats(p, &s0);

    /* Calling mod with identical events 50 times must elide all 50 syscalls */
    for (int i = 0; i < 50; i++) {
        assert(poller_mod(p, sp[0], POLLER_IN, (void *)(uintptr_t)i) == 0);
    }
    poller_get_stats(p, &s1);
    if (s1.using_fast) {
        assert(s1.ctl_calls == s0.ctl_calls);
        assert(s1.ctl_elided == 50);
    }

    assert(poller_del(p, sp[0]) == 0);
    close(sp[0]);
    close(sp[1]);
    poller_destroy(p);
}

static void test_auto_startup_selection(void) {
    poller_t *p_small = poller_create(POLLER_AUTO, 64);
    poller_t *p_large = poller_create(POLLER_AUTO, 2048);
    poller_stats_t s1, s2;
    poller_get_stats(p_small, &s1);
    poller_get_stats(p_large, &s2);

    assert(s1.using_fast == 0 && s1.switches == 0);
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    assert(s2.using_fast == 1 && s2.switches == 0);
#endif
    poller_destroy(p_small);
    poller_destroy(p_large);
}

static void test_adaptive_hysteresis_and_demotion(void) {
    poller_config_t cfg = poller_default_config(POLLER_ADAPTIVE, 32);
    cfg.high_water = 64;
    cfg.low_water = 16;
    cfg.cooldown_ms = 0; /* immediate demotion allowed for unit test */

    poller_t *p = poller_create_with_config(&cfg);
    assert(p != NULL);

    int fds[80][2];
    for (int i = 0; i < 80; i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]) == 0);
        set_nonblock(fds[i][0]);
        assert(poller_add(p, fds[i][0], POLLER_IN, (void *)(uintptr_t)i) == 0);
    }

    poller_stats_t st;
    poller_get_stats(p, &st);
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    assert(st.using_fast == 1);
    assert(st.switches == 1);
#endif

    /* Verify event delivery AFTER runtime migration from poll -> fast */
    char ping = 'P';
    assert(write(fds[70][1], &ping, 1) == 1);
    poller_event_t ev[4];
    assert(poller_wait(p, ev, 4, 100) == 1);
    assert(ev[0].fd == fds[70][0] && (uintptr_t)ev[0].udata == 70);
    char pong = 0;
    assert(read(fds[70][0], &pong, 1) == 1 && pong == 'P');

    /* Drop to 40 fds (still > low_water=16): must stay on fast backend */
    for (int i = 40; i < 80; i++) {
        assert(poller_del(p, fds[i][0]) == 0);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    poller_get_stats(p, &st);
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    assert(st.using_fast == 1);
    assert(st.switches == 1);
#endif

    /* Drop to 10 fds (<= low_water=16): demotes cleanly back to poll */
    for (int i = 10; i < 40; i++) {
        assert(poller_del(p, fds[i][0]) == 0);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    poller_get_stats(p, &st);
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    assert(st.using_fast == 0);
    assert(st.switches == 2);
#endif

    /* Verify event delivery AFTER demotion back to poll */
    assert(write(fds[3][1], &ping, 1) == 1);
    assert(poller_wait(p, ev, 4, 100) == 1);
    assert(ev[0].fd == fds[3][0] && (uintptr_t)ev[0].udata == 3);

    for (int i = 0; i < 10; i++) {
        assert(poller_del(p, fds[i][0]) == 0);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    poller_destroy(p);
}

int main(void) {
    test_basic_event_delivery();
    test_mod_elision();
    test_auto_startup_selection();
    test_adaptive_hysteresis_and_demotion();
    printf("[PASS] All poller unit & hysteresis tests succeeded.\n");
    return 0;
}

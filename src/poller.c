#define _POSIX_C_SOURCE 200809L

#include "poller.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#define BACKEND_EPOLL 1
#define BACKEND_KQUEUE 0
#define FAST_BACKEND_NAME "epoll"
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/event.h>
#define BACKEND_EPOLL 0
#define BACKEND_KQUEUE 1
#define FAST_BACKEND_NAME "kqueue"
#else
#define BACKEND_EPOLL 0
#define BACKEND_KQUEUE 0
#define FAST_BACKEND_NAME "poll"
#endif

#define DEFAULT_HIGH_WATER  256
#define DEFAULT_LOW_WATER   64
#define DEFAULT_COOLDOWN_MS 2000

typedef struct {
    int active;
    int poll_idx;
    uint32_t events;
    void *udata;
} fd_slot_t;

struct poller {
    poller_config_t cfg;
    int using_fast;

    /* Compact array for poll() and O(N) migration snapshot */
    struct pollfd *pfds;
    int pfd_len;
    int pfd_cap;

    /* Direct-indexed fd -> slot table for O(1) lookup */
    fd_slot_t *slots;
    int slot_cap;

#if BACKEND_EPOLL
    int epfd;
    struct epoll_event *epevs;
    int epev_cap;
#elif BACKEND_KQUEUE
    int kqfd;
    struct kevent *kevs;
    int kev_cap;
#endif

    uint64_t switches;
    uint64_t ctl_calls;
    uint64_t ctl_elided;
    uint64_t wait_calls;
    uint64_t last_switch_ms;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static short to_poll_events(uint32_t ev) {
    short r = 0;
    if (ev & POLLER_IN)  r |= POLLIN;
    if (ev & POLLER_OUT) r |= POLLOUT;
    return r;
}

static uint32_t from_poll_revents(short rev) {
    uint32_t r = 0;
    if (rev & POLLIN)  r |= POLLER_IN;
    if (rev & POLLOUT) r |= POLLER_OUT;
    if (rev & (POLLERR | POLLNVAL)) r |= POLLER_ERR;
    if (rev & POLLHUP) r |= POLLER_HUP;
    return r;
}

#if BACKEND_EPOLL
static uint32_t to_epoll_events(uint32_t ev) {
    uint32_t r = 0;
    if (ev & POLLER_IN)    r |= EPOLLIN;
    if (ev & POLLER_OUT)   r |= EPOLLOUT;
    if (ev & POLLER_RDHUP) r |= EPOLLRDHUP;
    return r;
}

static uint32_t from_epoll_events(uint32_t ev) {
    uint32_t r = 0;
    if (ev & EPOLLIN)    r |= POLLER_IN;
    if (ev & EPOLLOUT)   r |= POLLER_OUT;
    if (ev & EPOLLERR)   r |= POLLER_ERR;
    if (ev & EPOLLHUP)   r |= POLLER_HUP;
    if (ev & EPOLLRDHUP) r |= POLLER_RDHUP;
    return r;
}
#endif

static int ensure_slot_cap(poller_t *p, int fd) {
    if (fd < 0) return -1;
    if (fd < p->slot_cap) return 0;
    int new_cap = p->slot_cap ? p->slot_cap : 64;
    while (new_cap <= fd) new_cap *= 2;
    fd_slot_t *ns = realloc(p->slots, (size_t)new_cap * sizeof(fd_slot_t));
    if (!ns) return -1;
    memset(ns + p->slot_cap, 0, (size_t)(new_cap - p->slot_cap) * sizeof(fd_slot_t));
    p->slots = ns;
    p->slot_cap = new_cap;
    return 0;
}

static int ensure_pfd_cap(poller_t *p, int need) {
    if (need <= p->pfd_cap) return 0;
    int new_cap = p->pfd_cap ? p->pfd_cap : 64;
    while (new_cap < need) new_cap *= 2;
    struct pollfd *np = realloc(p->pfds, (size_t)new_cap * sizeof(struct pollfd));
    if (!np) return -1;
    p->pfds = np;
    p->pfd_cap = new_cap;
    return 0;
}

#if BACKEND_KQUEUE
static int kq_apply(poller_t *p, int fd, uint32_t old_ev, uint32_t new_ev) {
    struct kevent ch[2];
    int n = 0;
    if ((old_ev & POLLER_IN) != (new_ev & POLLER_IN)) {
        EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_READ,
               (new_ev & POLLER_IN) ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, NULL);
    }
    if ((old_ev & POLLER_OUT) != (new_ev & POLLER_OUT)) {
        EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_WRITE,
               (new_ev & POLLER_OUT) ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, NULL);
    }
    if (n == 0) return 0;
    if (kevent(p->kqfd, ch, n, NULL, 0, NULL) < 0) return -1;
    p->ctl_calls++;
    return 0;
}
#endif

#if BACKEND_EPOLL || BACKEND_KQUEUE
static int switch_to_fast(poller_t *p) {
    if (p->using_fast) return 0;
#if BACKEND_EPOLL
    if (p->epfd < 0) {
        p->epfd = epoll_create1(EPOLL_CLOEXEC);
        if (p->epfd < 0) return -1;
    }
    for (int i = 0; i < p->pfd_len; i++) {
        int fd = p->pfds[i].fd;
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = to_epoll_events(p->slots[fd].events);
        ev.data.fd = fd;
        if (epoll_ctl(p->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            close(p->epfd);
            p->epfd = -1;
            return -1;
        }
        p->ctl_calls++;
    }
#elif BACKEND_KQUEUE
    if (p->kqfd < 0) {
        p->kqfd = kqueue();
        if (p->kqfd < 0) return -1;
    }
    for (int i = 0; i < p->pfd_len; i++) {
        int fd = p->pfds[i].fd;
        if (kq_apply(p, fd, 0, p->slots[fd].events) < 0) {
            close(p->kqfd);
            p->kqfd = -1;
            return -1;
        }
    }
#endif
    p->using_fast = 1;
    p->switches++;
    p->last_switch_ms = now_ms();
    return 0;
}

static void switch_to_poll(poller_t *p) {
    if (!p->using_fast) return;
#if BACKEND_EPOLL
    if (p->epfd >= 0) {
        close(p->epfd);
        p->epfd = -1;
    }
#elif BACKEND_KQUEUE
    if (p->kqfd >= 0) {
        close(p->kqfd);
        p->kqfd = -1;
    }
#endif
    p->using_fast = 0;
    p->switches++;
    p->last_switch_ms = now_ms();
}

static void maybe_adapt(poller_t *p) {
    if (p->cfg.mode != POLLER_ADAPTIVE) return;
    if (!p->using_fast && p->pfd_len >= p->cfg.high_water) {
        (void)switch_to_fast(p);
    } else if (p->using_fast && p->pfd_len <= p->cfg.low_water) {
        uint64_t t = now_ms();
        if (t - p->last_switch_ms >= (uint64_t)p->cfg.cooldown_ms) {
            switch_to_poll(p);
        }
    }
}
#endif

poller_config_t poller_default_config(poller_mode_t mode, int max_conns_hint) {
    poller_config_t cfg;
    cfg.mode = mode;
    cfg.max_conns_hint = max_conns_hint > 0 ? max_conns_hint : 128;
    cfg.high_water = DEFAULT_HIGH_WATER;
    cfg.low_water = DEFAULT_LOW_WATER;
    cfg.cooldown_ms = DEFAULT_COOLDOWN_MS;
    return cfg;
}

poller_t *poller_create_with_config(const poller_config_t *cfg) {
    if (!cfg) return NULL;
    poller_t *p = calloc(1, sizeof(poller_t));
    if (!p) return NULL;

    p->cfg = *cfg;
    if (p->cfg.high_water <= 0) p->cfg.high_water = DEFAULT_HIGH_WATER;
    if (p->cfg.low_water < 0) p->cfg.low_water = DEFAULT_LOW_WATER;
    if (p->cfg.low_water >= p->cfg.high_water) p->cfg.low_water = p->cfg.high_water / 4;
    if (p->cfg.cooldown_ms < 0) p->cfg.cooldown_ms = DEFAULT_COOLDOWN_MS;

#if BACKEND_EPOLL
    p->epfd = -1;
#elif BACKEND_KQUEUE
    p->kqfd = -1;
#endif

#if BACKEND_EPOLL || BACKEND_KQUEUE
    int start_fast = 0;
    if (p->cfg.mode == POLLER_FAST) {
        start_fast = 1;
    } else if (p->cfg.mode == POLLER_AUTO) {
        start_fast = (p->cfg.max_conns_hint > p->cfg.high_water) ? 1 : 0;
    }
    if (start_fast) {
#if BACKEND_EPOLL
        p->epfd = epoll_create1(EPOLL_CLOEXEC);
        if (p->epfd < 0) { free(p); return NULL; }
#elif BACKEND_KQUEUE
        p->kqfd = kqueue();
        if (p->kqfd < 0) { free(p); return NULL; }
#endif
        p->using_fast = 1;
    }
#endif
    return p;
}

poller_t *poller_create(poller_mode_t mode, int max_conns_hint) {
    poller_config_t cfg = poller_default_config(mode, max_conns_hint);
    return poller_create_with_config(&cfg);
}

void poller_destroy(poller_t *p) {
    if (!p) return;
#if BACKEND_EPOLL
    if (p->epfd >= 0) close(p->epfd);
    free(p->epevs);
#elif BACKEND_KQUEUE
    if (p->kqfd >= 0) close(p->kqfd);
    free(p->kevs);
#endif
    free(p->pfds);
    free(p->slots);
    free(p);
}

int poller_add(poller_t *p, int fd, uint32_t events, void *udata) {
    if (!p || fd < 0) return -1;
    if (ensure_slot_cap(p, fd) < 0) return -1;
    if (p->slots[fd].active) return -1;
    if (ensure_pfd_cap(p, p->pfd_len + 1) < 0) return -1;

#if BACKEND_EPOLL
    if (p->using_fast) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = to_epoll_events(events);
        ev.data.fd = fd;
        if (epoll_ctl(p->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) return -1;
        p->ctl_calls++;
    }
#elif BACKEND_KQUEUE
    if (p->using_fast) {
        if (kq_apply(p, fd, 0, events) < 0) return -1;
    }
#endif

    int idx = p->pfd_len++;
    p->pfds[idx].fd = fd;
    p->pfds[idx].events = to_poll_events(events);
    p->pfds[idx].revents = 0;

    p->slots[fd].active = 1;
    p->slots[fd].poll_idx = idx;
    p->slots[fd].events = events;
    p->slots[fd].udata = udata;

#if BACKEND_EPOLL || BACKEND_KQUEUE
    maybe_adapt(p);
#endif
    return 0;
}

int poller_mod(poller_t *p, int fd, uint32_t events, void *udata) {
    if (!p || fd < 0 || fd >= p->slot_cap || !p->slots[fd].active) return -1;

    uint32_t prev_events = p->slots[fd].events;
    p->slots[fd].udata = udata;

    /* Optimization: elide redundant kernel ctl syscall if interest mask is unchanged */
    if (prev_events == events) {
        if (p->using_fast) p->ctl_elided++;
        return 0;
    }

#if BACKEND_EPOLL
    if (p->using_fast) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = to_epoll_events(events);
        ev.data.fd = fd;
        if (epoll_ctl(p->epfd, EPOLL_CTL_MOD, fd, &ev) < 0) return -1;
        p->ctl_calls++;
    }
#elif BACKEND_KQUEUE
    if (p->using_fast) {
        if (kq_apply(p, fd, prev_events, events) < 0) return -1;
    }
#endif

    int idx = p->slots[fd].poll_idx;
    p->pfds[idx].events = to_poll_events(events);
    p->slots[fd].events = events;
    return 0;
}

int poller_del(poller_t *p, int fd) {
    if (!p || fd < 0 || fd >= p->slot_cap || !p->slots[fd].active) return -1;

#if BACKEND_EPOLL
    if (p->using_fast) {
        if (epoll_ctl(p->epfd, EPOLL_CTL_DEL, fd, NULL) < 0 && errno != EBADF && errno != ENOENT) {
            return -1;
        }
        p->ctl_calls++;
    }
#elif BACKEND_KQUEUE
    if (p->using_fast) {
        (void)kq_apply(p, fd, p->slots[fd].events, 0);
    }
#endif

    int idx = p->slots[fd].poll_idx;
    int last = p->pfd_len - 1;
    if (idx != last) {
        p->pfds[idx] = p->pfds[last];
        p->slots[p->pfds[idx].fd].poll_idx = idx;
    }
    p->pfd_len--;
    p->slots[fd].active = 0;
    p->slots[fd].poll_idx = -1;
    p->slots[fd].events = 0;
    p->slots[fd].udata = NULL;

#if BACKEND_EPOLL || BACKEND_KQUEUE
    maybe_adapt(p);
#endif
    return 0;
}

int poller_wait(poller_t *p, poller_event_t *out, int max_events, int timeout_ms) {
    if (!p || !out || max_events <= 0) return -1;
    p->wait_calls++;

#if BACKEND_EPOLL
    if (p->using_fast) {
        if (p->epev_cap < max_events) {
            struct epoll_event *ne = realloc(p->epevs, (size_t)max_events * sizeof(struct epoll_event));
            if (!ne) return -1;
            p->epevs = ne;
            p->epev_cap = max_events;
        }
        int n = epoll_wait(p->epfd, p->epevs, max_events, timeout_ms);
        if (n <= 0) return n;
        for (int i = 0; i < n; i++) {
            int fd = p->epevs[i].data.fd;
            out[i].fd = fd;
            out[i].events = from_epoll_events(p->epevs[i].events);
            out[i].udata = (fd >= 0 && fd < p->slot_cap) ? p->slots[fd].udata : NULL;
        }
        return n;
    }
#elif BACKEND_KQUEUE
    if (p->using_fast) {
        if (p->kev_cap < max_events) {
            struct kevent *nk = realloc(p->kevs, (size_t)max_events * sizeof(struct kevent));
            if (!nk) return -1;
            p->kevs = nk;
            p->kev_cap = max_events;
        }
        struct timespec ts;
        struct timespec *tsp = NULL;
        if (timeout_ms >= 0) {
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
            tsp = &ts;
        }
        int n = kevent(p->kqfd, NULL, 0, p->kevs, max_events, tsp);
        if (n <= 0) return n;
        for (int i = 0; i < n; i++) {
            int fd = (int)p->kevs[i].ident;
            uint32_t ev = 0;
            if (p->kevs[i].filter == EVFILT_READ)  ev |= POLLER_IN;
            if (p->kevs[i].filter == EVFILT_WRITE) ev |= POLLER_OUT;
            if (p->kevs[i].flags & EV_EOF)         ev |= POLLER_HUP;
            if (p->kevs[i].flags & EV_ERROR)       ev |= POLLER_ERR;
            out[i].fd = fd;
            out[i].events = ev;
            out[i].udata = (fd >= 0 && fd < p->slot_cap) ? p->slots[fd].udata : NULL;
        }
        return n;
    }
#endif

    int ready = poll(p->pfds, (nfds_t)p->pfd_len, timeout_ms);
    if (ready <= 0) return ready;

    int count = 0;
    for (int i = 0; i < p->pfd_len && count < max_events; i++) {
        if (p->pfds[i].revents != 0) {
            int fd = p->pfds[i].fd;
            out[count].fd = fd;
            out[count].events = from_poll_revents(p->pfds[i].revents);
            out[count].udata = p->slots[fd].udata;
            count++;
        }
    }
    return count;
}

void poller_get_stats(const poller_t *p, poller_stats_t *out) {
    if (!p || !out) return;
    out->requested_mode = p->cfg.mode;
    out->using_fast = p->using_fast;
    out->active_backend = p->using_fast ? FAST_BACKEND_NAME : "poll";
    out->active_fds = p->pfd_len;
    out->switches = p->switches;
    out->ctl_calls = p->ctl_calls;
    out->ctl_elided = p->ctl_elided;
    out->wait_calls = p->wait_calls;
}

const char *poller_mode_name(poller_mode_t mode) {
    switch (mode) {
        case POLLER_POLL:     return "poll";
        case POLLER_FAST:     return FAST_BACKEND_NAME;
        case POLLER_AUTO:     return "auto";
        case POLLER_ADAPTIVE: return "adaptive";
        default:              return "unknown";
    }
}

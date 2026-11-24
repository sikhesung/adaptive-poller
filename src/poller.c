#define _POSIX_C_SOURCE 200809L

#include "poller.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#define HAS_EPOLL 1
#else
#define HAS_EPOLL 0
#endif

/* Hysteresis thresholds for POLLER_ADAPTIVE to prevent 255 <-> 257 thrashing */
#define ADAPTIVE_HIGH_WATER  256
#define ADAPTIVE_LOW_WATER   64
#define ADAPTIVE_COOLDOWN_MS 2000

/* Startup threshold for POLLER_AUTO (zero runtime migration cost) */
#define AUTO_EPOLL_THRESHOLD 256

typedef struct {
    int active;
    int poll_idx;      /* Index in compact pollfds array */
    uint32_t events;
    void *udata;
} fd_slot_t;

struct poller {
    poller_mode_t requested_mode;
    int using_epoll;

    /* Compact array for poll() and fast iteration during migration */
    struct pollfd *pfds;
    int pfd_len;
    int pfd_cap;

    /* Direct-indexed fd -> slot table for O(1) lookup */
    fd_slot_t *slots;
    int slot_cap;

#if HAS_EPOLL
    int epfd;
    struct epoll_event *epevs;
    int epev_cap;
#endif

    uint64_t switches;
    uint64_t epoll_ctl_calls;
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

#if HAS_EPOLL
static uint32_t to_epoll_events(uint32_t ev) {
    uint32_t r = 0;
    if (ev & POLLER_IN)  r |= EPOLLIN;
    if (ev & POLLER_OUT) r |= EPOLLOUT;
    return r;
}

static uint32_t from_epoll_events(uint32_t ev) {
    uint32_t r = 0;
    if (ev & EPOLLIN)  r |= POLLER_IN;
    if (ev & EPOLLOUT) r |= POLLER_OUT;
    if (ev & EPOLLERR) r |= POLLER_ERR;
    if (ev & EPOLLHUP) r |= POLLER_HUP;
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

#if HAS_EPOLL
static int switch_to_epoll(poller_t *p) {
    if (p->using_epoll) return 0;
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
            /* Clean up partial migration and stay on poll safely */
            close(p->epfd);
            p->epfd = -1;
            return -1;
        }
        p->epoll_ctl_calls++;
    }
    p->using_epoll = 1;
    p->switches++;
    p->last_switch_ms = now_ms();
    return 0;
}

static void switch_to_poll(poller_t *p) {
    if (!p->using_epoll) return;
    if (p->epfd >= 0) {
        close(p->epfd);
        p->epfd = -1;
    }
    p->using_epoll = 0;
    p->switches++;
    p->last_switch_ms = now_ms();
}

static void maybe_adapt(poller_t *p) {
    if (p->requested_mode != POLLER_ADAPTIVE) return;
    if (!p->using_epoll && p->pfd_len >= ADAPTIVE_HIGH_WATER) {
        (void)switch_to_epoll(p);
    } else if (p->using_epoll && p->pfd_len <= ADAPTIVE_LOW_WATER) {
        uint64_t t = now_ms();
        if (t - p->last_switch_ms >= ADAPTIVE_COOLDOWN_MS) {
            switch_to_poll(p);
        }
    }
}
#endif

poller_t *poller_create(poller_mode_t mode, int max_conns_hint) {
    poller_t *p = calloc(1, sizeof(poller_t));
    if (!p) return NULL;
    p->requested_mode = mode;

#if HAS_EPOLL
    p->epfd = -1;
    int start_epoll = 0;
    if (mode == POLLER_EPOLL) {
        start_epoll = 1;
    } else if (mode == POLLER_AUTO) {
        start_epoll = (max_conns_hint > AUTO_EPOLL_THRESHOLD) ? 1 : 0;
    } else {
        /* POLLER_POLL or POLLER_ADAPTIVE starts in lightweight poll mode */
        start_epoll = 0;
    }

    if (start_epoll) {
        p->epfd = epoll_create1(EPOLL_CLOEXEC);
        if (p->epfd < 0) {
            free(p);
            return NULL;
        }
        p->using_epoll = 1;
    }
#else
    (void)max_conns_hint;
    p->using_epoll = 0;
#endif
    return p;
}

void poller_destroy(poller_t *p) {
    if (!p) return;
#if HAS_EPOLL
    if (p->epfd >= 0) close(p->epfd);
    free(p->epevs);
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

#if HAS_EPOLL
    if (p->using_epoll) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = to_epoll_events(events);
        ev.data.fd = fd;
        if (epoll_ctl(p->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) return -1;
        p->epoll_ctl_calls++;
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

#if HAS_EPOLL
    maybe_adapt(p);
#endif
    return 0;
}

int poller_mod(poller_t *p, int fd, uint32_t events, void *udata) {
    if (!p || fd < 0 || fd >= p->slot_cap || !p->slots[fd].active) return -1;

#if HAS_EPOLL
    if (p->using_epoll) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = to_epoll_events(events);
        ev.data.fd = fd;
        if (epoll_ctl(p->epfd, EPOLL_CTL_MOD, fd, &ev) < 0) return -1;
        p->epoll_ctl_calls++;
    }
#endif

    int idx = p->slots[fd].poll_idx;
    p->pfds[idx].events = to_poll_events(events);
    p->slots[fd].events = events;
    p->slots[fd].udata = udata;
    return 0;
}

int poller_del(poller_t *p, int fd) {
    if (!p || fd < 0 || fd >= p->slot_cap || !p->slots[fd].active) return -1;

#if HAS_EPOLL
    if (p->using_epoll) {
        if (epoll_ctl(p->epfd, EPOLL_CTL_DEL, fd, NULL) < 0 && errno != EBADF && errno != ENOENT) {
            return -1;
        }
        p->epoll_ctl_calls++;
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

#if HAS_EPOLL
    maybe_adapt(p);
#endif
    return 0;
}

int poller_wait(poller_t *p, poller_event_t *out, int max_events, int timeout_ms) {
    if (!p || !out || max_events <= 0) return -1;
    p->wait_calls++;

#if HAS_EPOLL
    if (p->using_epoll) {
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
    out->requested_mode = p->requested_mode;
    out->using_epoll = p->using_epoll;
    out->active_fds = p->pfd_len;
    out->switches = p->switches;
    out->epoll_ctl_calls = p->epoll_ctl_calls;
    out->wait_calls = p->wait_calls;
}

const char *poller_mode_name(poller_mode_t mode) {
    switch (mode) {
        case POLLER_POLL:     return "poll";
        case POLLER_EPOLL:    return "epoll";
        case POLLER_AUTO:     return "auto";
        case POLLER_ADAPTIVE: return "adaptive";
        default:              return "unknown";
    }
}

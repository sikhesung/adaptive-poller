#ifndef POLLER_H
#define POLLER_H

#include <stddef.h>
#include <stdint.h>

#define POLLER_IN    (1u << 0)
#define POLLER_OUT   (1u << 1)
#define POLLER_ERR   (1u << 2)
#define POLLER_HUP   (1u << 3)
#define POLLER_RDHUP (1u << 4)

typedef enum {
    POLLER_POLL = 0,      /* Always POSIX poll() */
    POLLER_FAST = 1,      /* Always OS fast backend (Linux epoll / macOS kqueue; fallback poll) */
    POLLER_EPOLL = 1,     /* Alias for POLLER_FAST */
    POLLER_AUTO = 2,      /* ROI+ #1: Zero-cost startup selection by OS + max_conns_hint */
    POLLER_ADAPTIVE = 3   /* ROI+ #2: Hysteresis-guarded runtime poll <-> fast backend switching */
} poller_mode_t;

typedef struct {
    poller_mode_t mode;
    int max_conns_hint;   /* Expected peak concurrency (used by POLLER_AUTO) */
    int high_water;       /* Promote poll -> fast when active_fds >= high_water (default: 256) */
    int low_water;        /* Demote fast -> poll when active_fds <= low_water (default: 64) */
    int cooldown_ms;      /* Minimum ms between demotions to prevent thrashing (default: 2000) */
} poller_config_t;

typedef struct {
    int fd;
    uint32_t events;
    void *udata;
} poller_event_t;

typedef struct {
    poller_mode_t requested_mode;
    const char *active_backend; /* "poll", "epoll", or "kqueue" */
    int using_fast;             /* 0 = poll, 1 = epoll/kqueue */
    int active_fds;
    uint64_t switches;          /* Runtime poll <-> fast transitions */
    uint64_t ctl_calls;         /* Actual kernel epoll_ctl / kevent registration syscalls */
    uint64_t ctl_elided;        /* Avoided no-op poller_mod syscalls */
    uint64_t wait_calls;        /* Total poll / epoll_wait / kevent wait calls */
} poller_stats_t;

typedef struct poller poller_t;

poller_config_t poller_default_config(poller_mode_t mode, int max_conns_hint);
poller_t *poller_create_with_config(const poller_config_t *cfg);
poller_t *poller_create(poller_mode_t mode, int max_conns_hint);
void poller_destroy(poller_t *p);

int poller_add(poller_t *p, int fd, uint32_t events, void *udata);
int poller_mod(poller_t *p, int fd, uint32_t events, void *udata);
int poller_del(poller_t *p, int fd);

int poller_wait(poller_t *p, poller_event_t *out, int max_events, int timeout_ms);
void poller_get_stats(const poller_t *p, poller_stats_t *out);
const char *poller_mode_name(poller_mode_t mode);

#endif /* POLLER_H */

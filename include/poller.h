#ifndef POLLER_H
#define POLLER_H

#include <stddef.h>
#include <stdint.h>

#define POLLER_IN  (1u << 0)
#define POLLER_OUT (1u << 1)
#define POLLER_ERR (1u << 2)
#define POLLER_HUP (1u << 3)

typedef enum {
    POLLER_POLL = 0,      /* Always POSIX poll() */
    POLLER_EPOLL = 1,     /* Always Linux epoll() (falls back to poll on non-Linux) */
    POLLER_AUTO = 2,      /* ROI+ #1: Zero-cost startup selection by OS + max_conns_hint */
    POLLER_ADAPTIVE = 3   /* ROI+ #2: Hysteresis-guarded runtime poll <-> epoll switching */
} poller_mode_t;

typedef struct {
    int fd;
    uint32_t events;
    void *udata;
} poller_event_t;

typedef struct {
    poller_mode_t requested_mode;
    int using_epoll;          /* 0 = poll, 1 = epoll */
    int active_fds;
    uint64_t switches;        /* Number of runtime poll <-> epoll transitions */
    uint64_t epoll_ctl_calls; /* Total epoll_ctl syscalls made */
    uint64_t wait_calls;      /* Total poll/epoll_wait syscalls made */
} poller_stats_t;

typedef struct poller poller_t;

poller_t *poller_create(poller_mode_t mode, int max_conns_hint);
void poller_destroy(poller_t *p);

int poller_add(poller_t *p, int fd, uint32_t events, void *udata);
int poller_mod(poller_t *p, int fd, uint32_t events, void *udata);
int poller_del(poller_t *p, int fd);

int poller_wait(poller_t *p, poller_event_t *out, int max_events, int timeout_ms);
void poller_get_stats(const poller_t *p, poller_stats_t *out);
const char *poller_mode_name(poller_mode_t mode);

#endif /* POLLER_H */

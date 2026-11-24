#define _POSIX_C_SOURCE 200809L

#include "poller.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 128

typedef struct conn {
    int fd;
    char out[1024];
    size_t out_len;
    size_t out_sent;
    struct conn *prev;
    struct conn *next;
} conn_t;

static int g_sig_pipe[2] = { -1, -1 };

static void on_sig(int sig) {
    (void)sig;
    if (g_sig_pipe[1] >= 0) {
        char b = 1;
        ssize_t w = write(g_sig_pipe[1], &b, 1);
        (void)w;
    }
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int listen_port(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_nonblock(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 512) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static poller_mode_t parse_mode(const char *s) {
    if (strcmp(s, "poll") == 0) return POLLER_POLL;
    if (strcmp(s, "epoll") == 0 || strcmp(s, "kqueue") == 0 || strcmp(s, "fast") == 0) return POLLER_FAST;
    if (strcmp(s, "auto") == 0) return POLLER_AUTO;
    return POLLER_ADAPTIVE;
}

static void conn_close(poller_t *poller, conn_t **head, conn_t *c) {
    if (!c) return;
    poller_del(poller, c->fd);
    close(c->fd);
    if (c->prev) c->prev->next = c->next;
    else *head = c->next;
    if (c->next) c->next->prev = c->prev;
    free(c);
}

static int conn_flush(poller_t *poller, conn_t *c) {
    while (c->out_sent < c->out_len) {
        ssize_t w = write(c->fd, c->out + c->out_sent, c->out_len - c->out_sent);
        if (w > 0) {
            c->out_sent += (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            poller_mod(poller, c->fd, POLLER_IN | POLLER_OUT | POLLER_RDHUP, c);
            return 0;
        } else {
            return -1;
        }
    }
    c->out_len = 0;
    c->out_sent = 0;
    poller_mod(poller, c->fd, POLLER_IN | POLLER_RDHUP, c);
    return 0;
}

int main(int argc, char **argv) {
    int port = 18900;
    int max_conns = 1024;
    poller_mode_t mode = POLLER_ADAPTIVE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) max_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) mode = parse_mode(argv[++i]);
    }

    if (pipe(g_sig_pipe) < 0) {
        perror("pipe");
        return 1;
    }
    set_nonblock(g_sig_pipe[0]);
    set_nonblock(g_sig_pipe[1]);

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int lfd = listen_port(port);
    if (lfd < 0) {
        perror("listen_port");
        close(g_sig_pipe[0]);
        close(g_sig_pipe[1]);
        return 1;
    }

    poller_t *poller = poller_create(mode, max_conns);
    if (!poller) {
        perror("poller_create");
        close(lfd);
        close(g_sig_pipe[0]);
        close(g_sig_pipe[1]);
        return 1;
    }
    poller_add(poller, g_sig_pipe[0], POLLER_IN, NULL);
    poller_add(poller, lfd, POLLER_IN, NULL);

    poller_stats_t st;
    poller_get_stats(poller, &st);
    printf("adaptive_server listening on :%d | mode=%s | initial_backend=%s | hint=%d\n",
           port, poller_mode_name(mode), st.active_backend, max_conns);
    fflush(stdout);

    conn_t *conns = NULL;
    poller_event_t evs[MAX_EVENTS];
    char rbuf[4096];
    int running = 1;

    while (running) {
        int n = poller_wait(poller, evs, MAX_EVENTS, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].fd;
            if (fd == g_sig_pipe[0]) {
                running = 0;
                break;
            }
            if (fd == lfd) {
                for (;;) {
                    int cfd = accept(lfd, NULL, NULL);
                    if (cfd < 0) break;
                    int one = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    set_nonblock(cfd);
                    conn_t *c = calloc(1, sizeof(conn_t));
                    if (!c || poller_add(poller, cfd, POLLER_IN | POLLER_RDHUP, c) < 0) {
                        free(c);
                        close(cfd);
                        continue;
                    }
                    c->fd = cfd;
                    c->next = conns;
                    if (conns) conns->prev = c;
                    conns = c;
                }
                continue;
            }

            conn_t *c = (conn_t *)evs[i].udata;
            if (!c) continue;

            if (evs[i].events & (POLLER_ERR | POLLER_HUP | POLLER_RDHUP)) {
                conn_close(poller, &conns, c);
                continue;
            }
            if (evs[i].events & POLLER_OUT) {
                if (conn_flush(poller, c) < 0) {
                    conn_close(poller, &conns, c);
                    continue;
                }
            }
            if (evs[i].events & POLLER_IN) {
                ssize_t r = read(fd, rbuf, sizeof(rbuf) - 1);
                if (r <= 0) {
                    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    conn_close(poller, &conns, c);
                    continue;
                }
                rbuf[r] = '\0';
                poller_get_stats(poller, &st);

                char body[384];
                int blen;
                if (strncmp(rbuf, "GET /api/health", 15) == 0) {
                    blen = snprintf(body, sizeof(body), "{\"status\":\"ok\"}\n");
                } else {
                    blen = snprintf(body, sizeof(body),
                        "{\"mode\":\"%s\",\"active_backend\":\"%s\",\"active_fds\":%d,"
                        "\"switches\":%llu,\"ctl_calls\":%llu,\"ctl_elided\":%llu,\"wait_calls\":%llu}\n",
                        poller_mode_name(st.requested_mode),
                        st.active_backend,
                        st.active_fds,
                        (unsigned long long)st.switches,
                        (unsigned long long)st.ctl_calls,
                        (unsigned long long)st.ctl_elided,
                        (unsigned long long)st.wait_calls);
                }

                c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: keep-alive\r\n\r\n%s",
                    blen, body);
                c->out_sent = 0;
                if (conn_flush(poller, c) < 0) {
                    conn_close(poller, &conns, c);
                }
            }
        }
    }

    while (conns) {
        conn_close(poller, &conns, conns);
    }
    poller_destroy(poller);
    close(lfd);
    close(g_sig_pipe[0]);
    close(g_sig_pipe[1]);
    return 0;
}

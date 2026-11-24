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

static volatile sig_atomic_t g_stop = 0;

static void on_sig(int sig) {
    (void)sig;
    g_stop = 1;
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
    if (strcmp(s, "epoll") == 0) return POLLER_EPOLL;
    if (strcmp(s, "auto") == 0) return POLLER_AUTO;
    return POLLER_ADAPTIVE;
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

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    int lfd = listen_port(port);
    if (lfd < 0) {
        perror("listen_port");
        return 1;
    }

    poller_t *poller = poller_create(mode, max_conns);
    if (!poller) {
        perror("poller_create");
        close(lfd);
        return 1;
    }
    poller_add(poller, lfd, POLLER_IN, NULL);

    poller_stats_t st;
    poller_get_stats(poller, &st);
    printf("adaptive_server listening on :%d | mode=%s | initial_backend=%s | hint=%d\n",
           port, poller_mode_name(mode), st.using_epoll ? "epoll" : "poll", max_conns);
    fflush(stdout);

    poller_event_t evs[128];
    char buf[4096];
    char resp[512];

    while (!g_stop) {
        int n = poller_wait(poller, evs, 128, 250);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].fd;
            if (fd == lfd) {
                for (;;) {
                    int cfd = accept(lfd, NULL, NULL);
                    if (cfd < 0) break;
                    int one = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    set_nonblock(cfd);
                    if (poller_add(poller, cfd, POLLER_IN, NULL) < 0) {
                        close(cfd);
                    }
                }
            } else {
                ssize_t r = read(fd, buf, sizeof(buf) - 1);
                if (r <= 0) {
                    poller_del(poller, fd);
                    close(fd);
                    continue;
                }
                buf[r] = '\0';
                poller_get_stats(poller, &st);
                char body[256];
                int blen = snprintf(body, sizeof(body),
                    "{\"mode\":\"%s\",\"active_backend\":\"%s\",\"active_fds\":%d,"
                    "\"switches\":%llu,\"epoll_ctl_calls\":%llu,\"wait_calls\":%llu}\n",
                    poller_mode_name(st.requested_mode),
                    st.using_epoll ? "epoll" : "poll",
                    st.active_fds,
                    (unsigned long long)st.switches,
                    (unsigned long long)st.epoll_ctl_calls,
                    (unsigned long long)st.wait_calls);
                int rlen = snprintf(resp, sizeof(resp),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: keep-alive\r\n\r\n%s",
                    blen, body);
                ssize_t w = write(fd, resp, (size_t)rlen);
                if (w < 0) {
                    poller_del(poller, fd);
                    close(fd);
                }
            }
        }
    }

    poller_destroy(poller);
    close(lfd);
    return 0;
}

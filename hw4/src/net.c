#include "net.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
volatile sig_atomic_t interrupted;
static void signal_handler(int sig) { interrupted = sig; }
int signals_init(void) {
    struct sigaction action = {.sa_handler = signal_handler};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return -1;
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}
int64_t clock_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int parse_number(const char *s, int min, int max, int *n) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < min || v > max) return 0;
    *n = (int)v; return 1;
}
int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
void connection_init(Connection *c, int fd) {
    memset(c, 0, sizeof *c);
    c->fd = fd;
}
int net_queue(Connection *c, const char *format, ...) {
    if (c->bad || c->fd < 0) return -1;
    if (c->tx_start) {
        memmove(c->tx, c->tx + c->tx_start, c->tx_len - c->tx_start);
        c->tx_len -= c->tx_start; c->tx_start = 0;
    }
    va_list args;
    va_start(args, format);
    int n = vsnprintf(c->tx + c->tx_len, TX_SIZE - c->tx_len, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= TX_SIZE - c->tx_len) {
        c->bad = 1; return -1;
    }
    c->tx_len += (size_t)n;
    return 0;
}
int net_flush(Connection *c) {
    while (c->tx_start < c->tx_len) {
        ssize_t n = send(c->fd, c->tx + c->tx_start, c->tx_len - c->tx_start, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) { c->bad = 1; return -1; }
        c->tx_start += (size_t)n;
    }
    c->tx_start = c->tx_len = 0;
    return 0;
}
int net_read(Connection *c) {
    if (c->rx_len == RX_SIZE) return -1;
    ssize_t n = recv(c->fd, c->rx + c->rx_len, RX_SIZE - c->rx_len, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (n <= 0) return -1;
    // Protocol is printable text; NUL is never a valid frame character.
    if (memchr(c->rx + c->rx_len, '\0', (size_t)n)) return -1;
    c->rx_len += (size_t)n;
    return 1;
}
int net_line(Connection *c, char line[LINE_SIZE]) {
    char *end = memchr(c->rx, '\n', c->rx_len);
    if (!end) return c->rx_len >= LINE_SIZE ? -1 : 0;
    size_t n = (size_t)(end - c->rx);
    if (n >= LINE_SIZE) return -1;
    memcpy(line, c->rx, n); line[n] = 0;
    memmove(c->rx, end + 1, c->rx_len - n - 1);
    c->rx_len -= n + 1;
    return 1;
}
static int address(const char *host, int port, struct sockaddr_in *addr) {
    memset(addr, 0, sizeof *addr);
    addr->sin_family = AF_INET; addr->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr->sin_addr) != 1) { errno = EINVAL; return -1; }
    return 0;
}
int tcp_listen(const char *host, int port, int *actual_port) {
    struct sockaddr_in addr;
    if (address(host, port, &addr)) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    if (nonblocking(fd) || bind(fd, (struct sockaddr *)&addr, sizeof addr) || listen(fd, PEERS)) goto error;
    socklen_t len = sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &len)) goto error;
    *actual_port = ntohs(addr.sin_port);
    return fd;
error: {
        int saved = errno; close(fd); errno = saved; return -1;
    }
}
int tcp_connect(const char *host, int port) {
    struct sockaddr_in addr;
    if (address(host, port, &addr)) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (nonblocking(fd)) goto error;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        if (errno != EINPROGRESS) goto error;
        struct pollfd p = {.fd=fd, .events=POLLOUT};
        int rc;
        do { rc = poll(&p, 1, 2000); } while (rc < 0 && errno == EINTR && !interrupted);
        if (rc <= 0 || interrupted) { errno = ETIMEDOUT; goto error; }
        int err = 0; socklen_t len = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len)) goto error;
        if (err) { errno = err; goto error; }
    }
    return fd;
error: {
        int saved = errno; close(fd); errno = saved; return -1;
    }
}
int write_all(int fd, const char *s, size_t len) {
    while (len) {
        ssize_t n = write(fd, s, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        s += n; len -= (size_t)n;
    }
    return 0;
}

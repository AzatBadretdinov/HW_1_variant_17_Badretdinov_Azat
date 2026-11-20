#ifndef NET_H
#define NET_H
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#define LIMIT 32
#define PEERS 128
#define RX_SIZE 1024
#define TX_SIZE 32768
#define LINE_SIZE 512
typedef struct {
    int fd, bad, closing;
    size_t rx_len, tx_start, tx_len;
    char rx[RX_SIZE], tx[TX_SIZE];
} Connection;
extern volatile sig_atomic_t interrupted;
int signals_init(void);
int64_t clock_ms(void);
int parse_number(const char *s, int min, int max, int *n);
int nonblocking(int fd);
void connection_init(Connection *c, int fd);
int net_queue(Connection *c, const char *format, ...);
int net_flush(Connection *c);
int net_read(Connection *c);
int net_line(Connection *c, char line[LINE_SIZE]);
int tcp_listen(const char *host, int port, int *actual_port);
int tcp_connect(const char *host, int port);
int write_all(int fd, const char *s, size_t len);
#endif

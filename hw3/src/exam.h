#ifndef EXAM_H
#define EXAM_H
#define _POSIX_C_SOURCE 200809L
#include <mqueue.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>

enum { LIMIT = 32, ACTORS = 2 * LIMIT, SESSION_MAX = 24 };
enum Mode { UNNAMED, NAMED, SHM, MQ };
enum Kind { HELLO = 1, PREPARE, PREPARED, CHECK, GRADE, RESULT, REJECT, ACK };
enum Control { READY, STOP, CANCEL, CONTROLS };
enum SemSlot { COMMAND, EVENT, CLAIM, EXITED, SEMS };
typedef struct {
    int kind, id, value, aux;
    int64_t delay;
    uint32_t seed;
} Message;
typedef struct { Message command, event; sem_t sem[SEMS]; } Box;
typedef struct { Box box[ACTORS]; sem_t control[CONTROLS]; } Shared;
typedef struct {
    sem_t *sem[SEMS];
    mqd_t command, event;
} Endpoint;
typedef struct {
    enum Mode mode;
    char prefix[96];
    Shared *mem;
    int owns_shm, owner;
    sem_t *control[CONTROLS];
    Endpoint ep[ACTORS];
} Ipc;
typedef struct {
    int students, teachers, tickets, queue, prep_min, prep_max, check_time;
    int max_time, delay_ms, random_tickets, random_grade;
    uint32_t seed;
    const char *log, *session;
} Config;

extern volatile sig_atomic_t interrupted;
int setup_signals(void);
int64_t now_ms(void);
void pause_ms(int ms);
int write_all(int fd, const char *str, size_t n);
int number(const char *str, int min, int max, int *value);
int active(const Config *c, int slot);
int ipc_create(Ipc *ipc, enum Mode mode, const char *session, const Config *c);
int ipc_attach(Ipc *ipc, enum Mode mode, const char *session, int slot);
void ipc_close(Ipc *ipc);
int ipc_send(Ipc *ipc, int slot, int direction, Message m);
int ipc_receive(Ipc *ipc, int slot, int direction, Message *m);
int flag_set(sem_t *sem);
int worker(Ipc *ipc, int slot);
#endif

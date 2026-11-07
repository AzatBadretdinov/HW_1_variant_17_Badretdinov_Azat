#include "exam.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t interrupted;
static void on_signal(int sig) { interrupted = sig; }
int setup_signals(void) {
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return -1;
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}
int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void pause_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (!interrupted && nanosleep(&ts, &ts) && errno == EINTR) {}
}
int write_all(int fd, const char *str, size_t n) {
    while (n) {
        ssize_t written = write(fd, str, n);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        str += written; n -= (size_t)written;
    }
    return 0;
}
int active(const Config *c, int slot) {
    return slot < LIMIT ? slot < c->students : slot - LIMIT < c->teachers;
}
static int init(Ipc *ipc, enum Mode mode, const char *session, int owner) {
    memset(ipc, 0, sizeof *ipc);
    ipc->mode = mode;
    ipc->owner = owner;
    for (int i = 0; i < ACTORS; ++i)
        ipc->ep[i].command = ipc->ep[i].event = (mqd_t)-1;
    size_t n = strlen(session);
    if (!n || n > SESSION_MAX ||
        strspn(session, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != n) {
        errno = EINVAL; return -1;
    }
    snprintf(ipc->prefix, sizeof ipc->prefix, "/prj17_%lu_%d_%s",
             (unsigned long)getuid(), mode, session);
    return 0;
}
static void resource_name(const Ipc *ipc, char name[128], char type, int slot, int key) {
    snprintf(name, 128, "%s_%c%d_%d", ipc->prefix, type, slot, key);
}
static int sem_resource(Ipc *ipc, sem_t **dst, sem_t *storage, int slot, int key, unsigned value) {
    if (ipc->mode == UNNAMED) {
        if (sem_init(storage, 1, value) < 0) return -1;
        *dst = storage;
    } else {
        char name[128];
        resource_name(ipc, name, 's', slot, key);
        sem_t *sem = ipc->owner ? sem_open(name, O_CREAT | O_EXCL, 0600, value)
                                : sem_open(name, 0);
        if (sem == SEM_FAILED) return -1;
        *dst = sem;
    }
    return 0;
}
static int map_shared(Ipc *ipc) {
    char name[128];
    resource_name(ipc, name, 'm', 0, 0);
    int fd = shm_open(name, O_RDWR | (ipc->owner ? O_CREAT | O_EXCL : 0), 0600);
    if (fd < 0) return -1;
    ipc->owns_shm = ipc->owner;
    if (ipc->owner && ftruncate(fd, sizeof(Shared)) < 0) goto error;
    if (!ipc->owner) {
        struct stat st;
        if (fstat(fd, &st) < 0) goto error;
        if (st.st_size != (off_t)sizeof(Shared)) { errno = EINVAL; goto error; }
    }
    void *mapping = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) goto error;
    ipc->mem = mapping;
    close(fd);
    return 0;
error: {
        int err = errno; close(fd); errno = err; return -1;
    }
}
static int open_endpoint(Ipc *ipc, int slot) {
    Endpoint *ep = &ipc->ep[slot];
    for (int key = 0; key < SEMS; ++key) {
        sem_t *storage = ipc->mem ? &ipc->mem->box[slot].sem[key] : NULL;
        if (sem_resource(ipc, &ep->sem[key], storage, slot, key, key == CLAIM) < 0)
            return -1;
    }
    if (ipc->mode == MQ) {
        char name[128];
        struct mq_attr attr = {.mq_maxmsg = 1, .mq_msgsize = sizeof(Message)};
        int flags = O_RDWR | O_NONBLOCK | (ipc->owner ? O_CREAT | O_EXCL : 0);
        resource_name(ipc, name, 'q', slot, COMMAND);
        ep->command = mq_open(name, flags, 0600, &attr);
        if (ep->command == (mqd_t)-1) return -1;
        resource_name(ipc, name, 'q', slot, EVENT);
        ep->event = mq_open(name, flags, 0600, &attr);
        if (ep->event == (mqd_t)-1) return -1;
    }
    return 0;
}
static int controls(Ipc *ipc, int start) {
    for (int key = start; key < CONTROLS; ++key) {
        sem_t *storage = ipc->mem ? &ipc->mem->control[key] : NULL;
        if (sem_resource(ipc, &ipc->control[key], storage, -1, key, 0) < 0) return -1;
    }
    return 0;
}
int ipc_create(Ipc *ipc, enum Mode mode, const char *session, const Config *c) {
    if (init(ipc, mode, session, 1) < 0) return -1;
    if (mode != MQ && map_shared(ipc) < 0) return -1;
    if (controls(ipc, 0) < 0) return -1;
    for (int slot = 0; slot < ACTORS; ++slot)
        if (active(c, slot) && open_endpoint(ipc, slot) < 0) return -1;
    // READY is posted by the coordinator only after its journal is ready.
    return 0;
}
int flag_set(sem_t *sem) {
    int value;
    return sem_getvalue(sem, &value) == 0 && value > 0;
}
int ipc_attach(Ipc *ipc, enum Mode mode, const char *session, int slot) {
    if (init(ipc, mode, session, 0) < 0) return -1;
    if (sem_resource(ipc, &ipc->control[READY], NULL, -1, READY, 0) < 0) return -1;
    if (!flag_set(ipc->control[READY])) { errno = EAGAIN; return -1; }
    if (controls(ipc, STOP) < 0 || (mode == SHM && map_shared(ipc) < 0)) return -1;
    if (flag_set(ipc->control[STOP])) { errno = ECANCELED; return -1; }
    if (open_endpoint(ipc, slot) < 0) return -1;
    // Reject an attachment racing with a restart under the same session name.
    char name[128];
    resource_name(ipc, name, 's', -1, READY);
    sem_t *current = sem_open(name, 0);
    if (current == SEM_FAILED) return -1;
    int same = current == ipc->control[READY];
    sem_close(current);
    if (!same || flag_set(ipc->control[STOP])) { errno = ECANCELED; return -1; }
    return 0;
}
static void close_sem(Ipc *ipc, sem_t *sem, int slot, int key) {
    if (!sem) return;
    if (ipc->mode == UNNAMED) {
        if (ipc->owner) sem_destroy(sem);
    } else {
        if (ipc->owner) {
            char name[128];
            resource_name(ipc, name, 's', slot, key);
            sem_unlink(name);
        }
        sem_close(sem);
    }
}
static void close_queue(Ipc *ipc, mqd_t q, int slot, int key) {
    if (q == (mqd_t)-1) return;
    if (ipc->owner) {
        char name[128];
        resource_name(ipc, name, 'q', slot, key);
        mq_unlink(name);
    }
    mq_close(q);
}
void ipc_close(Ipc *ipc) {
    for (int slot = 0; slot < ACTORS; ++slot) {
        Endpoint *ep = &ipc->ep[slot];
        close_queue(ipc, ep->command, slot, COMMAND);
        close_queue(ipc, ep->event, slot, EVENT);
        for (int key = 0; key < SEMS; ++key) close_sem(ipc, ep->sem[key], slot, key);
    }
    for (int key = STOP; key < CONTROLS; ++key) close_sem(ipc, ipc->control[key], -1, key);
    if (ipc->mode == UNNAMED) close_sem(ipc, ipc->control[READY], -1, READY);
    if (ipc->mem) munmap(ipc->mem, sizeof(Shared));
    if (ipc->owns_shm) {
        char name[128];
        resource_name(ipc, name, 'm', 0, 0);
        shm_unlink(name);
    }
    if (ipc->mode != UNNAMED) close_sem(ipc, ipc->control[READY], -1, READY);
}
static Message *mailbox(Ipc *ipc, int slot, int dir) {
    Box *box = &ipc->mem->box[slot];
    return dir == COMMAND ? &box->command : &box->event;
}
// Protocol permits one outstanding message in each direction. The semaphore
// publishes the complete message; the receiver consumes its permit first.
int ipc_send(Ipc *ipc, int slot, int direction, Message m) {
    Endpoint *ep = &ipc->ep[slot];
    if (ipc->mode == MQ) {
        mqd_t q = direction == COMMAND ? ep->command : ep->event;
        while (mq_send(q, (const char *)&m, sizeof m, 0) < 0) {
            if (errno != EINTR) return -1;
        }
    } else {
        *mailbox(ipc, slot, direction) = m;
    }
    return sem_post(ep->sem[direction]);
}
int ipc_receive(Ipc *ipc, int slot, int direction, Message *m) {
    Endpoint *ep = &ipc->ep[slot];
    while (sem_trywait(ep->sem[direction]) < 0) {
        if (errno == EAGAIN) return 0;
        if (errno != EINTR) return -1;
    }
    if (ipc->mode == MQ) {
        mqd_t q = direction == COMMAND ? ep->command : ep->event;
        ssize_t n;
        do { n = mq_receive(q, (char *)m, sizeof *m, NULL); } while (n < 0 && errno == EINTR);
        if (n != (ssize_t)sizeof *m) { if (n >= 0) errno = EPROTO; return -1; }
    } else {
        *m = *mailbox(ipc, slot, direction);
    }
    return 1;
}

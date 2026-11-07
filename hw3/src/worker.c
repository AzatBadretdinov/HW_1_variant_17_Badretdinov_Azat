#include "exam.h"
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

static int stopping(const Ipc *ipc) {
    return interrupted || flag_set(ipc->control[STOP]);
}
static int simulate_delay(const Ipc *ipc, int64_t delay) {
    int64_t deadline = now_ms() + delay;
    while (!stopping(ipc)) {
        int64_t left = deadline - now_ms();
        if (left <= 0) return 0;
        pause_ms(left > 5 ? 5 : (int)left);
    }
    return 1;
}
static int wait_command(Ipc *ipc, int slot, Message *command) {
    while (!stopping(ipc)) {
        int rc = ipc_receive(ipc, slot, COMMAND, command);
        if (rc) return rc;
        pause_ms(1);
    }
    return 0;
}
int worker(Ipc *ipc, int slot) {
    Endpoint *ep = &ipc->ep[slot];
    if (stopping(ipc)) return interrupted ? 128 + interrupted : 0;
    int claimed;
    do { claimed = sem_trywait(ep->sem[CLAIM]); } while (claimed < 0 && errno == EINTR);
    if (claimed < 0) {
        fprintf(stderr, "Номер участника уже занят или недоступен.\n");
        return 2;
    }
    int rc = 0;
    if (stopping(ipc)) goto done;
    if (ipc_send(ipc, slot, EVENT, (Message){.kind = HELLO, .value = (int)getpid()}) < 0) goto failed;
    for (;;) {
        Message in;
        int received = wait_command(ipc, slot, &in);
        if (received < 0) goto failed;
        if (!received || stopping(ipc)) break;
        Message reply = {.id = in.id};
        if (slot < LIMIT && in.kind == PREPARE) {
            if (simulate_delay(ipc, in.delay)) break;
            reply.kind = PREPARED;
        } else if (slot < LIMIT && (in.kind == RESULT || in.kind == REJECT)) {
            reply.kind = ACK;
            reply.value = in.value;
        } else if (slot >= LIMIT && in.kind == CHECK) {
            if (simulate_delay(ipc, in.delay)) break;
            reply.kind = GRADE;
            reply.value = 2 + (in.aux ? (int)(in.seed % 4u) : (in.value - 1) % 4);
        } else {
            errno = EPROTO; goto failed;
        }
        if (stopping(ipc)) break;
        if (ipc_send(ipc, slot, EVENT, reply) < 0) goto failed;
        if (reply.kind == ACK) break;
    }
    goto done;
failed:
    perror("Обмен сообщениями");
    rc = 2;
done:
    if (interrupted) rc = 128 + interrupted;
    if (rc && !flag_set(ipc->control[STOP])) sem_post(ipc->control[CANCEL]);
    sem_post(ep->sem[EXITED]);
    return rc;
}

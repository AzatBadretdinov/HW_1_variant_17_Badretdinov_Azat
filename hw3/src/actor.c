#include "exam.h"
#include <stdio.h>
#include <string.h>

#ifndef TEACHER
#error TEACHER is required
#endif
int main(int argc, char **argv) {
    const char *session = NULL;
    enum Mode mode = SHM;
    int id = 0;
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) {
            printf("%s --mode shm|mq --session NAME --id 1..32\n", argv[0]);
            return 0;
        }
        if (++i == argc) goto invalid;
        const char *val = argv[i];
        if (!strcmp(key, "--session")) session = val;
        else if (!strcmp(key, "--id") && number(val, 1, LIMIT, &id)) {}
        else if (!strcmp(key, "--mode") && !strcmp(val, "shm")) mode = SHM;
        else if (!strcmp(key, "--mode") && !strcmp(val, "mq")) mode = MQ;
        else goto invalid;
    }
    if (!session || !id) goto invalid;
    if (setup_signals() < 0) { perror("sigaction"); return 2; }
    Ipc ipc;
    int slot = (TEACHER ? LIMIT : 0) + id - 1;
    if (ipc_attach(&ipc, mode, session, slot) < 0) {
        perror("Подключение к сессии");
        ipc_close(&ipc); return 2;
    }
    int rc = worker(&ipc, slot);
    ipc_close(&ipc);
    return rc;
invalid:
    fprintf(stderr, "Использование: %s --mode shm|mq --session NAME --id 1..32\n", argv[0]);
    return 2;
}

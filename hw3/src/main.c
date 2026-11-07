#include "exam.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MODE
#error MODE is required
#endif

enum State { S_NEW, S_PREPARING, S_READY, S_QUEUED, S_CHECKING, S_GRADED, S_DONE, S_REJECTED };
typedef struct { enum State state; int ticket, prep, grade, teacher, waited; uint32_t seed; } Student;
typedef struct { int data[LIMIT], head, len; } Queue;
typedef struct {
    Config c;
    Ipc ipc;
    int fd, io_error, registered, settled, completed, admitted, peak, waited, sum;
    int64_t start;
    Student student[LIMIT];
    int checking[LIMIT], actor_pid[ACTORS];
    pid_t child[ACTORS];
    Queue ready, queue;
} Exam;

static void event(Exam *e, const char *format, ...) {
    char line[1024];
    int prefix = snprintf(line, sizeof line, "[ms=%" PRId64 "] ", now_ms() - e->start);
    va_list args;
    va_start(args, format);
    int len = vsnprintf(line + prefix, sizeof line - (size_t)prefix, format, args);
    va_end(args);
    if (len < 0 || (size_t)(len + prefix) >= sizeof line) { e->io_error = 1; return; }
    int output = write_all(STDOUT_FILENO, line, (size_t)(len + prefix));
    int log = write_all(e->fd, line, (size_t)(len + prefix));
    if (output || log) e->io_error = 1;
}
static uint32_t random_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}
static void prepare_inputs(Exam *e) {
    int order[10000];
    uint32_t rng = e->c.seed;
    e->admitted = e->c.students < e->c.tickets ? e->c.students : e->c.tickets;
    for (int i = 0; i < e->c.tickets; ++i) order[i] = i + 1;
    if (e->c.random_tickets)
        for (int i = e->c.tickets - 1; i > 0; --i) {
            int j = (int)(random_next(&rng) % (uint32_t)(i + 1));
            int t = order[i]; order[i] = order[j]; order[j] = t;
        }
    for (int i = 0; i < e->c.students; ++i) {
        Student *s = &e->student[i];
        if (i >= e->admitted) { s->state = S_REJECTED; continue; }
        s->ticket = order[i];
        s->prep = e->c.prep_min + (int)(random_next(&rng) %
                  (uint32_t)(e->c.prep_max - e->c.prep_min + 1));
        s->seed = random_next(&rng);
    }
    for (int i = 0; i < LIMIT; ++i) e->checking[i] = -1;
}
static void push(Queue *q, int id) {
    q->data[(q->head + q->len) % LIMIT] = id; ++q->len;
}
static int pop(Queue *q) {
    int id = q->data[q->head];
    q->head = (q->head + 1) % LIMIT; --q->len;
    return id;
}
static int send(Exam *e, int slot, Message m) { return ipc_send(&e->ipc, slot, COMMAND, m); }
static int64_t duration(const Exam *e, int units) {
    return (int64_t)units * (e->c.delay_ms ? e->c.delay_ms : 1);
}
static int accept(Exam *e, int slot, Message m) {
    if (m.kind == HELLO && !e->actor_pid[slot] && m.value > 0) {
        e->actor_pid[slot] = m.value; ++e->registered;
        int student = slot < LIMIT;
        event(e, "ПОДКЛЮЧЕНИЕ role=%s id=%d pid=%d\n",
              student ? "student" : "teacher", (student ? slot : slot - LIMIT) + 1, m.value);
        if (!student) return 0;
        Student *s = &e->student[slot];
        if (!s->ticket) {
            event(e, "Студент %d не допущен: нет уникального билета.\n", slot + 1);
            return send(e, slot, (Message){.kind = REJECT, .id = slot});
        }
        s->state = S_PREPARING;
        event(e, "Студент %d получил и сообщил билет %d.\n", slot + 1, s->ticket);
        event(e, "Студент %d начал подготовку; длительность %d.\n", slot + 1, s->prep);
        return send(e, slot, (Message){.kind = PREPARE, .id = slot,
                    .value = s->ticket, .delay = duration(e, s->prep)});
    }
    if (slot < LIMIT && m.id == slot && e->actor_pid[slot]) {
        Student *s = &e->student[slot];
        if (m.kind == PREPARED && s->state == S_PREPARING) {
            s->state = S_READY; push(&e->ready, slot);
            event(e, "Студент %d закончил подготовку и готов передать работу.\n", slot + 1);
            return 0;
        }
        if (m.kind == ACK && s->state == S_REJECTED && !m.value) {
            ++e->settled;
            return 0;
        }
        if (m.kind == ACK && s->state == S_GRADED && m.value == s->grade) {
            s->state = S_DONE; ++e->settled; ++e->completed; e->sum += s->grade;
            event(e, "Студент %d получил свою оценку %d и завершил экзамен.\n", slot + 1, s->grade);
            return 0;
        }
    }
    if (slot >= LIMIT && m.kind == GRADE && m.id >= 0 &&
        m.id < e->c.students && e->checking[slot - LIMIT] == m.id &&
        m.value >= 2 && m.value <= 5) {
        Student *s = &e->student[m.id];
        s->state = S_GRADED; s->grade = m.value; e->checking[slot - LIMIT] = -1;
        event(e, "Преподаватель %d закончил проверку студента %d; оценка %d.\n",
              slot - LIMIT + 1, m.id + 1, s->grade);
        return send(e, m.id, (Message){.kind = RESULT, .id = m.id, .value = s->grade});
    }
    errno = EPROTO;
    return -1;
}
// Only the coordinator owns the FIFO and states. No participant holds a lock
// that can remain locked if it is interrupted.
static int dispatch(Exam *e) {
    for (;;) {
        for (int t = 0; t < e->c.teachers && e->queue.len; ++t) {
            if (!e->actor_pid[LIMIT + t] || e->checking[t] >= 0) continue;
            int id = pop(&e->queue);
            Student *s = &e->student[id];
            s->state = S_CHECKING; s->teacher = t + 1; e->checking[t] = id;
            event(e, "Преподаватель %d начал проверку работы студента %d.\n", t + 1, id + 1);
            if (send(e, LIMIT + t, (Message){.kind = CHECK, .id = id, .value = s->ticket,
                     .aux = e->c.random_grade, .seed = s->seed,
                     .delay = duration(e, e->c.check_time)}) < 0) return -1;
        }
        if (!e->ready.len || e->queue.len == e->c.queue) break;
        while (e->ready.len && e->queue.len < e->c.queue) {
            int id = pop(&e->ready); push(&e->queue, id);
            e->student[id].state = S_QUEUED;
            if (e->queue.len > e->peak) e->peak = e->queue.len;
            event(e, "Студент %d передал работу; поставлен в очередь (%d/%d).\n",
                  id + 1, e->queue.len, e->c.queue);
        }
    }
    for (int i = 0; i < e->ready.len; ++i) {
        int id = e->ready.data[(e->ready.head + i) % LIMIT];
        if (e->student[id].waited) continue;
        e->student[id].waited = 1; ++e->waited;
        event(e, "Студент %d ожидает места в очереди.\n", id + 1);
    }
    return 0;
}
static int collect_children(Exam *e) {
    int bad = 0;
    for (int i = 0; i < ACTORS; ++i) {
        if (!e->child[i]) continue;
        int status;
        pid_t p = waitpid(e->child[i], &status, WNOHANG);
        if (p > 0) {
            e->child[i] = 0;
            if (!WIFEXITED(status) || WEXITSTATUS(status)) bad = 1;
        } else if (p < 0 && errno != EINTR) {
            e->child[i] = 0; bad = 1;
        }
    }
    return bad;
}
static int spawn(Exam *e) {
    for (int i = 0; i < ACTORS && !interrupted; ++i) {
        if (!active(&e->c, i)) continue;
        pid_t p = fork();
        if (p < 0) return -1;
        if (p == 0) {
            close(e->fd);
            e->ipc.owner = e->ipc.owns_shm = 0;
            int rc = worker(&e->ipc, i);
            ipc_close(&e->ipc);
            _exit(rc);
        }
        e->child[i] = p;
    }
    return 0;
}
static int outstanding(Exam *e) {
    for (int i = 0; i < ACTORS; ++i) {
        if (e->child[i]) return 1;
        if (e->ipc.mode < SHM || !active(&e->c, i)) continue;
        Endpoint *ep = &e->ipc.ep[i];
        if (!flag_set(ep->sem[CLAIM]) && !flag_set(ep->sem[EXITED])) return 1;
    }
    return 0;
}
static void shutdown(Exam *e) {
    sem_post(e->ipc.control[STOP]);
    int64_t until = now_ms() + 600;
    while (now_ms() < until) {
        collect_children(e);
        if (!outstanding(e)) break;
        // Keep waiting on shutdown even when SIGINT has already arrived.
        struct timespec ts = {.tv_nsec = 2000000};
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < ACTORS; ++i) {
        pid_t p = e->child[i];
        if (!p) continue;
        kill(p, SIGKILL);
        while (waitpid(p, NULL, 0) < 0 && errno == EINTR) {}
    }
}
static const char *loop(Exam *e) {
    for (;;) {
        if (interrupted) return "interrupted";
        if (collect_children(e) || flag_set(e->ipc.control[CANCEL])) return "participant_error";
        if (e->io_error) return "io_error";
        int changed = 0;
        for (int i = 0; i < ACTORS && !interrupted; ++i) {
            if (!active(&e->c, i)) continue;
            Message m;
            int received = ipc_receive(&e->ipc, i, EVENT, &m);
            if (received < 0 || (received && accept(e, i, m) < 0)) {
                perror("Обмен сообщениями"); return "ipc_error";
            }
            changed |= received;
        }
        if (interrupted) return "interrupted";
        if (e->registered == e->c.students + e->c.teachers && e->settled == e->c.students)
            return "complete";
        if (e->c.max_time && now_ms() - e->start >= duration(e, e->c.max_time)) return "timeout";
        if (dispatch(e) < 0) { perror("Отправка работы"); return "ipc_error"; }
        if (!changed) pause_ms(1);
    }
}
static int simulate(Config c) {
    static const char *modes[] = {"unnamed", "named", "shm", "mq"};
    static const char *states[] = {"not_started", "preparing", "ready", "queued",
                                  "checking", "graded", "done", "not_admitted"};
    Exam e = {.c = c, .fd = -1};
    char automatic[32];
    snprintf(automatic, sizeof automatic, "%ld_%" PRId64, (long)getpid(), now_ms());
    const char *session = c.session ? c.session : automatic;
    if (setup_signals() < 0) { perror("sigaction"); return 2; }
    if (ipc_create(&e.ipc, MODE, session, &c) < 0) {
        perror("Создание IPC"); ipc_close(&e.ipc); return 2;
    }
    e.fd = open(c.log, O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
    if (e.fd < 0) { perror("Журнал"); ipc_close(&e.ipc); return 2; }
    prepare_inputs(&e);
    e.start = now_ms();
    event(&e, "НАЧАЛО mode=%s session=%s teachers=%d students=%d tickets=%d queue=%d unit_ms=%d seed=%u pid=%ld\n",
          modes[MODE], session, c.teachers, c.students, c.tickets, c.queue,
          c.delay_ms ? c.delay_ms : 1, c.seed, (long)getpid());
    sem_post(e.ipc.control[READY]);
    // The marker is emitted after publishing all named objects for attachment.
    event(&e, "ГОТОВО mode=%s session=%s\n", modes[MODE], session);
    const char *reason;
    if (MODE < SHM && spawn(&e) < 0) {
        perror("fork"); reason = "startup_error";
    } else reason = loop(&e);
    shutdown(&e);
    for (int i = 0; i < c.students; ++i) {
        Student *s = &e.student[i];
        event(&e, "СТУДЕНТ id=%d ticket=%d state=%s teacher=%d grade=%d\n",
              i + 1, s->ticket, states[s->state], s->teacher, s->grade);
    }
    event(&e, "ИТОГ reason=%s admitted=%d completed=%d unfinished=%d rejected=%d peak_queue=%d waited=%d average=%.2f\n",
          reason, e.admitted, e.completed, e.admitted - e.completed, c.students - e.admitted,
          e.peak, e.waited, e.completed ? (double)e.sum / e.completed : 0.0);
    ipc_close(&e.ipc);
    int close_error = close(e.fd);
    if (e.io_error || close_error) return 2;
    if (interrupted) return 128 + interrupted;
    return (strcmp(reason, "complete") && strcmp(reason, "timeout")) ? 2 : 0;
}
static void help(const char *name) {
    printf("Экзамен, вариант 17. %s [параметры]\n"
           "  --students 0..32 (6), --teachers 1..32 (2), --tickets 0..10000 (6)\n"
           "  --queue 1..32 (3), --prep-min 1..100000 (2), --prep-max 1..100000 (5)\n"
           "  --check-time 1..100000 (2), --max-time 0..100000 (100; 0 без лимита)\n"
           "  --delay-ms 0..60000 (10; 0 означает 1 мс), --seed 1..2147483647 (17)\n"
           "  --ticket-mode sequential|random, --grade-mode ticket|random (random)\n"
           "  --log PATH (exam.log), --session NAME (для exam-shm и exam-mq)\n", name);
}
int main(int argc, char **argv) {
    Config c = {.students=6, .teachers=2, .tickets=6, .queue=3,
                .prep_min=2, .prep_max=5, .check_time=2, .max_time=100, .delay_ms=10,
                .random_tickets=1, .random_grade=1, .seed=17, .log="exam.log"};
    int seed = 17;
    struct Option { const char *key; int *dst, low, high; };
    struct Option options[] = {
        {"--students",&c.students,0,LIMIT}, {"--teachers",&c.teachers,1,LIMIT},
        {"--tickets",&c.tickets,0,10000}, {"--queue",&c.queue,1,LIMIT},
        {"--prep-min",&c.prep_min,1,100000}, {"--prep-max",&c.prep_max,1,100000},
        {"--check-time",&c.check_time,1,100000}, {"--max-time",&c.max_time,0,100000},
        {"--delay-ms",&c.delay_ms,0,60000}, {"--seed",&seed,1,2147483647}
    };
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) { help(argv[0]); return 0; }
        if (++i == argc) { fprintf(stderr, "Нет значения для %s\n", key); return 2; }
        const char *value = argv[i];
        if (!strcmp(key, "--log") && *value) { c.log = value; continue; }
        if (!strcmp(key, "--session") && MODE >= SHM && *value) { c.session = value; continue; }
        if (!strcmp(key, "--ticket-mode") && (!strcmp(value,"sequential") || !strcmp(value,"random"))) {
            c.random_tickets = !strcmp(value,"random"); continue;
        }
        if (!strcmp(key, "--grade-mode") && (!strcmp(value,"ticket") || !strcmp(value,"random"))) {
            c.random_grade = !strcmp(value,"random"); continue;
        }
        int valid = 0;
        for (size_t j = 0; j < sizeof options / sizeof options[0]; ++j)
            if (!strcmp(key, options[j].key))
                valid = number(value, options[j].low, options[j].high, options[j].dst);
        if (!valid) { fprintf(stderr, "Некорректный параметр %s: %s\n", key, value); return 2; }
    }
    if (MODE >= SHM && !c.session) { fprintf(stderr, "Укажите --session NAME.\n"); return 2; }
    if (c.prep_min > c.prep_max) { fprintf(stderr, "prep-min > prep-max\n"); return 2; }
    c.seed = (uint32_t)seed;
    return simulate(c);
}

#define _POSIX_C_SOURCE 200809L
#include "exam.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Каждый студент владеет ровно одной работой, помещаемой в очередь по индексу. */
typedef enum { NOT_ADMITTED, PREPARING, READY, QUEUED, CHECKING, DONE } State;
typedef struct { int ticket, grade, teacher; } Work;
typedef struct { State state; int ready_at; Work work; } Student;
typedef struct { int student; long long finish_at; } Teacher;
typedef struct { int *items, head, size, capacity; } Queue;
typedef struct {
    const Config *config;
    Student *students;
    Teacher *teachers;
    Queue queue;
    uint32_t rng;
    long long now;
    int log_fd, io_error, admitted, completed, peak, waits, grade_sum;
} Exam;
static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { interrupted = sig; }

/* Собственный генератор случайных чисел даёт одинаковую последовательность на разных libc. */
static uint32_t next_random(Exam *e) {
    uint32_t x = e->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return e->rng = x;
}
static int random_range(Exam *e, int low, int high) {
    return low + (int)(next_random(e) % (uint32_t)(high - low + 1));
}
static int write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t k = write(fd, s, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        s += k; n -= (size_t)k;
    }
    return 0;
}
/* Единое форматирование для терминала и файла; write обрабатывает короткую запись. */
static void event(Exam *e, const char *format, ...) {
    char buffer[1024];
    int prefix = snprintf(buffer, sizeof buffer, "[t=%lld] ", e->now);
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buffer + prefix, sizeof buffer - (size_t)prefix, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof buffer - (size_t)prefix) { e->io_error = 1; return; }
    size_t size = (size_t)(prefix + n);
    int terminal = write_all(STDOUT_FILENO, buffer, size);
    int log = write_all(e->log_fd, buffer, size);
    if (terminal || log) e->io_error = 1;
}
static void push(Queue *q, int value) {
    q->items[(q->head + q->size) % q->capacity] = value;
    ++q->size;
}
static int pop(Queue *q) {
    int result = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    --q->size;
    return result;
}
static void start_check(Exam *e, int teacher, int student) {
    Student *s = &e->students[student];
    s->state = CHECKING;
    s->work.teacher = teacher + 1;
    e->teachers[teacher].student = student;
    e->teachers[teacher].finish_at = e->now + e->config->check_time;
    event(e, "Преподаватель %d начал проверку работы студента %d.\n", teacher + 1, student + 1);
}
static void finish_checks(Exam *e) {
    for (int i = 0; i < e->config->teachers; ++i) {
        Teacher *t = &e->teachers[i];
        if (t->student < 0 || t->finish_at > e->now) continue;
        int id = t->student;
        Student *s = &e->students[id];
        s->work.grade = e->config->random_grade ? random_range(e, 2, 5) : 2 + (s->work.ticket - 1) % 4;
        s->state = DONE;
        ++e->completed;
        e->grade_sum += s->work.grade;
        event(e, "Преподаватель %d закончил проверку студента %d; оценка %d.\n", i + 1, id + 1, s->work.grade);
        event(e, "Студент %d получил свою оценку %d и завершил экзамен.\n", id + 1, s->work.grade);
        t->student = -1;
    }
}
static void assign_waiting(Exam *e) {
    for (int i = 0; i < e->config->teachers && e->queue.size; ++i)
        if (e->teachers[i].student < 0) start_check(e, i, pop(&e->queue));
}
static int oldest_ready(const Exam *e) {
    int best = -1;
    for (int i = 0; i < e->admitted; ++i) {
        if (e->students[i].state != READY) continue;
        if (best < 0 || e->students[i].ready_at < e->students[best].ready_at) best = i;
    }
    return best; /* При одинаковом времени меньший номер студента идёт первым. */
}
static void accept_ready(Exam *e) {
    while (e->queue.size < e->queue.capacity) {
        int id = oldest_ready(e);
        if (id < 0) break;
        push(&e->queue, id);
        e->students[id].state = QUEUED;
        if (e->queue.size > e->peak) e->peak = e->queue.size;
        event(e, "Студент %d передал работу; поставлен в очередь (%d/%d).\n", id + 1, e->queue.size, e->queue.capacity);
        assign_waiting(e);
    }
}
static void pause_tick(int ms) {
    struct timespec delay = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (!interrupted && nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}
int simulate(const Config *c) {
    Exam e = {.config = c, .rng = c->seed, .log_fd = -1};
    interrupted = 0;
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) { perror("sigaction"); return 2; }
    /* Закрытый stdout должен давать ошибку записи, а не аварийное завершение. */
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL)) { perror("sigaction"); return 2; }
    e.log_fd = open(c->log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (e.log_fd < 0) { perror("Не удалось открыть журнал"); return 2; }
    e.students = calloc((size_t)c->students + 1, sizeof *e.students);
    e.teachers = calloc((size_t)c->teachers, sizeof *e.teachers);
    e.queue = (Queue){.items = calloc((size_t)c->queue_capacity, sizeof(int)), .capacity = c->queue_capacity};
    int *tickets = calloc((size_t)c->tickets + 1, sizeof(int));
    int result = 0;
    if (!e.students || !e.teachers || !e.queue.items || !tickets) {
        dprintf(2, "Недостаточно памяти\n"); result = 2; goto cleanup;
    }
    e.admitted = c->students < c->tickets ? c->students : c->tickets;
    for (int i = 0; i < c->teachers; ++i) e.teachers[i].student = -1;
    for (int i = 0; i < c->tickets; ++i) tickets[i] = i + 1;
    if (c->random_tickets)
        for (int i = c->tickets - 1; i > 0; --i) {
            int j = random_range(&e, 0, i), temp = tickets[i];
            tickets[i] = tickets[j]; tickets[j] = temp;
        }
    event(&e, "НАЧАЛО teachers=%d students=%d tickets=%d queue=%d prep=%d..%d check=%d max=%d seed=%u ticket_mode=%s grade_mode=%s delay_ms=%d\n",
          c->teachers, c->students, c->tickets, c->queue_capacity, c->prep_min, c->prep_max,
          c->check_time, c->max_time, c->seed, c->random_tickets ? "random" : "sequential",
          c->random_grade ? "random" : "ticket", c->delay_ms);
    for (int i = 0; i < c->students; ++i) {
        Student *s = &e.students[i];
        if (i >= e.admitted) { event(&e, "Студент %d не допущен: нет уникального билета.\n", i + 1); continue; }
        s->state = PREPARING;
        s->work.ticket = tickets[i];
        s->ready_at = random_range(&e, c->prep_min, c->prep_max);
        event(&e, "Студент %d получил и сообщил билет %d.\n", i + 1, s->work.ticket);
        event(&e, "Студент %d начал подготовку; длительность %d.\n", i + 1, s->ready_at);
    }
    const char *reason = "complete";
    for (;;) {
        if (interrupted) { reason = "interrupted"; break; }
        if (e.io_error) { reason = "io_error"; break; }
        finish_checks(&e);
        for (int i = 0; i < e.admitted; ++i) {
            Student *s = &e.students[i];
            if (s->state == PREPARING && s->ready_at <= e.now) {
                s->state = READY;
                event(&e, "Студент %d закончил подготовку и готов передать работу.\n", i + 1);
            }
        }
        if (e.completed == e.admitted) break;
        /* На границе времени результаты выдаются, но новые проверки не начинаются. */
        if (c->max_time && e.now >= c->max_time) { reason = "timeout"; break; }
        assign_waiting(&e);
        accept_ready(&e);
        for (int i = 0; i < e.admitted; ++i)
            if (e.students[i].state == READY && e.students[i].ready_at == e.now) {
                ++e.waits;
                event(&e, "Студент %d ожидает места: очередь заполнена, работа остаётся у студента.\n", i + 1);
            }
        if (e.io_error) { reason = "io_error"; break; }
        if (c->delay_ms) pause_tick(c->delay_ms);
        if (!interrupted) ++e.now;
    }
    static const char *names[] = {"not_admitted", "preparing", "ready", "queued", "checking", "done"};
    for (int i = 0; i < c->students; ++i) {
        Student *s = &e.students[i];
        event(&e, "СТУДЕНТ id=%d ticket=%d state=%s teacher=%d grade=%d\n", i + 1,
              s->work.ticket, names[s->state], s->work.teacher, s->work.grade);
    }
    event(&e, "ИТОГ reason=%s admitted=%d completed=%d unfinished=%d rejected=%d peak_queue=%d waited=%d average=%.2f\n",
          reason, e.admitted, e.completed, e.admitted - e.completed, c->students - e.admitted,
          e.peak, e.waits, e.completed ? (double)e.grade_sum / e.completed : 0.0);
    if (e.io_error) { dprintf(2, "Ошибка записи вывода или журнала\n"); result = 2; }
    else if (interrupted) result = 128 + interrupted;
cleanup:
    free(tickets); free(e.queue.items); free(e.teachers); free(e.students);
    if (close(e.log_fd) < 0) { perror("close"); result = 2; }
    return result;
}

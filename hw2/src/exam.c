#define _POSIX_C_SOURCE 200809L
#include "exam.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum { NOT_ADMITTED, PREPARING, READY, QUEUED, CHECKING, DONE } State;
typedef struct Exam Exam;
typedef struct {
    Exam *exam;
    int id, ticket, preparation, planned_grade, teacher, grade;
    State state;
    sem_t result;
    pthread_t thread;
} Student;

typedef struct { Exam *exam; int id; pthread_t thread; } Teacher;
struct Exam {
    const Config *c;
    Student *students;
    Teacher *teachers;
    int *queue, head, size, admitted, completed, peak, waited, grade_sum;
    int stop, io_error, log_fd;
    pthread_mutex_t mutex;
    sem_t gate, slots, items;
    struct timespec start;
};

/* Only the main thread reads this signal flag; workers use mutex-protected stop.  */
static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { interrupted = sig; }
static long long milliseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static long long start_ms(const Exam *e) {
    return (long long)e->start.tv_sec * 1000 + e->start.tv_nsec / 1000000;
}

static void pause_ms(int ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}

static int write_all(int fd, const char *text, size_t n) {
    while (n) {
        ssize_t k = write(fd, text, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        text += k; n -= (size_t)k;
    }
    return 0;
}
/*  Called with the mutex held, or after all workers have joined. */
static void event(Exam *e, const char *format, ...) {
    char b[1024];
    int p = snprintf(b, sizeof b, "[ms=%lld] ", milliseconds() - start_ms(e));
    va_list args;
    va_start(args, format);
    int n = vsnprintf(b + p, sizeof b - (size_t)p, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof b - (size_t)p) { e->io_error = 1; return; }
    int a = write_all(STDOUT_FILENO, b, (size_t)(p + n));
    int c = write_all(e->log_fd, b, (size_t)(p + n));
    if (a || c) e->io_error = 1;
}
static int stopped(Exam *e) {
    pthread_mutex_lock(&e->mutex);
    int stop = e->stop;
    pthread_mutex_unlock(&e->mutex);
    return stop;
}
static int wait_sem(sem_t *s) {
    while (sem_wait(s) < 0) if (errno != EINTR) return -1;
    return 0;
}
/* Long  preparation/checking sleeps react to shutdown within approximately 10 ms. */
static int delay(Exam *e, int units) {
    long long end = milliseconds() + (long long)units * (e->c->delay_ms ? e->c->delay_ms : 1);
    while (!stopped(e)) {
        long long left = end - milliseconds();
        if (left <= 0) return 1;
        pause_ms(left > 10 ? 10 : (int)left);
    }
    return 0;
}

static void *student_main(void *arg) {
    Student *s = arg;
    Exam *e = s->exam;
    if (wait_sem(&e->gate) || stopped(e)) return NULL;
    pthread_mutex_lock(&e->mutex);
    if (!s->ticket) {
        event(e, "Студент %d не допущен: нет уникального билета.\n", s->id + 1);
        pthread_mutex_unlock(&e->mutex);
        return NULL;
    }
    s->state = PREPARING;
    event(e, "Студент %d получил и сообщил билет %d.\n", s->id + 1, s->ticket);
    event(e, "Студент %d начал подготовку; длительность %d.\n", s->id + 1, s->preparation);
    pthread_mutex_unlock(&e->mutex);
    if (!delay(e, s->preparation)) return NULL;
    pthread_mutex_lock(&e->mutex);
    if (e->stop) { pthread_mutex_unlock(&e->mutex); return NULL; }
    s->state = READY;
    event(e, "Студент %d закончил подготовку и готов передать работу.\n", s->id + 1);
    pthread_mutex_unlock(&e->mutex);

    /* A slot is reserved before touching the queue. A full queue keeps work with its owner. */
    if (sem_trywait(&e->slots) < 0) {
        pthread_mutex_lock(&e->mutex);
        ++e->waited;
        event(e, "Студент %d ожидает места в очереди.\n", s->id + 1);
        pthread_mutex_unlock(&e->mutex);
        if (wait_sem(&e->slots)) return NULL;
    }
    pthread_mutex_lock(&e->mutex);
    if (e->stop) { pthread_mutex_unlock(&e->mutex); return NULL; }
    e->queue[(e->head + e->size) % e->c->queue_capacity] = s->id;
    ++e->size;
    if (e->size > e->peak) e->peak = e->size;
    s->state = QUEUED;
    event(e, "Студент %d передал работу; поставлен в очередь (%d/%d).\n", s->id + 1, e->size, e->c->queue_capacity);
    sem_post(&e->items);
    pthread_mutex_unlock(&e->mutex);
    if (wait_sem(&s->result)) return NULL;
    pthread_mutex_lock(&e->mutex);
    if (s->state == DONE)
        event(e, "Студент %d получил свою оценку %d и завершил экзамен.\n", s->id + 1, s->grade);
    pthread_mutex_unlock(&e->mutex);
    return NULL;
}
static void *teacher_main(void *arg) {
    Teacher *t = arg;
    Exam *e = t->exam;
    if (wait_sem(&e->gate)) return NULL;
    for (;;) {
        if (wait_sem(&e->items)) return NULL;
        pthread_mutex_lock(&e->mutex);
        if (e->stop) { pthread_mutex_unlock(&e->mutex); return NULL; }
        int id = e->queue[e->head];
        e->head = (e->head + 1) % e->c->queue_capacity;
        --e->size;
        Student *s = &e->students[id];
        s->state = CHECKING;
        s->teacher = t->id + 1;
        event(e, "Преподаватель %d начал проверку работы студента %d.\n", t->id + 1, id + 1);
        sem_post(&e->slots);
        pthread_mutex_unlock(&e->mutex);
        if (!delay(e, e->c->check_time)) return NULL;
        pthread_mutex_lock(&e->mutex);
        if (e->stop) { pthread_mutex_unlock(&e->mutex); return NULL; }
        s->grade = s->planned_grade;
        s->state = DONE;
        ++e->completed;
        e->grade_sum += s->grade;
        event(e, "Преподаватель %d закончил проверку студента %d; оценка %d.\n", t->id + 1, id + 1, s->grade);
        sem_post(&s->result);
        pthread_mutex_unlock(&e->mutex);
    }
}
static uint32_t random_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}
int simulate(const Config *c) {
    Exam e = {.c = c, .log_fd = -1};
    int result = 2, ns = 0, nt = 0, initialized = 0, common = 0;
    const char *reason = "startup_error";
    interrupted = 0;
    if (signal(SIGINT, on_signal) == SIG_ERR || signal(SIGTERM, on_signal) == SIG_ERR) return 2;
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    e.log_fd = open(c->log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (e.log_fd < 0) { perror("log"); return 2; }
    e.students = calloc((size_t)c->students + 1, sizeof *e.students);
    e.teachers = calloc((size_t)c->teachers, sizeof *e.teachers);
    e.queue = calloc((size_t)c->queue_capacity, sizeof *e.queue);
    int *tickets = calloc((size_t)c->tickets + 1, sizeof *tickets);
    if (!e.students || !e.teachers || !e.queue || !tickets) goto cleanup;
    if (pthread_mutex_init(&e.mutex, NULL)) goto cleanup;
    common = 1;
    if (sem_init(&e.gate, 0, 0)) goto cleanup;
    common = 2;
    if (sem_init(&e.slots, 0, (unsigned)c->queue_capacity)) goto cleanup;
    common = 3;
    if (sem_init(&e.items, 0, 0)) goto cleanup;
    common = 4;
    e.admitted = c->students < c->tickets ? c->students : c->tickets;
    uint32_t rng = c->seed;
    for (int i = 0; i < c->tickets; ++i) tickets[i] = i + 1;
    if (c->random_tickets) for (int i = c->tickets - 1; i > 0; --i) {
        int j = (int)(random_next(&rng) % (uint32_t)(i + 1));
        int temp = tickets[i]; tickets[i] = tickets[j]; tickets[j] = temp;
    }
    /* Random inputs are generated before threads start: seed fixes inputs, not scheduling. */
    for (int i = 0; i < c->students; ++i) {
        Student *s = &e.students[i];
        s->exam = &e; s->id = i;
        if (i < e.admitted) {
            s->state = PREPARING;
            s->ticket = tickets[i];
            s->preparation = c->prep_min + (int)(random_next(&rng) % (uint32_t)(c->prep_max - c->prep_min + 1));
            s->planned_grade = c->random_grade ? 2 + (int)(random_next(&rng) % 4) : 2 + (s->ticket - 1) % 4;
        }
        if (sem_init(&s->result, 0, 0)) goto cleanup;
        ++initialized;
    }
    clock_gettime(CLOCK_MONOTONIC, &e.start);
    event(&e, "НАЧАЛО teachers=%d students=%d tickets=%d queue=%d unit_ms=%d seed=%u\n", c->teachers, c->students, c->tickets, c->queue_capacity, c->delay_ms ? c->delay_ms : 1, c->seed);
    for (; nt < c->teachers; ++nt) {
        e.teachers[nt].exam = &e; e.teachers[nt].id = nt;
        int rc = pthread_create(&e.teachers[nt].thread, NULL, teacher_main, &e.teachers[nt]);
        if (rc) { fprintf(stderr, "pthread_create: %s\n", strerror(rc)); goto shutdown; }
    }
    for (; ns < c->students; ++ns) {
        int rc = pthread_create(&e.students[ns].thread, NULL, student_main, &e.students[ns]);
        if (rc) { fprintf(stderr, "pthread_create: %s\n", strerror(rc)); goto shutdown; }
    }
    clock_gettime(CLOCK_MONOTONIC, &e.start);
    for (int i = 0; i < ns + nt; ++i) sem_post(&e.gate);
    result = 0;
    for (;;) {
        pthread_mutex_lock(&e.mutex);
        if (e.io_error) reason = "io_error";
        else if (interrupted) reason = "interrupted";
        else if (e.completed == e.admitted) reason = "complete";
        else if (c->max_time && milliseconds() - start_ms(&e) >= (long long)c->max_time * (c->delay_ms ? c->delay_ms : 1)) reason = "timeout";
        else { pthread_mutex_unlock(&e.mutex); pause_ms(1); continue; }
        e.stop = 1;
        pthread_mutex_unlock(&e.mutex);
        break;
    }
shutdown:
    pthread_mutex_lock(&e.mutex);
    e.stop = 1;
    pthread_mutex_unlock(&e.mutex);
    /* Wake every possible wait; no cancellation or destroying live synchronization objects. */
    for (int i = 0; i < ns + nt; ++i) sem_post(&e.gate);
    for (int i = 0; i < nt; ++i) sem_post(&e.items);
    for (int i = 0; i < ns; ++i) { sem_post(&e.slots); sem_post(&e.students[i].result); }
    for (int i = 0; i < ns; ++i) pthread_join(e.students[i].thread, NULL);
    for (int i = 0; i < nt; ++i) pthread_join(e.teachers[i].thread, NULL);
    {
        static const char *names[] = {"not_admitted", "preparing", "ready", "queued", "checking", "done"};
        for (int i = 0; i < c->students; ++i) {
            Student *s = &e.students[i];
            event(&e, "СТУДЕНТ id=%d ticket=%d state=%s teacher=%d grade=%d\n", i + 1, s->ticket, names[s->state], s->teacher, s->grade);
        }
        event(&e, "ИТОГ reason=%s admitted=%d completed=%d unfinished=%d rejected=%d peak_queue=%d waited=%d average=%.2f\n", reason, e.admitted, e.completed, e.admitted - e.completed, c->students - e.admitted, e.peak, e.waited, e.completed ? (double)e.grade_sum / e.completed : 0.0);
    }
    if (interrupted) result = 128 + interrupted;
    if (e.io_error) result = 2;
cleanup:
    for (int i = 0; i < initialized; ++i) sem_destroy(&e.students[i].result);
    if (common >= 4) sem_destroy(&e.items);
    if (common >= 3) sem_destroy(&e.slots);
    if (common >= 2) sem_destroy(&e.gate);
    if (common >= 1) pthread_mutex_destroy(&e.mutex);
    free(tickets); free(e.queue); free(e.students); free(e.teachers);
    if (close(e.log_fd)) result = 2;
    return result;
}


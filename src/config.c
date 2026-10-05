#define _POSIX_C_SOURCE 200809L
#include "exam.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void help(void) {
    dprintf(STDOUT_FILENO,
        "Экзамен, вариант 17. Использование: ./exam [параметры]\n"
        "--teachers N      преподаватели, 1..1000 (2)\n"
        "--students N      студенты, 0..10000 (6)\n"
        "--tickets N       уникальные билеты, 0..10000 (6)\n"
        "--ticket-mode M   sequential или random (random)\n"
        "--prep-min N      минимум подготовки, 1..100000 (2)\n"
        "--prep-max N      максимум подготовки, 1..100000 (5)\n"
        "--check-time N    длительность проверки, 1..100000 (2)\n"
        "--grade-mode M    random или ticket (random)\n"
        "--queue N         места в очереди, 1..10000 (3)\n"
        "--max-time N      предел времени, 0 = без предела (100)\n"
        "--seed N          начальное значение ГСЧ, 1..2147483647 (17)\n"
        "--delay-ms N      задержка такта, 0..60000 (0)\n"
        "--log PATH        файл журнала (exam.log)\n"
        "--help            справка\n");
}
static int number(const char *text, int low, int high, int *out) {
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || *text == '\0' || *end || value < low || value > high) return 0;
    *out = (int)value;
    return 1;
}
int parse_config(int argc, char **argv, Config *c) {
    *c = (Config){2, 6, 6, 2, 5, 2, 3, 100, 0, 1, 1, 17, "exam.log"};
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) { help(); return 1; }
        if (++i == argc) { dprintf(2, "Нет значения для %s\n", key); return -1; }
        const char *v = argv[i];
        int *target = NULL, low = 0, high = 100000;
        if (!strcmp(key, "--teachers")) { target = &c->teachers; low = 1; high = 1000; }
        else if (!strcmp(key, "--students")) { target = &c->students; high = 10000; }
        else if (!strcmp(key, "--tickets")) { target = &c->tickets; high = 10000; }
        else if (!strcmp(key, "--prep-min")) { target = &c->prep_min; low = 1; }
        else if (!strcmp(key, "--prep-max")) { target = &c->prep_max; low = 1; }
        else if (!strcmp(key, "--check-time")) { target = &c->check_time; low = 1; }
        else if (!strcmp(key, "--queue")) { target = &c->queue_capacity; low = 1; high = 10000; }
        else if (!strcmp(key, "--max-time")) target = &c->max_time;
        else if (!strcmp(key, "--delay-ms")) { target = &c->delay_ms; high = 60000; }
        else if (!strcmp(key, "--seed")) {
            int seed;
            if (!number(v, 1, 2147483647, &seed)) goto invalid;
            c->seed = (uint32_t)seed;
            continue;
        } else if (!strcmp(key, "--log")) {
            if (!*v) goto invalid;
            c->log_path = v; continue;
        } else if (!strcmp(key, "--ticket-mode")) {
            if (strcmp(v, "random") && strcmp(v, "sequential")) goto invalid;
            c->random_tickets = !strcmp(v, "random"); continue;
        } else if (!strcmp(key, "--grade-mode")) {
            if (strcmp(v, "random") && strcmp(v, "ticket")) goto invalid;
            c->random_grade = !strcmp(v, "random"); continue;
        } else { dprintf(2, "Неизвестный параметр: %s\n", key); return -1; }
        if (!number(v, low, high, target)) goto invalid;
        continue;
invalid:
        dprintf(2, "Некорректное значение %s: %s\n", key, v); return -1;
    }
    if (c->prep_min > c->prep_max) {
        dprintf(2, "prep-min не должен превышать prep-max\n"); return -1;
    }
    return 0;
}

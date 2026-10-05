#ifndef EXAM_H
#define EXAM_H
#include <stdint.h>
typedef struct {
    int teachers, students, tickets, prep_min, prep_max, check_time;
    int queue_capacity, max_time, delay_ms, random_tickets, random_grade;
    uint32_t seed;
    const char *log_path;
} Config;
int parse_config(int argc, char **argv, Config *config);
int simulate(const Config *config);
#endif

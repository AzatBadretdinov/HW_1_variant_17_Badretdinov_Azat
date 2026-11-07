#include "exam.h"
#include <errno.h>
#include <stdlib.h>

int number(const char *str, int min, int max, int *value) {
    char *end;
    errno = 0;
    long n = strtol(str, &end, 10);
    if (errno || end == str || *end || n < min || n > max) return 0;
    *value = (int)n;
    return 1;
}

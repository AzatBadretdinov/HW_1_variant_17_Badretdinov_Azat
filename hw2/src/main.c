#include "exam.h"
int main(int argc, char **argv) {
    Config config;
    int result = parse_config(argc, argv, &config);
    if (result != 0) return result == 1 ? 0 : 2;
    return simulate(&config);
}

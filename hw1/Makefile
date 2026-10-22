CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
SRC = src/main.c src/config.c src/exam.c

all: exam
exam: $(SRC) src/exam.h
	$(CC) $(CFLAGS) $(SRC) -o $@
test: exam
	python3 tests/test_exam.py
sanitize:
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined $(SRC) -o exam
	python3 tests/test_exam.py
clean:
	rm -f exam exam.log
.PHONY: all test sanitize clean

CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS = -pthread -lrt
COMMON = src/ipc.c src/worker.c src/config.c
COORD = src/main.c $(COMMON)
ROLE = src/actor.c $(COMMON)
BINS = exam-unnamed exam-named exam-shm exam-mq exam-student exam-teacher

all: $(BINS)
exam-unnamed: $(COORD) src/exam.h
	$(CC) $(CFLAGS) -DMODE=UNNAMED $(COORD) -o $@ $(LDLIBS)
exam-named: $(COORD) src/exam.h
	$(CC) $(CFLAGS) -DMODE=NAMED $(COORD) -o $@ $(LDLIBS)
exam-shm: $(COORD) src/exam.h
	$(CC) $(CFLAGS) -DMODE=SHM $(COORD) -o $@ $(LDLIBS)
exam-mq: $(COORD) src/exam.h
	$(CC) $(CFLAGS) -DMODE=MQ $(COORD) -o $@ $(LDLIBS)
exam-student: $(ROLE) src/exam.h
	$(CC) $(CFLAGS) -DTEACHER=0 $(ROLE) -o $@ $(LDLIBS)
exam-teacher: $(ROLE) src/exam.h
	$(CC) $(CFLAGS) -DTEACHER=1 $(ROLE) -o $@ $(LDLIBS)
test: all
	python3 tests/test_exam.py
clean:
	rm -f $(BINS) exam.log
.PHONY: all test clean

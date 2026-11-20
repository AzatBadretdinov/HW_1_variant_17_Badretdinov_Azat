CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
BINS = exam-server exam-student exam-teacher exam-monitor
all: $(BINS)
exam-server: src/server.c src/net.c src/net.h
	$(CC) $(CFLAGS) src/server.c src/net.c -o $@
exam-student: src/client.c src/net.c src/net.h
	$(CC) $(CFLAGS) -DROLE=1 src/client.c src/net.c -o $@
exam-teacher: src/client.c src/net.c src/net.h
	$(CC) $(CFLAGS) -DROLE=2 src/client.c src/net.c -o $@
exam-monitor: src/client.c src/net.c src/net.h
	$(CC) $(CFLAGS) -DROLE=3 src/client.c src/net.c -o $@
test-net: tests/test_net.c src/net.c src/net.h
	$(CC) $(CFLAGS) -Isrc tests/test_net.c src/net.c -o $@
test: all test-net
	./test-net > results/net-summary.txt
	cat results/net-summary.txt
	python3 tests/test_exam.py
clean:
	rm -f $(BINS) test-net exam.log
.PHONY: all test clean

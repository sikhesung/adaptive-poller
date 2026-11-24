CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -Iinclude

.PHONY: all test bench clean

all: adaptive_server adaptive_bench

adaptive_server: src/server.c src/poller.c include/poller.h
	$(CC) $(CFLAGS) -o $@ src/server.c src/poller.c

adaptive_bench: src/bench.c src/poller.c include/poller.h
	$(CC) $(CFLAGS) -o $@ src/bench.c src/poller.c

test: adaptive_bench
	./adaptive_bench --test

bench: adaptive_bench
	./adaptive_bench

clean:
	rm -f adaptive_server adaptive_bench

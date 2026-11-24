CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -Iinclude
ASAN_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude

.PHONY: all test asan bench clean

all: adaptive_server adaptive_bench test_poller

adaptive_server: src/server.c src/poller.c include/poller.h
	$(CC) $(CFLAGS) -o $@ src/server.c src/poller.c

adaptive_bench: src/bench.c src/poller.c include/poller.h
	$(CC) $(CFLAGS) -o $@ src/bench.c src/poller.c

test_poller: tests/test_poller.c src/poller.c include/poller.h
	$(CC) $(CFLAGS) -o $@ tests/test_poller.c src/poller.c

test: test_poller
	./test_poller

asan:
	$(CC) $(ASAN_FLAGS) -o test_poller_asan tests/test_poller.c src/poller.c
	./test_poller_asan
	rm -f test_poller_asan

bench: adaptive_bench
	./adaptive_bench

clean:
	rm -f adaptive_server adaptive_bench test_poller test_poller_asan

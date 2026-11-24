# adaptive-poller

[![CI](https://github.com/sikhesung/adaptive-poller/actions/workflows/ci.yml/badge.svg)](https://github.com/sikhesung/adaptive-poller/actions/workflows/ci.yml)

A lightweight C11 event-loop multiplexer that combines POSIX `poll()` with OS-native `epoll` (Linux) and `kqueue` (macOS/BSD) behind a single API — built to keep the trade-off between small-scale syscall overhead and large-scale `O(K)` readiness actually net-positive.

Zero external dependencies. Verified with `-Wall -Wextra -Werror`, AddressSanitizer + UBSan, and multi-OS CI (Ubuntu gcc/clang + macOS clang).

---

## Why I Built This

While writing a single-threaded non-blocking C web server from scratch, I kept running into the classic `poll()` vs `epoll()` trade-off:

- **`poll()`** is portable across Linux/macOS and needs zero registration syscalls when connection counts are small, but copying and scanning `O(N)` descriptors on every loop falls apart once thousands of idle keep-alive sockets accumulate.
- **`epoll` / `kqueue`** scales cleanly to thousands of idle connections (`O(K)` ready events), but is OS-specific and pays extra `epoll_ctl` / `kevent` syscall overhead when small workloads frequently toggle interest masks.

At first I wondered whether switching between `poll` and `epoll` dynamically at runtime could get the best of both worlds. Naive threshold switching actually loses performance due to boundary thrashing (e.g., oscillating around 255 ↔ 257 fds) and migration spikes.

So I built `adaptive-poller` to test and implement two designs where combining `poll` and `epoll`/`kqueue` actually pays off:

1. **`POLLER_AUTO` (Zero-cost startup selection):** Chooses `poll` or `epoll`/`kqueue` once at creation time based on OS and `max_conns_hint` — zero runtime migration overhead.
2. **`POLLER_ADAPTIVE` (Hysteresis + cooldown runtime switching):** Starts on `poll` (zero `ctl` syscalls while small), promotes once to `epoll`/`kqueue` at `high_water` (default `256`), and only demotes back when `active_fds <= low_water` (`64`) **and** `cooldown_ms` (`2000ms`) has elapsed.
3. **No-op `poller_mod()` elision:** Tracks each descriptor's current interest mask in an `O(1)` fd-indexed slot table and skips redundant `epoll_ctl(MOD)` / `kevent` syscalls when the mask has not changed.

---

## Architecture

```text
                    +-------------------------------------------+
                    |           Unified API (poller.h)          |
                    |  poller_add / mod / del / wait / stats    |
                    +---------------------+---------------------+
                                          |
        +---------------------------------+---------------------------------+
        v                                 v                                 v
  [POLLER_AUTO]                   [POLLER_ADAPTIVE]               [No-Op Mod Elision]
  Selects backend once            Promotes at HIGH_WATER (256)    Skips kernel ctl
  from OS + hint                  Demotes at LOW_WATER (64)       syscall when mask
  (0 migration cost)              + 2000ms cooldown               is unchanged
```

The repo also includes a small non-blocking HTTP/1.1 demo server (`src/server.c`) using:
- `SO_REUSEADDR`, `TCP_NODELAY`, and `SIGPIPE` ignored
- Per-connection write buffers for non-blocking partial `write()` via `POLLER_OUT`
- A `pipe()`-based self-pipe for async-signal-safe `SIGINT`/`SIGTERM` shutdown (`exit(0)` with full resource cleanup)
- `/api/health` and `/api/stats` endpoints exposing live backend stats in JSON

---

## Benchmarks

Measured on WSL2 Ubuntu (`gcc 15.2.0 -O2`) using real `AF_UNIX` non-blocking socketpairs via `make bench`.

### Workload A — Small `N = 32`, Frequent State Churn & Redundant `mod` (10,000 iters)
| Mode | Active Backend | Total Time | Throughput (op/s) | p50 | p99 | `ctl` Calls (Elided) | Switches |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `poll` | `poll` | `43.90 ms` | `227,791` | `3.7 µs` | `8.7 µs` | `0 (0)` | `0` |
| `epoll` | `epoll` | `41.48 ms` | `241,088` | `3.7 µs` | `8.7 µs` | `48 (9,984)` | `0` |
| `auto` | `poll` | `45.07 ms` | `221,893` | `3.9 µs` | `8.5 µs` | `0 (0)` | `0` |
| **`adaptive`** | **`poll`** | **`40.84 ms`** | **`244,835`** | **`3.7 µs`** | **`7.5 µs`** | **`0 (0)`** | **`0`** |

> No-op elision eliminated **9,984 of 10,032** `epoll_ctl` syscalls, while `adaptive` stayed on `poll` with zero `ctl` syscalls and the lowest `p99` (`7.5 µs`).

### Workload B — Large `N = 2,000` Idle Keep-Alive + 8 Active (4,000 iters)
| Mode | Active Backend | Total Time | Throughput (op/s) | p50 | p99 | `ctl` Calls | Switches |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `poll` | `poll` | `547.65 ms` | `7,304` | `120.5 µs` | `330.9 µs` | `0` | `0` |
| **`epoll`** | **`epoll`** | **`3.72 ms`** | **`1,076,419`** | **`0.8 µs`** | **`1.1 µs`** | `2,000` | `0` |
| **`auto`** | **`epoll`** | **`4.54 ms`** | **`881,576`** | **`0.8 µs`** | **`1.3 µs`** | `2,000` | `0` |
| **`adaptive`** | **`epoll`** | **`5.02 ms`** | **`796,868`** | **`0.8 µs`** | **`1.9 µs`** | `2,000` | **`1`** |

> ~**150x lower `p50` latency** and **109x–147x higher throughput** than `poll`.

### Workload C — Unexpected Traffic Spike `24 → 600 → 24` fds (4,500 iters, `hint = 24`)
| Mode | Final Backend | Total Time | Throughput (op/s) | p50 | p99 | `ctl` Calls | Switches |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `poll` | `poll` | `31.72 ms` | `141,861` | `1.2 µs` | `25.3 µs` | `0` | `0` |
| `auto` (`hint=24`) | `poll` | `33.56 ms` | `134,097` | `1.4 µs` | `31.0 µs` | `0` | `0` |
| `epoll` | `epoll` | `6.59 ms` | `682,600` | `0.9 µs` | `1.1 µs` | `1,176` | `0` |
| **`adaptive`** | **`epoll`** | **`7.64 ms`** | **`589,174`** | **`0.9 µs`** | **`1.9 µs`** | `1,176` | **`1`** |

> When traffic spiked past the initial `hint=24`, `auto` stayed pinned on `poll` (`p99 = 31.0 µs`), whereas `adaptive` promoted once (`switches = 1`) and cut `p99` latency by **16x** (`1.9 µs`) and total time by **4.4x**.

---

## Quick Example

```c
#include "poller.h"

poller_config_t cfg = poller_default_config(POLLER_ADAPTIVE, 64);
cfg.high_water  = 256;   /* Promote to epoll/kqueue at >= 256 fds */
cfg.low_water   = 64;    /* Demote to poll at <= 64 fds after cooldown */
cfg.cooldown_ms = 2000;

poller_t *p = poller_create_with_config(&cfg);
poller_add(p, listen_fd, POLLER_IN, NULL);

poller_event_t events[128];
int n = poller_wait(p, events, 128, 500);
```

---

## Build, Test & Run

```bash
make all     # Build adaptive_server, adaptive_bench, test_poller
make test    # Run unit & hysteresis tests
make asan    # Run tests under AddressSanitizer + UBSan
make bench   # Run Workload A / B / C benchmarks
```

### Demo HTTP Server
```bash
./adaptive_server -p 18900 -m adaptive -c 64
curl http://127.0.0.1:18900/api/health
curl http://127.0.0.1:18900/api/stats
```

---

## Design Notes & Trade-offs

- **Single-threaded by design:** Each `poller_t` is meant to be owned by one event-loop thread to avoid lock contention. Multi-core scaling is better handled with worker processes (`SO_REUSEPORT`, similar to Nginx).
- **Readiness vs. Completion I/O:** This library targets readiness-based APIs (`poll`, `epoll`, `kqueue`). On modern Linux (5.1+), adding an `io_uring` completion backend behind a higher-level async I/O interface is the natural next step to reduce syscall overhead even further.

## License

[MIT](LICENSE)

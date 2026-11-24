# adaptive-poller

> **ROI-Positive Event Loop Multiplexer (`poll` · Linux `epoll` · macOS `kqueue`) in Pure C11**

`adaptive-poller`는 `poll()`의 소규모·이식성 장점과 `epoll`/`kqueue`의 대규모(`C10K+`) `O(K)` 성능을 **실측 기반 트레이드오프 설계**로 결합한 경량 C11 이벤트 루프 라이브러리 및 논블로킹 HTTP 데모 서버입니다.

외부 라이브러리 없이 POSIX/OS 표준 API만으로 구현되었으며, **AddressSanitizer + UBSan(메모리 누수 0)**과 **GitHub Actions 멀티 OS CI(Ubuntu gcc/clang + macOS clang)**로 검증됩니다.

---

## 1. 문제 정의: 왜 단순 `poll ↔ epoll` 전환은 손해인가?

| 백엔드 | 강점 | 약점 |
| --- | --- | --- |
| **`poll()`** | POSIX 이식성, 소규모에서 `ctl` 시스템 콜 `0` | 유휴 연결 증가 시 매 루프 `O(N)` 배열 복사·스캔 폭증 |
| **`epoll` / `kqueue`** | 유휴 연결이 수천~수만 개여도 `O(K)` 반환 | Linux/macOS 비호환, 잦은 관심 이벤트 변경 시 `ctl` 시스템 콜 누적 |
| **나이브한 런타임 전환** | — | 임계치(예: 255↔257) 경계 진동(Thrashing)과 마이그레이션 지연으로 오히려 손해 |

---

## 2. 가성비(+)를 만드는 3가지 핵심 최적화

```text
                    +-------------------------------------------+
                    |           Unified API (poller.h)          |
                    |  poller_add / mod / del / wait / stats    |
                    +---------------------+---------------------+
                                          |
        +---------------------------------+---------------------------------+
        v                                 v                                 v
  [POLLER_AUTO]                   [POLLER_ADAPTIVE]               [No-Op Mod Elision]
  시작 시 OS + hint로             HIGH_WATER(256) 승격            이전 이벤트 마스크와
  1회 고정 선택                   LOW_WATER(64) + 2000ms          동일하면 epoll_ctl/
  (런타임 전환 비용 0)            쿨다운으로 진동(Thrash) 차단     kevent 시스템 콜 생략
```

1. **No-Op `poller_mod()` Elision (`ctl_elided`):**
   - 기존 관심 이벤트 마스크와 동일한 `poller_mod()` 호출을 커널로 넘기지 않고 유저 공간에서 즉시 생략합니다. (Workload A에서 `10,000`회 중 **`9,984`회의 `epoll_ctl` 시스템 콜 제거**)
2. **`POLLER_AUTO` (Zero-Cost Startup Selection):**
   - Nginx 스타일로 시작 시점에 OS(`__linux__`, `__APPLE__`)와 예상 동시 접속 수(`max_conns_hint`)로 백엔드를 1회 결정해 런타임 전환 비용을 `0`으로 만듭니다.
3. **`POLLER_ADAPTIVE` (Hysteresis + Cooldown Runtime Switching):**
   - 예측 불가능한 트래픽 급증(Spike)에 대응하기 위해 소규모에서는 `poll`로 시작하다가 `active_fds >= high_water` 시 `O(N)` 1회 마이그레이션으로 `epoll`/`kqueue`에 승격합니다.
   - 부하가 내려갈 때는 `active_fds <= low_water` **AND** `cooldown_ms` 경과 조건을 모두 만족할 때만 복귀해 경계 진동을 원천 차단합니다.
4. **`O(1)` FD Slot Table + Self-Pipe Safe Shutdown (`src/server.c`):**
   - `fd` 직접 인덱싱 테이블과 swap-remove로 `add`/`mod`/`del`을 `O(1)`에 처리하며, 데모 서버는 `SIGPIPE` 무시, `TCP_NODELAY`, 비동기 partial-write 버퍼, `pipe()` 기반 self-pipe로 `SIGINT`/`SIGTERM` 시 모든 fd와 메모리를 정리하고 `exit(0)`으로 종료합니다.

---

## 3. 실측 벤치마크 (WSL2 Ubuntu · `gcc 15.2.0 -O2`)

### Workload A: 소규모 `N = 32` + 고빈도 상태 변경 & 중복 `mod` (`10,000 iters`)
| 모드 | 실제 백엔드 | 소요 시간 | 처리량 (op/s) | p50 | p99 | `ctl` 호출 (생략) | 전환 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `poll` | `poll` | `43.90 ms` | `227,791` | `3.7 µs` | `8.7 µs` | `0 (0)` | `0` |
| `epoll` | `epoll` | `41.48 ms` | `241,088` | `3.7 µs` | `8.7 µs` | `48 (9,984)` | `0` |
| `auto` | `poll` | `45.07 ms` | `221,893` | `3.9 µs` | `8.5 µs` | `0 (0)` | `0` |
| **`adaptive`** | **`poll`** | **`40.84 ms`** | **`244,835`** | **`3.7 µs`** | **`7.5 µs`** | **`0 (0)`** | **`0`** |

### Workload B: 대규모 유휴 Keep-Alive `N = 2,000` + 활성 `8` (`4,000 iters`)
| 모드 | 실제 백엔드 | 소요 시간 | 처리량 (op/s) | p50 | p99 | `ctl` 호출 | 전환 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `poll` | `poll` | `547.65 ms` | `7,304` | `120.5 µs` | `330.9 µs` | `0` | `0` |
| **`epoll`** | **`epoll`** | **`3.72 ms`** | **`1,076,419`** | **`0.8 µs`** | **`1.1 µs`** | `2,000` | `0` |
| **`auto`** | **`epoll`** | **`4.54 ms`** | **`881,576`** | **`0.8 µs`** | **`1.3 µs`** | `2,000` | `0` |
| **`adaptive`** | **`epoll`** | **`5.02 ms`** | **`796,868`** | **`0.8 µs`** | **`1.9 µs`** | `2,000` | **`1`** |

> `poll` 대비 **`p50` 지연 150배 단축**, 처리량 **109배~147배 향상**

### Workload C: 예측 불가능한 트래픽 급증 `24 → 600 → 24 fds` (`4,500 iters`, `hint = 24`)
| 모드 | 최종 백엔드 | 소요 시간 | 처리량 (op/s) | p50 | p99 | `ctl` 호출 | 전환 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `poll` | `poll` | `31.72 ms` | `141,861` | `1.2 µs` | `25.3 µs` | `0` | `0` |
| `auto` (`hint=24`) | `poll` | `33.56 ms` | `134,097` | `1.4 µs` | `31.0 µs` | `0` | `0` |
| `epoll` | `epoll` | `6.59 ms` | `682,600` | `0.9 µs` | `1.1 µs` | `1,176` | `0` |
| **`adaptive`** | **`epoll`** | **`7.64 ms`** | **`589,174`** | **`0.9 µs`** | **`1.9 µs`** | `1,176` | **`1`** |

> 초기 예상치(`hint=24`)를 벗어난 급증 구간에서 `auto`는 `poll`에 묶여 `p99 = 31.0 µs`로 밀린 반면, **`adaptive`는 단 1회 승격으로 `p99 = 1.9 µs`(16배 개선), 총 시간 4.4배 단축**을 달성했습니다.

---

## 4. API 사용 예시

```c
#include "poller.h"

poller_config_t cfg = poller_default_config(POLLER_ADAPTIVE, 64);
cfg.high_water  = 256;   /* 256개 이상 시 epoll/kqueue 승격 */
cfg.low_water   = 64;    /* 64개 이하 + 쿨다운 경과 시 poll 복귀 */
cfg.cooldown_ms = 2000;

poller_t *p = poller_create_with_config(&cfg);
poller_add(p, listen_fd, POLLER_IN, NULL);

poller_event_t events[128];
int n = poller_wait(p, events, 128, 500);
```

---

## 5. 빌드 · 테스트 · ASan · 실행

```bash
make all       # adaptive_server, adaptive_bench, test_poller 빌드 (-Wall -Wextra -Werror)
make test      # 단위 테스트 (이벤트 전달, mod 생략, auto 선택, adaptive 히스테리시스)
make asan      # AddressSanitizer + UndefinedBehaviorSanitizer 메모리 누수/UB 검증
make bench     # Workload A / B / C 실측 벤치마크 실행
```

### 데모 HTTP 서버 실행
```bash
./adaptive_server -p 18900 -m adaptive -c 64
curl http://127.0.0.1:18900/api/health
curl http://127.0.0.1:18900/api/stats
```

---

## 6. 설계 트레이드오프 & 향후 확장
- **단일 스레드 이벤트 루프:** `poller_t`는 락 경합을 없애기 위해 스레드당 1개 인스턴스 사용을 전제로 설계했습니다(멀티코어 확장은 Nginx처럼 `SO_REUSEPORT` 멀티 워커 프로세스 모델 권장).
- **차세대 확장 (`io_uring`):** 현재는 `poll`/`epoll`/`kqueue` Readiness 모델 기반이며, 향후 Linux 5.1+ 환경에서는 시스템 콜 자체를 최소화하는 Completion 모델(`io_uring`) 백엔드를 동일 인터페이스(`poller.h`) 아래 추가할 수 있습니다.

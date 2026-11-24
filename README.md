# adaptive-poller

**`poll`과 `epoll`의 트레이드오프를 측정하고, 가성비(ROI)가 플러스(+)가 되도록 결합한 C11 이벤트 루프 추상화 및 데모 서버입니다.**

## 왜 만들었나?

- **`poll()`**: 동시 연결 수가 적고 상태 변경(`IN ↔ OUT`)이 잦을 때는 `epoll_ctl` 시스템 콜이 없어 오히려 빠르며, POSIX 표준이라 Linux/macOS 모두 빌드됩니다. 하지만 유휴 연결이 수천 개를 넘으면 매 루프 `O(N)` 복사·스캔 때문에 급격히 느려집니다.
- **`epoll()`**: 유휴 연결이 많아도 준비된 소켓(`O(K)`)만 꺼내므로 압도적으로 빠르지만, 소규모 고빈도 상태 변경 구간에서는 매번 `epoll_ctl` 시스템 콜 비용이 붙고 Linux 전용입니다.

단순히 경계값(예: 256)에서 무조건 전환하면 255↔257에서 **경계 진동(Thrashing)**과 전환 지연으로 손해가 납니다. 이 프로젝트는 **가성비가 플러스(+)가 되는 2가지 방식**을 구현하고 실제 Linux 커널에서 벤치마크로 검증했습니다.

### 가성비(+) 전략 2가지 (`include/poller.h`, `src/poller.c`)
1. **`POLLER_AUTO` (Nginx 스타일 시작 시 고정 선택):**
   - 서버 시작 시 OS(`__linux__`)와 `max_conns_hint`를 보고 `<= 256`(또는 비-Linux)이면 `poll`, `> 256`이면 `epoll`로 고정합니다.
   - **런타임 전환 비용 `0`**으로 두 구간의 장점을 모두 얻습니다.
2. **`POLLER_ADAPTIVE` (히스테리시스 + 쿨다운 런타임 전환):**
   - 소규모에서는 `poll`로 시작해 `epoll_ctl` 시스템 콜을 `0`으로 유지합니다.
   - 연결 수가 `HIGH_WATER(256)` 이상일 때 `epoll`로 **1회 승격**하고, `LOW_WATER(64)` 이하로 떨어져도 `2000ms` 쿨다운을 통과해야만 `poll`로 복귀해 경계 진동을 원천 차단합니다.

---

## 실제 측정 결과 (WSL2 Ubuntu · `gcc 15.2.0 -O2`)

### Workload A: 소규모 `N = 32` + 고빈도 상태 변경 (`10,000 iters`)
| 모드 | 실제 백엔드 | 소요 시간 | `epoll_ctl` 호출 수 | 전환 횟수 | 비고 |
| --- | --- | ---: | ---: | ---: | --- |
| `poll` | `poll` | `36.73 ms` | `0` | `0` | 시스템 콜 최소 |
| `epoll` | `epoll` | `43.86 ms` | `10,032` | `0` | 매 변경마다 `epoll_ctl` 비용 (+19% 느림) |
| **`auto`** | **`poll`** | **`36.12 ms`** | **`0`** | **`0`** | **최고 성능 (전환 비용 0)** |
| **`adaptive`** | **`poll`** | **`39.89 ms`** | **`0`** | **`0`** | `epoll` 단독보다 빠름 |

### Workload B: 대규모 유휴 `N = 2,000` + 활성 `8` (`4,000 iters`)
| 모드 | 실제 백엔드 | 소요 시간 | `epoll_ctl` 호출 수 | 전환 횟수 | 비고 |
| --- | --- | ---: | ---: | ---: | --- |
| `poll` | `poll` | `561.74 ms` | `0` | `0` | 매 루프 2,000칸 `O(N)` 스캔 폭발 |
| `epoll` | `epoll` | `3.35 ms` | `2,000` | `0` | `poll` 대비 약 168배 빠름 |
| **`auto`** | **`epoll`** | **`3.13 ms`** | **`2,000`** | **`0`** | **최고 성능 (약 179배 빠름)** |
| **`adaptive`** | **`epoll`** | **`3.31 ms`** | **`2,000`** | **`1`** | 단 1회 자동 승격으로 약 170배 빠름 |

---

## 빌드 및 실행 (Linux / WSL / macOS)

```bash
make all       # adaptive_server, adaptive_bench 빌드 (-Wall -Wextra -Werror)
make test      # 단위 테스트 (poll / epoll / auto / adaptive 히스테리시스 검증)
make bench     # Workload A & B 실측 벤치마크 실행
```

### 데모 HTTP 서버 실행
```bash
./adaptive_server -p 18900 -m adaptive -c 1024
curl http://127.0.0.1:18900/api/stats
```
응답 예시:
```json
{"mode":"adaptive","active_backend":"poll","active_fds":2,"switches":0,"epoll_ctl_calls":0,"wait_calls":3}
```

## 프로젝트 구조
- `include/poller.h` — 통합 이벤트 루프 인터페이스 (`POLLER_POLL`, `POLLER_EPOLL`, `POLLER_AUTO`, `POLLER_ADAPTIVE`)
- `src/poller.c` — `O(1)` fd 슬롯 테이블, `poll`↔`epoll` 무손실 마이그레이션, 히스테리시스 전환 구현
- `src/server.c` — 논블로킹 HTTP/1.1 데모 서버 (`/api/stats` 실시간 백엔드 상태 출력)
- `src/bench.c` — 단위 테스트 + 소규모 고빈도 / 대규모 유휴 벤치마크 하네스
- `Makefile` — `-std=c11 -O2 -Wall -Wextra -Werror` 빌드

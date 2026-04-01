# SPDK 디버깅 치트시트

**과정**: SPDK 마스터리
**참조**: REF-Debugging-Cheatsheet
**대상**: SPDK 개발자 및 시스템 통합 엔지니어

---

## 목차

1. [GDB 빠른 참조](#1-gdb-빠른-참조)
2. [로그 레벨과 로그 플래그](#2-로그-레벨과-로그-플래그)
3. [트레이스 포인트 사용법](#3-트레이스-포인트-사용법)
4. [자주 발생하는 오류 메시지와 원인](#4-자주-발생하는-오류-메시지와-원인)
5. [메모리 디버깅](#5-메모리-디버깅)
6. [코어 덤프 분석](#6-코어-덤프-분석)
7. [성능 디버깅](#7-성능-디버깅)
8. [빠른 진단 흐름도](#8-빠른-진단-흐름도)
9. [유용한 RPC 명령어](#9-유용한-rpc-명령어)
10. [환경 변수](#10-환경-변수)

---

## 1. GDB 빠른 참조

### 디버그 심볼 포함 빌드

```bash
# 디버그 심볼을 포함하여 빌드 (최적화 비활성화)
./configure --enable-debug

# 기존 빌드에 추가
make CFLAGS="-g -O0"

# 디버그 정보 포함 여부 확인
file build/bin/spdk_tgt
readelf -S build/bin/spdk_tgt | grep debug
```

### 실행 중인 SPDK 프로세스에 GDB 연결

```bash
# PID 확인
pgrep -a spdk_tgt

# 연결 (ptrace 권한 또는 root 필요)
gdb -p <PID>

# 직접 실행
gdb --args ./build/bin/spdk_tgt -c spdk.json

# GDB 내부: 첫 번째 유용한 중단점까지 실행
(gdb) set follow-fork-mode child
(gdb) set detach-on-fork off
(gdb) run
```

### 스레드 검사

SPDK는 CPU 코어당 하나의 리액터(reactor) 스레드를 실행합니다. 각 리액터는 하나의 pthread입니다.

```gdb
# 모든 스레드 목록 확인
(gdb) info threads

# 특정 스레드로 전환
(gdb) thread 3

# 모든 스레드의 백트레이스 출력
(gdb) thread apply all bt

# 모든 스레드의 백트레이스 출력 (간략하게, 프레임당 한 줄)
(gdb) thread apply all bt 1

# 이름으로 리액터 스레드 찾기
(gdb) thread find reactor
```

### 폴러(Poller) 내부의 브레이크포인트

SPDK 폴러는 빠른 루프에서 실행됩니다. 내부에 브레이크포인트를 설정하면 전체 리액터가 멈춥니다.

```gdb
# 과도한 중단을 방지하기 위해 조건부 브레이크포인트 설정
(gdb) break nvmf_poll_group_poll if g_num_aborts > 0

# 특정 소스 파일에서 브레이크
(gdb) break lib/nvmf/tcp.c:nvmf_tcp_poll_group_poll

# 특정 NVMe 명령 오프코드에서 브레이크
(gdb) break nvmf_request_exec if cmd->opc == SPDK_NVME_OPC_READ

# 삭제하지 않고 브레이크포인트 비활성화
(gdb) disable 2

# 일회성 브레이크포인트 (첫 번째 히트 후 자동 삭제)
(gdb) tbreak spdk_bdev_io_complete
```

### SPDK 구조체 검사

```gdb
# spdk_bdev_io 구조체 출력
(gdb) p *bdev_io

# bdev의 I/O 채널 목록 출력
(gdb) p *spdk_io_channel

# TAILQ 순회 (예: 대기 중인 I/O)
(gdb) p *((struct spdk_bdev_io *)bdev->internal.io_stat)

# 구조체 보기 좋게 출력
(gdb) set print pretty on
(gdb) p *channel

# 포인터가 어디에 위치하는지 확인 (힙 vs 스택 vs BSS)
(gdb) info symbol 0xADDRESS
```

### 워치포인트(Watchpoint)

```gdb
# 변수에 대한 쓰기 감시
(gdb) watch bdev->internal.status

# 메모리 주소 범위 감시
(gdb) watch -l buf[0]

# 읽기 워치포인트 (읽기 시 발동)
(gdb) rwatch channel->ref_count
```

### 유용한 GDB 초기화 파일 (~/.gdbinit, SPDK용)

```
set print pretty on
set print array on
set print array-indexes on
set pagination off
set confirm off
handle SIGTERM nostop noprint
# DPDK 시그널에서 멈추지 않도록 설정
handle SIGRTMIN nostop noprint pass
handle SIGRTMIN+1 nostop noprint pass
```

---

## 2. 로그 레벨과 로그 플래그

### 로그 레벨 계층 구조

| 레벨 | 열거형 상수 | 의미 |
|-------|--------------|---------|
| -1 | `SPDK_LOG_DISABLED` | 모든 메시지 억제 |
| 0 | `SPDK_LOG_ERROR` | 치명적이거나 복구 불가능한 오류 |
| 1 | `SPDK_LOG_WARN` | 복구 가능한 문제, 성능 저하 상태 |
| 2 | `SPDK_LOG_NOTICE` | 정상적인 주요 이벤트 (기본값) |
| 3 | `SPDK_LOG_INFO` | 세부 서브시스템 정보 |
| 4 | `SPDK_LOG_DEBUG` | 요청별 / 폴링별 디버그 출력 |

### 소스 코드의 로그 매크로

```c
/* 지정된 레벨로 항상 출력 */
SPDK_ERRLOG("Failed to allocate IO channel: %d\n", rc);
SPDK_WARNLOG("Queue depth %u exceeds recommended limit\n", qd);
SPDK_NOTICELOG("NVMe-oF target started on %s\n", addr);

/* 명명된 플래그로 제어 (시작 시 또는 RPC를 통해 활성화 필요) */
SPDK_INFOLOG(nvmf, "Connection accepted from %s\n", addr_str);
SPDK_DEBUGLOG(bdev, "bdev_io %p submitted to %s\n", bdev_io, bdev->name);

/* 플래그로 제어되는 바이너리 덤프 */
SPDK_LOGDUMP(nvme, "NVMe command", cmd_buf, sizeof(*cmd));

/* 속도 제한 오류 (초당 최대 1회) */
SPDK_ERRLOG_RATELIMIT("CRC mismatch on LBA %" PRIu64 "\n", lba);
```

### 서브시스템에서 로그 플래그 등록

```c
/* .c 파일에서 — 번역 단위당 하나 */
SPDK_LOG_REGISTER_COMPONENT(my_module)

/* 사용 방법 */
SPDK_DEBUGLOG(my_module, "Processing request %p\n", req);
```

> `SPDK_DEBUGLOG`와 `SPDK_LOGDUMP`는 `--enable-debug`로 빌드하지 않으면 컴파일에서 제외됩니다. `SPDK_INFOLOG`는 항상 컴파일에 포함되지만 플래그의 런타임 상태에 의해 제어됩니다.

### 시작 시: CLI를 통한 로그 플래그 활성화

```bash
# 단일 플래그 활성화
spdk_tgt -c spdk.json --logflag nvmf

# 여러 플래그 활성화
spdk_tgt -c spdk.json --logflag nvmf --logflag bdev

# 모든 플래그 활성화 (매우 상세한 출력)
spdk_tgt -c spdk.json --logflag all

# 최소 로그 레벨을 DEBUG로 설정
spdk_tgt -c spdk.json -L debug --logflag nvme
```

### 런타임: RPC를 통한 로그 레벨 변경

```bash
# 런타임에 로그 레벨을 DEBUG로 올리기
scripts/rpc.py log_set_level DEBUG

# NOTICE로 다시 내리기
scripts/rpc.py log_set_level NOTICE

# 런타임에 로그 플래그 활성화
scripts/rpc.py log_set_flag nvmf

# 로그 플래그 비활성화
scripts/rpc.py log_clear_flag nvmf

# 모든 가용 플래그 및 현재 상태 목록 확인
scripts/rpc.py log_get_flags

# 현재 로그 레벨 조회
scripts/rpc.py log_get_level
```

### 로그 출력 리디렉션

```bash
# 파일로 로그 출력 (--msg-mempool-size로 초기 메시지 손실 방지)
spdk_tgt -c spdk.json 2>spdk.log

# syslog 사용
spdk_tgt -c spdk.json --use-syslog spdk_tgt

# 타임스탬프와 함께 실시간 출력 확인
tail -f spdk.log | ts '[%Y-%m-%d %H:%M:%.S]'
```

---

## 3. 트레이스 포인트 사용법

### 아키텍처 개요

SPDK 트레이스포인트(tracepoint)는 잠금 없는 공유 메모리 링 버퍼(`/dev/shm/_trace.<app_name>.<pid>`)를 사용합니다. 각 엔트리는 다음을 기록합니다:
- 64비트 TSC 타임스탬프
- 트레이스포인트 ID (그룹 x 64 + 인덱스)
- 소유자 ID (예: NVMe-oF 연결 번호)
- 객체 ID (예: bdev_io 포인터)
- 최대 8바이트의 인라인 인수

### 시작 시 트레이스 활성화

```bash
# nvmf 그룹의 모든 트레이스포인트 활성화
spdk_tgt -c spdk.json --num-trace-entries 131072

# 공유 메모리 파일 위치 확인:
ls /dev/shm/_trace.*
```

### RPC를 통한 런타임 활성화 / 비활성화

```bash
# 모든 트레이스포인트 그룹 목록 확인
scripts/rpc.py trace_get_tpoint_group_mask

# 이름으로 그룹 활성화
scripts/rpc.py trace_set_tpoint_group_mask --tpoint-group-mask nvmf_tcp

# 숫자 마스크로 개별 트레이스포인트 활성화
scripts/rpc.py trace_set_tpoint_group_mask --tpoint-group-mask 0x4

# 모든 트레이싱 비활성화
scripts/rpc.py trace_set_tpoint_group_mask --tpoint-group-mask 0x0
```

### 트레이스 스냅샷 수집

```bash
# 실행 중인 공유 메모리에서 파일로 덤프
spdk_trace -s spdk_tgt -p <PID> -f trace_output.bin

# shm을 직접 복사
cp /dev/shm/_trace.spdk_tgt.<PID> trace_output.bin
```

### spdk_trace를 이용한 분석

```bash
# 사람이 읽을 수 있는 타임라인 출력 (stdout)
build/bin/spdk_trace -f trace_output.bin

# 단일 객체로 필터링 (예: 하나의 bdev_io)
build/bin/spdk_trace -f trace_output.bin -o 0xADDRESS

# 소유자(연결/채널 ID)로 필터링
build/bin/spdk_trace -f trace_output.bin -w 3

# 시간 구간의 이벤트만 표시 (TSC 단위)
build/bin/spdk_trace -f trace_output.bin -b 1000000 -e 5000000

# 외부 도구를 위한 JSON 출력
build/bin/spdk_trace -f trace_output.bin --json > trace.json
```

### 사용자 정의 트레이스포인트 추가

```c
#include "spdk/trace.h"

/* 모듈당 한 번 그룹과 트레이스포인트 ID 정의 */
#define TRACE_GROUP_MY_MOD  15
#define TRACE_MY_MOD_START  SPDK_TPOINT_ID(TRACE_GROUP_MY_MOD, 0)
#define TRACE_MY_MOD_DONE   SPDK_TPOINT_ID(TRACE_GROUP_MY_MOD, 1)

/* 정수 인수 하나와 함께 트레이스포인트 기록 */
spdk_trace_record(TRACE_MY_MOD_START, 0, 0, (uint64_t)req, req->length);
```

---

## 4. 자주 발생하는 오류 메시지와 원인

### NVMe / bdev 계층

| 오류 메시지 | 가능한 원인 | 해결 방법 |
|--------------|-------------|-----|
| `NVMe read/write error: SC 0x02` | 미디어 오류 (배드 블록) | 드라이브 SMART 확인; 교체 또는 재할당 |
| `Timeout occurred for CID xx` | 컨트롤러 무응답 또는 리셋 대기 중 | `nvme_timeout_ms` 증가; PCIe 링크 확인 |
| `bdev_io_split failed - no memory` | I/O 분할 풀 소진 | `--bdev-io-pool-size` 증가 |
| `bdev_get_io_channel failed` | bdev당 최대 채널 수 도달 | `num_shared_buffers` 감소 또는 제한 증가 |
| `Failed to allocate large buffer` | 휴지페이지(hugepage) 메모리 소진 | `-m` 휴지페이지 플래그 추가 또는 큐 뎁스 감소 |

### NVMe-oF 타겟

| 오류 메시지 | 가능한 원인 | 해결 방법 |
|--------------|-------------|-----|
| `Could not create CQ/SQ, out of resources` | RDMA MR 또는 CQ 소진 | `num_shared_buffers` 증가; ulimits 확인 |
| `Poll group poller exited abnormally` | 폴러에서 처리되지 않은 예외 | 전체 백트레이스 확인; 주로 NULL 역참조 |
| `transport_create failed` | 포트/주소가 이미 사용 중 | `ss -tlnp` 확인; 충돌하는 프로세스 종료 |
| `Subsystem not found` | NQN 불일치 또는 서브시스템 미등록 | `nvmf_get_subsystems` RPC 출력 확인 |
| `Connect request rejected: FABRICS error 0x02` | 잘못된 호스트 NQN 또는 allow-hostnqn 미설정 | 서브시스템 허용 목록에 호스트 NQN 추가 |

### DPDK / 휴지페이지 / 메모리

| 오류 메시지 | 가능한 원인 | 해결 방법 |
|--------------|-------------|-----|
| `Cannot get hugepage information` | 커널에 휴지페이지가 설정되지 않음 | `echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages` |
| `Cannot mmap memory for rte_config` | 비정상 종료로 인한 `/dev/hugepages` 잔여 파일 | `rm /dev/hugepages/rte_*; rm /dev/shm/spdk_*` |
| `EAL: No NUMA socket memory allocated` | NIC/NVMe와 메모리 간 NUMA 불일치 | 올바른 NUMA 노드에 휴지페이지 고정 |
| `Memory hotplug is not supported` | RTE 버전 비호환 | DPDK 버전이 SPDK 빌드와 일치하는지 확인 |

### 리액터 / 스레드

| 오류 메시지 | 가능한 원인 | 해결 방법 |
|--------------|-------------|-----|
| `Reactor X is not responding` | 폴러 내 무한 루프 또는 블로킹 시스콜 | `perf top -p <PID>`로 프로파일링; 멈춘 함수 찾기 |
| `spdk_thread_poll: deferred msgs overflow` | 지연 메시지 큐 소진 | `msg_mempool_size` 증가 |
| `Cannot run without DPDK EAL threads` | CPU 마스크 잘못 설정 | `-c` CPU 마스크가 가용 코어를 포함하는지 확인 |

---

## 5. 메모리 디버깅

### AddressSanitizer (ASan) 빌드

```bash
# ASan을 포함하여 빌드
./configure --enable-asan

make -j$(nproc)

# ASan 옵션과 함께 실행
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:log_path=/tmp/asan \
  ./build/bin/spdk_tgt -c spdk.json

# 알려진 DPDK 오탐(false positive) 억제
export LSAN_OPTIONS=suppressions=/path/to/spdk/test/common/asan.supp
```

ASan이 탐지하는 항목: 힙 버퍼 오버플로, use-after-free, 스택 오버플로, use-after-return, 메모리 누수.

> ASan은 일부 설정에서 휴지페이지와 호환되지 않습니다. ASan 테스트 시 `--no-huge --iova-mode=va`를 사용하세요.

### UndefinedBehaviorSanitizer

```bash
./configure --enable-ubsan
make -j$(nproc)

UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ./build/bin/spdk_tgt -c spdk.json
```

### Valgrind

Valgrind는 느리지만 재컴파일 없이 동작합니다.

```bash
# 기본 memcheck (힙 오류 및 누수)
valgrind --tool=memcheck \
         --leak-check=full \
         --track-origins=yes \
         --suppressions=test/common/valgrind.supp \
         ./build/bin/spdk_tgt -c spdk.json --no-huge

# Massif 힙 프로파일러
valgrind --tool=massif \
         --pages-as-heap=yes \
         ./build/bin/spdk_tgt -c spdk.json --no-huge
ms_print massif.out.<PID> | less
```

### Valgrind 억제 규칙 작성

```
{
   dpdk_mmap_suppression
   Memcheck:Addr8
   fun:rte_eal_init
   ...
}
```

`my_suppressions.supp`로 저장하고 `--suppressions=my_suppressions.supp`를 전달합니다.

### RPC를 통한 SPDK 메모리 풀 검사

```bash
# 메모리 풀 사용률 덤프
scripts/rpc.py spdk_get_memory_pools

# bdev 버퍼 풀 확인
scripts/rpc.py bdev_get_iostat
```

---

## 6. 코어 덤프 분석

### 코어 덤프 활성화

```bash
# 현재 세션에서 코어 크기 무제한
ulimit -c unlimited

# 재부팅 후에도 유지 (/etc/security/limits.conf에 추가)
echo "* soft core unlimited" >> /etc/security/limits.conf
echo "* hard core unlimited" >> /etc/security/limits.conf

# 코어 파일 저장 위치 제어 (systemd 시스템)
echo "/tmp/core.%e.%p.%t" > /proc/sys/kernel/core_pattern

# systemd-coredump의 경우 다음으로 조회:
coredumpctl list
coredumpctl gdb <PID>
```

### GDB로 코어 파일 로드

```bash
# 바이너리 + 코어 파일 로드
gdb ./build/bin/spdk_tgt /tmp/core.spdk_tgt.12345.1711800000

# GDB 내부
(gdb) bt               # 크래시된 스레드의 백트레이스
(gdb) info threads     # 크래시 시점의 모든 스레드 목록
(gdb) thread 2         # 스레드 2로 전환
(gdb) bt full          # 로컬 변수 포함 전체 백트레이스
(gdb) frame 3          # 스택 프레임 3 선택
(gdb) info locals      # 프레임 내 모든 로컬 변수 출력
(gdb) p *bdev_io       # 포인터 역참조
```

### 코어 분석 체크리스트

1. 크래시된 스레드에서 `bt full` 실행 -- 오류를 일으킨 함수와 라인을 확인합니다.
2. `thread apply all bt` 실행 -- 다른 스레드가 의심스러운 상태에 있는지 확인합니다.
3. 상위 몇 개 프레임의 로컬 변수와 인수를 검사합니다.
4. NULL 역참조 확인: SIGSEGV의 주소는 보통 GDB에서 `0x0` 또는 작은 오프셋으로 표시됩니다.
5. 스택 손상 확인: `bt`가 쓰레기 프레임을 보여주면 스택이 덮어씌워진 것입니다 -- ASan으로 버퍼 오버플로를 확인하세요.
6. 크래시 전 마지막 SPDK 로그 라인과 대조합니다.

### 코어에서 로그 버퍼 추출

로깅이 stderr/파일로 리디렉션된 경우, 코어의 타임스탬프와 `dmesg` 및 로그 파일을 교차 참조하세요.

```bash
# 심볼을 알고 있다면 GDB에서 인메모리 로그 링을 출력할 수 있습니다
(gdb) p spdk_log_get_print_level()
```

---

## 7. 성능 디버깅

### spdk_top -- 실시간 리액터 사용률

`spdk_top`은 SPDK 리액터 스레드, 폴러, I/O 채널에 대한 top과 유사한 뷰를 제공합니다.

```bash
# spdk_top 시작 (RPC 소켓을 통해 연결)
scripts/spdk_top.py

# 특정 RPC 소켓 사용
scripts/spdk_top.py -s /var/tmp/spdk.sock

# spdk_top 내부 유용한 키 바인딩:
#   r   - 리액터 사용률 기준 정렬
#   p   - 폴러 busy 카운트 기준 정렬
#   c   - 채널별 I/O 통계 표시
#   q   - 종료
```

출력 열:
- **Busy %** -- 폴러가 작업을 찾은 폴링 사이클의 비율
- **Idle %** -- 작업이 없었던 사이클의 비율 (폴러가 0을 반환)
- **Poller count** -- 리액터의 활성 폴러 수

### RPC를 통한 리액터 사용률

```bash
# 리액터 통계 조회
scripts/rpc.py framework_get_reactors | python3 -m json.tool

# 스레드 통계 확인 (폴러 수, 메시지 큐 깊이)
scripts/rpc.py thread_get_stats | python3 -m json.tool

# NVMe-oF 폴 그룹 통계
scripts/rpc.py nvmf_get_stats | python3 -m json.tool
```

### I/O 통계

```bash
# bdev별 IOPS, 대역폭, 지연시간
scripts/rpc.py bdev_get_iostat

# 연속 폴링 (1초 간격)
watch -n1 'scripts/rpc.py bdev_get_iostat | python3 -m json.tool'

# NVMe 컨트롤러 통계
scripts/rpc.py bdev_nvme_get_controller_health_info --name Nvme0
```

### Linux perf를 이용한 핫스팟 분석

```bash
# 실행 중인 SPDK 프로세스에 대해 30초간 CPU 샘플 기록
perf record -F 99 -p <PID> -g -- sleep 30
perf report --sort comm,dso,sym

# flamegraph (Brendan Gregg의 FlameGraph 스크립트 필요)
perf script | stackcollapse-perf.pl | flamegraph.pl > spdk_flame.svg

# 캐시 미스 카운트
perf stat -e cache-misses,cache-references,instructions,cycles \
  -p <PID> sleep 10
```

### 지연시간 히스토그램

```bash
# SPDK 내장 히스토그램 사용 (컴파일 시 포함된 경우)
scripts/rpc.py bdev_enable_histogram --name Nvme0n1 --enable true

# 워크로드 실행 후 히스토그램 조회
scripts/rpc.py bdev_get_histogram --name Nvme0n1
```

### 흔한 성능 안티패턴

| 증상 | 가능한 원인 | 해결 방법 |
|---------|-------------|-----|
| 하나의 리액터가 100%, 나머지는 유휴 | bdev/네임스페이스 할당 불균형 | 네임스페이스를 CPU 간에 분산 |
| 높은 I/O 지연시간 편차 | I/O 경로에서 메모리 복사 | `--enable-rdma` 또는 DIF로 제로 카피 사용 |
| 낮은 CPU 사용률에도 낮은 IOPS | 큐 뎁스가 너무 작음 | 설정에서 `qd` 증가; `num_io_queues` 확인 |
| CPU는 바쁘지만 I/O 진행 없음 | 스핀락(spinlock) 경합 | `perf`로 프로파일링; 잠금 순서 확인 |
| 메모리 대역폭 포화 | 여러 스레드가 동일 NUMA 노드에 접근 | 스레드와 메모리를 동일 NUMA에 고정 |

---

## 8. 빠른 진단 흐름도

```
증상: SPDK 프로세스가 즉시 크래시
    |
    +-- 확인: 코어 파일이 있는가?
    |       예 --> 6절: 코어 덤프 분석
    |       아니오 --> ulimit -c unlimited 설정 후 재현
    |
    +-- 확인: SPDK_ERRLOG 라인이 있는가?
            예 --> 4절: 자주 발생하는 오류 메시지
            아니오 --> --logflag all 활성화 후 재현

증상: SPDK 프로세스가 멈춤 / I/O 처리 중단
    |
    +-- 확인: 100% 사용률을 보이는 리액터가 있는가?
    |       예 --> 폴러 내 무한 루프 (perf top -p <PID>)
    |       아니오 --> 죽은 리액터 (작업이 디스패치되지 않음)
    |
    +-- 확인: RPC가 여전히 응답하는가?
            예 --> scripts/rpc.py thread_get_stats (멈춘 스레드 찾기)
            아니오 --> GDB 연결, thread apply all bt

증상: 낮은 I/O 성능
    |
    +-- 확인: spdk_top 리액터 busy %
    |       < 50% --> I/O가 리액터에 도달하지 않음 (제출 경로 확인)
    |       ~ 100% --> CPU 바운드 (perf 프로파일, 큐 뎁스 확인)
    |
    +-- 확인: bdev_get_iostat 지연시간
            높은 평균 --> 혼잡, 큐 뎁스 / CPU 수 확인
            높은 p99/p999 --> 주기적 지연 (GC, GDB, 지터)

증상: NVMe-oF 클라이언트가 연결할 수 없음
    |
    +-- 확인: 트랜스포트가 리스닝하고 있는가?
    |       scripts/rpc.py nvmf_get_transports
    |
    +-- 확인: 서브시스템이 존재하는가?
    |       scripts/rpc.py nvmf_get_subsystems
    |
    +-- 확인: 호스트 NQN이 허용되어 있는가?
            로그에서 "Connect request rejected" 확인

증상: 메모리 오류 / 데이터 손상
    |
    +-- --enable-asan으로 재빌드 후 재현
    +-- valgrind --tool=memcheck로 실행
    +-- 휴지페이지 설정 확인 (잔여 /dev/hugepages 파일?)
```

---

## 9. 디버깅을 위한 유용한 RPC 명령어

모든 명령어는 `scripts/rpc.py`를 사용합니다 (기본 소켓: `/var/tmp/spdk.sock`).

```bash
# 사용자 정의 소켓 경로
scripts/rpc.py -s /var/tmp/my_app.sock <command>

# 스크립팅을 위한 JSON 출력
scripts/rpc.py --json <command>
```

### 프레임워크 / 리액터

| 명령어 | 용도 |
|---------|---------|
| `framework_get_reactors` | 리액터, CPU 코어, 스레드 할당 목록 |
| `thread_get_stats` | 스레드별 폴러 수, I/O 채널 수, 메시지 큐 깊이 |
| `thread_get_pollers` | 각 스레드의 모든 활성 폴러 목록 |
| `framework_get_config` | 현재 JSON 설정 덤프 |
| `spdk_kill_instance` | SPDK 프로세스에 시그널 전송 (예: `SIGTERM`) |

### 블록 디바이스 (bdev)

| 명령어 | 용도 |
|---------|---------|
| `bdev_get_bdevs` | 모든 블록 디바이스와 설정 목록 |
| `bdev_get_iostat` | bdev별 IOPS, 대역폭, 지연시간 |
| `bdev_enable_histogram` | bdev에 지연시간 히스토그램 활성화 |
| `bdev_get_histogram` | 히스토그램 데이터 조회 |
| `bdev_nvme_get_controllers` | NVMe 컨트롤러 목록 |
| `bdev_nvme_get_controller_health_info` | NVMe 컨트롤러의 SMART 데이터 |

### NVMe-oF 타겟

| 명령어 | 용도 |
|---------|---------|
| `nvmf_get_transports` | 활성 트랜스포트 목록 (TCP, RDMA 등) |
| `nvmf_get_subsystems` | 서브시스템, 네임스페이스, 허용 호스트 목록 |
| `nvmf_get_stats` | 폴 그룹 및 연결 통계 |
| `nvmf_subsystem_get_controllers` | 서브시스템별 연결된 컨트롤러 목록 |

### 로깅

| 명령어 | 용도 |
|---------|---------|
| `log_get_level` | 현재 로그 레벨 조회 |
| `log_set_level` | 로그 레벨 설정 (ERROR/WARN/NOTICE/INFO/DEBUG) |
| `log_get_flags` | 모든 플래그와 활성화 상태 목록 |
| `log_set_flag` | 런타임에 로그 플래그 활성화 |
| `log_clear_flag` | 런타임에 로그 플래그 비활성화 |

### 트레이싱

| 명령어 | 용도 |
|---------|---------|
| `trace_get_tpoint_group_mask` | 현재 트레이스포인트 그룹 마스크 조회 |
| `trace_set_tpoint_group_mask` | 트레이스 그룹 활성화/비활성화 |

### iSCSI / Vhost (해당되는 경우)

| 명령어 | 용도 |
|---------|---------|
| `iscsi_get_connections` | 활성 iSCSI 연결 목록 |
| `iscsi_get_options` | iSCSI 설정 표시 |
| `vhost_get_controllers` | vhost 컨트롤러 목록 |

---

## 10. 디버깅을 위한 환경 변수

### DPDK / EAL 변수

| 변수 | 효과 | 예시 |
|----------|--------|---------|
| `RTE_LOG_LEVEL` | DPDK 내부 로그 상세도 설정 (0=emerg ... 8=debug) | `RTE_LOG_LEVEL=8` |
| `RTE_SDK` | DPDK SDK 경로 재정의 | `RTE_SDK=/opt/dpdk` |
| `RTE_TARGET` | DPDK 빌드 타겟 재정의 | `RTE_TARGET=x86_64-native-linuxapp-gcc` |

### ASan / Sanitizer 변수

| 변수 | 효과 | 예시 |
|----------|--------|---------|
| `ASAN_OPTIONS` | AddressSanitizer 동작 설정 | `detect_leaks=1:abort_on_error=1` |
| `LSAN_OPTIONS` | 누수 탐지기 (ASan의 하위 집합) | `suppressions=/path/to/asan.supp` |
| `UBSAN_OPTIONS` | UBSan 동작 설정 | `print_stacktrace=1:halt_on_error=1` |
| `TSAN_OPTIONS` | ThreadSanitizer 동작 설정 | `halt_on_error=1:log_path=/tmp/tsan` |

### SPDK 전용 변수

| 변수 | 효과 | 예시 |
|----------|--------|---------|
| `HUGEMEM` | setup 스크립트에 전달되는 휴지페이지 메모리 크기 (MB) | `HUGEMEM=4096` |
| `NRHUGE` | 2MB 휴지페이지 수 (`setup.sh`에서 사용) | `NRHUGE=1024` |
| `PCI_ALLOWED` | NVMe 디바이스 바인딩을 특정 PCI 주소로 제한 | `PCI_ALLOWED=0000:01:00.0` |
| `PCI_BLOCKED` | 특정 PCI 디바이스의 바인딩 차단 | `PCI_BLOCKED=0000:02:00.0` |
| `SPDK_COREDUMP_PATH` | 기본 코어 덤프 경로 재정의 | `SPDK_COREDUMP_PATH=/data/cores` |

### Linux 커널 디버그 변수

| 변수 / 경로 | 효과 |
|-----------------|--------|
| `/proc/sys/kernel/core_pattern` | 코어 덤프 파일 명명 패턴 |
| `/proc/sys/kernel/perf_event_paranoid` | `echo -1`로 root 없이 perf 허용 |
| `/proc/sys/vm/nr_hugepages` | 2MB 휴지페이지 수 설정 |
| `/proc/sys/vm/nr_overcommit_hugepages` | SPDK가 사용할 수 있는 추가 휴지페이지 |
| `/sys/bus/pci/devices/<BDF>/numa_node` | PCIe 디바이스의 NUMA 친화도 확인 |

### 유용한 원라이너

```bash
# 휴지페이지 할당 확인
cat /proc/meminfo | grep -i huge

# NUMA 토폴로지 표시
numactl --hardware

# IOMMU 활성화 여부 확인 (VFIO에 필요)
dmesg | grep -i iommu

# VFIO 바인딩된 디바이스 목록
ls /dev/vfio/

# NVMe 디바이스의 현재 UIO/VFIO 바인딩 확인
scripts/setup.sh status

# DPDK 디바이스 바인딩 확인
dpdk-devbind.py --status

# SPDK 프로세스 파일 디스크립터 모니터링
ls /proc/<PID>/fd | wc -l

# 실시간 휴지페이지 소비량 모니터링
watch -n1 'grep -i huge /proc/meminfo'
```

---

## 요약 카드

| 작업 | 명령어 |
|------|---------|
| 디버그 로그 활성화 | `--logflag all -L debug` |
| 실시간 로그 레벨 변경 | `scripts/rpc.py log_set_level DEBUG` |
| 트레이스 덤프 | `spdk_trace -s spdk_tgt -p <PID> -f out.bin` |
| 트레이스 분석 | `build/bin/spdk_trace -f out.bin` |
| 실시간 리액터 뷰 | `scripts/spdk_top.py` |
| I/O 통계 | `scripts/rpc.py bdev_get_iostat` |
| bdev 목록 | `scripts/rpc.py bdev_get_bdevs` |
| NVMe-oF 서브시스템 목록 | `scripts/rpc.py nvmf_get_subsystems` |
| GDB 연결 | `gdb -p <PID>` |
| 모든 스레드 백트레이스 | `(gdb) thread apply all bt` |
| ASan 빌드 | `./configure --enable-asan && make` |
| 코어 덤프 분석 | `gdb ./build/bin/spdk_tgt /tmp/core.*` |
| 휴지페이지 확인 | `grep -i huge /proc/meminfo` |
| 핫스팟 프로파일링 | `perf record -F 99 -p <PID> -g -- sleep 30` |

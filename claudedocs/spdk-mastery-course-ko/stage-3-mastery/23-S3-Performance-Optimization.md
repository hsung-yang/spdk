# 모듈 23: 성능 최적화 (Performance Optimization)

**단계**: 3 - 마스터리 (Mastery)
**선수 과목**: 모듈 1-22, SPDK 스레딩 모델, NVMe 드라이버 내부 구조, bdev 계층에 대한 확실한 이해

---

## 개요

SPDK에서의 성능 최적화는 체계적인 방법론이 필요합니다: 먼저 측정하고, 병목 지점(bottleneck)을 식별하고, 가장 작은 효과적인 변경을 적용한 후, 다시 측정합니다. SPDK의 실행 완료 폴링(run-to-completion polling) 모델은 커널 I/O 경로에서 발견되는 대부분의 오버헤드 — 인터럽트 처리, 컨텍스트 스위치, 락 경합, 스케줄러 지연 — 를 이미 제거합니다. 남아 있는 것은 더 미묘한 병목입니다: NUMA 정렬 불일치, 캐시 스래싱(cache thrashing), 최적이 아닌 큐 깊이(queue depth), 비효율적인 폴링 루프, 잘못 설정된 하드웨어 등입니다.

이 모듈에서는 프로파일링 도구부터 하드웨어 설정, 프로덕션 튜닝 패턴까지 전체 최적화 생명주기를 다룹니다.

---

## 1. SPDK 애플리케이션 프로파일링

### 1.1 CPU 프로파일링을 위한 Linux `perf`

`perf`는 CPU 바운드 최적화의 가장 정확한 시작점입니다. SPDK는 타이트한 폴링 루프에서 실행되므로 CPU 사용률이 높은 것이 당연합니다 — 목표는 CPU가 *무엇을* 하고 있는지 이해하는 것이지, 사용률 자체를 줄이는 것이 아닙니다.

```bash
# Record a call-graph profile of a running SPDK application
# -g enables call-graph (stack trace) recording
# -p targets a specific PID
sudo perf record -g -p $(pidof spdk_tgt) -- sleep 30

# Generate a human-readable report
sudo perf report --stdio --no-children | head -80

# Alternatively, open the interactive TUI
sudo perf report
```

휴지 페이지(huge pages)와 커스텀 할당자를 사용하는 애플리케이션의 경우, 99 Hz로 샘플링하기 위해 `-F 99`를 추가합니다(스케줄링 틱의 정확한 배수를 피하기 위함):

```bash
sudo perf record -F 99 -g -p $(pidof spdk_tgt) -- sleep 30
```

**결과 해석**: 잘 튜닝된 SPDK 애플리케이션에서 핫 패스(hot path)는 `spdk_nvme_qpair_process_completions`나 `spdk_bdev_io_complete`가 대부분의 사이클을 소비하는 것으로 나타나야 합니다. 메모리 할당(`rte_malloc`, `spdk_malloc`)이나 락 프리미티브에서 높은 시간을 보인다면, 할당 또는 잠금 문제가 있는 것입니다.

### 1.2 플레임 그래프 (Flame Graphs)

플레임 그래프는 `perf` 출력을 전체 호출 트리에서 CPU 시간이 어디에 소비되는지 보여주는 시각적 계층 구조로 변환합니다.

```bash
# Install FlameGraph tools (one-time setup)
git clone https://github.com/brendangregg/FlameGraph ~/flamegraph

# Capture stack traces
sudo perf record -F 99 -g -p $(pidof spdk_tgt) -- sleep 30
sudo perf script > out.perf

# Collapse and render
~/flamegraph/stackcollapse-perf.pl out.perf > out.folded
~/flamegraph/flamegraph.pl out.folded > flame.svg

# Open in browser
open flame.svg   # macOS
xdg-open flame.svg  # Linux
```

오프-CPU 분석(실행되지 않고 대기하는 시간):

```bash
# Requires BCC tools
sudo /usr/share/bcc/tools/offcputime -p $(pidof spdk_tgt) 30 > out.offcpu
~/flamegraph/flamegraph.pl --color=io --title="Off-CPU Time" --countname=us out.offcpu > offcpu.svg
```

SPDK의 오프-CPU 플레임 그래프는 의도적인 슬립이나 블로킹 시스템 콜이 없는 한 거의 0에 가까운 시간을 보여야 합니다. 그렇지 않다면 설계 문제를 나타냅니다.

### 1.3 SPDK 내장 트레이스 프레임워크

SPDK는 고성능, 락-프리(lock-free), 스레드별 트레이스 링 버퍼(trace ring buffer)를 갖추고 있습니다. 트레이스 포인트(trace point)는 NVMe 드라이버, bdev 계층, NVMe-oF 타겟 전체에 정의되어 있습니다. 오버헤드는 트레이스 포인트당 단일 원자적(atomic) 쓰기이므로, 프로덕션 환경에서도 사용할 수 있습니다.

**애플리케이션 시작 시 트레이싱 활성화**:

```bash
# Enable all trace point groups
./build/bin/spdk_tgt --tpoint-group-mask 0xFFFF

# Enable only NVMe trace points (group 0)
./build/bin/spdk_tgt --tpoint-group-mask 0x1

# Enable bdev trace points (group 1)
./build/bin/spdk_tgt --tpoint-group-mask 0x2

# Enable NVMe-oF trace points (group 3)
./build/bin/spdk_tgt --tpoint-group-mask 0x8
```

**캡처 및 분석**:

```bash
# Record trace data from a running application
# This reads from the shared memory trace buffer
sudo ./scripts/rpc.py trace_get_tpoint_group_mask

# Use the trace_record script to capture to a file
sudo ./build/bin/spdk_trace_record -p $(pidof spdk_tgt) -s spdk_tgt

# Analyze the captured trace
./build/bin/spdk_trace -f /dev/shm/spdk_tgt_trace.pid12345

# Generate a human-readable timeline
./build/bin/spdk_trace -f /dev/shm/spdk_tgt_trace.pid12345 -s 1000000
```

**애플리케이션에 커스텀 트레이스 포인트 추가하기**:

```c
#include "spdk/trace.h"

/* Define trace point group (application-specific, use high IDs) */
#define MY_APP_TRACE_GROUP  15

/* Define trace points */
SPDK_TRACE_REGISTER_FN(my_trace_init, "my_app", MY_APP_TRACE_GROUP)
{
    spdk_trace_register_description("MY_IO_START",
        SPDK_TRACE_TPOINT_ID(MY_APP_TRACE_GROUP, 0),
        OWNER_NONE, OBJECT_NONE, 0,
        SPDK_TRACE_ARG_TYPE_INT, "size");
    spdk_trace_register_description("MY_IO_COMPLETE",
        SPDK_TRACE_TPOINT_ID(MY_APP_TRACE_GROUP, 1),
        OWNER_NONE, OBJECT_NONE, 0,
        SPDK_TRACE_ARG_TYPE_INT, "latency_us");
}

/* Instrument your hot path */
void submit_my_io(size_t io_size)
{
    SPDK_TRACE_RECORD(SPDK_TRACE_TPOINT_ID(MY_APP_TRACE_GROUP, 0), 0, io_size, 0, 0);
    /* ... submit I/O ... */
}
```

### 1.4 spdk_top: 실시간 스레드 및 폴러 모니터링

`spdk_top`은 Linux `top` 유틸리티에 해당하는 SPDK 도구입니다. JSON-RPC를 통해 실행 중인 SPDK 애플리케이션에 연결하고 스레드별, 폴러별 통계를 ncurses 인터페이스로 표시합니다.

```bash
# Launch spdk_top against a running spdk_tgt
./build/bin/spdk_top -r /var/tmp/spdk.sock

# Key columns to watch:
# - Busy% : percentage of time the thread's pollers found work to do
# - Idle%  : percentage of time pollers returned 0 (no work)
# - Poller count: number of active pollers on this thread
```

**spdk_top 출력 해석**:

| Busy% | 해석 |
|-------|------|
| 95-100% | 스레드가 포화 상태; 리액터 추가 또는 부하 재분배 고려 |
| 50-80% | 여유가 있는 건강한 사용률 |
| <20% | 스레드가 저부하; 작업 통합이 캐시 지역성을 개선할 수 있음 |
| 0% | 스레드에 폴러가 없거나 모든 폴러가 유휴 — 낭비되는 코어일 수 있음 |

`spdk_top` 데이터는 `framework_get_reactors`와 `thread_get_stats` RPC 메서드에서 가져오며, 직접 쿼리할 수도 있습니다:

```bash
# Get per-thread statistics via RPC
./scripts/rpc.py thread_get_stats

# Get reactor (core) statistics
./scripts/rpc.py framework_get_reactors
```

### 1.5 히스토그램 기반 레이턴시 추적

SPDK는 최소한의 오버헤드로 프로세스 내 레이턴시 측정을 위한 `spdk_histogram_data`를 제공합니다. `spdk_nvme_perf` 애플리케이션(`app/spdk_nvme_perf/perf.c`)은 이 패턴을 광범위하게 사용하여 I/O별 TSC 델타를 추적합니다.

```c
#include "spdk/histogram_data.h"

struct spdk_histogram_data *latency_hist;

/* Initialize once */
latency_hist = spdk_histogram_data_alloc();
assert(latency_hist != NULL);

/* Per-I/O measurement (in completion callback) */
static void
io_complete_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = arg;
    uint64_t end_tsc = spdk_get_ticks();
    uint64_t latency_tsc = end_tsc - task->submit_tsc;

    /* Convert TSC to nanoseconds before recording */
    uint64_t latency_ns = latency_tsc * UINT64_C(1000000000) / spdk_get_ticks_hz();
    spdk_histogram_data_tally(latency_hist, latency_ns);

    /* Resubmit or mark completion */
}

/* Print percentile breakdown (called periodically) */
static void
print_latency_bucket(void *ctx, uint64_t start, uint64_t end,
                     uint64_t count, uint64_t total, uint64_t so_far)
{
    if (count == 0) return;
    double percentile = (double)so_far / total * 100.0;
    printf("[%6" PRIu64 "-%6" PRIu64 " ns] count=%" PRIu64 " cumulative=%.3f%%\n",
           start, end, count, percentile);
}

void print_latency_stats(void)
{
    spdk_histogram_data_iterate(latency_hist, print_latency_bucket, NULL);
    spdk_histogram_data_reset(latency_hist);
}
```

`spdk_nvme_perf`에서 사용하는 레이턴시 기준점은 주목할 만합니다 — P50, P90, P99, P99.9, P99.99, P99.999, P99.9999를 다루며, 이는 스토리지 성능 특성화에 적합한 세트입니다.

---

## 2. CPU 친화성(Affinity)과 NUMA 인식

### 2.1 리액터 CPU 할당

SPDK의 스레딩 모델은 CPU 코어당 하나의 리액터(reactor) 스레드를 매핑합니다. 올바른 CPU 할당은 성능에 가장 큰 영향을 미치는 단일 설정 결정입니다.

**JSON을 통한 설정**:

```json
{
  "subsystems": [
    {
      "subsystem": "scheduler",
      "config": [
        {
          "method": "framework_set_scheduler",
          "params": {
            "name": "static"
          }
        }
      ]
    }
  ]
}
```

**명령줄 CPU 마스크** (16진수 비트마스크):

```bash
# Use cores 0-3 (cores 0, 1, 2, 3)
./build/bin/spdk_tgt -m 0xF

# Use cores 4-7 on NUMA node 1
./build/bin/spdk_tgt -m 0xF0

# Use cores 2, 4, 6, 8 (even cores on NUMA node 0, skipping HT siblings)
./build/bin/spdk_tgt -m 0x154
```

**코어 할당 전 NUMA 토폴로지 확인**:

```bash
# Show NUMA node to CPU mapping
numactl --hardware

# Show which NVMe controllers are on which NUMA node
cat /sys/bus/pci/devices/0000:01:00.0/numa_node

# Show NUMA statistics for a process
numastat -p $(pidof spdk_tgt)
```

### 2.2 NUMA 인식 메모리 할당

SPDK는 메모리 관리를 위해 DPDK의 EAL을 사용합니다. 휴지 페이지 메모리는 가장 자주 접근하는 CPU와 동일한 NUMA 노드에 할당되어야 합니다.

```bash
# Allocate 4GB of hugepages on NUMA node 0 only
echo 2048 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages

# For NUMA node 1
echo 2048 > /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages

# Pass NUMA-specific memory to SPDK (2GB on node0, 2GB on node1)
./build/bin/spdk_tgt -m 0xFF --mem-size 4096 --main-core 0
```

**애플리케이션 코드에서 로컬 NUMA 노드에 할당하기**:

```c
#include "spdk/env.h"

/* Allocate on the NUMA node of the current CPU */
int numa_node = spdk_env_get_current_core();  /* returns socket-aware core ID */
void *buf = spdk_malloc(size, alignment, NULL, numa_node, SPDK_MALLOC_DMA);

/* For I/O buffers, always use SPDK_MALLOC_DMA to ensure DMA-capability */
struct spdk_dma_buf *dma_buf = spdk_dma_malloc(io_size, 4096, NULL);
```

### 2.3 하이퍼스레딩 시블링 회피

하이퍼스레딩(Hyperthreading) 시블링은 L1/L2 캐시와 실행 유닛을 공유합니다. 두 개의 SPDK 리액터 스레드를 HT 시블링에서 실행하면 물리 코어 두 개를 사용하는 것에 비해 성능이 저하됩니다.

```bash
# Identify HT sibling pairs
cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list
# Output: 0,16  (core 0 has HT siblings 0 and 16)

# Script to find all physical cores (avoiding HT siblings)
for cpu in /sys/devices/system/cpu/cpu*/topology/core_id; do
    echo "$(dirname $cpu | sed 's/.*cpu//'): $(cat $cpu)"
done | sort -t: -k2 -n | awk -F: '!seen[$2]++'
```

**규칙**: 물리 코어당 하나의 SPDK 리액터를 할당합니다. 의심스러우면 낮은 번호의 시블링을 사용하세요.

### 2.4 CPU 주파수 스케일링

SPDK 폴링 루프는 타이밍을 위해 `spdk_get_ticks()` (RDTSC)에 의존합니다. 가변 CPU 주파수는 타이밍 드리프트를 유발하고 레이턴시 일관성을 해칠 수 있습니다.

```bash
# Set all CPUs to performance governor
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > $cpu
done

# Disable Intel Turbo Boost (for consistent latency benchmarking)
echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo

# Verify current frequency
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
```

---

## 3. 메모리 접근 패턴과 캐시 최적화

### 3.1 캐시 라인 정렬 (Cache Line Alignment)

핫 패스에서 접근하는 SPDK 구조체는 거짓 공유(false sharing)를 방지하고 단일 캐시 라인 읽기를 보장하기 위해 캐시 라인 정렬되어야 합니다.

```c
#include "spdk/util.h"

/* SPDK_CACHE_LINE_SIZE is 64 bytes on x86 */
struct hot_path_ctx {
    /* Fields accessed in the poller loop */
    uint64_t            submit_count;
    uint64_t            complete_count;
    uint64_t            current_depth;
    struct spdk_nvme_qpair *qpair;

    /* Pad to cache line boundary */
} __attribute__((aligned(SPDK_CACHE_LINE_SIZE)));

/* For per-thread data that should not be shared between threads */
struct per_thread_stats {
    uint64_t    io_submitted;
    uint64_t    io_completed;
    uint64_t    bytes_read;
    uint64_t    bytes_written;
    char        _pad[SPDK_CACHE_LINE_SIZE -
                     (4 * sizeof(uint64_t)) % SPDK_CACHE_LINE_SIZE];
} __attribute__((aligned(SPDK_CACHE_LINE_SIZE)));
```

### 3.2 거짓 공유 방지

거짓 공유(false sharing)는 두 스레드가 동일한 캐시 라인에 있는 서로 다른 필드를 수정할 때 발생합니다. 이는 SPDK의 통계 수집 코드에서 특히 문제가 됩니다.

```c
/* BAD: Two threads updating adjacent fields */
struct shared_counters {
    uint64_t thread0_count;   /* cache line 0 */
    uint64_t thread1_count;   /* same cache line 0 -- FALSE SHARING */
};

/* GOOD: Each thread's counter on its own cache line */
struct per_thread_counter {
    uint64_t count;
    uint64_t _pad[7];  /* pad to 64 bytes */
} __attribute__((aligned(64)));

struct per_thread_counter counters[MAX_THREADS];
```

### 3.3 I/O 버퍼 프리페칭 (Prefetching)

순차적 워크로드의 경우, 다음 I/O 버퍼를 필요하기 전에 프리페치하면 메모리 레이턴시를 숨길 수 있습니다:

```c
/* Prefetch the next task's buffer while processing the current one */
static void
process_completion(struct io_task *task)
{
    struct io_task *next_task = get_next_task();
    if (next_task) {
        /* Prefetch for read: hint that data will be read soon */
        __builtin_prefetch(next_task->buf, 0, 1);
    }

    /* Process current task */
    handle_completion(task);
}
```

### 3.4 휴지 페이지 설정

모든 SPDK DMA 버퍼는 DMA 연산 중 TLB 미스를 방지하기 위해 휴지 페이지(hugepage) 메모리에 상주해야 합니다.

```bash
# Check current hugepage allocation
cat /proc/meminfo | grep -i huge

# Allocate 1GB hugepages (requires kernel support and hardware support)
echo 8 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages

# Allocate 2MB hugepages (standard)
echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Mount hugetlbfs if not auto-mounted
mount -t hugetlbfs nodev /mnt/huge

# Verify hugepage usage after starting SPDK
cat /proc/meminfo | grep HugePages_
```

**휴지 페이지 할당 크기 산정**: 일반적인 규칙은 모든 리액터에 걸친 모든 I/O 버퍼 풀 합의 2배입니다. 8개 리액터, 리액터당 512개 미처리 I/O, 128KB I/O 크기의 시스템: 8 x 512 x 128KB = 최소 512MB, 1-2GB를 할당합니다.

---

## 4. 락 경합 분석

SPDK의 설계 철학은 I/O 패스트 패스(fast path)에서 락을 완전히 제거하는 것입니다. 각 리액터 스레드가 자신의 데이터 구조를 독점적으로 소유합니다. 그러나 락은 다음에서 나타납니다:

- 초기화 경로
- 공유 설정 구조체
- RPC 핸들러 경로
- 크로스-스레드 메시지 전달 (spdk_thread_send_msg)

### 4.1 락-프리 동작 확인

```bash
# Check for mutex contention using perf lock
sudo perf lock record -p $(pidof spdk_tgt) -- sleep 5
sudo perf lock report

# If you see pthread_mutex_lock in the top symbols during steady-state I/O,
# there is a design problem — locks should not appear in the polling path.
```

### 4.2 크로스-스레드 통신 패턴

SPDK는 크로스-스레드 통신에 락 대신 메시지 전달(message passing)을 사용합니다. 패턴은 다음과 같습니다:

```c
struct migrate_ctx {
    struct spdk_bdev_desc   *desc;
    struct spdk_io_channel  *new_channel;
    spdk_msg_fn              cb;
    void                    *cb_arg;
};

/* Called on the destination thread */
static void
complete_migration(void *arg)
{
    struct migrate_ctx *ctx = arg;
    ctx->cb(ctx->cb_arg);
    free(ctx);
}

/* Initiate cross-thread operation — no lock needed */
void
migrate_io_channel(struct spdk_thread *dest_thread,
                   spdk_msg_fn cb, void *cb_arg)
{
    struct migrate_ctx *ctx = calloc(1, sizeof(*ctx));
    ctx->cb = cb;
    ctx->cb_arg = cb_arg;

    /* This enqueues a message; dest_thread polls its message queue */
    spdk_thread_send_msg(dest_thread, complete_migration, ctx);
}
```

### 4.3 의도치 않은 잠금 식별

```bash
# Use strace to check for futex calls (mutex implementation) during I/O
sudo strace -p $(pidof spdk_tgt) -e trace=futex -c -- sleep 10

# Non-zero futex count during steady-state I/O is a red flag.
# Expected: futex calls only during startup/teardown and RPC handling.

# Use lockstat (if available) or BPF mutex tracking
sudo bpftrace -e 'tracepoint:lock:contention_begin { @[kstack] = count(); }'
```

---

## 5. I/O 깊이 튜닝과 큐 깊이 최적화

큐 깊이(queue depth)는 가장 영향력 있는 튜닝 파라미터 중 하나입니다. 너무 낮으면 디바이스 대역폭을 활용하지 못하고, 너무 높으면 추가 처리량 이득 없이 평균 레이턴시만 증가합니다.

### 5.1 최적 큐 깊이 찾기

최적 큐 깊이는 디바이스의 내부 병렬성과 애플리케이션이 요구하는 레이턴시/처리량 트레이드오프에 따라 달라집니다. 대부분의 NVMe SSD의 경우:

| 큐 깊이 | 일반적인 효과 |
|---------|--------------|
| 1 | 최대 레이턴시 가시성, 최소 처리량 |
| 4-8 | 적당한 처리량과 좋은 레이턴시 |
| 32-64 | 대부분의 컨슈머/프로슈머 NVMe에서 거의 최대 처리량 |
| 128-512 | 높은 내부 병렬성을 가진 엔터프라이즈 NVMe SSD |
| >512 | 거의 이점 없음; 테일 레이턴시가 증가할 수 있음 |

### 5.2 spdk_nvme_perf에서의 큐 깊이

`spdk_nvme_perf` 애플리케이션(`app/spdk_nvme_perf/perf.c`)은 큐 깊이 튜닝을 직접 노출합니다:

```bash
# Test with queue depth 1 (pure latency measurement)
./build/bin/spdk_nvme_perf -q 1 -o 4096 -w randread -t 30 -r 'trtype:PCIe traddr:0000:01:00.0'

# Test with queue depth 32 (throughput-oriented)
./build/bin/spdk_nvme_perf -q 32 -o 4096 -w randread -t 30 -r 'trtype:PCIe traddr:0000:01:00.0'

# Test with queue depth 128 (high-throughput enterprise SSDs)
./build/bin/spdk_nvme_perf -q 128 -o 128k -w randread -t 30 -r 'trtype:PCIe traddr:0000:01:00.0'

# Multiple namespaces with per-namespace queue count
./build/bin/spdk_nvme_perf -q 32 -o 4096 -w randrw -M 70 -t 30 \
    -r 'trtype:PCIe traddr:0000:01:00.0' \
    -r 'trtype:PCIe traddr:0000:02:00.0'
```

### 5.3 NVMe 큐 페어 설정

NVMe 드라이버의 큐 페어(queue pair) 크기는 애플리케이션 레벨 큐 깊이와 별도로 제어됩니다:

```c
struct spdk_nvme_io_qpair_opts opts;

/* Get defaults */
spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));

/* io_queue_size: number of entries in the NVMe submission/completion queues
 * Must be a power of 2. Max is MQES (from controller capabilities).
 * Default UINT16_MAX causes driver to use MQES automatically. */
opts.io_queue_size = 1024;

/* io_queue_requests: number of spdk_nvme_request objects pre-allocated
 * for this queue pair. Must be >= io_queue_size.
 * Higher values reduce allocation overhead for deep queues. */
opts.io_queue_requests = 2048;

/* delay_cmd_submit: batch command submissions for throughput optimization
 * Set to true to accumulate commands and submit them together */
opts.delay_cmd_submit = true;

struct spdk_nvme_qpair *qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
```

### 5.4 큐 깊이 스위프 스크립트

```bash
#!/bin/bash
# Queue depth sweep for a single NVMe device
TRADDR="trtype:PCIe traddr:0000:01:00.0"
IO_SIZE=4096
DURATION=10

echo "QD,IOPS,LatencyAvg_us,LatencyP99_us"
for QD in 1 2 4 8 16 32 64 128 256; do
    OUTPUT=$(./build/bin/spdk_nvme_perf \
        -q $QD -o $IO_SIZE -w randread -t $DURATION -r "$TRADDR" 2>/dev/null)
    IOPS=$(echo "$OUTPUT" | grep "IOPS" | awk '{print $NF}')
    echo "$QD,$IOPS"
done
```

---

## 6. 배치 처리 전략

### 6.1 제출 배치 (Submission Batching)

`spdk_nvme_qpair_process_completions`를 호출하기 전에 여러 I/O를 제출하면 NVMe 컨트롤러가 내부적으로 연산을 파이프라인할 수 있습니다. 이것은 SPDK 애플리케이션의 기본 패턴입니다.

```c
static int
run_poller(void *arg)
{
    struct worker_ctx *ctx = arg;
    int submitted = 0;

    /* Fill the queue to the target depth */
    while (ctx->current_depth < ctx->target_depth) {
        struct io_task *task = get_next_task(ctx);
        if (!task) break;

        task->submit_tsc = spdk_get_ticks();
        int rc = spdk_nvme_ns_cmd_read(ctx->ns, ctx->qpair,
                                        task->buf, task->lba,
                                        task->num_blocks,
                                        io_complete_cb, task, 0);
        if (rc == 0) {
            ctx->current_depth++;
            submitted++;
        } else if (rc == -ENOMEM) {
            /* Queue is full, stop submitting */
            break;
        }
    }

    /* Process completions — returns number of completions processed */
    int completed = spdk_nvme_qpair_process_completions(ctx->qpair, 0);

    /* Return SPDK_POLLER_BUSY if any work was done, IDLE otherwise */
    return (submitted > 0 || completed > 0) ?
           SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}
```

### 6.2 완료 배치 (Completion Batching)

`spdk_nvme_qpair_process_completions`는 `max_completions` 인수를 받습니다. 0으로 설정하면 한 번의 호출로 사용 가능한 모든 완료를 처리하여 완료당 오버헤드를 줄입니다.

```c
/* Process up to 32 completions per poll — limits maximum latency spike */
int completed = spdk_nvme_qpair_process_completions(qpair, 32);

/* Process all available completions — maximum throughput */
int completed = spdk_nvme_qpair_process_completions(qpair, 0);
```

레이턴시에 민감한 워크로드의 경우, 호출당 완료 수를 제한하여(예: 8-16) 폴러 루프가 빠르게 반환하고 새 제출을 확인할 수 있도록 합니다. 처리량 지향 워크로드의 경우 0을 사용합니다.

### 6.3 다중 큐 페어를 위한 폴 그룹 (Poll Group)

여러 NVMe 네임스페이스나 큐 페어를 관리할 때, 폴 그룹을 사용하여 폴러 등록 수를 줄입니다:

```c
/* Create a poll group for this thread */
struct spdk_nvme_poll_group *group = spdk_nvme_poll_group_create(NULL, NULL);

/* Add all queue pairs to the group */
for (int i = 0; i < num_qpairs; i++) {
    spdk_nvme_poll_group_add(group, qpairs[i]);
}

/* In the poller: one call processes completions for all qpairs */
static int
poll_group_poller(void *arg)
{
    struct spdk_nvme_poll_group *group = arg;

    int64_t completed = spdk_nvme_poll_group_process_completions(
        group, 0, disconnected_qpair_cb);

    return completed > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}
```

---

## 7. 하드웨어 큐 설정

### 7.1 NVMe 컨트롤러 능력 (Capabilities)

큐 깊이를 설정하기 전에 컨트롤러의 한계를 파악합니다:

```bash
# Use SPDK's identify controller utility
./build/bin/spdk_nvme_identify -r 'trtype:PCIe traddr:0000:01:00.0'

# Key fields to note:
# MQES (Maximum Queue Entries Supported) - max queue depth per queue pair
# NCQR (Number of Completion Queues Requested)
# NSQR (Number of Submission Queues Requested)
```

```c
/* Query capabilities programmatically */
const struct spdk_nvme_ctrlr_data *ctrlr_data = spdk_nvme_ctrlr_get_data(ctrlr);
union spdk_nvme_cap_register cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);

uint32_t mqes = cap.bits.mqes + 1;  /* MQES is 0-based */
printf("Max queue entries: %u\n", mqes);
printf("Max IO queues: %u\n", ctrlr_data->mqes);
```

### 7.2 네임스페이스당 큐 페어 수

멀티코어 SPDK 애플리케이션의 경우, 각 리액터 스레드는 네임스페이스당 고유한 큐 페어를 가져야 합니다. 이렇게 하면 모든 큐 페어 경합이 제거됩니다.

```c
/* Allocate one queue pair per reactor thread */
int num_reactors = spdk_env_get_core_count();
struct spdk_nvme_qpair **qpairs = calloc(num_reactors, sizeof(*qpairs));

SPDK_ENV_FOREACH_CORE(core_id) {
    struct spdk_nvme_io_qpair_opts opts;
    spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
    opts.io_queue_size = 256;
    opts.io_queue_requests = 512;

    qpairs[core_index] = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
    core_index++;
}
```

### 7.3 PCIe 설정

NVMe 성능은 PCIe 링크 폭(width)과 세대(generation)에 따라 달라집니다. 링크가 의도된 설정으로 실행되고 있는지 확인합니다:

```bash
# Check PCIe link status for NVMe device
sudo lspci -vv -s 0000:01:00.0 | grep -E "LnkSta|LnkCap"
# LnkCap: Port #0, Speed 16GT/s, Width x4   (physical capability)
# LnkSta: Speed 16GT/s, Width x4            (actual running state)

# If LnkSta width < LnkCap width, the slot may be electrically x1
# despite being physically x4 — a hardware configuration problem.

# Enable ASPM (Active State Power Management) L0s/L1 for power savings
# DISABLE ASPM for latency-critical NVMe (it adds latency on wake)
sudo setpci -s 0000:01:00.0 CAP_EXP+0x10.w=0x0142  # Disable ASPM
```

---

## 8. 폴링 모드 vs. 인터럽트 모드 트레이드오프

### 8.1 폴링 모드 (기본값)

SPDK의 기본 모드는 순수 폴링입니다 — 리액터 스레드가 완료를 지속적으로 확인합니다. 이 접근 방식은 리액터 코어에서 100% CPU 사용률을 대가로 가장 낮은 레이턴시와 가장 높은 처리량을 제공합니다.

**폴링을 사용해야 할 때**:
- 레이턴시 중요 애플리케이션 (P99 < 100us)
- 고처리량 애플리케이션 (>500K IOPS)
- CPU 코어가 스토리지 전용인 환경 (시분할 없음)

**폴링 오버헤드 분석**:

```bash
# Measure cycles per poll when the device is idle
# Look for spdk_nvme_qpair_process_completions in perf output during idle
sudo perf stat -p $(pidof spdk_tgt) -e cycles,instructions,cache-misses -- sleep 5

# Expected: high cycles, high instructions, very low cache-misses
# (polling is cache-friendly — same code path repeatedly)
```

### 8.2 인터럽트 모드

NVMe-oF 이니시에이터와 일부 특수 사용 사례는 인터럽트 기반 완료를 지원합니다. SPDK는 `SPDK_NVME_QPAIR_FAILURE_FUNC`와 트랜스포트별 옵션을 통해 이를 지원합니다.

```c
/* For NVMe-oF TCP transport, configure interrupt mode */
struct spdk_nvme_transport_opts transport_opts;
spdk_nvme_transport_get_opts(&transport_opts, sizeof(transport_opts));

/* sock_impl controls whether the TCP socket uses busy-poll or epoll */
transport_opts.rdma_srq_size = 4096;  /* RDMA-specific */

/* For TCP: set sock_impl to "posix" for interrupt mode
 * or "uring" for io_uring-based I/O */
```

```bash
# Configure interrupt mode for NVMe-oF TCP transport
./scripts/rpc.py sock_set_default_impl posix   # epoll-based
./scripts/rpc.py sock_set_default_impl uring   # io_uring-based (lower overhead)
```

### 8.3 적응형 폴링 (혼합 워크로드)

버스트성 I/O 패턴이 있는 워크로드의 경우, SPDK의 스케줄러는 유휴 기간 동안 인터럽트 모드로 전환하는 적응형 폴링을 지원합니다:

```bash
# Enable the dynamic scheduler
./scripts/rpc.py framework_set_scheduler dynamic

# Configure scheduler parameters
./scripts/rpc.py framework_set_scheduler_options \
    --core-limit 95 \    # Back off when CPU is >95% busy
    --period 1000000     # Re-evaluate every 1ms
```

동적 스케줄러는 폴러 반환 값(`SPDK_POLLER_BUSY` vs `SPDK_POLLER_IDLE`)을 모니터링하고, 유휴 스레드를 타이머 인터럽트에서 슬립 상태로 전환하여 인터럽트 전용 모드로 완전히 전환하지 않고도 전력 소비를 줄일 수 있습니다.

---

## 9. NVMe-oF 전용 성능 튜닝

### 9.1 트랜스포트 설정

```bash
# Create NVMe-oF TCP transport with tuned parameters
./scripts/rpc.py nvmf_create_transport \
    -t TCP \
    --max-queue-depth 128 \
    --max-io-qpairs-per-ctrlr 8 \
    --io-unit-size 131072 \
    --max-aq-depth 32 \
    --num-shared-buffers 4096 \
    --buf-cache-size 64 \
    --dif-insert-or-strip \
    --sock-priority 6

# Create NVMe-oF RDMA transport
./scripts/rpc.py nvmf_create_transport \
    -t RDMA \
    --max-queue-depth 128 \
    --max-io-qpairs-per-ctrlr 4 \
    --io-unit-size 131072 \
    --num-shared-buffers 8192
```

**주요 트랜스포트 파라미터**:

| 파라미터 | 효과 | 튜닝 방향 |
|---------|------|----------|
| `max-queue-depth` | 큐 페어당 최대 미처리 I/O | 처리량 위해 증가, 레이턴시 위해 감소 |
| `num-shared-buffers` | 사전 할당된 네트워크 I/O 버퍼 | 버퍼 할당 오류가 사라질 때까지 증가 |
| `buf-cache-size` | 스레드당 버퍼 캐시 크기 | 버퍼 풀의 락 경합을 줄이기 위해 증가 |
| `io-unit-size` | 최대 캡슐/PDU 데이터 크기 | 애플리케이션 I/O 크기에 맞춤 |

### 9.2 TCP 트랜스포트를 위한 네트워크 스택 튜닝

```bash
# Increase TCP socket buffer sizes
sysctl -w net.core.rmem_max=134217728
sysctl -w net.core.wmem_max=134217728
sysctl -w net.ipv4.tcp_rmem="4096 87380 134217728"
sysctl -w net.ipv4.tcp_wmem="4096 65536 134217728"

# Enable TCP fast open
sysctl -w net.ipv4.tcp_fastopen=3

# Disable Nagle algorithm for low-latency (SPDK NVMe-oF TCP does this automatically)
# setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag))

# For SPDK sock layer, use the uring implementation for lower overhead
./scripts/rpc.py sock_impl_get_options uring
./scripts/rpc.py sock_impl_set_options uring \
    --recv-buf-size 2097152 \
    --send-buf-size 2097152 \
    --enable-placement-id 1
```

### 9.3 RDMA 트랜스포트 튜닝

```bash
# Verify RDMA link speed and MTU
ibv_devinfo -v | grep -E "active_mtu|active_speed|active_width"

# Set MTU to 4096 for better large I/O efficiency
ibv_devinfo | grep hca_id  # Get device name
sudo ibv_devinfo -d mlx5_0 | grep active_mtu

# Configure RoCE/RoCEv2 (required for RDMA over Ethernet)
# Check with: rdma dev show
# Configure DSCP marking for RoCE:
cma_roce_tos -d mlx5_0 -t 106  # DSCP 26 for storage traffic

# Increase RDMA completion queue size
./scripts/rpc.py nvmf_create_transport -t RDMA \
    --max-queue-depth 256 \
    --num-shared-buffers 16384
```

### 9.4 NVMe-oF 이니시에이터 (호스트 측) 튜닝

```bash
# Connect to NVMe-oF target with optimized parameters
./build/bin/spdk_nvme_perf \
    -r 'trtype:TCP adrfam:IPv4 traddr:192.168.1.100 trsvcid:4420 subnqn:nqn.2023-01.io.spdk:cnode1' \
    -q 64 \
    -o 4096 \
    -w randread \
    -t 60 \
    --io-queue-size 256 \
    --keepalive-timeout 10000

# For multi-path (connecting to same subsystem via multiple paths)
./build/bin/spdk_nvme_perf \
    -r 'trtype:TCP adrfam:IPv4 traddr:192.168.1.100 trsvcid:4420' \
    -r 'trtype:TCP adrfam:IPv4 traddr:192.168.2.100 trsvcid:4420' \
    -q 64 -o 4096 -w randread -t 60
```

---

## 10. 벤치마킹 방법론

### 10.1 bdevperf

`bdevperf`(`examples/bdev/bdevperf/bdevperf.c`)는 주요 SPDK 블록 디바이스 벤치마크 도구입니다. bdev 추상화 계층 위에서 동작하므로 모든 bdev 타입을 테스트하는 데 적합합니다.

```bash
# Basic bdevperf invocation
./build/bin/bdevperf \
    -c bdev.conf \
    -q 128 \
    -o 4096 \
    -w randread \
    -t 30

# Configuration file format
cat > bdev.conf << 'EOF'
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_nvme_attach_controller",
          "params": {
            "name": "Nvme0",
            "trtype": "pcie",
            "traddr": "0000:01:00.0"
          }
        }
      ]
    }
  ]
}
EOF

# Test a null bdev (CPU overhead measurement only)
./build/bin/bdevperf \
    --json <(echo '{"subsystems":[{"subsystem":"bdev","config":[{"method":"bdev_null_create","params":{"name":"Null0","num_blocks":102400,"block_size":512}}]}]}') \
    -q 128 -o 4096 -w randread -t 10

# Sequential write test
./build/bin/bdevperf -c bdev.conf -q 1 -o 131072 -w write -t 30

# Mixed read/write (70% read)
./build/bin/bdevperf -c bdev.conf -q 64 -o 4096 -w randrw -M 70 -t 60
```

### 10.2 spdk_nvme_perf

`spdk_nvme_perf`(`app/spdk_nvme_perf/perf.c`)는 bdev 계층을 우회하여 NVMe 드라이버와 직접 동작합니다. 레이턴시 히스토그램 출력을 포함하고 다중 워커 스레드를 지원합니다.

```bash
# Single device, multiple workers (one per core)
./build/bin/spdk_nvme_perf \
    -r 'trtype:PCIe traddr:0000:01:00.0' \
    -q 32 \
    -o 4096 \
    -w randread \
    -t 30 \
    -c 0xF \            # Use cores 0-3
    -i 1 \              # Shared memory key (for multi-process)
    --latency-tracking  # Enable per-I/O latency histogram

# Multi-device test (aggregate bandwidth)
./build/bin/spdk_nvme_perf \
    -r 'trtype:PCIe traddr:0000:01:00.0' \
    -r 'trtype:PCIe traddr:0000:02:00.0' \
    -q 64 -o 128k -w read -t 30

# NVMe-oF performance test
./build/bin/spdk_nvme_perf \
    -r 'trtype:TCP adrfam:IPv4 traddr:10.0.0.1 trsvcid:4420 subnqn:nqn.2023.spdk:cnode0' \
    -q 64 -o 4096 -w randread -t 60 \
    --io-queue-size 128 \
    -c 0x3  # Two cores
```

**spdk_nvme_perf 출력 읽기**:

```
Device Information
  Controller Name: INTEL_P4610
  Transport type: PCIe
  Namespace id: 1
  Namespace size: 3200 GB
  Queue pairs per namespace: 1
  IO size: 4096
  IO pattern: randread
  Queue depth: 32

========================================================
                      IOPS          MiB/s    Average      min         P50         P99      P99.999      max
INTEL_P4610 IOPS: 750413.45        2931.30    42.61      9.15      40.99     104.45      498.23     1201.45
========================================================
```

### 10.3 SPDK 플러그인을 사용한 FIO

SPDK FIO 플러그인은 fio의 작업 파일 구문을 SPDK를 I/O 엔진으로 사용하여 복잡한 워크로드 모델링을 가능하게 합니다.

```bash
# Build the SPDK fio plugin
make -C examples/nvme/fio_plugin/

# Create fio job file
cat > spdk_test.fio << 'EOF'
[global]
ioengine=spdk
thread=1
group_reporting=1
direct=1
verify=0
time_based=1
ramp_time=5
runtime=30
iodepth=64
bs=4k
rw=randread

[job1]
filename=trtype=PCIe traddr=0000:01:00.0 ns=1

[job2]
filename=trtype=PCIe traddr=0000:02:00.0 ns=1
EOF

# Run with SPDK fio plugin
sudo LD_PRELOAD=examples/nvme/fio_plugin/fio_plugin fio spdk_test.fio \
    --spdk_mem=4096 --spdk_single_seg=1

# Latency percentile reporting
cat >> spdk_test.fio << 'EOF'
percentile_list=1:5:10:25:50:75:90:95:99:99.5:99.9:99.99:99.999
EOF
```

### 10.4 벤치마킹 방법론 체크리스트

벤치마크를 실행하기 전에:

1. **시스템 안정화**: 측정 전에 5-10분간 워밍업 I/O 워크로드를 실행합니다. NVMe SSD는 컨디셔닝 중에 안정화되는 쓰기 증폭(write amplification) 효과가 있습니다.
2. **전력 관리 비활성화**: CPU 거버너를 `performance`로 설정하고 ASPM을 비활성화합니다.
3. **CPU 고정**: 리액터를 HT 시블링이 아닌 물리 코어에 할당합니다.
4. **충분한 휴지 페이지 할당**: 예상 버퍼 작업 세트의 최소 2배.
5. **OS 스케줄러로부터 CPU 격리**: `isolcpus`와 `nohz_full` 커널 파라미터를 사용합니다.
6. **기준 메트릭 기록**: CPU 사용률, 메모리 대역폭, PCIe 오류 카운터.
7. **관심 변수 스위프**: 한 번에 하나의 변수만 변경(큐 깊이, I/O 크기, 코어 수).
8. **통계적 지표 보고**: 평균, P50, P99, P99.9 — 절대 평균 레이턴시만 보고하지 마세요.

```bash
# Kernel parameters for benchmarking isolation
# Add to /etc/default/grub GRUB_CMDLINE_LINUX:
# isolcpus=2-15 nohz_full=2-15 rcu_nocbs=2-15 intel_pstate=disable

# Verify isolation
cat /sys/devices/system/cpu/isolated  # Should show 2-15

# Use taskset to pin SPDK to isolated cores
taskset -c 2-15 ./build/bin/spdk_tgt -m 0xFFFC
```

---

## 11. 실제 성능 튜닝 예제

### 11.1 예제: 저지연 NVMe 최적화

**이전**: 4KB 랜덤 읽기, 단일 큐 깊이 — P99 = 250us

**증상**: `perf report`에서 `spdk_malloc`(핫 패스의 버퍼 할당)에 15%의 시간이 소비됨.

**조사**:

```bash
# Check if buffer pool is being exhausted
./scripts/rpc.py bdev_get_bdevs | python3 -c "
import json,sys
data = json.load(sys.stdin)
for bdev in data:
    if 'driver_specific' in bdev:
        print(bdev['name'], bdev.get('claimed_by',''))
"

# Enable trace to see buffer wait events
./scripts/rpc.py trace_set_tpoint_group_mask 0xFF
```

**수정**: 최대 큐 깊이에 맞게 버퍼 풀을 사전 할당:

```c
/* Pre-allocate all I/O buffers at initialization */
#define MAX_IO_SIZE     (128 * 1024)
#define QUEUE_DEPTH     128

struct io_task tasks[QUEUE_DEPTH];

for (int i = 0; i < QUEUE_DEPTH; i++) {
    tasks[i].buf = spdk_dma_malloc(MAX_IO_SIZE, 4096, NULL);
    assert(tasks[i].buf != NULL);
}
/* Free list management replaces malloc/free in hot path */
```

**이후**: P99 = 85us (66% 개선). 임계 경로에서 버퍼 할당이 제거됨.

### 11.2 예제: NUMA 잘못된 설정

**이전**: 2개 NUMA 노드에 걸친 8개 NVMe 디바이스에서 4M IOPS 예상, 2.8M IOPS 관측.

**증상**: `numastat -p $(pidof spdk_tgt)`에서 노드 1의 높은 `numa_miss` 표시.

```bash
numastat -p $(pidof spdk_tgt)
# Output:
# Per-node process memory usage (in MBs) for PID 12345
#                            Node 0          Node 1           Total
#                   -------- --------------- --------------- ---------------
# Huge                         1024.00          0.00         1024.00  <-- ALL on node 0
# ...
# numa_miss:                     0.00       148392.00              <-- Remote access
```

**수정**: NUMA 노드별로 리액터와 휴지 페이지를 분할:

```bash
# Allocate hugepages on both nodes
echo 1024 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
echo 1024 > /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages

# Run SPDK with NUMA-aware core mask
# Node 0 devices: 0000:01:00.0, 0000:02:00.0, 0000:03:00.0, 0000:04:00.0
# Node 1 devices: 0000:81:00.0, 0000:82:00.0, 0000:83:00.0, 0000:84:00.0

# Node 0 cores: 0-7, Node 1 cores: 8-15
./build/bin/spdk_tgt -m 0xFFFF  # All cores; DPDK will use NUMA-local memory
```

**이후**: 3.9M IOPS (39% 개선). 원격 NUMA 접근 제거됨.

### 11.3 예제: NVMe-oF TCP 처리량 최적화

**이전**: 단일 연결 TCP NVMe-oF에서 순차 읽기 2GB/s 달성, 타겟 디바이스 능력은 6GB/s.

**증상**: `sar -n DEV 1`에서 네트워크 인터페이스가 ~16Gbps (2GB/s)로 100GbE 라인 레이트보다 훨씬 낮음.

**조사**: CPU 프로파일링에서 NVMe-oF 타겟의 TCP/IP 스택 처리에 40% 시간 소비됨.

**수정 1**: 버퍼 부족 방지를 위해 공유 버퍼 수를 늘림:

```bash
./scripts/rpc.py nvmf_create_transport -t TCP \
    --num-shared-buffers 8192 \    # Was 512
    --buf-cache-size 256 \          # Per-thread buffer cache
    --io-unit-size 131072           # Match to 128KB I/O size
```

**수정 2**: 여러 타겟 리액터에 분산하는 이니시에이터 연결 추가:

```bash
# Use 4 connections in parallel (fio multipath)
cat > nvmf_multi.fio << 'EOF'
[global]
ioengine=spdk
iodepth=64
bs=128k
rw=read

[conn1]
filename=trtype=TCP adrfam=IPv4 traddr=10.0.0.1 trsvcid=4420 ns=1
[conn2]
filename=trtype=TCP adrfam=IPv4 traddr=10.0.0.1 trsvcid=4421 ns=1
[conn3]
filename=trtype=TCP adrfam=IPv4 traddr=10.0.0.2 trsvcid=4420 ns=1
[conn4]
filename=trtype=TCP adrfam=IPv4 traddr=10.0.0.2 trsvcid=4421 ns=1
EOF
```

**이후**: 5.8GB/s (190% 개선). 버퍼 부족 제거, 리액터 간 부하 분산.

---

## 12. 일반적인 성능 함정

### 12.1 리액터 스레드에서의 블로킹 호출

**증상**: 높은 레이턴시 스파이크, 같은 스레드의 다른 폴러에서도 레이턴시 증가.

**원인**: 폴러 내부의 블로킹 시스템 콜(`sleep`, 블로킹 fd에 대한 `read`, 메모리 압박 하의 `malloc`)이 해당 스레드의 모든 다른 폴러를 정지시킴.

```c
/* WRONG: Never block in a poller */
static int bad_poller(void *arg)
{
    sleep(1);  /* Blocks the entire reactor thread */
    return SPDK_POLLER_IDLE;
}

/* CORRECT: Use SPDK timer for deferred work */
static int setup_deferred_work(void *arg)
{
    struct spdk_poller *timer;
    /* One-shot timer fires after 1 second */
    timer = spdk_poller_register_named(deferred_work_cb, arg, 1000000, "deferred");
    return SPDK_POLLER_BUSY;
}
```

### 12.2 활용되지 않는 큐 깊이

**증상**: 디바이스 처리량이 정격 사양보다 훨씬 낮고, CPU 사용률이 낮음.

**원인**: 애플리케이션이 하나의 I/O를 제출하고 다음을 제출하기 전에 완료를 대기.

```c
/* WRONG: Serial I/O */
submit_io(task);
wait_for_completion();  /* Leaves device idle */
submit_next_io();

/* CORRECT: Keep queue filled */
/* Submit to target depth, process completions, resubmit from callback */
static void io_complete(void *arg, ...)
{
    struct io_task *task = arg;
    ctx->current_depth--;
    process_result(task);
    /* Immediately submit next I/O to maintain depth */
    submit_next_io(ctx);
}

void start_io_engine(struct worker_ctx *ctx)
{
    /* Fill queue to target depth */
    for (int i = 0; i < ctx->target_depth; i++) {
        submit_io(ctx);
    }
}
```

### 12.3 핫 패스에서의 버퍼 할당

**증상**: I/O 완료 경로의 perf 플레임 그래프에서 `spdk_malloc` 또는 `rte_malloc`이 보임.

**원인**: I/O 버퍼나 태스크 구조체가 I/O별로 할당/해제됨.

**수정**: 사전 할당된 태스크와 버퍼의 프리리스트(링 버퍼)를 사용합니다. 섹션 11.1의 예제를 참조하세요.

### 12.4 디바이스 친화성에 대한 잘못된 CPU 토폴로지

**증상**: 단일 디바이스 성능은 좋지만 다중 디바이스 합산 성능이 나쁨.

**원인**: 리액터가 잘못된 NUMA 노드의 NVMe 디바이스에 접근.

**수정**: 리액터 NUMA 노드를 디바이스 NUMA 노드에 맞춥니다. `cat /sys/bus/pci/devices/ADDR/numa_node`로 디바이스 NUMA 노드를 확인합니다.

### 12.5 과도한 RPC/관리 트래픽

**증상**: 모니터링 간격과 일치하는 간헐적인 레이턴시 스파이크.

**원인**: RPC 핸들러는 전용 관리 스레드에서 실행되지만 `framework_get_reactors`와 `thread_get_stats` 호출은 메시지 전달로 리액터 스레드 전체의 상태를 집계하여, 폴링 I/O 경로에 레이턴시를 추가함.

**수정**: 프로덕션에서 모니터링 빈도를 줄입니다. 1초 미만 폴링 대신 5초 이상 간격으로 `spdk_top`을 사용합니다.

### 12.6 완료 콜백에서의 메모리 누수

**증상**: 수 시간에 걸친 점진적 성능 저하, 증가하는 휴지 페이지 소비.

**원인**: `spdk_bdev_io` 객체가 완료 후 해제되지 않거나, DMA 버퍼 누수.

```c
/* CORRECT: Always free in completion callback */
static void
bdev_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct my_request *req = cb_arg;

    if (!success) {
        SPDK_ERRLOG("I/O failed\n");
    }

    /* MUST free the bdev_io */
    spdk_bdev_free_io(bdev_io);

    /* Handle req */
    complete_request(req);
}
```

---

## 13. bdev I/O 통계 수집

SPDK의 bdev 계층은 내장 I/O 통계 수집을 제공합니다. 애플리케이션에 계측 오버헤드를 추가하지 않고 성능 모니터링에 사용할 수 있습니다.

```c
#include "spdk/bdev.h"

/* Collect per-channel I/O statistics */
static void
collect_stats(void *arg)
{
    struct spdk_bdev *bdev = arg;
    struct spdk_bdev_io_stat stat;

    /* Non-callback variant: collects on current thread's channel */
    spdk_bdev_get_io_stat(bdev, my_io_channel, &stat,
                          SPDK_BDEV_RESET_STAT_INTERVAL);

    printf("Read:  %" PRIu64 " ops, %" PRIu64 " bytes\n",
           stat.num_read_ops, stat.bytes_read);
    printf("Write: %" PRIu64 " ops, %" PRIu64 " bytes\n",
           stat.num_write_ops, stat.bytes_written);
}

/* Aggregate statistics across all channels (async, uses callback) */
static void
device_stat_cb(struct spdk_bdev *bdev, struct spdk_bdev_io_stat *stat,
               void *cb_arg, int rc)
{
    if (rc == 0) {
        double read_mbps = (double)stat->bytes_read / 1048576.0;
        double write_mbps = (double)stat->bytes_written / 1048576.0;
        printf("Device %s: read=%.1f MiB/s write=%.1f MiB/s\n",
               spdk_bdev_get_name(bdev), read_mbps, write_mbps);
    }
}

spdk_bdev_get_device_stat(bdev, &stat,
                           SPDK_BDEV_RESET_STAT_INTERVAL,
                           device_stat_cb, NULL);
```

RPC를 통해:

```bash
# Get bdev I/O statistics
./scripts/rpc.py bdev_get_iostat

# Get statistics for a specific bdev
./scripts/rpc.py bdev_get_iostat -b Nvme0n1

# Reset statistics after reading
./scripts/rpc.py bdev_reset_iostat -b Nvme0n1 --mode reset_on_read
```

---

## 핵심 요점

1. **최적화하기 전에 측정하세요**: `perf`, `spdk_top`, SPDK 트레이스 포인트는 가정이 아닌 실제 병목을 타겟으로 삼을 근거를 제공합니다.

2. **NUMA 정렬은 기본입니다**: NUMA 잘못된 설정은 20-40%의 성능 저하를 조용히 유발합니다. 다른 것을 튜닝하기 전에 항상 `numastat`으로 확인하세요.

3. **큐 깊이에는 포화점이 있습니다**: 큐 깊이를 체계적으로 스위프하세요. 대부분의 워크로드는 QD=64-128에서 포화합니다; 더 깊은 큐는 IOPS 없이 레이턴시만 추가합니다.

4. **핫 패스의 모든 것을 사전 할당하세요**: I/O 경로에서의 버퍼 할당, 태스크 구조체 할당, 프리리스트 연산은 레이턴시 스파이크의 일반적인 원인을 제거합니다.

5. **리액터 스레드를 절대 블록하지 마세요**: 폴러에서의 단일 `sleep()`이나 블로킹 `read()`는 해당 스레드의 모든 다른 폴러를 정지시킵니다. SPDK의 메시지 전달과 타이머 API를 사용하세요.

6. **폴 그룹은 폴러 오버헤드를 줄입니다**: 많은 큐 페어를 관리할 때, 단일 `spdk_nvme_poll_group_process_completions`는 N번의 `spdk_nvme_qpair_process_completions` 호출보다 효율적입니다.

7. **직감보다 플레임 그래프**: 성숙한 SPDK 애플리케이션의 실제 병목은 예상하는 곳에 거의 없습니다. 먼저 플레임 그래프를 생성하세요.

8. **하드웨어 설정이 중요합니다**: 소프트웨어를 탓하기 전에 PCIe 링크 폭, NVMe 펌웨어 버전, CPU 주파수 스케일링을 확인하세요.

---

## 실습 과제

### 과제 1: 큐 깊이 스위프

`spdk_nvme_perf`를 사용하여 로컬 NVMe 디바이스에 대해 QD=1부터 QD=256까지 큐 깊이 스위프를 수행합니다. IOPS 대 레이턴시(P50, P99) 곡선을 그립니다. 큐 깊이를 추가해도 IOPS가 개선되지 않으면서 P99 레이턴시만 계속 증가하는 "무릎(knee)" 지점을 식별합니다.

```bash
# Template command
./build/bin/spdk_nvme_perf \
    -r 'trtype:PCIe traddr:<YOUR_ADDR>' \
    -q <QD> -o 4096 -w randread -t 10 --latency-tracking
```

결과를 기록하고 답하세요: 어떤 큐 깊이에서 P99 레이턴시가 500us를 초과합니까? 달성된 최대 IOPS는 얼마입니까?

### 과제 2: NUMA 영향 측정

멀티-NUMA 시스템이 있다면:
1. SPDK를 NUMA 노드 0의 CPU에 고정
2. NUMA 노드 0과 노드 1의 NVMe 디바이스에 대해 각각 bdevperf를 실행
3. IOPS와 레이턴시 차이를 측정
4. `numastat`으로 원격 메모리 접근을 확인

싱글-NUMA인 경우, 불일치하는 노드에 휴지 페이지 할당을 고정하여 영향을 시뮬레이션하고 관찰합니다.

### 과제 3: 플레임 그래프 분석

1. bdev 워크로드가 실행 중인 `spdk_tgt`를 시작
2. `perf` + FlameGraph를 사용하여 CPU 플레임 그래프를 생성
3. 폭이 가장 큰 상위 3개 호출 스택을 식별
4. 각각에 대해 예상되는 오버헤드인지 잠재적 최적화 대상인지 판단
5. SPDK 애플리케이션에 최소 하나의 커스텀 트레이스 포인트를 추가하고 트레이스를 캡처

### 과제 4: spdk_top 프로파일링 세션

1. 여러 bdev 타입(NVMe, null bdev, malloc bdev)으로 `spdk_tgt`를 시작
2. `spdk_top`을 실행하고 유휴, 저부하, 고부하에서 스레드 Busy%를 관찰
3. 부하 증가 시 어떤 스레드가 먼저 병목이 되는지 식별
4. `framework_set_scheduler dynamic`을 사용하고 다양한 부하에 스케줄러가 어떻게 반응하는지 관찰
5. 동적 스케줄러가 백오프하는 최소 Busy% 임계값을 문서화

### 과제 5: NVMe-oF 레이턴시 vs. 로컬 NVMe

1. 루프백 NVMe-oF TCP 타겟을 설정 (동일 머신에서 타겟과 이니시에이터)
2. `spdk_nvme_perf`를 사용하여 `trtype:TCP` vs. `trtype:PCIe`로 벤치마크
3. QD=1(순수 레이턴시)과 QD=64(처리량)에서 P50과 P99 읽기 레이턴시를 측정
4. NVMe-oF TCP에 의해 도입된 트랜스포트 오버헤드를 식별
5. `uring` sock 구현과 튜닝된 버퍼 크기를 사용하여 차이를 줄이는 시도

---

## 참고 자료

- `app/spdk_nvme_perf/perf.c` - 히스토그램 레이턴시 추적 기능을 갖춘 완전한 NVMe 성능 벤치마크
- `examples/bdev/bdevperf/bdevperf.c` - bdev 계층 성능 벤치마크
- `app/spdk_top/spdk_top.c` - 실시간 리액터/스레드/폴러 모니터링 도구
- `lib/trace/trace.c` - 락-프리 트레이스 프레임워크 구현
- `app/trace/trace.cpp` - 트레이스 후처리 및 분석 도구
- `include/spdk/histogram_data.h` - 레이턴시 측정을 위한 히스토그램 API
- `include/spdk/bdev.h` - `spdk_bdev_io_stat`, `spdk_bdev_get_io_stat`, `spdk_bdev_get_device_stat`
- `include/spdk/nvme.h` - `spdk_nvme_io_qpair_opts`, 큐 페어 설정
- `scripts/rpc.py` - 런타임 설정 및 통계 수집을 위한 RPC 클라이언트
- SPDK 성능 튜닝 가이드: https://spdk.io/doc/performance_tuning.html

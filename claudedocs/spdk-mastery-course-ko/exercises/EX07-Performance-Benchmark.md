# 실습 07: 성능 벤치마크

## 개요

| 항목 | 내용 |
|-------|-------|
| 실습 | 07 |
| 제목 | 성능 벤치마크 (Performance Benchmark) |
| 예상 소요 시간 | 2-3시간 |
| 난이도 | 중급 |
| 사전 요구 사항 | EX01-EX06 완료, SPDK 빌드 완료, NVMe 장치 또는 NVMe-over-TCP 타겟 사용 가능 |

## 목표

SPDK NVMe 드라이버를 사용하여 다양한 I/O 패턴의 IOPS와 지연 시간(latency)을 측정하는 커스텀 벤치마크 도구를 구축합니다. 이 실습을 완료하면 다음을 수행할 수 있습니다:

- NVMe 네임스페이스(namespace)에 대해 순차 읽기(sequential read), 순차 쓰기(sequential write), 랜덤 읽기(random read), 랜덤 쓰기(random write)를 직접 수행
- 큐 깊이(queue depth)를 1에서 128까지 변화시키며 포화 동작(saturation behaviour) 관찰
- `spdk_get_ticks` / `spdk_get_ticks_hz`를 사용한 정확한 시간 측정
- 참조 도구인 `spdk_nvme_perf` 및 `bdevperf`와 결과 비교
- 폴링 모드(polling mode)가 인터럽트 기반 I/O 대비 지연 시간에 미치는 영향 이해 (보너스)
- I/O별 지연 시간 샘플을 수집하고 간단한 히스토그램(histogram) 작성 (보너스)

---

## 사전 요구 사항

```
SPDK built:   $SPDK_DIR/build/bin/spdk_nvme_perf  must exist
Root access:  required for DPDK hugepage setup and UIO/VFIO binding
NVMe target:  a local NVMe SSD bound to vfio-pci/uio_pci_generic
              OR a running NVMe-over-TCP target (127.0.0.1:4420)
Tooling:      python3 (optional, for histogram visualisation)
              gnuplot   (optional, for throughput plots)
```

시작하기 전에 장치를 한 번 바인딩합니다:

```bash
sudo $SPDK_DIR/scripts/setup.sh
# Verify at least one NVMe is visible
ls /dev/hugepages
```

---

## 배경 지식

SPDK는 폴링 모드(polled mode)로 실행하여 커널 오버헤드를 방지합니다: 애플리케이션 스레드가 인터럽트를 대기하는 대신 `spdk_nvme_qpair_process_completions()`를 타이트 루프에서 호출합니다. 이를 통해 마이크로초 수준의 지연 시간을 달성하지만, 전용 CPU 코어가 필요합니다.

주요 타이밍 API:

```c
/* Returns raw CPU timestamp counter value */
uint64_t spdk_get_ticks(void);

/* Returns ticks per second (typically matches TSC frequency) */
uint64_t spdk_get_ticks_hz(void);

/* Convert ticks to microseconds */
static inline double ticks_to_us(uint64_t ticks)
{
    return (double)ticks / spdk_get_ticks_hz() * 1e6;
}
```

---

## 과제 설명

`$SPDK_DIR/app/custom_perf/perf_bench.c`를 생성합니다 -- 다음 기능을 갖춘 독립형 벤치마크입니다:

1. 처음 사용 가능한 NVMe 컨트롤러와 네임스페이스에 연결
2. 단일 I/O 큐 페어(queue pair) 할당
3. 4가지 테스트 단계를 순차적으로 실행: 순차 읽기, 순차 쓰기, 랜덤 읽기, 랜덤 쓰기
4. 각 단계에서 큐 깊이를 변화: 1, 2, 4, 8, 16, 32, 64, 128
5. 각 (패턴, 큐 깊이) 조합을 설정 가능한 시간(기본 5초) 동안 실행
6. IOPS, 처리량(MiB/s), 평균 지연 시간(us), 99번째 백분위수 지연 시간(us) 보고

---

## 단계별 지침

### 단계 1 -- 애플리케이션 디렉토리와 Makefile 생성

```bash
mkdir -p $SPDK_DIR/app/custom_perf
```

`$SPDK_DIR/app/custom_perf/Makefile` 생성:

```makefile
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = perf_bench
C_SRCS = perf_bench.c

SPDK_LIB_LIST = nvme env_dpdk log util

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
```

### 단계 2 -- 데이터 구조 정의

`$SPDK_DIR/app/custom_perf/perf_bench.c`를 생성하고 헤더와 구조체로 시작합니다:

```c
/* SPDX-License-Identifier: BSD-3-Clause */
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/histogram_data.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                         */
/* ------------------------------------------------------------------ */
#define IO_SIZE_BYTES       4096          /* 4 KiB — standard benchmark block */
#define RUN_SECONDS         5             /* seconds per (pattern, qd) pair   */
#define MAX_QUEUE_DEPTH     128
#define LATENCY_SAMPLES_MAX 1048576       /* 1 M entries for histogram          */

static const uint32_t g_queue_depths[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
#define NUM_QD SPDK_COUNTOF(g_queue_depths)

typedef enum {
    PATTERN_SEQ_READ  = 0,
    PATTERN_SEQ_WRITE = 1,
    PATTERN_RND_READ  = 2,
    PATTERN_RND_WRITE = 3,
    PATTERN_COUNT     = 4,
} io_pattern_t;

static const char *g_pattern_names[PATTERN_COUNT] = {
    "seq-read", "seq-write", "rnd-read", "rnd-write"
};

/* Per-I/O request context */
struct io_task {
    uint64_t        submit_tsc;   /* tsc at submission */
    bool            in_flight;
};

/* Global state shared by callbacks */
struct bench_ctx {
    struct spdk_nvme_ctrlr  *ctrlr;
    struct spdk_nvme_ns     *ns;
    struct spdk_nvme_qpair  *qpair;

    void                    *buf;           /* DMA buffer for all I/Os    */
    uint64_t                 ns_size_ios;   /* namespace size in 4K units  */

    /* per-run counters (reset for each (pattern, qd) combination) */
    volatile uint64_t        ios_completed;
    volatile uint64_t        io_errors;
    uint64_t                 run_start_tsc;
    uint64_t                 run_end_tsc;

    /* latency accumulator */
    uint64_t                 lat_total_tsc; /* sum of completion latencies */
    uint64_t                *lat_samples;   /* raw tsc deltas              */
    uint32_t                 lat_count;

    /* current run parameters */
    io_pattern_t             pattern;
    uint32_t                 queue_depth;
    uint64_t                 lba_counter;   /* sequential LBA cursor        */

    bool                     run_active;
};

static struct bench_ctx g_ctx;
```

### 단계 3 -- I/O 완료 콜백 구현

```c
/* ------------------------------------------------------------------ */
/* Completion callback                                                   */
/* ------------------------------------------------------------------ */
static void
io_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
    struct io_task *task = (struct io_task *)arg;
    uint64_t now = spdk_get_ticks();
    uint64_t lat = now - task->submit_tsc;

    task->in_flight = false;

    if (spdk_nvme_cpl_is_error(cpl)) {
        g_ctx.io_errors++;
    } else {
        g_ctx.ios_completed++;
        g_ctx.lat_total_tsc += lat;

        if (g_ctx.lat_count < LATENCY_SAMPLES_MAX) {
            g_ctx.lat_samples[g_ctx.lat_count++] = lat;
        }
    }
}
```

### 단계 4 -- I/O 제출 헬퍼 구현

```c
/* ------------------------------------------------------------------ */
/* I/O submission                                                        */
/* ------------------------------------------------------------------ */
static uint64_t
next_lba(void)
{
    uint64_t lba;

    switch (g_ctx.pattern) {
    case PATTERN_SEQ_READ:
    case PATTERN_SEQ_WRITE:
        lba = g_ctx.lba_counter % g_ctx.ns_size_ios;
        g_ctx.lba_counter++;
        break;
    case PATTERN_RND_READ:
    case PATTERN_RND_WRITE:
    default:
        lba = rand() % g_ctx.ns_size_ios;
        break;
    }

    return lba;
}

static int
submit_io(struct io_task *task)
{
    uint64_t lba = next_lba();
    int rc;

    task->submit_tsc = spdk_get_ticks();
    task->in_flight  = true;

    if (g_ctx.pattern == PATTERN_SEQ_READ ||
        g_ctx.pattern == PATTERN_RND_READ) {
        rc = spdk_nvme_ns_cmd_read(g_ctx.ns, g_ctx.qpair,
                                   g_ctx.buf, lba, 1,
                                   io_complete, task, 0);
    } else {
        rc = spdk_nvme_ns_cmd_write(g_ctx.ns, g_ctx.qpair,
                                    g_ctx.buf, lba, 1,
                                    io_complete, task, 0);
    }

    if (rc != 0) {
        task->in_flight = false;
        SPDK_ERRLOG("I/O submission failed: %d\n", rc);
    }

    return rc;
}
```

### 단계 5 -- 실행 루프 구현

```c
/* ------------------------------------------------------------------ */
/* Run loop for one (pattern, queue_depth) combination                  */
/* ------------------------------------------------------------------ */
static void
run_bench(io_pattern_t pattern, uint32_t qd)
{
    struct io_task *tasks;
    uint64_t deadline_tsc;
    uint32_t i;

    /* Reset per-run state */
    g_ctx.pattern       = pattern;
    g_ctx.queue_depth   = qd;
    g_ctx.ios_completed = 0;
    g_ctx.io_errors     = 0;
    g_ctx.lat_total_tsc = 0;
    g_ctx.lat_count     = 0;
    g_ctx.lba_counter   = 0;

    tasks = calloc(qd, sizeof(*tasks));
    if (!tasks) {
        SPDK_ERRLOG("Failed to allocate task array\n");
        return;
    }

    deadline_tsc = spdk_get_ticks() + RUN_SECONDS * spdk_get_ticks_hz();
    g_ctx.run_start_tsc = spdk_get_ticks();

    /* Pre-fill the queue */
    for (i = 0; i < qd; i++) {
        submit_io(&tasks[i]);
    }

    /* Poll until deadline */
    while (spdk_get_ticks() < deadline_tsc) {
        spdk_nvme_qpair_process_completions(g_ctx.qpair, 0);

        /* Re-submit completed slots to maintain queue depth */
        for (i = 0; i < qd; i++) {
            if (!tasks[i].in_flight) {
                submit_io(&tasks[i]);
            }
        }
    }

    /* Drain remaining in-flight I/Os */
    bool draining = true;
    while (draining) {
        spdk_nvme_qpair_process_completions(g_ctx.qpair, 0);
        draining = false;
        for (i = 0; i < qd; i++) {
            if (tasks[i].in_flight) {
                draining = true;
                break;
            }
        }
    }

    g_ctx.run_end_tsc = spdk_get_ticks();
    free(tasks);
}
```

### 단계 6 -- 결과 출력 구현

```c
/* ------------------------------------------------------------------ */
/* Results                                                               */
/* ------------------------------------------------------------------ */
static double
ticks_to_us(uint64_t ticks)
{
    return (double)ticks / (double)spdk_get_ticks_hz() * 1e6;
}

static double
percentile_us(uint32_t pct)
{
    uint64_t *s = g_ctx.lat_samples;
    uint32_t  n = g_ctx.lat_count;
    uint32_t  idx;

    if (n == 0) {
        return 0.0;
    }

    /* Simple sort-based percentile — sufficient for exercise purposes.    */
    /* For production use, replace with spdk_histogram_data or HDR histogram. */
    /* Sort is O(n log n); cap samples to keep runtime acceptable.           */
    for (uint32_t i = 1; i < n; i++) {
        uint64_t key = s[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && s[j] > key) {
            s[j + 1] = s[j];
            j--;
        }
        s[j + 1] = key;
    }

    idx = (uint32_t)((double)pct / 100.0 * (n - 1) + 0.5);
    return ticks_to_us(s[idx]);
}

static void
print_result(io_pattern_t pattern, uint32_t qd)
{
    double elapsed_s   = (double)(g_ctx.run_end_tsc - g_ctx.run_start_tsc)
                         / (double)spdk_get_ticks_hz();
    double iops        = (double)g_ctx.ios_completed / elapsed_s;
    double tput_mib    = iops * IO_SIZE_BYTES / (1024.0 * 1024.0);
    double mean_lat_us = (g_ctx.ios_completed > 0)
                         ? ticks_to_us(g_ctx.lat_total_tsc / g_ctx.ios_completed)
                         : 0.0;
    double p99_lat_us  = percentile_us(99);

    printf("%-12s  QD=%-4u  IOPS=%8.0f  BW=%7.1f MiB/s  "
           "lat_mean=%7.1f us  lat_p99=%7.1f us  errors=%" PRIu64 "\n",
           g_pattern_names[pattern], qd,
           iops, tput_mib, mean_lat_us, p99_lat_us,
           g_ctx.io_errors);
}
```

### 단계 7 -- main() 구현

```c
/* ------------------------------------------------------------------ */
/* Probe / attach callbacks                                              */
/* ------------------------------------------------------------------ */
static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
         struct spdk_nvme_ctrlr_opts *opts)
{
    (void)cb_ctx;
    (void)opts;
    printf("Attaching to NVMe controller at %s\n", trid->traddr);
    return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
          struct spdk_nvme_ctrlr *ctrlr,
          const struct spdk_nvme_ctrlr_opts *opts)
{
    (void)cb_ctx;
    (void)trid;
    (void)opts;

    if (g_ctx.ctrlr != NULL) {
        return;  /* Use only the first controller */
    }

    g_ctx.ctrlr = ctrlr;
    g_ctx.ns    = spdk_nvme_ctrlr_get_ns(ctrlr, 1);  /* namespace ID 1 */
    if (!spdk_nvme_ns_is_active(g_ctx.ns)) {
        SPDK_ERRLOG("Namespace 1 is not active\n");
        g_ctx.ns = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */
int
main(int argc, char **argv)
{
    int rc;
    struct spdk_env_opts env_opts;
    struct spdk_nvme_transport_id trid = {};

    spdk_env_opts_init(&env_opts);
    env_opts.name = "perf_bench";
    env_opts.shm_id = 0;

    if (spdk_env_init(&env_opts) < 0) {
        fprintf(stderr, "Failed to initialize SPDK environment\n");
        return 1;
    }

    /* Allocate latency sample buffer */
    g_ctx.lat_samples = calloc(LATENCY_SAMPLES_MAX, sizeof(uint64_t));
    if (!g_ctx.lat_samples) {
        fprintf(stderr, "Failed to allocate latency buffer\n");
        return 1;
    }

    /* Probe PCIe NVMe devices */
    spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
    rc = spdk_nvme_probe(&trid, NULL, probe_cb, attach_cb, NULL);
    if (rc != 0 || g_ctx.ctrlr == NULL || g_ctx.ns == NULL) {
        fprintf(stderr, "NVMe probe failed or no usable namespace found\n");
        return 1;
    }

    /* Allocate I/O queue pair */
    g_ctx.qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctx.ctrlr, NULL, 0);
    if (!g_ctx.qpair) {
        fprintf(stderr, "Failed to allocate I/O queue pair\n");
        return 1;
    }

    /* Allocate DMA-capable I/O buffer (one block for all requests) */
    g_ctx.buf = spdk_zmalloc(IO_SIZE_BYTES, IO_SIZE_BYTES, NULL,
                             SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    if (!g_ctx.buf) {
        fprintf(stderr, "Failed to allocate DMA buffer\n");
        return 1;
    }

    /* Compute namespace size in I/O units */
    uint32_t sector_size = spdk_nvme_ns_get_sector_size(g_ctx.ns);
    uint64_t ns_sectors  = spdk_nvme_ns_get_num_sectors(g_ctx.ns);
    g_ctx.ns_size_ios    = ns_sectors / (IO_SIZE_BYTES / sector_size);

    printf("\n=== SPDK Custom Performance Benchmark ===\n");
    printf("Namespace size: %.1f GiB  |  Block size: %u B  |  Run time: %d s/combo\n\n",
           (double)(ns_sectors * sector_size) / (1024.0 * 1024.0 * 1024.0),
           IO_SIZE_BYTES, RUN_SECONDS);

    printf("%-12s  %-6s  %-17s  %-16s  %-16s  %-16s\n",
           "Pattern", "QD", "IOPS", "BW", "Lat_mean", "Lat_p99");
    printf("%s\n", "---------------------------------------------------------------"
           "-------------------------------------------");

    /* Run all (pattern, queue_depth) combinations */
    for (int p = 0; p < PATTERN_COUNT; p++) {
        for (uint32_t q = 0; q < NUM_QD; q++) {
            run_bench((io_pattern_t)p, g_queue_depths[q]);
            print_result((io_pattern_t)p, g_queue_depths[q]);
        }
        printf("\n");
    }

    /* Cleanup */
    spdk_free(g_ctx.buf);
    spdk_nvme_ctrlr_free_io_qpair(g_ctx.qpair);
    spdk_nvme_detach(g_ctx.ctrlr);
    free(g_ctx.lat_samples);
    spdk_env_fini();

    return 0;
}
```

---

## 빌드 지침

```bash
cd $SPDK_DIR/app/custom_perf
make -j$(nproc)
# Binary appears at: $SPDK_DIR/app/custom_perf/perf_bench
```

빌드 시 심볼 누락 오류가 발생하면 SPDK가 NVMe 지원으로 빌드되었는지 확인합니다:

```bash
cd $SPDK_DIR
./configure --with-dpdk
make -j$(nproc)
```

---

## 실행 지침

```bash
# Run as root (required for DPDK hugepages and VFIO)
sudo $SPDK_DIR/app/custom_perf/perf_bench
```

예상 출력 (값은 장치에 따라 다름):

```
=== SPDK Custom Performance Benchmark ===
Namespace size: 476.9 GiB  |  Block size: 4096 B  |  Run time: 5 s/combo

Pattern       QD      IOPS              BW               Lat_mean         Lat_p99
---------------------------------------------------------------------------------------------------
seq-read      QD=1     IOPS=   85000  BW=  332.0 MiB/s  lat_mean=   11.7 us  lat_p99=   15.2 us
seq-read      QD=2     IOPS=  160000  BW=  625.0 MiB/s  lat_mean=   12.4 us  lat_p99=   18.3 us
...
rnd-write     QD=128   IOPS=  410000  BW= 1601.6 MiB/s  lat_mean=  312.0 us  lat_p99=  850.0 us
```

---

## 기존 SPDK 도구를 사용한 비교

### spdk_nvme_perf

`spdk_nvme_perf`는 `$SPDK_DIR/app/spdk_nvme_perf/perf.c`에 위치한 프로덕션 수준의 참조 벤치마크입니다. 결과를 검증하는 데 사용합니다:

```bash
# Sequential read, 4K, queue depth 32, 10 seconds
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -q 32 -o 4096 -w read -t 10

# Random read, 4K, queue depth 1 (single-threaded latency)
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -q 1 -o 4096 -w randread -t 10

# Random write, 4K, queue depth 128 (throughput mode)
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -q 128 -o 4096 -w randwrite -t 10

# Mixed 70% read / 30% write workload
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -q 32 -o 4096 -w randrw -M 70 -t 10
```

주요 플래그:

| 플래그 | 의미 |
|------|---------|
| `-q N` | I/O 큐 깊이 |
| `-o N` | I/O 블록 크기 (바이트) |
| `-w TYPE` | 워크로드: `read`, `write`, `randread`, `randwrite`, `randrw` |
| `-t N` | 실행 시간 (초) |
| `-M N` | 혼합(`randrw`) 워크로드에서 읽기 비율 |
| `-L` | I/O별 지연 시간 추적 활성화 (백분위수 테이블 출력) |
| `-H` | 실행 후 지연 시간 히스토그램 출력 |

### bdevperf (블록 디바이스 계층 벤치마크)

`bdevperf`는 NVMe 드라이버를 직접 사용하는 대신 bdev 추상화 계층(abstraction layer)을 통해 벤치마크합니다. NVMe-oF bdev를 통해 NVMe 네임스페이스를 벤치마크하거나 bdev 모듈(예: crypto, compress, QoS)을 테스트할 때 사용합니다:

```bash
# Create a minimal config referencing an NVMe bdev
cat > /tmp/bdevperf.json <<'EOF'
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_nvme_attach_controller",
          "params": {
            "name": "Nvme0",
            "trtype": "PCIe",
            "traddr": "0000:00:04.0"
          }
        }
      ]
    }
  ]
}
EOF

sudo $SPDK_DIR/build/bin/bdevperf \
    -c /tmp/bdevperf.json \
    -q 32 -o 4096 -w randread -t 10 -b Nvme0n1
```

`bdevperf`와 `spdk_nvme_perf` 결과를 비교하면 bdev 추상화 계층이 추가하는 오버헤드를 알 수 있습니다 (직접 NVMe bdev의 경우 일반적으로 2% 미만).

---

## 결과 분석 및 시각화 팁

### 결과를 CSV로 수집

`print_result()`를 수정하여 CSV 파일도 작성합니다:

```c
/* Add near the top of main() */
FILE *csv = fopen("bench_results.csv", "w");
fprintf(csv, "pattern,queue_depth,iops,bw_mib,lat_mean_us,lat_p99_us\n");

/* Replace / augment the printf in print_result() */
fprintf(csv, "%s,%u,%.0f,%.1f,%.1f,%.1f\n",
        g_pattern_names[pattern], qd,
        iops, tput_mib, mean_lat_us, p99_lat_us);
```

### gnuplot으로 IOPS 대 큐 깊이 그래프

```gnuplot
# iops_plot.gp
set terminal png size 900,600
set output "iops_vs_qd.png"
set title  "IOPS vs Queue Depth"
set xlabel "Queue Depth"
set ylabel "IOPS"
set logscale x 2
set key top left
set datafile separator ","

plot \
  "< grep seq-read bench_results.csv"  using 2:3 with linespoints title "seq-read",  \
  "< grep seq-write bench_results.csv" using 2:3 with linespoints title "seq-write", \
  "< grep rnd-read bench_results.csv"  using 2:3 with linespoints title "rnd-read",  \
  "< grep rnd-write bench_results.csv" using 2:3 with linespoints title "rnd-write"
```

```bash
gnuplot iops_plot.gp
# Opens iops_vs_qd.png
```

### 지연 시간 대 큐 깊이 그래프

```gnuplot
# lat_plot.gp
set terminal png size 900,600
set output "lat_vs_qd.png"
set title  "Mean Latency vs Queue Depth"
set xlabel "Queue Depth"
set ylabel "Latency (us)"
set logscale x 2
set key top left
set datafile separator ","

plot \
  "< grep rnd-read bench_results.csv"  using 2:5 with linespoints title "rnd-read mean",  \
  "< grep rnd-read bench_results.csv"  using 2:6 with linespoints title "rnd-read p99",   \
  "< grep rnd-write bench_results.csv" using 2:5 with linespoints title "rnd-write mean", \
  "< grep rnd-write bench_results.csv" using 2:6 with linespoints title "rnd-write p99"
```

### Python 빠른 분석

```python
#!/usr/bin/env python3
import csv, sys
from collections import defaultdict

data = defaultdict(list)
with open("bench_results.csv") as f:
    reader = csv.DictReader(f)
    for row in reader:
        data[row['pattern']].append(row)

for pattern, rows in sorted(data.items()):
    max_row = max(rows, key=lambda r: float(r['iops']))
    print(f"{pattern}: peak IOPS = {float(max_row['iops']):,.0f} "
          f"at QD={max_row['queue_depth']}  "
          f"(lat_p99 = {float(max_row['lat_p99_us']):.1f} us)")
```

---

## 보너스 과제 A: 히스토그램 지연 시간 추적

SPDK는 컴팩트한 2의 거듭제곱 버킷 히스토그램을 구현한 `spdk/histogram_data.h`를 제공합니다. 원시 샘플 배열을 `spdk_histogram_data` 인스턴스로 교체합니다:

```c
#include "spdk/histogram_data.h"

/* Replace lat_samples / lat_count in bench_ctx with: */
struct spdk_histogram_data *histogram;

/* In main(), allocate: */
g_ctx.histogram = spdk_histogram_data_alloc();

/* In io_complete(), replace the sample store with: */
spdk_histogram_data_tally(g_ctx.histogram, lat);

/* Reporting — dump per-percentile latency table: */
static void
print_histogram(void)
{
    static const double percentiles[] = {
        50.0, 90.0, 95.0, 99.0, 99.9, 99.99
    };

    printf("  Latency histogram:\n");
    for (size_t i = 0; i < SPDK_COUNTOF(percentiles); i++) {
        uint64_t ticks = 0;
        spdk_histogram_data_get_value(g_ctx.histogram,
                                      percentiles[i] / 100.0, &ticks);
        printf("    p%6.2f = %8.1f us\n",
               percentiles[i], ticks_to_us(ticks));
    }
}

/* Free after use: */
spdk_histogram_data_free(g_ctx.histogram);
```

이 방식은 O(n log n) 정렬을 피하고 8 MiB 샘플 버퍼를 제거합니다. `spdk_histogram_data`는 128개 버킷을 사용하며 낮은 지연 시간에서는 마이크로초 미만의 정밀도를, 높은 지연 시간에서는 더 거친 단위를 제공합니다.

---

## 보너스 과제 B: 폴링 모드 대 인터럽트 모드 비교

SPDK는 큐 페어에서 인터럽트 모드(interrupt mode)를 지원합니다. 인터럽트 모드가 활성화된 두 번째 큐 페어를 할당하고 동일한 워크로드를 양쪽에서 실행합니다:

```c
struct spdk_nvme_io_qpair_opts qpair_opts;
spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctx.ctrlr, &qpair_opts,
                                           sizeof(qpair_opts));

/* Polling mode (default) */
qpair_opts.io_queue_size = 128;
g_ctx.qpair_poll = spdk_nvme_ctrlr_alloc_io_qpair(g_ctx.ctrlr,
                                                    &qpair_opts, sizeof(qpair_opts));

/* Interrupt mode */
qpair_opts.delay_cmd_submit = false;
qpair_opts.async_mode = true;   /* enables interrupt/epoll path where supported */
g_ctx.qpair_intr = spdk_nvme_ctrlr_alloc_io_qpair(g_ctx.ctrlr,
                                                    &qpair_opts, sizeof(qpair_opts));
```

예상 결과 패턴:
- 폴링 모드: 깊은 큐에서 더 낮은 평균 지연 시간과 더 높은 IOPS, CPU 사용률 100%
- 인터럽트 모드: 더 높은 테일 지연 시간(tail latency), 더 낮은 CPU 소모, 스토리지 지연에 관대한 워크로드에 적합

---

## 흔한 실수

### 1. `spdk_nvme_qpair_process_completions()` 호출 누락

NVMe 드라이버는 커널 인터럽트를 사용하지 않습니다. 완료(completion)는 `spdk_nvme_qpair_process_completions()`를 명시적으로 호출해야만 진행됩니다. 루프에서 이를 호출하지 않으면 완료가 쌓이고 드레인(drain) 단계에서 벤치마크가 멈춥니다.

### 2. `in_flight` 확인 없이 재제출

항상 `!task->in_flight`를 확인한 후 재제출합니다. 아직 미완료 I/O가 있는 슬롯에 제출하면 DMA 버퍼가 손상되고, 완료 콜백에서 무음 데이터 오류나 세그폴트(segfault)가 발생합니다.

### 3. DMA에 스택 할당 버퍼 사용

NVMe DMA는 DPDK의 메모리 서브시스템에 등록된 물리적으로 연속된 메모리가 필요합니다. `spdk_zmalloc(..., SPDK_MALLOC_DMA)`로 할당하고 `spdk_free()`로 해제합니다. 스택이나 `malloc()` 버퍼는 무음 손상이나 커널 패닉을 유발합니다.

### 4. `process_completions()` 반환 후 지연 시간 계산

`io_complete()` 내부에서 `spdk_get_ticks()`를 호출하면 I/O별 지연 시간을 얻습니다. 제출과 `process_completions()` 반환 사이의 시간 차이를 계산하면 배치 단위의 완료를 측정하게 되어 지연 시간 수치가 크게 부풀려집니다.

### 5. 큐 드레인 전 경과 시간 계산

`run_end_tsc`는 모든 진행 중 I/O가 완료된 후에만 설정합니다. 드레인 시간을 IOPS 공식의 분모에 포함하면 큰 큐 깊이에서 처리량이 과소평가됩니다 (드레인에 수십 밀리초가 걸릴 수 있음).

### 6. 모드 차이를 고려하지 않고 커널 `fio`와 비교

커널 `fio`는 블록 계층을 사용하며 컨텍스트 스위치(context switch)와 인터럽트 오버헤드가 발생합니다. SPDK는 유저 스페이스에서 폴링합니다. 이 차이를 언급하지 않고 원시 수치를 비교하면 잘못된 결론을 내릴 수 있습니다. 항상 데이터 소스를 표기하세요.

### 7. 공유 시스템에서 실행

다른 프로세스가 동일한 NVMe 장치에 쓰기를 하면 지연 시간 측정이 오염됩니다. 정확한 벤치마크를 위해: VFIO로 장치를 격리하고, `taskset`으로 벤치마크 스레드를 전용 CPU 코어에 고정하고, CPU 주파수 스케일링을 비활성화합니다 (`cpupower frequency-set -g performance`).

---

## 체크리스트

- [ ] `make`가 경고 없이 성공
- [ ] 도구가 NVMe 컨트롤러에 연결되고 네임스페이스 크기 출력
- [ ] 4가지 패턴 모두 오류 없이 실행
- [ ] 큐 깊이 증가에 따라 IOPS가 장치 포화까지 증가
- [ ] 큐 깊이 증가에 따라 평균 지연 시간 증가 (큐잉 지연)
- [ ] `spdk_nvme_perf -q 32 -w randread` 결과가 QD=32에서 도구 결과의 10% 이내
- [ ] (보너스 A) 히스토그램 출력이 합리적인 p50/p99/p99.9 값 표시
- [ ] (보너스 B) 인터럽트 모드가 QD=1에서 폴링 모드보다 높은 p99 지연 시간 표시

---

## 추가 참고 자료

- `$SPDK_DIR/app/spdk_nvme_perf/perf.c` -- 멀티 코어, 멀티 컨트롤러, 지연 시간 히스토그램을 지원하는 참조 구현
- `$SPDK_DIR/include/spdk/histogram_data.h` -- SPDK 히스토그램 API
- `$SPDK_DIR/include/spdk/env.h` -- `spdk_get_ticks`, `spdk_get_ticks_hz`, `spdk_zmalloc`
- `$SPDK_DIR/include/spdk/nvme.h` -- 전체 NVMe API
- SPDK 문서: https://spdk.io/doc/nvme.html
- NVMe 사양 2.0, 섹션 4 -- 큐 페어 및 완료 모델

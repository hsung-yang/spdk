# Exercise 07: Performance Benchmark

## Overview

| Field | Value |
|-------|-------|
| Exercise | 07 |
| Title | Performance Benchmark |
| Estimated Time | 2-3 hours |
| Difficulty | Intermediate |
| Prerequisites | EX01-EX06 completed, SPDK built, NVMe device or NVMe-over-TCP target available |

## Objective

Build a custom benchmark tool that measures IOPS and latency for different I/O patterns using the SPDK NVMe driver. By the end of this exercise you will:

- Issue sequential reads, sequential writes, random reads, and random writes directly against an NVMe namespace
- Sweep queue depth from 1 to 128 and observe saturation behaviour
- Time operations accurately with `spdk_get_ticks` / `spdk_get_ticks_hz`
- Compare your results against the reference tools `spdk_nvme_perf` and `bdevperf`
- Understand how polling mode affects latency versus interrupt-driven I/O (bonus)
- Collect per-I/O latency samples and plot a simple histogram (bonus)

---

## Prerequisites

```
SPDK built:   $SPDK_DIR/build/bin/spdk_nvme_perf  must exist
Root access:  required for DPDK hugepage setup and UIO/VFIO binding
NVMe target:  a local NVMe SSD bound to vfio-pci/uio_pci_generic
              OR a running NVMe-over-TCP target (127.0.0.1:4420)
Tooling:      python3 (optional, for histogram visualisation)
              gnuplot   (optional, for throughput plots)
```

Bind your device once before starting:

```bash
sudo $SPDK_DIR/scripts/setup.sh
# Verify at least one NVMe is visible
ls /dev/hugepages
```

---

## Background

SPDK avoids kernel overhead by running in polled mode: your application thread spins in a tight loop calling `spdk_nvme_qpair_process_completions()` instead of blocking on interrupts. This delivers microsecond-class latency but requires dedicated CPU cores.

Key timing API:

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

## Task Description

Create `$SPDK_DIR/app/custom_perf/perf_bench.c` — a standalone benchmark that:

1. Attaches to the first available NVMe controller and namespace
2. Allocates a single I/O queue pair
3. Runs four test phases in sequence: sequential read, sequential write, random read, random write
4. Within each phase, sweeps queue depths: 1, 2, 4, 8, 16, 32, 64, 128
5. Runs each (pattern, queue-depth) combination for a configurable duration (default 5 s)
6. Reports IOPS, throughput (MiB/s), mean latency (µs), and 99th-percentile latency (µs)

---

## Step-by-Step Instructions

### Step 1 — Create the application directory and Makefile

```bash
mkdir -p $SPDK_DIR/app/custom_perf
```

Create `$SPDK_DIR/app/custom_perf/Makefile`:

```makefile
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = perf_bench
C_SRCS = perf_bench.c

SPDK_LIB_LIST = nvme env_dpdk log util

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
```

### Step 2 — Define data structures

Create `$SPDK_DIR/app/custom_perf/perf_bench.c` and start with the headers and structures:

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

### Step 3 — Implement the I/O completion callback

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

### Step 4 — Implement I/O submission helpers

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

### Step 5 — Implement the run loop

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

### Step 6 — Implement results reporting

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

### Step 7 — Implement main()

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

## Build Instructions

```bash
cd $SPDK_DIR/app/custom_perf
make -j$(nproc)
# Binary appears at: $SPDK_DIR/app/custom_perf/perf_bench
```

If the build fails with missing symbols, ensure SPDK was built with NVMe support:

```bash
cd $SPDK_DIR
./configure --with-dpdk
make -j$(nproc)
```

---

## Run Instructions

```bash
# Run as root (required for DPDK hugepages and VFIO)
sudo $SPDK_DIR/app/custom_perf/perf_bench
```

Expected output (values depend on your device):

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

## Using Existing SPDK Tools for Comparison

### spdk_nvme_perf

`spdk_nvme_perf` is the production-grade reference benchmark located at
`$SPDK_DIR/app/spdk_nvme_perf/perf.c`. Use it to validate your results:

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

Key flags:

| Flag | Meaning |
|------|---------|
| `-q N` | I/O queue depth |
| `-o N` | I/O block size in bytes |
| `-w TYPE` | Workload: `read`, `write`, `randread`, `randwrite`, `randrw` |
| `-t N` | Run time in seconds |
| `-M N` | Read percentage for mixed (`randrw`) workloads |
| `-L` | Enable per-I/O latency tracking (outputs percentile table) |
| `-H` | Print latency histogram after run |

### bdevperf (block device layer benchmark)

`bdevperf` exercises the bdev abstraction layer rather than the NVMe driver directly. Use it when you want to benchmark an NVMe namespace via an NVMe-oF bdev or when testing bdev modules (e.g., crypto, compress, QoS):

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

Comparing `bdevperf` vs `spdk_nvme_perf` results reveals the overhead added by the bdev abstraction layer (typically < 2% for direct NVMe bdevs).

---

## Results Analysis and Visualisation Tips

### Collect results to CSV

Modify `print_result()` to also write a CSV file:

```c
/* Add near the top of main() */
FILE *csv = fopen("bench_results.csv", "w");
fprintf(csv, "pattern,queue_depth,iops,bw_mib,lat_mean_us,lat_p99_us\n");

/* Replace / augment the printf in print_result() */
fprintf(csv, "%s,%u,%.0f,%.1f,%.1f,%.1f\n",
        g_pattern_names[pattern], qd,
        iops, tput_mib, mean_lat_us, p99_lat_us);
```

### Plot IOPS vs Queue Depth with gnuplot

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

### Plot latency vs queue depth

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

### Python quick analysis

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

## Bonus Task A: Histogram Latency Tracking

SPDK ships `spdk/histogram_data.h` which implements a compact power-of-two bucket histogram. Replace the raw sample array with an `spdk_histogram_data` instance:

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

This avoids the O(n log n) sort and eliminates the 8 MiB sample buffer. `spdk_histogram_data` uses 128 buckets with sub-microsecond precision at low latencies and coarser granularity at high latencies.

---

## Bonus Task B: Compare Polling vs Interrupt Mode

SPDK supports interrupt mode on queue pairs. Allocate a second queue pair with interrupt mode enabled and run the same workload on both:

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

Expected result pattern:
- Polling mode: lower mean latency and higher IOPS at deep queues, 100% CPU utilisation
- Interrupt mode: higher tail latency, lower CPU burn, suitable for storage-latency-tolerant workloads

---

## Common Mistakes

### 1. Forgetting to call `spdk_nvme_qpair_process_completions()`

The NVMe driver does not use kernel interrupts. Completions only advance when you
explicitly call `spdk_nvme_qpair_process_completions()`. If your loop does not
call it, completions queue up and your bench hangs at the drain step.

### 2. Re-submitting before checking `in_flight`

Always gate re-submission on `!task->in_flight`. Submitting to a slot that still
has an outstanding I/O corrupts the DMA buffer and causes silent data errors or
segfaults in the completion callback.

### 3. Using stack-allocated buffers for DMA

NVMe DMA requires physically contiguous memory registered with DPDK's memory
subsystem. Allocate with `spdk_zmalloc(..., SPDK_MALLOC_DMA)` and free with
`spdk_free()`. Stack or `malloc()` buffers cause silent corruption or kernel panics.

### 4. Computing latency after `process_completions()` returns

Calling `spdk_get_ticks()` inside `io_complete()` gives per-I/O latency.
Computing the delta between submission and the return of `process_completions()`
measures a batch of completions and inflates latency numbers significantly.

### 5. Not draining the queue before computing elapsed time

Set `run_end_tsc` only after all in-flight I/Os have completed. Including drain
time in the denominator of your IOPS formula under-counts throughput for large
queue depths where drain can take tens of milliseconds.

### 6. Comparing against kernel `fio` without accounting for mode differences

Kernel `fio` uses the block layer and incurs context-switch and interrupt
overhead. SPDK polls from user space. Comparing raw numbers without noting this
difference leads to misleading conclusions. Always label your data source.

### 7. Running on a shared system

Other processes writing to the same NVMe device will pollute your latency
measurements. For accurate benchmarks: isolate the device with VFIO, pin the
benchmark thread to a dedicated CPU core with `taskset`, and disable CPU frequency
scaling (`cpupower frequency-set -g performance`).

---

## Checklist

- [ ] `make` succeeds without warnings
- [ ] Tool attaches to NVMe controller and prints namespace size
- [ ] All four patterns run without errors
- [ ] IOPS increases with queue depth up to device saturation
- [ ] Mean latency increases with queue depth (queueing delay)
- [ ] `spdk_nvme_perf -q 32 -w randread` result is within 10% of your tool at QD=32
- [ ] (Bonus A) Histogram output shows sane p50/p99/p99.9 values
- [ ] (Bonus B) Interrupt mode shows higher p99 latency than polling mode at QD=1

---

## Further Reading

- `$SPDK_DIR/app/spdk_nvme_perf/perf.c` — reference implementation with multi-core, multi-controller, and latency-histogram support
- `$SPDK_DIR/include/spdk/histogram_data.h` — SPDK histogram API
- `$SPDK_DIR/include/spdk/env.h` — `spdk_get_ticks`, `spdk_get_ticks_hz`, `spdk_zmalloc`
- `$SPDK_DIR/include/spdk/nvme.h` — full NVMe API surface
- SPDK documentation: https://spdk.io/doc/nvme.html
- NVMe specification 2.0, section 4 — queue pair and completion model

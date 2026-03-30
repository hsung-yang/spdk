# Module 23: Performance Optimization

**Stage**: 3 - Mastery
**Prerequisites**: Modules 1-22, solid understanding of SPDK threading model, NVMe driver internals, and bdev layer

---

## Overview

Performance optimization in SPDK requires a systematic methodology: measure first, identify the bottleneck, apply the smallest effective change, and measure again. SPDK's run-to-completion polling model already eliminates most of the overhead found in kernel I/O paths — interrupt handling, context switches, lock contention, and scheduler latency. What remains are subtler bottlenecks: NUMA misalignment, cache thrashing, suboptimal queue depths, inefficient polling loops, and misconfigured hardware.

This module covers the full optimization lifecycle from profiling tools through hardware configuration to production tuning patterns.

---

## 1. Profiling SPDK Applications

### 1.1 Linux `perf` for CPU Profiling

`perf` remains the most accurate starting point for CPU-bound optimization. Because SPDK runs in tight polling loops, CPU utilization is expected to be high — the goal is to understand *what* the CPU is doing, not to reduce utilization per se.

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

For applications that use huge pages and custom allocators, add `-F 99` to sample at 99 Hz (avoids exact multiples of scheduling ticks):

```bash
sudo perf record -F 99 -g -p $(pidof spdk_tgt) -- sleep 30
```

**Interpreting results**: In a well-tuned SPDK application, the hot path should show `spdk_nvme_qpair_process_completions` or `spdk_bdev_io_complete` consuming most cycles. If you see high time in memory allocation (`rte_malloc`, `spdk_malloc`) or in lock primitives, you have an allocation or locking problem.

### 1.2 Flame Graphs

Flame graphs convert `perf` output into a visual hierarchy showing where CPU time is spent across the entire call tree.

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

For off-CPU analysis (time spent waiting, not running):

```bash
# Requires BCC tools
sudo /usr/share/bcc/tools/offcputime -p $(pidof spdk_tgt) 30 > out.offcpu
~/flamegraph/flamegraph.pl --color=io --title="Off-CPU Time" --countname=us out.offcpu > offcpu.svg
```

Off-CPU flame graphs in SPDK should show near-zero time unless you have intentional sleeps or blocking system calls, which indicate a design problem.

### 1.3 SPDK Built-in Trace Framework

SPDK has a high-performance, lock-free, per-thread trace ring buffer. Trace points are defined throughout the NVMe driver, bdev layer, and NVMe-oF target. The overhead is a single atomic write per trace point, making it viable in production.

**Enable tracing at application startup**:

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

**Capture and analyze**:

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

**Adding custom trace points in your application**:

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

### 1.4 spdk_top: Real-Time Thread and Poller Monitoring

`spdk_top` is SPDK's equivalent of the Linux `top` utility. It connects to a running SPDK application via JSON-RPC and displays per-thread and per-poller statistics in an ncurses interface.

```bash
# Launch spdk_top against a running spdk_tgt
./build/bin/spdk_top -r /var/tmp/spdk.sock

# Key columns to watch:
# - Busy% : percentage of time the thread's pollers found work to do
# - Idle%  : percentage of time pollers returned 0 (no work)
# - Poller count: number of active pollers on this thread
```

**Interpreting spdk_top output**:

| Busy% | Interpretation |
|-------|----------------|
| 95-100% | Thread is saturated; consider adding a reactor or redistributing load |
| 50-80% | Healthy utilization with headroom |
| <20% | Thread is underloaded; work consolidation may improve cache locality |
| 0% | Thread has no pollers or all pollers are idle — may be wasted core |

The `spdk_top` data comes from the `framework_get_reactors` and `thread_get_stats` RPC methods, which you can also query directly:

```bash
# Get per-thread statistics via RPC
./scripts/rpc.py thread_get_stats

# Get reactor (core) statistics
./scripts/rpc.py framework_get_reactors
```

### 1.5 Histogram-Based Latency Tracking

SPDK provides `spdk_histogram_data` for in-process latency measurement with minimal overhead. The `spdk_nvme_perf` application (`app/spdk_nvme_perf/perf.c`) uses this pattern extensively, tracking per-I/O TSC deltas.

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

The latency cutoffs used in `spdk_nvme_perf` are worth noting — they cover P50, P90, P99, P99.9, P99.99, P99.999, and P99.9999, which is the correct set for storage performance characterization.

---

## 2. CPU Affinity and NUMA Awareness

### 2.1 Reactor CPU Assignment

SPDK's threading model maps one reactor thread per CPU core. Correct CPU assignment is the single most impactful configuration decision for performance.

**Configuration via JSON**:

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

**Command-line CPU mask** (hexadecimal bitmask):

```bash
# Use cores 0-3 (cores 0, 1, 2, 3)
./build/bin/spdk_tgt -m 0xF

# Use cores 4-7 on NUMA node 1
./build/bin/spdk_tgt -m 0xF0

# Use cores 2, 4, 6, 8 (even cores on NUMA node 0, skipping HT siblings)
./build/bin/spdk_tgt -m 0x154
```

**Checking NUMA topology before assigning cores**:

```bash
# Show NUMA node to CPU mapping
numactl --hardware

# Show which NVMe controllers are on which NUMA node
cat /sys/bus/pci/devices/0000:01:00.0/numa_node

# Show NUMA statistics for a process
numastat -p $(pidof spdk_tgt)
```

### 2.2 NUMA-Aware Memory Allocation

SPDK uses DPDK's EAL for memory management. Hugepage memory must be allocated on the same NUMA node as the CPU that will access it most frequently.

```bash
# Allocate 4GB of hugepages on NUMA node 0 only
echo 2048 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages

# For NUMA node 1
echo 2048 > /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages

# Pass NUMA-specific memory to SPDK (2GB on node0, 2GB on node1)
./build/bin/spdk_tgt -m 0xFF --mem-size 4096 --main-core 0
```

**In application code, allocate on the local NUMA node**:

```c
#include "spdk/env.h"

/* Allocate on the NUMA node of the current CPU */
int numa_node = spdk_env_get_current_core();  /* returns socket-aware core ID */
void *buf = spdk_malloc(size, alignment, NULL, numa_node, SPDK_MALLOC_DMA);

/* For I/O buffers, always use SPDK_MALLOC_DMA to ensure DMA-capability */
struct spdk_dma_buf *dma_buf = spdk_dma_malloc(io_size, 4096, NULL);
```

### 2.3 Avoiding Hyperthreading Siblings

Hyperthreading siblings share L1/L2 cache and execution units. Running two SPDK reactor threads on HT siblings degrades performance compared to using two physical cores.

```bash
# Identify HT sibling pairs
cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list
# Output: 0,16  (core 0 has HT siblings 0 and 16)

# Script to find all physical cores (avoiding HT siblings)
for cpu in /sys/devices/system/cpu/cpu*/topology/core_id; do
    echo "$(dirname $cpu | sed 's/.*cpu//'): $(cat $cpu)"
done | sort -t: -k2 -n | awk -F: '!seen[$2]++'
```

**Rule**: Assign one SPDK reactor per physical core. Use the lower-numbered sibling when in doubt.

### 2.4 CPU Frequency Scaling

SPDK polling loops rely on `spdk_get_ticks()` (RDTSC) for timing. Variable CPU frequency causes timing drift and can hurt latency consistency.

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

## 3. Memory Access Patterns and Cache Optimization

### 3.1 Cache Line Alignment

SPDK structures that are accessed in the hot path must be cache-line aligned to avoid false sharing and ensure single-cache-line reads.

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

### 3.2 Avoiding False Sharing

False sharing occurs when two threads modify different fields that reside on the same cache line. This is particularly problematic in SPDK's statistics collection code.

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

### 3.3 I/O Buffer Prefetching

For sequential workloads, prefetching the next I/O buffer before it is needed can hide memory latency:

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

### 3.4 Hugepage Configuration

All SPDK DMA buffers must reside in hugepage memory to avoid TLB misses during DMA operations.

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

**Sizing hugepage allocation**: A typical rule of thumb is 2x the sum of all I/O buffer pools across all reactors. For a system with 8 reactors, 512 outstanding I/Os per reactor, and 128KB I/O size: 8 × 512 × 128KB = 512MB minimum, allocate 1-2GB.

---

## 4. Lock Contention Analysis

SPDK's design philosophy is to eliminate locks from the I/O fast path entirely. Each reactor thread owns its data structures exclusively. However, locks do appear in:

- The initialization path
- Shared configuration structures
- The RPC handler path
- Cross-thread message passing (spdk_thread_send_msg)

### 4.1 Verifying Lock-Free Operation

```bash
# Check for mutex contention using perf lock
sudo perf lock record -p $(pidof spdk_tgt) -- sleep 5
sudo perf lock report

# If you see pthread_mutex_lock in the top symbols during steady-state I/O,
# there is a design problem — locks should not appear in the polling path.
```

### 4.2 Cross-Thread Communication Pattern

SPDK uses message passing instead of locks for cross-thread communication. The pattern is:

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

### 4.3 Identifying Unintended Locking

```bash
# Use strace to check for futex calls (mutex implementation) during I/O
sudo strace -p $(pidof spdk_tgt) -e trace=futex -c -- sleep 10

# Non-zero futex count during steady-state I/O is a red flag.
# Expected: futex calls only during startup/teardown and RPC handling.

# Use lockstat (if available) or BPF mutex tracking
sudo bpftrace -e 'tracepoint:lock:contention_begin { @[kstack] = count(); }'
```

---

## 5. I/O Depth Tuning and Queue Depth Optimization

Queue depth is one of the most impactful tuning parameters. Too low and you leave device bandwidth on the table. Too high and you increase average latency without additional throughput gain.

### 5.1 Finding the Optimal Queue Depth

The optimal queue depth depends on the device's internal parallelism and the latency/throughput trade-off your application requires. For most NVMe SSDs:

| Queue Depth | Typical Effect |
|-------------|----------------|
| 1 | Maximum latency visibility, minimum throughput |
| 4-8 | Good latency with moderate throughput |
| 32-64 | Near-peak throughput for most consumer/prosumer NVMe |
| 128-512 | Enterprise NVMe SSDs with high internal parallelism |
| >512 | Rarely beneficial; may increase tail latency |

### 5.2 Queue Depth in spdk_nvme_perf

The `spdk_nvme_perf` application (`app/spdk_nvme_perf/perf.c`) exposes queue depth tuning directly:

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

### 5.3 NVMe Queue Pair Configuration

The NVMe driver's queue pair size is controlled separately from the application-level queue depth:

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

### 5.4 Queue Depth Sweep Script

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

## 6. Batch Processing Strategies

### 6.1 Submission Batching

Submitting multiple I/Os before calling `spdk_nvme_qpair_process_completions` allows the NVMe controller to pipeline operations internally. This is the default pattern in SPDK applications.

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

### 6.2 Completion Batching

`spdk_nvme_qpair_process_completions` accepts a `max_completions` argument. Setting it to 0 processes all available completions in one call, which reduces per-completion overhead.

```c
/* Process up to 32 completions per poll — limits maximum latency spike */
int completed = spdk_nvme_qpair_process_completions(qpair, 32);

/* Process all available completions — maximum throughput */
int completed = spdk_nvme_qpair_process_completions(qpair, 0);
```

For latency-sensitive workloads, limit completions per call (e.g., 8-16) so that the poller loop returns quickly and can check for new submissions. For throughput-oriented workloads, use 0.

### 6.3 Poll Group for Multiple Queue Pairs

When managing multiple NVMe namespaces or multiple queue pairs, use a poll group to reduce the number of poller registrations:

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

## 7. Hardware Queue Configuration

### 7.1 NVMe Controller Capabilities

Before configuring queue depths, understand the controller's limits:

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

### 7.2 Number of Queue Pairs per Namespace

For multi-core SPDK applications, each reactor thread should have its own queue pair per namespace. This eliminates all queue pair contention.

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

### 7.3 PCIe Configuration

NVMe performance depends on PCIe link width and generation. Verify the link is running at its intended configuration:

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

## 8. Polling vs. Interrupt Mode Trade-offs

### 8.1 Polling Mode (Default)

SPDK's default is pure polling — the reactor thread continuously checks for completions. This approach delivers the lowest latency and highest throughput at the cost of 100% CPU utilization on the reactor core.

**When to use polling**:
- Latency-critical applications (P99 < 100µs)
- High-throughput applications (>500K IOPS)
- Environments where CPU cores are dedicated to storage (no time-sharing)

**Polling overhead analysis**:

```bash
# Measure cycles per poll when the device is idle
# Look for spdk_nvme_qpair_process_completions in perf output during idle
sudo perf stat -p $(pidof spdk_tgt) -e cycles,instructions,cache-misses -- sleep 5

# Expected: high cycles, high instructions, very low cache-misses
# (polling is cache-friendly — same code path repeatedly)
```

### 8.2 Interrupt Mode

NVMe-oF initiators and some specialized use cases support interrupt-driven completion. SPDK supports this via `SPDK_NVME_QPAIR_FAILURE_FUNC` and transport-specific options.

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

### 8.3 Adaptive Polling (Mixed Workloads)

For workloads with bursty I/O patterns, SPDK's scheduler supports adaptive polling that backs off to interrupt mode during idle periods:

```bash
# Enable the dynamic scheduler
./scripts/rpc.py framework_set_scheduler dynamic

# Configure scheduler parameters
./scripts/rpc.py framework_set_scheduler_options \
    --core-limit 95 \    # Back off when CPU is >95% busy
    --period 1000000     # Re-evaluate every 1ms
```

The dynamic scheduler monitors poller return values (`SPDK_POLLER_BUSY` vs `SPDK_POLLER_IDLE`) and can put idle threads to sleep on a timed interrupt, reducing power consumption without fully committing to interrupt-only mode.

---

## 9. NVMe-oF Specific Performance Tuning

### 9.1 Transport Configuration

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

**Key transport parameters**:

| Parameter | Effect | Tuning Direction |
|-----------|--------|-----------------|
| `max-queue-depth` | Maximum outstanding I/Os per queue pair | Increase for throughput, decrease for latency |
| `num-shared-buffers` | Pre-allocated network I/O buffers | Increase until buffer allocation errors disappear |
| `buf-cache-size` | Per-thread buffer cache size | Increase to reduce lock contention on buffer pool |
| `io-unit-size` | Maximum capsule/PDU data size | Match to application I/O size |

### 9.2 Network Stack Tuning for TCP Transport

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

### 9.3 RDMA Transport Tuning

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

### 9.4 NVMe-oF Initiator (Host-Side) Tuning

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

## 10. Benchmarking Methodology

### 10.1 bdevperf

`bdevperf` (`examples/bdev/bdevperf/bdevperf.c`) is the primary SPDK block device benchmark tool. It operates above the bdev abstraction layer, making it suitable for testing any bdev type.

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

`spdk_nvme_perf` (`app/spdk_nvme_perf/perf.c`) works directly with the NVMe driver, bypassing the bdev layer. It includes latency histogram output and supports multiple worker threads.

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

**Reading spdk_nvme_perf output**:

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

### 10.3 FIO with SPDK Plugin

The SPDK FIO plugin allows using fio's job file syntax with SPDK as the I/O engine, enabling complex workload modeling.

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

### 10.4 Benchmarking Methodology Checklist

Before running any benchmark:

1. **Stabilize the system**: Run a 5-10 minute warm-up I/O workload before measuring. NVMe SSDs have write amplification effects that settle during conditioning.
2. **Disable power management**: Set CPU governor to `performance`, disable ASPM.
3. **Pin CPUs**: Assign reactors to physical cores, not HT siblings.
4. **Allocate sufficient hugepages**: At least 2x the expected buffer working set.
5. **Isolate CPUs from OS scheduler**: Use `isolcpus` and `nohz_full` kernel parameters.
6. **Record baseline metrics**: CPU utilization, memory bandwidth, PCIe error counters.
7. **Sweep the variable of interest**: One variable at a time (queue depth, I/O size, core count).
8. **Report statistical measures**: Mean, P50, P99, P99.9 — never report only average latency.

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

## 11. Real Performance Tuning Examples

### 11.1 Example: Low Latency NVMe Optimization

**Before**: 4KB random read, single queue depth — P99 = 250µs

**Symptoms**: `perf report` shows 15% of time in `spdk_malloc` (buffer allocation in hot path).

**Investigation**:

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

**Fix**: Pre-allocate a buffer pool sized to the maximum queue depth:

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

**After**: P99 = 85µs (66% improvement). Buffer allocation removed from critical path.

### 11.2 Example: NUMA Misconfiguration

**Before**: Expected 4M IOPS from 8 NVMe devices across 2 NUMA nodes, seeing 2.8M IOPS.

**Symptoms**: `numastat -p $(pidof spdk_tgt)` shows high `numa_miss` on node 1.

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

**Fix**: Partition reactors and hugepages by NUMA node:

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

**After**: 3.9M IOPS (39% improvement). Remote NUMA accesses eliminated.

### 11.3 Example: NVMe-oF TCP Throughput Optimization

**Before**: Single-connection TCP NVMe-oF achieving 2GB/s sequential read, target device capable of 6GB/s.

**Symptoms**: `sar -n DEV 1` shows network interface at ~16Gbps (2GB/s), well below 100GbE line rate.

**Investigation**: CPU profiling shows 40% of time in TCP/IP stack processing on the NVMe-oF target.

**Fix 1**: Increase number of shared buffers to avoid buffer starvation:

```bash
./scripts/rpc.py nvmf_create_transport -t TCP \
    --num-shared-buffers 8192 \    # Was 512
    --buf-cache-size 256 \          # Per-thread buffer cache
    --io-unit-size 131072           # Match to 128KB I/O size
```

**Fix 2**: Add more initiator connections distributing across multiple target reactors:

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

**After**: 5.8GB/s (190% improvement). Buffer starvation eliminated, load distributed across reactors.

---

## 12. Common Performance Pitfalls

### 12.1 Blocking Calls in the Reactor Thread

**Symptom**: High latency spikes, other pollers on the same thread showing increased latency.

**Cause**: Any blocking system call (`sleep`, `read` on a blocking fd, `malloc` under memory pressure) inside a poller stalls all other pollers on that thread.

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

### 12.2 Underutilized Queue Depth

**Symptom**: Device throughput far below rated specification, CPU utilization low.

**Cause**: Application submits one I/O and waits for completion before submitting the next.

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

### 12.3 Buffer Allocation in the Hot Path

**Symptom**: `spdk_malloc` or `rte_malloc` visible in perf flame graphs on the I/O completion path.

**Cause**: I/O buffers or task structures allocated and freed per-I/O.

**Fix**: Use a freelist (ring buffer) of pre-allocated tasks and buffers. See example in Section 11.1.

### 12.4 Wrong CPU Topology for Device Affinity

**Symptom**: Good single-device performance but poor multi-device aggregate performance.

**Cause**: Reactors accessing NVMe devices on the wrong NUMA node.

**Fix**: Match reactor NUMA node to device NUMA node. Query device NUMA node with `cat /sys/bus/pci/devices/ADDR/numa_node`.

### 12.5 Excessive RPC/Management Traffic

**Symptom**: Intermittent latency spikes coinciding with monitoring intervals.

**Cause**: RPC handlers execute on a dedicated management thread but the `framework_get_reactors` and `thread_get_stats` calls aggregate state across reactor threads using message passing, which adds latency to the polled I/O path.

**Fix**: Reduce monitoring frequency in production. Use `spdk_top` at intervals of 5+ seconds rather than sub-second polling.

### 12.6 Memory Leak in Completion Callbacks

**Symptom**: Gradual performance degradation over hours, increasing hugepage consumption.

**Cause**: `spdk_bdev_io` objects not being freed after completion, or DMA buffers leaking.

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

## 13. bdev I/O Statistics Collection

SPDK's bdev layer provides built-in I/O statistics collection. Use this for performance monitoring without adding instrumentation overhead to the application.

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

Via RPC:

```bash
# Get bdev I/O statistics
./scripts/rpc.py bdev_get_iostat

# Get statistics for a specific bdev
./scripts/rpc.py bdev_get_iostat -b Nvme0n1

# Reset statistics after reading
./scripts/rpc.py bdev_reset_iostat -b Nvme0n1 --mode reset_on_read
```

---

## Key Takeaways

1. **Measure before optimizing**: `perf`, `spdk_top`, and SPDK trace points give you the evidence to target the real bottleneck, not assumed ones.

2. **NUMA alignment is foundational**: A NUMA misconfiguration silently degrades performance by 20-40%. Always verify with `numastat` before tuning anything else.

3. **Queue depth has a saturation point**: Sweep queue depth systematically. Most workloads saturate at QD=64-128; deeper queues add latency without IOPS.

4. **Pre-allocate everything in the hot path**: Buffer allocation, task structure allocation, and freelist operations in the I/O path eliminate a common source of latency spikes.

5. **Never block a reactor thread**: A single `sleep()` or blocking `read()` in a poller stalls every other poller on that thread. Use SPDK's message passing and timer APIs.

6. **Poll groups reduce poller overhead**: When managing many queue pairs, a single `spdk_nvme_poll_group_process_completions` is more efficient than N calls to `spdk_nvme_qpair_process_completions`.

7. **Flame graphs over intuition**: The actual bottleneck in a mature SPDK application is rarely where you expect it. Generate a flame graph first.

8. **Hardware configuration matters**: Verify PCIe link width, NVMe firmware version, and CPU frequency scaling before blaming software.

---

## Practice Exercises

### Exercise 1: Queue Depth Sweep

Using `spdk_nvme_perf`, perform a queue depth sweep on a local NVMe device from QD=1 to QD=256. Plot the IOPS vs. latency (P50, P99) curve. Identify the "knee" where adding more queue depth stops improving IOPS but continues increasing P99 latency.

```bash
# Template command
./build/bin/spdk_nvme_perf \
    -r 'trtype:PCIe traddr:<YOUR_ADDR>' \
    -q <QD> -o 4096 -w randread -t 10 --latency-tracking
```

Record your results and answer: At what queue depth does P99 latency exceed 500µs? What is the maximum IOPS achieved?

### Exercise 2: NUMA Impact Measurement

If you have a multi-NUMA system:
1. Pin SPDK to CPUs on NUMA node 0
2. Run bdevperf against NVMe devices on NUMA node 0 and node 1 separately
3. Measure the IOPS and latency difference
4. Check `numastat` to confirm remote memory accesses

If single-NUMA, simulate by pinning hugepage allocation to mismatched nodes and observing the impact.

### Exercise 3: Flame Graph Analysis

1. Start `spdk_tgt` with a bdev workload running
2. Generate a CPU flame graph using `perf` + FlameGraph
3. Identify the three largest call stacks by width
4. For each, determine whether it represents expected overhead or a potential optimization target
5. Add at least one custom trace point to your SPDK application and capture a trace

### Exercise 4: spdk_top Profiling Session

1. Start `spdk_tgt` with multiple bdev types (NVMe, null bdev, malloc bdev)
2. Run `spdk_top` and observe thread Busy% at idle, low load, and high load
3. Identify which thread becomes the bottleneck first as load increases
4. Use `framework_set_scheduler dynamic` and observe how the scheduler responds to varying load
5. Document the minimum Busy% threshold below which the dynamic scheduler backs off

### Exercise 5: NVMe-oF Latency vs. Local NVMe

1. Configure a loopback NVMe-oF TCP target (target and initiator on the same machine)
2. Benchmark using `spdk_nvme_perf` with `trtype:TCP` vs. `trtype:PCIe`
3. Measure P50 and P99 read latency at QD=1 (pure latency) and QD=64 (throughput)
4. Identify the transport overhead introduced by NVMe-oF TCP
5. Attempt to reduce the gap using `uring` sock implementation and tuned buffer sizes

---

## References

- `app/spdk_nvme_perf/perf.c` - Full-featured NVMe performance benchmark with histogram latency tracking
- `examples/bdev/bdevperf/bdevperf.c` - bdev-layer performance benchmark
- `app/spdk_top/spdk_top.c` - Real-time reactor/thread/poller monitoring tool
- `lib/trace/trace.c` - Lock-free trace framework implementation
- `app/trace/trace.cpp` - Trace post-processing and analysis tool
- `include/spdk/histogram_data.h` - Histogram API for latency measurement
- `include/spdk/bdev.h` - `spdk_bdev_io_stat`, `spdk_bdev_get_io_stat`, `spdk_bdev_get_device_stat`
- `include/spdk/nvme.h` - `spdk_nvme_io_qpair_opts`, queue pair configuration
- `scripts/rpc.py` - RPC client for runtime configuration and statistics collection
- SPDK Performance Tuning Guide: https://spdk.io/doc/performance_tuning.html

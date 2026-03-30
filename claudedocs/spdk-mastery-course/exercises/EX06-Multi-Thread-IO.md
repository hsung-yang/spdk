# Exercise 06: Multi-Thread I/O

## Overview

| Field | Value |
|-------|-------|
| Exercise | EX06 |
| Topic | Multi-Thread I/O with Per-Thread Channels |
| Estimated Time | 2-3 hours |
| Difficulty | Intermediate |

## Objective

Build a multi-threaded SPDK application where each SPDK thread performs block I/O to the same
bdev using its own private I/O channel. You will practice the core SPDK threading model:
thread creation, per-thread channel acquisition, inter-thread message passing, and result
aggregation via a coordinator thread.

By the end of this exercise you will be able to:

- Create named SPDK threads pinned to specific CPU cores
- Understand why each thread must obtain its own I/O channel
- Send messages between threads with `spdk_thread_send_msg`
- Use a coordinator thread to distribute work and collect per-thread results
- Confirm threads run on different cores by examining `/proc/self/status`

## Prerequisites

- Completed EX01 through EX05 (or equivalent familiarity with SPDK bdev I/O)
- A working SPDK build with `libbdev` and the `null` bdev module available
- Understanding of the reactor/poller model from EX04

## Background

### The SPDK Thread Model

SPDK threads (`struct spdk_thread`) are lightweight, stackless cooperative units that run
inside OS threads (typically DPDK lcores or POSIX pthreads). The key contract is:

- One SPDK thread runs on exactly one OS thread at a time.
- All work for an SPDK thread is driven by calling `spdk_thread_poll()` in a tight loop on
  the hosting OS thread.
- An SPDK thread must never block; long work is split into pollers and message callbacks.

### I/O Channels Are Thread-Local

`struct spdk_io_channel` is the object through which bdev I/O is submitted. The bdev layer
allocates per-thread state inside each channel. Because of this:

- **Every SPDK thread that submits I/O must call `spdk_bdev_get_io_channel()` from its own
  thread context** to obtain a private channel.
- A channel obtained on thread A **must not** be used from thread B. Doing so corrupts
  internal queue state and produces silent data errors or crashes.
- Channels are released with `spdk_put_io_channel()` from the same thread that acquired them.

### Inter-Thread Communication

```
Thread A                     Thread B
--------                     --------
spdk_thread_send_msg(B, fn, ctx)
  -- enqueues fn+ctx into B's message ring -->
                             spdk_thread_poll() drains ring
                             fn(ctx) executes on B
```

`spdk_thread_send_msg` is the only safe way to touch another thread's state. It is
asynchronous: the call returns before `fn` runs.

---

## Task Description

Build `ex06_mt_io` with the following behavior:

1. Initialise SPDK env and the bdev layer.
2. Open a `null` bdev (zero-copy, always succeeds, no real disk required).
3. Create **N worker SPDK threads** (default N=2, configurable), each pinned to a distinct
   CPU core.
4. The **main thread** acts as coordinator: it sends a "start I/O" message to each worker.
5. Each worker:
   a. Acquires its own I/O channel via `spdk_bdev_get_io_channel`.
   b. Issues a configurable number of read I/Os (default 64 per worker).
   c. Reports completion count back to the coordinator via a message.
6. The coordinator collects all reports, prints a summary, then tears everything down in the
   correct order (channels, threads, bdev).

---

## Step-by-Step Instructions

### Step 1: Project Skeleton

Create the directory and source file:

```
ex06/
  Makefile
  ex06_mt_io.c
```

Copy the Makefile from EX05 (or from `examples/bdev/bdev_hello_world`) and change
`APP = ex06_mt_io`.

### Step 2: Includes and Global State

```c
#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"

#define NUM_WORKERS     2
#define IOS_PER_WORKER  64
#define BDEV_NAME       "Null0"

/* Per-worker state - one instance per SPDK worker thread */
struct worker_ctx {
    int                     id;
    struct spdk_thread     *thread;       /* the worker's SPDK thread */
    struct spdk_io_channel *ch;           /* thread-local bdev channel */
    struct spdk_bdev_desc  *desc;         /* shared bdev descriptor (read-only use) */
    uint64_t                ios_submitted;
    uint64_t                ios_completed;
    uint64_t                ios_target;
    bool                    done;
};

/* Coordinator state */
struct coordinator_ctx {
    struct spdk_thread     *thread;       /* coordinator's SPDK thread (app thread) */
    struct spdk_bdev_desc  *desc;
    struct spdk_bdev       *bdev;
    struct worker_ctx       workers[NUM_WORKERS];
    int                     workers_done; /* count of workers that reported back */
};

static struct coordinator_ctx g_coordinator;
```

### Step 3: bdev Event Callback

A bdev descriptor requires an event callback. Implement a minimal one:

```c
static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
    SPDK_NOTICELOG("Bdev event: type %d on %s\n", type, spdk_bdev_get_name(bdev));
}
```

### Step 4: I/O Completion Callback

```c
struct io_req {
    struct worker_ctx *worker;
    void              *buf;
};

static void
read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_req     *req    = cb_arg;
    struct worker_ctx *worker = req->worker;

    spdk_bdev_free_io(bdev_io);
    spdk_dma_free(req->buf);
    free(req);

    worker->ios_completed++;

    if (worker->ios_completed == worker->ios_target) {
        worker->done = true;
        /* Notify coordinator - send message back to coordinator thread */
        spdk_thread_send_msg(g_coordinator.thread, worker_report_done, worker);
    }
}
```

### Step 5: I/O Submission (runs on the worker thread)

```c
static void
submit_one_read(struct worker_ctx *worker)
{
    struct io_req *req;
    uint32_t       block_size;
    uint64_t       num_blocks;
    int            rc;

    block_size = spdk_bdev_get_block_size(g_coordinator.bdev);
    num_blocks = spdk_bdev_get_num_blocks(g_coordinator.bdev);

    req = calloc(1, sizeof(*req));
    assert(req != NULL);
    req->worker = worker;
    req->buf    = spdk_dma_zmalloc(block_size, block_size, NULL);
    assert(req->buf != NULL);

    /* Offset cycles through bdev blocks, one block at a time */
    uint64_t offset = (worker->ios_submitted % num_blocks);

    rc = spdk_bdev_read_blocks(worker->desc, worker->ch,
                               req->buf, offset, 1,
                               read_complete, req);
    if (rc != 0) {
        SPDK_ERRLOG("Worker %d: spdk_bdev_read_blocks failed: %d\n", worker->id, rc);
        spdk_dma_free(req->buf);
        free(req);
        return;
    }
    worker->ios_submitted++;
}
```

### Step 6: Worker Start Handler (runs on the worker thread)

This function is invoked via `spdk_thread_send_msg` from the coordinator. From here the
worker is running on its own SPDK thread, so it is safe to call `spdk_bdev_get_io_channel`.

```c
static void
worker_start(void *arg)
{
    struct worker_ctx *worker = arg;
    uint64_t           i;

    /* Acquire a thread-local I/O channel. MUST be called from this thread. */
    worker->ch = spdk_bdev_get_io_channel(worker->desc);
    if (worker->ch == NULL) {
        SPDK_ERRLOG("Worker %d: failed to get I/O channel\n", worker->id);
        spdk_thread_send_msg(g_coordinator.thread, worker_report_done, worker);
        return;
    }

    SPDK_NOTICELOG("Worker %d: channel acquired, submitting %lu I/Os "
                   "(lcore %u)\n",
                   worker->id, worker->ios_target,
                   spdk_env_get_current_core());

    worker->ios_submitted = 0;
    worker->ios_completed = 0;

    for (i = 0; i < worker->ios_target; i++) {
        submit_one_read(worker);
    }
}
```

### Step 7: Worker Done Handler (runs on the coordinator thread)

```c
static void
worker_report_done(void *arg)
{
    struct worker_ctx     *worker = arg;
    struct coordinator_ctx *coord = &g_coordinator;

    SPDK_NOTICELOG("Coordinator: worker %d finished (%lu/%lu I/Os completed)\n",
                   worker->id, worker->ios_completed, worker->ios_submitted);

    /* Release the worker's channel - must be done from that worker's thread */
    spdk_thread_send_msg(worker->thread, worker_put_channel, worker);

    coord->workers_done++;
    if (coord->workers_done == NUM_WORKERS) {
        print_summary(coord);
        cleanup(coord);
    }
}

static void
worker_put_channel(void *arg)
{
    struct worker_ctx *worker = arg;

    if (worker->ch != NULL) {
        spdk_put_io_channel(worker->ch);
        worker->ch = NULL;
    }
    /* Signal the thread to exit after channel teardown drains */
    spdk_thread_exit(worker->thread);
}
```

### Step 8: Summary and Cleanup

```c
static void
print_summary(struct coordinator_ctx *coord)
{
    int i;
    uint64_t total = 0;

    printf("\n=== EX06 Multi-Thread I/O Summary ===\n");
    for (i = 0; i < NUM_WORKERS; i++) {
        printf("  Worker %d: %lu I/Os completed\n",
               coord->workers[i].id,
               coord->workers[i].ios_completed);
        total += coord->workers[i].ios_completed;
    }
    printf("  Total   : %lu I/Os\n", total);
    printf("======================================\n\n");
}

static void
cleanup(struct coordinator_ctx *coord)
{
    /* Close the bdev descriptor from the coordinator (app) thread */
    spdk_bdev_close(coord->desc);

    /* spdk_app_stop tears down the reactor and exits */
    spdk_app_stop(0);
}
```

### Step 9: Application Entry Point

```c
static void
app_start(void *arg1)
{
    struct coordinator_ctx *coord = &g_coordinator;
    struct spdk_cpuset      cpumask;
    int                     rc, i;
    char                    name[32];

    /* Record the coordinator (app) thread */
    coord->thread = spdk_get_thread();

    /* Open the bdev once; workers share the descriptor but each gets its own channel */
    rc = spdk_bdev_open_ext(BDEV_NAME, false, bdev_event_cb, NULL, &coord->desc);
    if (rc != 0) {
        SPDK_ERRLOG("Failed to open bdev '%s': %d\n", BDEV_NAME, rc);
        spdk_app_stop(rc);
        return;
    }
    coord->bdev = spdk_bdev_desc_get_bdev(coord->desc);

    SPDK_NOTICELOG("Bdev '%s' opened: %u-byte blocks, %lu blocks total\n",
                   BDEV_NAME,
                   spdk_bdev_get_block_size(coord->bdev),
                   spdk_bdev_get_num_blocks(coord->bdev));

    /* Create worker threads, each pinned to its own core */
    for (i = 0; i < NUM_WORKERS; i++) {
        struct worker_ctx *w = &coord->workers[i];

        w->id         = i;
        w->desc       = coord->desc;
        w->ios_target = IOS_PER_WORKER;
        w->done       = false;

        /* Pin to core (i + 1) so that core 0 stays for the coordinator */
        spdk_cpuset_zero(&cpumask);
        spdk_cpuset_set_cpu(&cpumask, (i + 1) % spdk_env_get_core_count(), true);

        snprintf(name, sizeof(name), "worker_%d", i);
        w->thread = spdk_thread_create(name, &cpumask);
        if (w->thread == NULL) {
            SPDK_ERRLOG("Failed to create SPDK thread for worker %d\n", i);
            spdk_app_stop(-ENOMEM);
            return;
        }
    }

    /* Dispatch work - each message runs worker_start() on the target thread */
    for (i = 0; i < NUM_WORKERS; i++) {
        spdk_thread_send_msg(coord->workers[i].thread,
                             worker_start, &coord->workers[i]);
    }
}

int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name    = "ex06_mt_io";
    opts.reactor_mask = "0x7";   /* cores 0, 1, 2 */

    rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    rc = spdk_app_start(&opts, app_start, NULL);
    spdk_app_fini();
    return rc;
}
```

---

## Complete Solution

The sections above form the complete solution. Assemble them in order in `ex06_mt_io.c`:

```
1. Includes and macro definitions
2. struct worker_ctx
3. struct coordinator_ctx + g_coordinator global
4. Forward declarations (worker_report_done, worker_put_channel, cleanup, print_summary)
5. bdev_event_cb
6. read_complete
7. submit_one_read
8. worker_start
9. worker_put_channel
10. worker_report_done
11. print_summary
12. cleanup
13. app_start
14. main
```

Forward declare any function that is called before it is defined, for example:

```c
static void worker_report_done(void *arg);
static void worker_put_channel(void *arg);
static void cleanup(struct coordinator_ctx *coord);
static void print_summary(struct coordinator_ctx *coord);
```

---

## Build Instructions

```bash
# From the SPDK root
cd /path/to/spdk

# Minimal Makefile for the exercise
cat > ex06/Makefile << 'EOF'
APP = ex06_mt_io
SRCS = ex06_mt_io.c

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SPDK_LIB_LIST = event event_bdev bdev bdev_null log env_dpdk
LIBS += $(SPDK_LIB_LIST:%=-lspdk_%)
LIBS += $(SPDK_DPDK_LIB_LIST:%=-l%)

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
EOF

make -C ex06 -j$(nproc)
```

---

## Run Instructions

First, configure huge pages and bind your null bdev module:

```bash
# Bind huge pages (once per reboot)
sudo scripts/setup.sh

# Run with a JSON config that creates a Null bdev
# Create ex06/bdev.json:
cat > ex06/bdev.json << 'EOF'
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_null_create",
          "params": {
            "name": "Null0",
            "num_blocks": 102400,
            "block_size": 512
          }
        }
      ]
    }
  ]
}
EOF

sudo ./ex06/ex06_mt_io \
    --json ex06/bdev.json \
    -m 0x7 \
    --no-pci
```

Expected output (order of worker lines may vary):

```
[NOTICE]: Bdev 'Null0' opened: 512-byte blocks, 102400 blocks total
[NOTICE]: Worker 0: channel acquired, submitting 64 I/Os (lcore 1)
[NOTICE]: Worker 1: channel acquired, submitting 64 I/Os (lcore 2)
[NOTICE]: Coordinator: worker 0 finished (64/64 I/Os completed)
[NOTICE]: Coordinator: worker 1 finished (64/64 I/Os completed)

=== EX06 Multi-Thread I/O Summary ===
  Worker 0: 64 I/Os completed
  Worker 1: 64 I/Os completed
  Total   : 128 I/Os
======================================
```

---

## Verifying Threads Run on Different Cores

### Method 1: spdk_env_get_current_core()

The `worker_start` callback already logs the lcore number. Confirm that each worker prints a
different value.

### Method 2: Examine /proc/self/task

While the process is running, list its threads and their CPU affinity:

```bash
# Find the PID
PID=$(pgrep ex06_mt_io)

# List threads and their current CPU
for TID in /proc/$PID/task/*; do
    TNAME=$(cat $TID/comm 2>/dev/null)
    CPU=$(cat $TID/status 2>/dev/null | grep -i 'cpusallowed:')
    echo "TID $(basename $TID) ($TNAME): $CPU"
done
```

### Method 3: taskset

```bash
PID=$(pgrep ex06_mt_io)
for TID in $(ls /proc/$PID/task/); do
    echo -n "TID $TID: "
    taskset -p $TID 2>/dev/null
done
```

Each worker thread should show a single-bit CPU mask matching its pinned core.

### Method 4: perf or htop

Run `htop` in a second terminal and press `F2 -> Display -> Show custom thread names`. Each
SPDK thread will appear as a separate row. Worker threads pinned to different cores will show
`CPU` column values that differ.

---

## Bonus: Coordinator-Driven Work Distribution

Extend the exercise so that the coordinator controls exactly how many I/Os go to each worker,
and workers request more work instead of submitting a fixed batch upfront.

### Additional Structures

```c
struct work_request {
    struct worker_ctx      *worker;
    uint64_t                start_block;
    uint64_t                num_ios;
};

struct coordinator_ctx {
    /* ... existing fields ... */
    uint64_t total_ios_requested; /* coordinator tracks grand total */
    pthread_mutex_t results_lock; /* only needed if results are written off-thread */
};
```

### Coordinator Dispatches Batches

```c
static void
coordinator_dispatch_batch(struct coordinator_ctx *coord, int worker_id,
                           uint64_t start_block, uint64_t num_ios)
{
    struct work_request *req = calloc(1, sizeof(*req));
    assert(req != NULL);

    req->worker      = &coord->workers[worker_id];
    req->start_block = start_block;
    req->num_ios     = num_ios;

    spdk_thread_send_msg(coord->workers[worker_id].thread,
                         worker_execute_batch, req);
}
```

### Worker Executes Batch and Requests More

```c
static void
worker_execute_batch(void *arg)
{
    struct work_request *req    = arg;
    struct worker_ctx   *worker = req->worker;
    uint64_t             i;

    for (i = 0; i < req->num_ios; i++) {
        submit_read_at_block(worker, req->start_block + i);
    }
    free(req);
}
```

When all I/Os in a batch complete, the worker sends a message to the coordinator requesting
another batch or signalling that it is idle. The coordinator then re-distributes remaining
work dynamically, balancing load across workers.

This pattern models how SPDK's NVMe-oF target distributes incoming commands across I/O
queues and CPU cores.

---

## Common Mistakes

### 1. Sharing an I/O Channel Across Threads

**Wrong:**
```c
/* Coordinator gets one channel, passes it to all workers */
g_shared_ch = spdk_bdev_get_io_channel(desc);
spdk_thread_send_msg(worker->thread, worker_start, g_shared_ch); /* BUG */
```

**Why it breaks:** The bdev layer stores a back-pointer from the channel to its owning
thread. Submitting I/O from a different thread corrupts the channel's internal queue and
produces assertion failures or silent data corruption.

**Correct:** Each worker must call `spdk_bdev_get_io_channel` from within its own thread
context (i.e., inside a message callback or poller executing on that thread).

---

### 2. Calling spdk_bdev_get_io_channel Before the Thread Starts

**Wrong:**
```c
/* In app_start(), before sending any message to the worker thread */
worker->ch = spdk_bdev_get_io_channel(desc); /* runs on coordinator thread */
worker->thread = spdk_thread_create(...);
spdk_thread_send_msg(worker->thread, worker_start, worker); /* ch was created on wrong thread */
```

**Correct:** Always acquire the channel inside the first message or poller that executes on
the intended thread.

---

### 3. Putting a Channel from the Wrong Thread

**Wrong:**
```c
/* Coordinator tears down the channel on behalf of a worker */
spdk_put_io_channel(worker->ch); /* runs on coordinator thread - BUG */
```

**Correct:** Route the teardown back to the owning thread:
```c
spdk_thread_send_msg(worker->thread, worker_put_channel, worker);
```

---

### 4. Destroying a Thread Before Its Channel Is Released

SPDK defers channel destruction internally, but `spdk_thread_exit` must not be called until
`spdk_put_io_channel` has been called. The correct sequence is:

```
worker thread:
  1. spdk_put_io_channel(ch)
  2. spdk_thread_exit(thread)   /* only after put */
```

If you call `spdk_thread_exit` before releasing all channels, the thread's destructor will
assert or hang waiting for outstanding channel references.

---

### 5. Forgetting to Poll Worker Threads

An SPDK thread only makes progress when `spdk_thread_poll()` is called on it. The
`spdk_app_start` reactor polls the app thread automatically. Worker threads created with
`spdk_thread_create` are polled by the DPDK lcore assigned via the cpumask. If you use a
reactor mask (`-m`/`reactor_mask`) that does not include the cores you pin workers to, those
workers will never run.

Always ensure: **number of lcores in reactor mask >= number of worker threads + 1** (for the
coordinator).

---

### 6. Race Between Completion Callback and Thread Exit

Do not exit a thread while I/Os are still in flight. The safe pattern is:

```
worker: submit all I/Os
  -> read_complete increments ios_completed each time
  -> when ios_completed == ios_target, send "done" message to coordinator
coordinator: receives "done" message
  -> sends "put channel" message back to worker
worker: puts channel, then calls spdk_thread_exit
```

This guarantees no in-flight I/Os exist when the channel is released.

---

## Key API Reference

| API | Thread | Purpose |
|-----|--------|---------|
| `spdk_thread_create(name, cpumask)` | any | Create a new SPDK thread |
| `spdk_get_thread()` | any | Get pointer to current SPDK thread |
| `spdk_thread_send_msg(thread, fn, ctx)` | any | Schedule `fn(ctx)` on `thread` asynchronously |
| `spdk_thread_exit(thread)` | owning thread | Begin graceful thread shutdown |
| `spdk_thread_destroy(thread)` | any (after exit) | Free thread resources |
| `spdk_bdev_get_io_channel(desc)` | owning thread | Acquire a per-thread I/O channel |
| `spdk_put_io_channel(ch)` | owning thread | Release a per-thread I/O channel |
| `spdk_bdev_read_blocks(desc, ch, ...)` | owning thread | Submit async block read |
| `spdk_bdev_free_io(bdev_io)` | completion callback | Return I/O to free pool |
| `spdk_env_get_current_core()` | any | Query which lcore is executing |
| `spdk_env_get_core_count()` | any | Total number of available lcores |

---

## Review Questions

1. Why does `spdk_bdev_get_io_channel` need to be called from the thread that will use the
   channel, rather than from any convenient thread?

2. `spdk_thread_send_msg` is described as "asynchronous". What does this mean for the
   ordering of messages sent from the same source thread to the same destination thread?

3. If two workers finish at nearly the same time and both send a message to the coordinator,
   is there a race condition in `workers_done++`? Why or why not?

4. What happens if you set `reactor_mask = "0x1"` (core 0 only) but create two worker
   threads with cpumask targeting cores 1 and 2?

5. Modify the solution to support a configurable number of workers passed as a command-line
   argument using `spdk_app_parse_args`.

---

## Next Exercise

EX07: Poller-Based Rate Limiting - implement a token-bucket rate limiter using SPDK pollers
to cap the IOPS issued by the multi-threaded application from this exercise.

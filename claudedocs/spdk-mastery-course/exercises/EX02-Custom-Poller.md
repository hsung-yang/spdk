# Exercise 02: Custom Poller

**Stage**: 2 (Implementation)
**Difficulty**: Intermediate
**Estimated Time**: 1-2 hours
**Prerequisites**: Module 12 (Event Framework), Exercise 01 (Hello SPDK App)

---

## Objective

Build a standalone SPDK application that registers two pollers:

1. A **timed poller** that fires every second to log CPU tick counts and a simulated memory usage stat.
2. A **busy poller** (period = 0) that counts invocations and self-unregisters after a fixed number of cycles.

By the end of this exercise you will understand:
- How to register, pause, resume, and unregister pollers.
- The difference between timed pollers and busy pollers.
- How to return `SPDK_POLLER_IDLE` vs `SPDK_POLLER_BUSY` correctly.
- The lifetime rules for poller context data.

---

## Background: Poller Types

SPDK pollers are callbacks registered on an SPDK thread. The reactor calls them on every loop iteration (or after a minimum interval). There are two flavors:

| Type | `period_microseconds` | Behavior |
|------|-----------------------|----------|
| **Busy poller** | `0` | Called on every reactor loop iteration. Use for hot paths where latency matters. |
| **Timed poller** | `> 0` | Called no more often than the given interval. Use for background housekeeping. |

Both types return `enum spdk_thread_poller_rc`:

```c
SPDK_POLLER_IDLE  // No work was done this invocation
SPDK_POLLER_BUSY  // Work was done; hint to reactor that we are active
```

The return value is a hint to the reactor's interrupt-mode logic. Returning `SPDK_POLLER_BUSY` when you have done real work allows SPDK to stay in polling mode rather than transitioning to interrupt mode.

---

## Step-by-Step Instructions

### Step 1: Create the source file

```bash
mkdir -p /path/to/your/spdk-exercises/ex02
cd /path/to/your/spdk-exercises/ex02
```

Create `custom_poller.c` with the content shown in the Solution section below.

### Step 2: Write the stats poller callback

The stats poller fires every 1,000,000 microseconds (1 second). Inside it:
- Call `spdk_get_ticks()` to obtain the current CPU tick counter.
- Call `spdk_get_ticks_hz()` to get the tick frequency so you can compute elapsed seconds.
- Simulate a memory usage value (in a real application you would read `/proc/self/status`).
- Log the values with `SPDK_NOTICELOG`.
- Return `SPDK_POLLER_BUSY` because you did real work.

### Step 3: Write the busy poller callback

The busy poller runs on every reactor iteration. Inside it:
- Increment a counter stored in its context struct.
- When the counter reaches a threshold (e.g., 1,000,000 iterations), log a message and unregister itself by calling `spdk_poller_unregister(&ctx->busy_poller)`.
- Return `SPDK_POLLER_BUSY` while counting, `SPDK_POLLER_IDLE` if there is nothing left to do.

### Step 4: Wire pollers in the app-started callback

Register both pollers inside `app_started()`:

```c
ctx->stats_poller = SPDK_POLLER_REGISTER(stats_poller_cb, ctx, 1000000);
ctx->busy_poller  = SPDK_POLLER_REGISTER(busy_poller_cb,  ctx, 0);
```

`SPDK_POLLER_REGISTER` is a convenience macro that calls `spdk_poller_register_named` and automatically uses the function name as the poller name — useful when reading `spdk_top` or reactor stats.

### Step 5: Graceful shutdown

Register a signal handler or use a timed poller to call `spdk_app_stop(0)` after a few seconds so the exercise terminates cleanly.

---

## Solution Code

```c
/*
 * custom_poller.c - EX02: Custom Poller
 *
 * Demonstrates timed pollers and busy pollers in an SPDK application.
 * Build with the SPDK app framework (see Makefile below).
 */

#include "spdk/stdinc.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/util.h"

/* How many reactor iterations the busy poller runs before self-unregistering */
#define BUSY_POLLER_MAX_ITERS  1000000

/* How many seconds the stats poller runs before the app stops */
#define STATS_POLLER_MAX_TICKS 10

struct poller_ctx {
    struct spdk_poller *stats_poller;
    struct spdk_poller *busy_poller;

    /* Busy-poller state */
    uint64_t busy_count;

    /* Stats-poller state */
    uint64_t stats_fire_count;
    uint64_t start_ticks;
};

/* ------------------------------------------------------------------ */
/* Timed poller: fires once per second                                  */
/* ------------------------------------------------------------------ */
static int
stats_poller_cb(void *arg)
{
    struct poller_ctx *ctx = arg;
    uint64_t now      = spdk_get_ticks();
    uint64_t hz       = spdk_get_ticks_hz();
    uint64_t elapsed  = (now - ctx->start_ticks);      /* in ticks       */
    double   elapsed_s = (double)elapsed / (double)hz; /* in seconds     */

    ctx->stats_fire_count++;

    /* Simulate a memory reading; replace with a real /proc/self/status
     * parse in production code.                                          */
    uint64_t simulated_rss_kb = 4096 + (ctx->stats_fire_count * 128);

    SPDK_NOTICELOG("[stats #%lu] elapsed=%.2fs  ticks=%lu  hz=%lu  "
                   "sim_rss=%lu kB\n",
                   ctx->stats_fire_count, elapsed_s,
                   now, hz, simulated_rss_kb);

    /* Stop the application after STATS_POLLER_MAX_TICKS seconds */
    if (ctx->stats_fire_count >= STATS_POLLER_MAX_TICKS) {
        SPDK_NOTICELOG("Stats poller reached limit, stopping application.\n");
        spdk_app_stop(0);
    }

    return SPDK_POLLER_BUSY;
}

/* ------------------------------------------------------------------ */
/* Busy poller: runs every reactor iteration (period = 0)              */
/* ------------------------------------------------------------------ */
static int
busy_poller_cb(void *arg)
{
    struct poller_ctx *ctx = arg;

    ctx->busy_count++;

    if (ctx->busy_count >= BUSY_POLLER_MAX_ITERS) {
        SPDK_NOTICELOG("[busy] Reached %lu iterations — unregistering.\n",
                       ctx->busy_count);
        /*
         * Self-unregister: passing a pointer-to-pointer causes
         * spdk_poller_unregister to also NULL out ctx->busy_poller,
         * preventing double-free.
         */
        spdk_poller_unregister(&ctx->busy_poller);
        return SPDK_POLLER_IDLE;
    }

    /* Log a progress message every 100,000 iterations */
    if (ctx->busy_count % 100000 == 0) {
        SPDK_NOTICELOG("[busy] iteration %lu / %d\n",
                       ctx->busy_count, BUSY_POLLER_MAX_ITERS);
    }

    return SPDK_POLLER_BUSY;
}

/* ------------------------------------------------------------------ */
/* Application entry point (called by SPDK after subsystem init)       */
/* ------------------------------------------------------------------ */
static void
app_started(void *arg)
{
    struct poller_ctx *ctx = arg;

    ctx->start_ticks = spdk_get_ticks();
    ctx->busy_count  = 0;
    ctx->stats_fire_count = 0;

    SPDK_NOTICELOG("Application started. Registering pollers...\n");

    /*
     * SPDK_POLLER_REGISTER is preferred over spdk_poller_register because
     * it records the function name as the poller name, making it visible in
     * spdk_top and the JSON-RPC thread_get_stats output.
     */
    ctx->stats_poller = SPDK_POLLER_REGISTER(stats_poller_cb, ctx,
                                              1000000 /* 1 second */);
    if (ctx->stats_poller == NULL) {
        SPDK_ERRLOG("Failed to register stats poller\n");
        spdk_app_stop(1);
        return;
    }

    ctx->busy_poller = SPDK_POLLER_REGISTER(busy_poller_cb, ctx, 0);
    if (ctx->busy_poller == NULL) {
        SPDK_ERRLOG("Failed to register busy poller\n");
        spdk_poller_unregister(&ctx->stats_poller);
        spdk_app_stop(1);
        return;
    }

    SPDK_NOTICELOG("Both pollers registered successfully.\n");
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    struct poller_ctx    ctx  = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name        = "custom_poller";
    opts.reactor_mask = "0x1"; /* Use only CPU 0 to keep output readable */

    rc = spdk_app_parse_args(argc, argv, &opts, "", NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    rc = spdk_app_start(&opts, app_started, &ctx);

    spdk_app_fini();
    return rc;
}
```

---

## Makefile

Save this as `Makefile` in the same directory. Adjust `SPDK_DIR` to point to your SPDK build root.

```makefile
SPDK_DIR ?= $(HOME)/spdk

APP = custom_poller
SRCS = custom_poller.c

include $(SPDK_DIR)/mk/spdk.common.mk
include $(SPDK_DIR)/mk/spdk.app.mk

SPDK_LIB_LIST = event log thread util env_dpdk

include $(SPDK_DIR)/mk/spdk.app_vars.mk
include $(SPDK_DIR)/mk/spdk.deps.mk
```

---

## Build and Run

```bash
# Build (from the exercise directory)
make SPDK_DIR=/path/to/spdk

# Run — must be root or have hugepage permissions
sudo ./custom_poller --no-pci -m 256 --huge-dir /dev/hugepages

# Or with a minimal DPDK configuration:
sudo ./custom_poller --no-pci --iova-mode=pa
```

---

## Expected Output

```
[2024-01-15 10:00:00.001] NOTICE: Application started. Registering pollers...
[2024-01-15 10:00:00.001] NOTICE: Both pollers registered successfully.
[2024-01-15 10:00:00.001] NOTICE: [busy] iteration 100000 / 1000000
[2024-01-15 10:00:00.002] NOTICE: [busy] iteration 200000 / 1000000
...
[2024-01-15 10:00:00.004] NOTICE: [busy] iteration 1000000 / 1000000
[2024-01-15 10:00:00.004] NOTICE: [busy] Reached 1000000 iterations — unregistering.
[2024-01-15 10:00:01.001] NOTICE: [stats #1] elapsed=1.00s  ticks=2400000000  hz=2400000000  sim_rss=4224 kB
[2024-01-15 10:00:02.001] NOTICE: [stats #2] elapsed=2.00s  ticks=4800000000  hz=2400000000  sim_rss=4352 kB
...
[2024-01-15 10:00:10.001] NOTICE: [stats #10] elapsed=10.00s  ticks=24000000000  hz=2400000000  sim_rss=5376 kB
[2024-01-15 10:00:10.001] NOTICE: Stats poller reached limit, stopping application.
```

Key observations:
- The busy poller fires thousands of times before the first stats poller tick.
- The busy poller self-unregisters cleanly (no crash, no memory leak).
- The stats poller fires at approximately 1-second intervals.
- `spdk_get_ticks_hz()` gives the tick frequency — divide ticks by hz to get seconds.

---

## Bonus: Collect bdev I/O Statistics Periodically

Extend the exercise by adding a third poller that queries live bdev I/O stats. This requires a bdev to be present (use the `null` bdev for testing).

### Add a null bdev via JSON config (`config.json`)

```json
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
```

### bdev stats poller additions

Add these headers:

```c
#include "spdk/bdev.h"
```

Add to `poller_ctx`:

```c
    struct spdk_poller *bdev_stats_poller;
    struct spdk_io_channel *bdev_ch;
    struct spdk_bdev_desc *bdev_desc;
```

Add the poller callback:

```c
static int
bdev_stats_poller_cb(void *arg)
{
    struct poller_ctx *ctx = arg;

    if (ctx->bdev_ch == NULL) {
        return SPDK_POLLER_IDLE;
    }

    struct spdk_bdev_io_stat stat = {};
    /*
     * spdk_bdev_get_io_stat() is asynchronous in newer SPDK versions;
     * for local stats on a single channel use spdk_bdev_io_stat_add()
     * after polling the channel.  The pattern below uses the synchronous
     * channel-local stat that is always safe to read on the same thread
     * that owns the channel.
     */
    spdk_bdev_get_device_stat(spdk_bdev_desc_get_bdev(ctx->bdev_desc),
                              &stat, NULL, NULL);

    SPDK_NOTICELOG("[bdev_stats] bytes_read=%lu  bytes_written=%lu  "
                   "num_read_ops=%lu  num_write_ops=%lu\n",
                   stat.bytes_read, stat.bytes_written,
                   stat.num_read_ops, stat.num_write_ops);

    return SPDK_POLLER_BUSY;
}
```

Register it in `app_started()`:

```c
    /* Open the null bdev (created via JSON config) */
    int open_rc = spdk_bdev_open_ext("Null0", false, NULL, NULL,
                                     &ctx->bdev_desc);
    if (open_rc == 0) {
        ctx->bdev_ch = spdk_bdev_get_io_channel(ctx->bdev_desc);
        ctx->bdev_stats_poller = SPDK_POLLER_REGISTER(bdev_stats_poller_cb,
                                                       ctx, 2000000 /* 2s */);
    } else {
        SPDK_WARNLOG("Null0 bdev not found — skipping bdev stats poller\n");
    }
```

Run with the config:

```bash
sudo ./custom_poller --no-pci -c config.json
```

---

## Common Mistakes

### 1. Registering a poller outside an SPDK thread

`spdk_poller_register()` and `SPDK_POLLER_REGISTER` must be called from a function that runs on an SPDK thread (e.g., inside a callback passed to `spdk_app_start`, or a message sent via `spdk_thread_send_msg`). Calling it from `main()` directly will assert or silently fail.

```c
/* WRONG — called before the SPDK reactor is running */
int main(...) {
    spdk_app_opts_init(&opts, sizeof(opts));
    my_poller = spdk_poller_register(cb, NULL, 1000000); /* crash/assert */
    spdk_app_start(&opts, app_started, NULL);
}

/* CORRECT — called inside the app_started callback */
static void app_started(void *arg) {
    my_poller = SPDK_POLLER_REGISTER(cb, arg, 1000000); /* safe */
}
```

### 2. Unregistering a poller from a different thread

A poller must be unregistered from the same SPDK thread that registered it. Calling `spdk_poller_unregister` from another thread is undefined behavior. Use `spdk_thread_send_msg` to dispatch the unregister call to the correct thread if needed.

### 3. Forgetting to NULL-check after registration

`SPDK_POLLER_REGISTER` and `spdk_poller_register` return `NULL` on failure. Always check the return value before using the pointer.

### 4. Accessing freed context after unregistration

The poller context (`arg`) is owned by your code. After calling `spdk_poller_unregister`, no further calls will be made to your callback, but you must ensure the context outlives any in-flight invocation. The safest pattern is to free context only in a shutdown callback, not in the poller itself.

### 5. Returning the wrong poller_rc value

Always return `SPDK_POLLER_BUSY` when you performed meaningful work and `SPDK_POLLER_IDLE` when you had nothing to do. Returning `SPDK_POLLER_BUSY` unconditionally from a mostly-idle poller prevents the reactor from entering interrupt mode and wastes CPU cycles.

### 6. Using wall-clock sleep inside a poller

Never call `sleep()`, `usleep()`, or any blocking function inside a poller. This blocks the entire reactor thread and defeats the purpose of the event-driven model. Use a timed poller with the desired interval instead.

---

## Key API Reference

| Function | Description |
|----------|-------------|
| `SPDK_POLLER_REGISTER(fn, arg, period_us)` | Register a named poller on the current thread |
| `spdk_poller_register(fn, arg, period_us)` | Register a poller (no automatic name) |
| `spdk_poller_register_named(fn, arg, period_us, name)` | Register a poller with an explicit name |
| `spdk_poller_unregister(struct spdk_poller **)` | Unregister and NULL the pointer |
| `spdk_poller_pause(struct spdk_poller *)` | Suspend a poller temporarily |
| `spdk_poller_resume(struct spdk_poller *)` | Resume a paused poller |
| `spdk_get_ticks()` | Current CPU tick counter |
| `spdk_get_ticks_hz()` | CPU tick frequency (ticks per second) |

---

## Summary

In this exercise you:

1. Created a timed poller firing at a 1-second interval to log system stats using `SPDK_POLLER_REGISTER` with `period_microseconds = 1000000`.
2. Created a busy poller (`period_microseconds = 0`) that self-unregisters after a fixed number of iterations.
3. Used `spdk_get_ticks()` and `spdk_get_ticks_hz()` to compute elapsed time without blocking.
4. Learned the correct return values (`SPDK_POLLER_BUSY` / `SPDK_POLLER_IDLE`) and why they matter for reactor efficiency.
5. (Bonus) Extended the pattern to collect bdev I/O stats from a live bdev channel.

The poller pattern you practiced here is the foundation of every SPDK subsystem — bdev drivers, NVMf targets, and network stacks all rely on the same `spdk_poller_register` mechanism for their completion-polling loops.

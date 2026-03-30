# Exercise 03: Simple I/O Application

**Stage**: 2 (Implementation)
**Difficulty**: Intermediate
**Estimated Time**: 2-3 hours
**Prerequisites**: Modules 01-13, Exercise 01, Exercise 02

---

## Objective

Build a complete SPDK application that performs sequential and random I/O operations
against a bdev. You will open a bdev, allocate DMA-capable buffers, write data, read
it back, and verify correctness. By the end you will have a working I/O loop that
demonstrates the full async callback model used throughout SPDK.

---

## What You Will Learn

- Opening a bdev descriptor with `spdk_bdev_open_ext()`
- Obtaining a per-thread I/O channel with `spdk_bdev_get_io_channel()`
- Allocating DMA-capable buffers with `spdk_dma_malloc()`
- Submitting write and read I/Os via `spdk_bdev_write_blocks()` / `spdk_bdev_read_blocks()`
- Handling async I/O completions with `spdk_bdev_io_completion_cb`
- Freeing `spdk_bdev_io` objects with `spdk_bdev_free_io()`
- Tearing down cleanly (channel, descriptor, app)

---

## Background

The SPDK bdev layer provides a uniform block-device interface regardless of the
underlying transport (NVMe, AIO, Malloc, etc.). All I/O is submitted asynchronously
and a callback is invoked on the same reactor thread when the I/O completes. This
means **you must never block inside a callback**; any work that depends on a completed
I/O must be driven from the callback itself.

The pattern for every I/O operation is:

```
spdk_bdev_open_ext()        -> descriptor
spdk_bdev_get_io_channel()  -> io_channel   (per-thread)
spdk_dma_malloc()           -> buf          (DMA-safe memory)

spdk_bdev_write_blocks(desc, ch, buf, lba, num_blocks, write_cb, ctx)
  -- write_cb called on completion --
spdk_bdev_read_blocks(desc, ch, buf, lba, num_blocks, read_cb, ctx)
  -- read_cb called on completion --

spdk_bdev_free_io()         (inside every callback)
spdk_put_io_channel()
spdk_bdev_close()
spdk_app_stop()
```

---

## Prerequisites Checklist

Before starting, verify:

- [ ] SPDK is built: `ls $SPDK_DIR/build/lib/libspdk_bdev.so` (or `.a`)
- [ ] Huge pages are configured: `cat /proc/meminfo | grep HugePages_Total`
- [ ] You understand SPDK reactor threads (Module 07)
- [ ] You have read Module 13 (Bdev Layer)

---

## Part 1: Sequential I/O (Write then Read-Back Verify)

### Step 1: Create the Application Directory

```bash
cd $SPDK_DIR
mkdir -p app/ex03_simple_io
cd app/ex03_simple_io
```

### Step 2: Write the Source File

Create `ex03_simple_io.c` with the content below. Read every comment carefully
before you copy anything.

```c
/*
 * EX03: Simple I/O Application
 *
 * Demonstrates sequential write -> read -> verify using the SPDK bdev API.
 * Uses a Malloc bdev so no physical hardware is required.
 */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

#define BDEV_NAME        "Malloc0"
#define IO_SIZE_BLOCKS   4          /* blocks per I/O */
#define TOTAL_IOS        8          /* sequential I/Os to perform    */

/* ------------------------------------------------------------------ */
/* Application context                                                  */
/* ------------------------------------------------------------------ */

struct io_ctx {
    struct spdk_bdev_desc  *desc;
    struct spdk_io_channel *ch;

    struct spdk_bdev       *bdev;
    uint32_t                block_size;
    uint64_t                num_blocks;

    void                   *write_buf;
    void                   *read_buf;
    uint32_t                buf_size;    /* bytes */

    uint64_t                current_lba; /* next LBA to operate on */
    uint32_t                io_count;    /* completed I/O pairs    */
    int                     errors;
};

static struct io_ctx g_ctx = {};

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static void submit_write(struct io_ctx *ctx);
static void write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void cleanup(struct io_ctx *ctx);

/* ------------------------------------------------------------------ */
/* I/O helpers                                                          */
/* ------------------------------------------------------------------ */

/*
 * submit_write -- fill write_buf with a pattern and issue spdk_bdev_write_blocks.
 *
 * The pattern encodes the LBA so verification can confirm the correct data
 * was read back from the correct location.
 */
static void
submit_write(struct io_ctx *ctx)
{
    int rc;

    /* Fill buffer with a recognisable pattern: upper 32 bits = LBA,
     * lower 32 bits = sequential byte index within the block.           */
    uint32_t *p = ctx->write_buf;
    uint32_t  words = ctx->buf_size / sizeof(uint32_t);

    for (uint32_t i = 0; i < words; i++) {
        p[i] = (uint32_t)(ctx->current_lba & 0xFFFFFFFF) ^ i;
    }

    SPDK_NOTICELOG("Writing %u blocks at LBA %lu\n",
                   IO_SIZE_BLOCKS, ctx->current_lba);

    rc = spdk_bdev_write_blocks(ctx->desc, ctx->ch,
                                ctx->write_buf,
                                ctx->current_lba,
                                IO_SIZE_BLOCKS,
                                write_complete, ctx);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_bdev_write_blocks failed: %d\n", rc);
        ctx->errors++;
        cleanup(ctx);
    }
    /* On success, write_complete will be called asynchronously. */
}

/*
 * write_complete -- callback invoked when a write I/O finishes.
 *
 * IMPORTANT: Always call spdk_bdev_free_io() before returning from
 * any completion callback. Failure to do so leaks the bdev_io object.
 */
static void
write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_ctx *ctx = cb_arg;
    int rc;

    /* Free the bdev_io object - this MUST happen in every callback path. */
    spdk_bdev_free_io(bdev_io);

    if (!success) {
        SPDK_ERRLOG("Write at LBA %lu failed\n", ctx->current_lba);
        ctx->errors++;
        cleanup(ctx);
        return;
    }

    SPDK_NOTICELOG("Write complete at LBA %lu, issuing read-back\n",
                   ctx->current_lba);

    /* Immediately issue a read-back to verify. */
    rc = spdk_bdev_read_blocks(ctx->desc, ctx->ch,
                               ctx->read_buf,
                               ctx->current_lba,
                               IO_SIZE_BLOCKS,
                               read_complete, ctx);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_bdev_read_blocks failed: %d\n", rc);
        ctx->errors++;
        cleanup(ctx);
    }
}

/*
 * read_complete -- callback invoked when a read I/O finishes.
 *
 * Verifies that the data read back matches what was written, then
 * either advances to the next sequential I/O or finishes the test.
 */
static void
read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_ctx *ctx = cb_arg;

    spdk_bdev_free_io(bdev_io);

    if (!success) {
        SPDK_ERRLOG("Read at LBA %lu failed\n", ctx->current_lba);
        ctx->errors++;
        cleanup(ctx);
        return;
    }

    /* Verify data integrity. */
    uint32_t *wp = ctx->write_buf;
    uint32_t *rp = ctx->read_buf;
    uint32_t  words = ctx->buf_size / sizeof(uint32_t);
    bool      mismatch = false;

    for (uint32_t i = 0; i < words; i++) {
        if (wp[i] != rp[i]) {
            SPDK_ERRLOG("Data mismatch at word %u: wrote 0x%08x, read 0x%08x\n",
                        i, wp[i], rp[i]);
            mismatch = true;
            ctx->errors++;
            break;
        }
    }

    if (!mismatch) {
        SPDK_NOTICELOG("Read-back verified at LBA %lu [PASS]\n",
                       ctx->current_lba);
    }

    ctx->io_count++;
    ctx->current_lba += IO_SIZE_BLOCKS;

    if (ctx->io_count < TOTAL_IOS && ctx->current_lba + IO_SIZE_BLOCKS <= ctx->num_blocks) {
        /* Submit the next sequential I/O. */
        submit_write(ctx);
    } else {
        SPDK_NOTICELOG("Sequential I/O complete: %u I/Os, %d errors\n",
                       ctx->io_count, ctx->errors);
        cleanup(ctx);
    }
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                              */
/* ------------------------------------------------------------------ */

static void
cleanup(struct io_ctx *ctx)
{
    if (ctx->write_buf) {
        spdk_dma_free(ctx->write_buf);
        ctx->write_buf = NULL;
    }
    if (ctx->read_buf) {
        spdk_dma_free(ctx->read_buf);
        ctx->read_buf = NULL;
    }
    if (ctx->ch) {
        spdk_put_io_channel(ctx->ch);
        ctx->ch = NULL;
    }
    if (ctx->desc) {
        spdk_bdev_close(ctx->desc);
        ctx->desc = NULL;
    }

    spdk_app_stop(ctx->errors);
}

/* ------------------------------------------------------------------ */
/* Bdev event callback                                                  */
/* ------------------------------------------------------------------ */

/*
 * bdev_event_cb -- required by spdk_bdev_open_ext().
 *
 * Called on asynchronous bdev events (e.g., hot removal). For this
 * exercise we just log the event; a real application should handle
 * SPDK_BDEV_EVENT_REMOVE by closing the descriptor.
 */
static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
              void *event_ctx)
{
    SPDK_NOTICELOG("Bdev event: type=%d bdev=%s\n", type,
                   spdk_bdev_get_name(bdev));
}

/* ------------------------------------------------------------------ */
/* Application start                                                    */
/* ------------------------------------------------------------------ */

static void
app_start(void *arg)
{
    struct io_ctx *ctx = arg;
    int rc;

    /* Step 1: Open the bdev for read/write access. */
    rc = spdk_bdev_open_ext(BDEV_NAME, true /* write */, bdev_event_cb,
                            NULL, &ctx->desc);
    if (rc != 0) {
        SPDK_ERRLOG("Failed to open bdev '%s': %d\n", BDEV_NAME, rc);
        spdk_app_stop(rc);
        return;
    }

    /* Step 2: Retrieve the bdev pointer and its geometry. */
    ctx->bdev       = spdk_bdev_desc_get_bdev(ctx->desc);
    ctx->block_size = spdk_bdev_get_block_size(ctx->bdev);
    ctx->num_blocks = spdk_bdev_get_num_blocks(ctx->bdev);

    SPDK_NOTICELOG("Opened '%s': block_size=%u, num_blocks=%lu\n",
                   BDEV_NAME, ctx->block_size, ctx->num_blocks);

    if (ctx->num_blocks < (uint64_t)(IO_SIZE_BLOCKS * TOTAL_IOS)) {
        SPDK_ERRLOG("Bdev too small for this exercise\n");
        spdk_bdev_close(ctx->desc);
        spdk_app_stop(-ENOSPC);
        return;
    }

    /* Step 3: Get a per-thread I/O channel.
     * The channel must be obtained on the same thread that will submit I/O.
     * The reactor thread is that thread here (we are inside app_start).    */
    ctx->ch = spdk_bdev_get_io_channel(ctx->desc);
    if (ctx->ch == NULL) {
        SPDK_ERRLOG("Failed to get I/O channel\n");
        spdk_bdev_close(ctx->desc);
        spdk_app_stop(-ENOMEM);
        return;
    }

    /* Step 4: Allocate DMA-capable buffers.
     *
     * spdk_dma_malloc(size, alignment, phys_addr_out)
     *
     * - alignment must be at least the bdev block size.
     * - phys_addr_out can be NULL if you don't need the physical address.
     * - The returned pointer is virtually contiguous and DMA-safe.        */
    ctx->buf_size = ctx->block_size * IO_SIZE_BLOCKS;

    ctx->write_buf = spdk_dma_malloc(ctx->buf_size, ctx->block_size, NULL);
    if (ctx->write_buf == NULL) {
        SPDK_ERRLOG("Failed to allocate write buffer\n");
        cleanup(ctx);
        return;
    }

    ctx->read_buf = spdk_dma_malloc(ctx->buf_size, ctx->block_size, NULL);
    if (ctx->read_buf == NULL) {
        SPDK_ERRLOG("Failed to allocate read buffer\n");
        cleanup(ctx);
        return;
    }

    /* Step 5: Begin sequential I/O from LBA 0. */
    ctx->current_lba = 0;
    ctx->io_count    = 0;
    ctx->errors      = 0;

    submit_write(ctx);
    /* Execution continues asynchronously through write_complete / read_complete. */
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "ex03_simple_io";

    /*
     * The JSON config tells SPDK to create a Malloc bdev named "Malloc0"
     * with 256 blocks of 4096 bytes each (1 MiB total).
     * Pass it on the command line: --json malloc.json
     */

    rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    rc = spdk_app_start(&opts, app_start, &g_ctx);

    spdk_app_fini();
    return rc;
}
```

### Step 3: Write the JSON Configuration

Create `malloc.json` to define the Malloc bdev. No physical device is required.

```json
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_malloc_create",
          "params": {
            "name": "Malloc0",
            "num_blocks": 4096,
            "block_size": 512
          }
        }
      ]
    }
  ]
}
```

> Note: The block size here is 512. If your system bdev is 4096-byte blocks,
> change `block_size` to 4096 and `num_blocks` to 256 for equivalent capacity.

### Step 4: Write the Makefile

Create `Makefile`:

```makefile
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = ex03_simple_io
SRCS = ex03_simple_io.c

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
```

### Step 5: Build

```bash
cd $SPDK_DIR/app/ex03_simple_io
make
```

Expected output:

```
  CC ex03_simple_io.c
  LINK ex03_simple_io
```

If you see linker errors about missing symbols, verify `SPDK_ROOT_DIR` points to
the correct location and that SPDK was built with `./configure && make`.

### Step 6: Run

```bash
sudo ./ex03_simple_io --json malloc.json
```

### Expected Output

```
[2024-01-15 10:23:01.123456] NOTICE: Opened 'Malloc0': block_size=512, num_blocks=4096
[2024-01-15 10:23:01.123500] NOTICE: Writing 4 blocks at LBA 0
[2024-01-15 10:23:01.123510] NOTICE: Write complete at LBA 0, issuing read-back
[2024-01-15 10:23:01.123520] NOTICE: Read-back verified at LBA 0 [PASS]
[2024-01-15 10:23:01.123530] NOTICE: Writing 4 blocks at LBA 4
[2024-01-15 10:23:01.123540] NOTICE: Write complete at LBA 4, issuing read-back
[2024-01-15 10:23:01.123550] NOTICE: Read-back verified at LBA 4 [PASS]
...
[2024-01-15 10:23:01.123900] NOTICE: Sequential I/O complete: 8 I/Os, 0 errors
```

---

## Part 2: Bonus - Random I/O with Latency Measurement

Extend the application to perform random I/O and measure per-I/O latency.

### Step 1: Add Latency Tracking to the Context

Add these fields to `struct io_ctx`:

```c
    /* Bonus: random I/O and latency */
    bool                    random_mode;
    uint64_t                io_start_tsc;   /* TSC at I/O submission  */
    uint64_t                total_latency_ns;
    uint64_t                min_latency_ns;
    uint64_t                max_latency_ns;
    uint32_t                random_seed;
```

### Step 2: Add a Random LBA Generator

```c
static uint64_t
random_aligned_lba(struct io_ctx *ctx)
{
    /* Generate a random LBA aligned to IO_SIZE_BLOCKS. */
    uint64_t max_lba = (ctx->num_blocks / IO_SIZE_BLOCKS) - 1;
    uint64_t idx = (uint64_t)rand_r(&ctx->random_seed) % (max_lba + 1);
    return idx * IO_SIZE_BLOCKS;
}
```

### Step 3: Record TSC on Submission

In `submit_write()`, capture the TSC just before the I/O call:

```c
    ctx->io_start_tsc = spdk_get_ticks();

    rc = spdk_bdev_write_blocks(ctx->desc, ctx->ch,
                                ctx->write_buf,
                                ctx->current_lba,
                                IO_SIZE_BLOCKS,
                                write_complete, ctx);
```

### Step 4: Compute Latency in the Read Callback

At the top of `read_complete()`, after `spdk_bdev_free_io()`:

```c
    uint64_t tsc_diff = spdk_get_ticks() - ctx->io_start_tsc;
    uint64_t ticks_hz = spdk_get_ticks_hz();
    uint64_t latency_ns = tsc_diff * 1000000000ULL / ticks_hz;

    ctx->total_latency_ns += latency_ns;
    if (latency_ns < ctx->min_latency_ns || ctx->min_latency_ns == 0) {
        ctx->min_latency_ns = latency_ns;
    }
    if (latency_ns > ctx->max_latency_ns) {
        ctx->max_latency_ns = latency_ns;
    }

    SPDK_NOTICELOG("LBA %lu round-trip: %lu ns\n", ctx->current_lba, latency_ns);
```

### Step 5: Print Summary in cleanup()

Before `spdk_app_stop()`:

```c
    if (ctx->io_count > 0) {
        SPDK_NOTICELOG("=== I/O Summary ===\n");
        SPDK_NOTICELOG("  Total I/Os : %u\n", ctx->io_count);
        SPDK_NOTICELOG("  Errors     : %d\n", ctx->errors);
        SPDK_NOTICELOG("  Avg latency: %lu ns\n",
                       ctx->total_latency_ns / ctx->io_count);
        SPDK_NOTICELOG("  Min latency: %lu ns\n", ctx->min_latency_ns);
        SPDK_NOTICELOG("  Max latency: %lu ns\n", ctx->max_latency_ns);

        uint64_t total_bytes = (uint64_t)ctx->io_count
                               * IO_SIZE_BLOCKS * ctx->block_size * 2; /* read+write */
        uint64_t elapsed_ns  = ctx->total_latency_ns;
        if (elapsed_ns > 0) {
            uint64_t throughput_mbs = total_bytes * 1000ULL / elapsed_ns;
            SPDK_NOTICELOG("  Throughput : ~%lu MB/s (cumulative)\n", throughput_mbs);
        }
    }
```

### Step 6: Enable Random Mode

Set `ctx->random_mode = true` and `ctx->random_seed = 42` in `app_start()`.
In `read_complete()`, change LBA advancement to:

```c
    if (ctx->random_mode) {
        ctx->current_lba = random_aligned_lba(ctx);
    } else {
        ctx->current_lba += IO_SIZE_BLOCKS;
    }
```

### Expected Bonus Output

```
[...] NOTICE: LBA 256 round-trip: 1842 ns
[...] NOTICE: LBA 512 round-trip: 1791 ns
[...] NOTICE: LBA 128 round-trip: 1823 ns
...
[...] NOTICE: === I/O Summary ===
[...] NOTICE:   Total I/Os : 8
[...] NOTICE:   Errors     : 0
[...] NOTICE:   Avg latency: 1820 ns
[...] NOTICE:   Min latency: 1791 ns
[...] NOTICE:   Max latency: 1842 ns
[...] NOTICE:   Throughput : ~17 MB/s (cumulative)
```

> The Malloc bdev executes in software, so latency reflects reactor overhead
> rather than real storage latency. On an actual NVMe SSD you will see 50-200 us.

---

## Common Mistakes

### Mistake 1: Using a Stack or Heap Buffer Instead of DMA Memory

```c
/* WRONG - will cause undefined behavior or assert on real hardware */
char buf[4096];
spdk_bdev_write_blocks(desc, ch, buf, 0, 1, cb, ctx);

/* CORRECT */
void *buf = spdk_dma_malloc(4096, 512, NULL);
spdk_bdev_write_blocks(desc, ch, buf, 0, 1, cb, ctx);
```

**Why**: SPDK passes the buffer pointer directly to the DMA engine. A stack buffer
is not pinned in physical memory and not aligned to device requirements.

### Mistake 2: Forgetting spdk_bdev_free_io() in a Callback

```c
/* WRONG - leaks the bdev_io object on every I/O */
static void
my_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    /* Missing: spdk_bdev_free_io(bdev_io); */
    do_next_io(cb_arg);
}

/* CORRECT - free before any return path */
static void
my_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    spdk_bdev_free_io(bdev_io);   /* always, first */
    if (!success) { ... return; }
    do_next_io(cb_arg);
}
```

**Why**: Each `spdk_bdev_io` object is drawn from a fixed-size pool. Leaking even
one will eventually exhaust the pool and all subsequent I/Os will fail with `-ENOMEM`.

### Mistake 3: Blocking Inside a Callback

```c
/* WRONG - blocks the reactor, starves all other pollers */
static void
my_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    spdk_bdev_free_io(bdev_io);
    sleep(1);           /* NEVER do this */
    pthread_mutex_lock(&some_lock); /* NEVER do this either */
}

/* CORRECT - chain the next async operation from the callback */
static void
my_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    spdk_bdev_free_io(bdev_io);
    submit_next_io(cb_arg);  /* non-blocking, returns immediately */
}
```

**Why**: SPDK runs on a single-threaded reactor per core. Any blocking call inside
a callback stalls the entire reactor, including timer expiry and other pollers.

### Mistake 4: Using the I/O Channel on the Wrong Thread

```c
/* WRONG - channel obtained on thread A, used on thread B */
/* Thread A */
ctx->ch = spdk_bdev_get_io_channel(ctx->desc);

/* Thread B - RACE / undefined behavior */
spdk_bdev_write_blocks(ctx->desc, ctx->ch, ...);

/* CORRECT - obtain and use the channel on the same reactor thread */
```

**Why**: I/O channels are not thread-safe. Each reactor thread must get its own
channel via `spdk_bdev_get_io_channel()`.

### Mistake 5: Submitting I/O After spdk_bdev_close()

```c
/* WRONG */
spdk_bdev_close(ctx->desc);
spdk_bdev_write_blocks(ctx->desc, ctx->ch, ...);  /* use-after-free */

/* CORRECT - close only after all in-flight I/Os complete */
```

---

## Verification Checklist

After completing the exercise, verify:

- [ ] The application compiles without warnings
- [ ] All 8 sequential write+read pairs complete with `[PASS]`
- [ ] Final message shows `0 errors`
- [ ] (Bonus) Latency summary prints min/avg/max values
- [ ] Run under Valgrind or ASAN to confirm no memory leaks:
      `sudo valgrind --leak-check=full ./ex03_simple_io --json malloc.json`
- [ ] Change `TOTAL_IOS` to 64 and verify it still passes

---

## Key API Reference

| Function | Purpose |
|---|---|
| `spdk_bdev_open_ext(name, write, event_cb, ctx, &desc)` | Open a bdev, get descriptor |
| `spdk_bdev_desc_get_bdev(desc)` | Get `spdk_bdev*` from descriptor |
| `spdk_bdev_get_block_size(bdev)` | Block size in bytes |
| `spdk_bdev_get_num_blocks(bdev)` | Total logical blocks |
| `spdk_bdev_get_io_channel(desc)` | Allocate per-thread I/O channel |
| `spdk_bdev_write_blocks(desc, ch, buf, lba, n, cb, arg)` | Submit async write |
| `spdk_bdev_read_blocks(desc, ch, buf, lba, n, cb, arg)` | Submit async read |
| `spdk_bdev_free_io(bdev_io)` | Release I/O object (call in every callback) |
| `spdk_put_io_channel(ch)` | Release I/O channel |
| `spdk_bdev_close(desc)` | Close bdev descriptor |
| `spdk_dma_malloc(size, align, NULL)` | Allocate DMA-safe buffer |
| `spdk_dma_free(buf)` | Free DMA buffer |
| `spdk_get_ticks()` | Read TSC for latency measurement |
| `spdk_get_ticks_hz()` | TSC frequency for ns conversion |

---

## Summary

In this exercise you:

1. Opened a bdev with `spdk_bdev_open_ext()` and queried its geometry
2. Allocated per-thread I/O channels and DMA buffers
3. Implemented a chained async write -> read -> verify loop driven entirely by
   callbacks without any blocking calls
4. (Bonus) Measured per-I/O latency using TSC timestamps and reported throughput

The same pattern applies to any SPDK bdev: swap `"Malloc0"` for `"NVMe0n1"` in
the JSON and the application runs unchanged against a real NVMe drive.

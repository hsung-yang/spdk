# SPDK Common Patterns Cheatsheet

**Course**: SPDK Mastery Course
**Type**: Reference
**Source**: Verified against SPDK source tree

---

## Table of Contents

1. [Application Initialization Pattern](#1-application-initialization-pattern)
2. [Bdev Open / Read / Write / Close Pattern](#2-bdev-open--read--write--close-pattern)
3. [Async I/O with Completion Callback Pattern](#3-async-io-with-completion-callback-pattern)
4. [I/O Queue-Wait (ENOMEM) Pattern](#4-io-queue-wait-enomem-pattern)
5. [Thread Creation and Message Passing Pattern](#5-thread-creation-and-message-passing-pattern)
6. [Poller Registration Pattern (Timed and Busy)](#6-poller-registration-pattern-timed-and-busy)
7. [I/O Channel Management Pattern](#7-io-channel-management-pattern)
8. [RPC Method Registration Pattern](#8-rpc-method-registration-pattern)
9. [Error Handling Patterns](#9-error-handling-patterns)
10. [Resource Cleanup Pattern (Reverse-Order)](#10-resource-cleanup-pattern-reverse-order)
11. [NVMe Probe / Attach Pattern](#11-nvme-probe--attach-pattern)
12. [Configuration via JSON-RPC Pattern](#12-configuration-via-json-rpc-pattern)

---

## 1. Application Initialization Pattern

### When to Use
Every SPDK application that needs the full SPDK framework: EAL (DPDK), reactor system,
bdev layer, RPC server, and subsystem initialization. This is the mandatory starting point
for any SPDK app (targets, tools, examples).

### Complete Code Snippet

```c
#include "spdk/stdinc.h"
#include "spdk/event.h"
#include "spdk/log.h"

/*
 * Application context struct — passed through spdk_app_start into
 * the start function and all subsequent callbacks.
 */
struct app_context {
    /* Add application-specific fields here */
    int some_value;
};

/*
 * Custom usage help printed alongside SPDK built-in options.
 */
static void
app_usage(void)
{
    printf(" -x <value>   example custom argument\n");
}

/*
 * Custom argument parser. Called for each unrecognized option character.
 * Return 0 on success, -EINVAL on bad argument.
 */
static int
app_parse_arg(int ch, char *arg)
{
    switch (ch) {
    case 'x':
        /* parse arg */
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

/*
 * Entry point called on the SPDK reactor once all subsystems are up.
 * This is where application logic begins — open bdevs, start pollers, etc.
 * Must call spdk_app_stop() eventually to terminate.
 */
static void
app_start(void *arg1)
{
    struct app_context *ctx = arg1;

    SPDK_NOTICELOG("Application started\n");

    /* Application logic here ... */

    /* When done, stop the app (0 = success, negative = error) */
    spdk_app_stop(0);
}

int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    struct app_context ctx = {};
    int rc;

    /* 1. Initialize opts to defaults — always call before setting fields */
    spdk_app_opts_init(&opts, sizeof(opts));

    /* 2. Set required fields */
    opts.name = "my_app";
    /* opts.json_config_file = "config.json"; */  /* optional: load bdev config */
    /* opts.rpc_addr = "/var/tmp/spdk.sock";  */  /* optional: custom RPC socket */

    /* 3. Parse SPDK built-in + custom args */
    rc = spdk_app_parse_args(argc, argv, &opts, "x:", NULL,
                             app_parse_arg, app_usage);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        exit(rc);
    }

    /* 4. Start framework — blocks until spdk_app_stop() is called */
    rc = spdk_app_start(&opts, app_start, &ctx);
    if (rc) {
        SPDK_ERRLOG("Application failed: %d\n", rc);
    }

    /* 5. Finalize SPDK subsystems after reactor exits */
    spdk_app_fini();

    return rc;
}
```

### Notes
- `spdk_app_opts_init()` **must** be called before setting any `opts` field — it zero-initializes
  and sets internal size for ABI safety.
- `spdk_app_start()` blocks; all logic runs inside callbacks on SPDK threads.
- `spdk_app_fini()` is called **after** `spdk_app_start()` returns — it is not called from within
  the reactor.
- The `arg1` pointer passed to `spdk_app_start()` becomes the first argument of `app_start()`.
- Source reference: `app/spdk_tgt/spdk_tgt.c`, `examples/bdev/hello_world/hello_bdev.c`

---

## 2. Bdev Open / Read / Write / Close Pattern

### When to Use
Any time your application needs to perform I/O on a block device (NVMe, malloc, AIO, etc.)
through the bdev abstraction layer.

### Complete Code Snippet

```c
#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/env.h"

struct io_context {
    struct spdk_bdev        *bdev;          /* bdev pointer (valid while open) */
    struct spdk_bdev_desc   *bdev_desc;     /* open handle */
    struct spdk_io_channel  *io_channel;    /* per-thread I/O channel */
    char                    *buf;           /* DMA-safe buffer */
    uint32_t                 buf_size;
    struct spdk_bdev_io_wait_entry bdev_io_wait; /* for ENOMEM retry */
};

/*
 * Event callback for bdev asynchronous events (removal, resize, etc.).
 * Required by spdk_bdev_open_ext — must not be NULL.
 */
static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
              void *event_ctx)
{
    switch (type) {
    case SPDK_BDEV_EVENT_REMOVE:
        SPDK_WARNLOG("bdev removed\n");
        /* Handle hot-removal if needed */
        break;
    default:
        SPDK_NOTICELOG("Unhandled bdev event type: %d\n", type);
        break;
    }
}

static void
do_open_bdev(struct io_context *ctx, const char *bdev_name)
{
    uint32_t buf_align;
    int rc;

    /* 1. Open the bdev by name, requesting write access (true) */
    rc = spdk_bdev_open_ext(bdev_name, true, bdev_event_cb, NULL,
                            &ctx->bdev_desc);
    if (rc != 0) {
        SPDK_ERRLOG("Could not open bdev '%s': %s\n",
                    bdev_name, spdk_strerror(-rc));
        spdk_app_stop(-1);
        return;
    }

    /* 2. Retrieve the bdev pointer — valid while descriptor is open */
    ctx->bdev = spdk_bdev_desc_get_bdev(ctx->bdev_desc);

    /* 3. Get an I/O channel for this thread */
    ctx->io_channel = spdk_bdev_get_io_channel(ctx->bdev_desc);
    if (ctx->io_channel == NULL) {
        SPDK_ERRLOG("Failed to get I/O channel\n");
        spdk_bdev_close(ctx->bdev_desc);
        spdk_app_stop(-1);
        return;
    }

    /* 4. Allocate a DMA-safe buffer aligned to bdev requirements */
    ctx->buf_size = spdk_bdev_get_block_size(ctx->bdev) *
                    spdk_bdev_get_write_unit_size(ctx->bdev);
    buf_align = spdk_bdev_get_buf_align(ctx->bdev);
    ctx->buf = spdk_dma_zmalloc(ctx->buf_size, buf_align, NULL);
    if (ctx->buf == NULL) {
        SPDK_ERRLOG("Failed to allocate DMA buffer\n");
        spdk_put_io_channel(ctx->io_channel);
        spdk_bdev_close(ctx->bdev_desc);
        spdk_app_stop(-1);
        return;
    }
}

static void
do_close_bdev(struct io_context *ctx)
{
    /* Reverse order of open: channel -> descriptor -> free buffer */
    if (ctx->buf) {
        spdk_dma_free(ctx->buf);
        ctx->buf = NULL;
    }
    if (ctx->io_channel) {
        spdk_put_io_channel(ctx->io_channel);
        ctx->io_channel = NULL;
    }
    if (ctx->bdev_desc) {
        spdk_bdev_close(ctx->bdev_desc);
        ctx->bdev_desc = NULL;
    }
}
```

### Notes
- The `bdev_event_cb` parameter to `spdk_bdev_open_ext()` is **mandatory** (non-NULL).
- `spdk_bdev_desc_get_bdev()` returns a raw `spdk_bdev *` — use it for `spdk_bdev_get_*`
  property queries only; never cache it beyond the lifetime of the descriptor.
- `spdk_dma_zmalloc()` allocates hugepage-backed, zero-filled, DMA-capable memory.
  Always use it for I/O buffers; regular `malloc` will not work for NVMe/DMA.
- `spdk_bdev_get_buf_align()` returns the minimum buffer alignment required by the bdev.
- Source reference: `examples/bdev/hello_world/hello_bdev.c`

---

## 3. Async I/O with Completion Callback Pattern

### When to Use
All bdev I/O in SPDK is asynchronous. Use this pattern for every read or write operation
through the bdev layer. The caller submits I/O and returns immediately; the callback fires
when the I/O completes.

### Complete Code Snippet

```c
#include "spdk/bdev.h"
#include "spdk/log.h"

/* Forward declarations */
static void do_read(void *arg);
static void do_write(void *arg);

/*
 * Read completion callback.
 * Always call spdk_bdev_free_io() first, then do cleanup or next step.
 */
static void
read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_context *ctx = cb_arg;

    /* 1. Always free the I/O object first */
    spdk_bdev_free_io(bdev_io);

    if (!success) {
        SPDK_ERRLOG("Read failed\n");
        spdk_put_io_channel(ctx->io_channel);
        spdk_bdev_close(ctx->bdev_desc);
        spdk_app_stop(-1);
        return;
    }

    SPDK_NOTICELOG("Read complete: '%s'\n", ctx->buf);

    /* 2. Continue application logic */
    spdk_put_io_channel(ctx->io_channel);
    spdk_bdev_close(ctx->bdev_desc);
    spdk_app_stop(0);
}

/*
 * Write completion callback.
 */
static void
write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_context *ctx = cb_arg;

    spdk_bdev_free_io(bdev_io);

    if (!success) {
        SPDK_ERRLOG("Write failed\n");
        spdk_put_io_channel(ctx->io_channel);
        spdk_bdev_close(ctx->bdev_desc);
        spdk_app_stop(-1);
        return;
    }

    SPDK_NOTICELOG("Write complete\n");

    /* Chain to read after successful write */
    memset(ctx->buf, 0, ctx->buf_size);
    do_read(ctx);
}

static void
do_read(void *arg)
{
    struct io_context *ctx = arg;
    int rc;

    rc = spdk_bdev_read(ctx->bdev_desc, ctx->io_channel,
                        ctx->buf,
                        0,               /* offset in bytes */
                        ctx->buf_size,   /* length in bytes */
                        read_complete,   /* completion callback */
                        ctx);            /* callback argument */

    if (rc == -ENOMEM) {
        /* Queue the I/O for retry when resources free up */
        ctx->bdev_io_wait.bdev   = ctx->bdev;
        ctx->bdev_io_wait.cb_fn  = do_read;
        ctx->bdev_io_wait.cb_arg = ctx;
        spdk_bdev_queue_io_wait(ctx->bdev, ctx->io_channel,
                                &ctx->bdev_io_wait);
    } else if (rc) {
        SPDK_ERRLOG("Read submit failed: %s (%d)\n", spdk_strerror(-rc), rc);
        spdk_put_io_channel(ctx->io_channel);
        spdk_bdev_close(ctx->bdev_desc);
        spdk_app_stop(-1);
    }
}

static void
do_write(void *arg)
{
    struct io_context *ctx = arg;
    int rc;

    snprintf(ctx->buf, ctx->buf_size, "Hello SPDK!\n");

    rc = spdk_bdev_write(ctx->bdev_desc, ctx->io_channel,
                         ctx->buf,
                         0,               /* offset in bytes */
                         ctx->buf_size,   /* length in bytes */
                         write_complete,  /* completion callback */
                         ctx);            /* callback argument */

    if (rc == -ENOMEM) {
        ctx->bdev_io_wait.bdev   = ctx->bdev;
        ctx->bdev_io_wait.cb_fn  = do_write;
        ctx->bdev_io_wait.cb_arg = ctx;
        spdk_bdev_queue_io_wait(ctx->bdev, ctx->io_channel,
                                &ctx->bdev_io_wait);
    } else if (rc) {
        SPDK_ERRLOG("Write submit failed: %s (%d)\n", spdk_strerror(-rc), rc);
        spdk_put_io_channel(ctx->io_channel);
        spdk_bdev_close(ctx->bdev_desc);
        spdk_app_stop(-1);
    }
}
```

### Notes
- `spdk_bdev_free_io()` **must** be called in every completion callback, exactly once.
- The `success` boolean is the primary status indicator; check it before touching the buffer.
- Offsets and lengths are in **bytes**, not blocks.
- Do not block inside a completion callback — schedule further work with
  `spdk_thread_send_msg()` or chain the next I/O directly.
- Source reference: `examples/bdev/hello_world/hello_bdev.c`

---

## 4. I/O Queue-Wait (ENOMEM) Pattern

### When to Use
When `spdk_bdev_read()` / `spdk_bdev_write()` returns `-ENOMEM`, the bdev layer has
exhausted its internal I/O object pool. Use `spdk_bdev_queue_io_wait()` to register a
callback that retries the I/O automatically when resources become available.

### Complete Code Snippet

```c
#include "spdk/bdev.h"

/*
 * The io_wait_entry is embedded in your context struct:
 *
 *   struct my_ctx {
 *       ...
 *       struct spdk_bdev_io_wait_entry bdev_io_wait;
 *   };
 */

static void my_read(void *arg);  /* forward declaration */

static void
submit_read_with_retry(struct io_context *ctx)
{
    int rc;

    rc = spdk_bdev_read(ctx->bdev_desc, ctx->io_channel,
                        ctx->buf, 0, ctx->buf_size,
                        read_complete, ctx);

    if (rc == -ENOMEM) {
        /*
         * Pool exhausted — register a wait entry.
         * When an I/O object is freed elsewhere, cb_fn will be called
         * on this thread with cb_arg, so we simply retry submission.
         */
        ctx->bdev_io_wait.bdev   = ctx->bdev;
        ctx->bdev_io_wait.cb_fn  = my_read;    /* the retry function */
        ctx->bdev_io_wait.cb_arg = ctx;
        spdk_bdev_queue_io_wait(ctx->bdev, ctx->io_channel,
                                &ctx->bdev_io_wait);
    } else if (rc != 0) {
        SPDK_ERRLOG("I/O submit error: %s\n", spdk_strerror(-rc));
        spdk_app_stop(-1);
    }
    /* rc == 0: I/O submitted successfully, wait for completion callback */
}

static void
my_read(void *arg)
{
    submit_read_with_retry((struct io_context *)arg);
}
```

### Notes
- The `bdev_io_wait_entry` must remain valid until the retry callback fires.
  Embedding it in the application context struct (not stack-allocated) is the standard approach.
- `cb_fn` should point to the same function that originally submitted the I/O, not the
  completion callback.
- Only one wait entry per I/O channel at a time per context is valid.
- Source reference: `examples/bdev/hello_world/hello_bdev.c` (lines 89–96, 143–150)

---

## 5. Thread Creation and Message Passing Pattern

### When to Use
When you need to run a function on a specific SPDK thread — either a newly created thread
pinned to a CPU core, or an existing thread. `spdk_thread_send_msg()` is the safe way to
cross thread boundaries; never call bdev or other SPDK APIs from a foreign thread.

### Complete Code Snippet

```c
#include "spdk/thread.h"
#include "spdk/cpuset.h"
#include "spdk/log.h"

/*
 * --- Creating a dedicated SPDK thread ---
 */
static struct spdk_thread *g_my_thread;

static void
create_thread_example(void)
{
    struct spdk_cpuset cpumask;

    /* Pin thread to logical core 1 */
    spdk_cpuset_zero(&cpumask);
    spdk_cpuset_set_cpu(&cpumask, 1, true);

    /* Name is for diagnostics (/proc, SPDK trace) */
    g_my_thread = spdk_thread_create("my_worker", &cpumask);
    if (g_my_thread == NULL) {
        SPDK_ERRLOG("Failed to create thread\n");
        spdk_app_stop(-1);
        return;
    }
}

/*
 * --- Sending a message to another thread ---
 *
 * The message function runs on the target thread's reactor loop.
 * Do NOT touch thread-local resources of the calling thread inside fn.
 */
struct work_item {
    int value;
};

static void
do_work_on_target_thread(void *ctx)
{
    struct work_item *item = ctx;

    SPDK_NOTICELOG("Doing work on thread %s, value=%d\n",
                   spdk_thread_get_name(spdk_get_thread()),
                   item->value);

    free(item);
}

static void
dispatch_to_thread(struct spdk_thread *target)
{
    struct work_item *item = calloc(1, sizeof(*item));
    if (!item) {
        return;
    }
    item->value = 42;

    /* Send message — fn will execute on target's reactor */
    spdk_thread_send_msg(target, do_work_on_target_thread, item);
}

/*
 * --- Getting the current thread ---
 */
static void
current_thread_example(void)
{
    struct spdk_thread *self = spdk_get_thread();

    SPDK_NOTICELOG("Running on thread: %s\n",
                   spdk_thread_get_name(self));
}

/*
 * --- Destroying a thread ---
 *
 * Must be called from the thread itself or after all work is done.
 * The thread will exit after its message queue drains.
 */
static void
exit_thread_fn(void *arg)
{
    spdk_thread_exit(spdk_get_thread());
}

static void
stop_my_thread(void)
{
    spdk_thread_send_msg(g_my_thread, exit_thread_fn, NULL);
}
```

### Notes
- `spdk_thread_create()` creates a lightweight SPDK thread (not a POSIX thread).
  The SPDK reactor framework schedules it; you do not call `pthread_create`.
- Pass `NULL` for `cpumask` to let the scheduler pick any available core.
- `spdk_thread_send_msg()` is asynchronous and thread-safe — the only safe way to
  execute code on a specific SPDK thread from another.
- Never call bdev, NVMe, or other SPDK data-plane APIs from outside the correct thread;
  always use `spdk_thread_send_msg()` to marshal the call.
- Source reference: `lib/ftl/ftl_init.c`, `lib/bdev/bdev.c`

---

## 6. Poller Registration Pattern (Timed and Busy)

### When to Use
Pollers are the SPDK equivalent of periodic callbacks or event loops. Use a **timed poller**
for work that should run at a fixed interval (e.g., statistics sampling, timeout checks).
Use a **busy poller** (period = 0) for work that must run every reactor iteration
(e.g., NVMe queue completion processing, network I/O).

### Complete Code Snippet

```c
#include "spdk/thread.h"
#include "spdk/log.h"

struct poller_ctx {
    struct spdk_poller *timed_poller;
    struct spdk_poller *busy_poller;
    uint64_t            count;
};

/*
 * --- Timed poller (runs every N microseconds) ---
 *
 * Return SPDK_POLLER_BUSY if work was done, SPDK_POLLER_IDLE if not.
 * This hint helps the reactor decide whether to sleep.
 */
static int
timed_poller_fn(void *ctx)
{
    struct poller_ctx *pctx = ctx;

    pctx->count++;
    SPDK_DEBUGLOG(my_module, "Timed poll #%lu\n", pctx->count);

    /* Return BUSY if real work was done, IDLE otherwise */
    return SPDK_POLLER_IDLE;
}

/*
 * --- Busy poller (runs on every reactor iteration) ---
 *
 * Used for latency-sensitive completion processing.
 * Keep the function fast — it runs continuously.
 */
static int
busy_poller_fn(void *ctx)
{
    struct poller_ctx *pctx = ctx;
    int completions;

    /* Example: process NVMe completions (returns number processed) */
    completions = 0; /* replace with actual processing */

    return completions > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
register_pollers(struct poller_ctx *pctx)
{
    /*
     * Timed poller: period in microseconds.
     * 1000000 = 1 second interval.
     * Must be called from the thread the poller will run on.
     */
    pctx->timed_poller = spdk_poller_register(timed_poller_fn, pctx,
                                              1000000 /* us */);
    if (pctx->timed_poller == NULL) {
        SPDK_ERRLOG("Failed to register timed poller\n");
    }

    /*
     * Busy poller: period = 0 means run every reactor iteration.
     * Use sparingly — it burns CPU even when idle.
     */
    pctx->busy_poller = spdk_poller_register(busy_poller_fn, pctx,
                                             0 /* us: busy poll */);
    if (pctx->busy_poller == NULL) {
        SPDK_ERRLOG("Failed to register busy poller\n");
    }
}

static void
unregister_pollers(struct poller_ctx *pctx)
{
    /*
     * spdk_poller_unregister() takes a pointer-to-pointer and NULLs it.
     * Must be called from the same thread the poller was registered on.
     */
    spdk_poller_unregister(&pctx->timed_poller);
    spdk_poller_unregister(&pctx->busy_poller);
}

/*
 * --- Pausing and resuming a poller ---
 */
static void
pause_resume_example(struct poller_ctx *pctx)
{
    /* Temporarily disable without unregistering */
    spdk_poller_pause(pctx->timed_poller);

    /* Re-enable later */
    spdk_poller_resume(pctx->timed_poller);
}
```

### Notes
- Pollers run on the SPDK thread that called `spdk_poller_register()`.
- `spdk_poller_unregister()` accepts `**poller` and sets `*poller = NULL` after unregistration.
  Always check for NULL before unregistering (it is safe to call with a NULL pointer).
- For interrupt-capable pollers, implement and register a `spdk_poller_set_interrupt_mode_cb`
  via `spdk_poller_register_interrupt()`.
- Busy pollers (`period_microseconds = 0`) prevent the reactor from entering sleep even when
  there is no work. Only use them when you are certain continuous polling is required.
- Source reference: `include/spdk/thread.h`, `app/spdk_dd/spdk_dd.c`

---

## 7. I/O Channel Management Pattern

### When to Use
I/O channels provide per-thread state for a driver (queue pairs, connection handles, etc.).
Every thread that submits I/O to a bdev must obtain its own channel. Channels must be
obtained and released on the same thread.

### Complete Code Snippet

```c
#include "spdk/bdev.h"
#include "spdk/thread.h"
#include "spdk/log.h"

struct channel_ctx {
    struct spdk_bdev_desc  *desc;
    struct spdk_io_channel *ch;
};

/*
 * --- Obtaining a bdev I/O channel ---
 *
 * Must be called from the thread that will use it for I/O.
 * Each call increments a reference count.
 */
static int
acquire_io_channel(struct channel_ctx *ctx)
{
    ctx->ch = spdk_bdev_get_io_channel(ctx->desc);
    if (ctx->ch == NULL) {
        SPDK_ERRLOG("spdk_bdev_get_io_channel() failed\n");
        return -ENOMEM;
    }
    return 0;
}

/*
 * --- Releasing a bdev I/O channel ---
 *
 * Must be called from the same thread that called spdk_bdev_get_io_channel().
 * Decrements reference count; channel is destroyed when count reaches zero.
 */
static void
release_io_channel(struct channel_ctx *ctx)
{
    if (ctx->ch != NULL) {
        spdk_put_io_channel(ctx->ch);
        ctx->ch = NULL;
    }
}

/*
 * --- Iterating channels across all threads (driver use case) ---
 *
 * For drivers that need to perform an operation on every thread's channel
 * (e.g., draining queues during hot-remove), use spdk_for_each_channel().
 */
struct drain_ctx {
    struct spdk_io_channel_iter *iter;
};

static void
drain_channel(struct spdk_io_channel_iter *i)
{
    struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);

    /* Perform per-channel work here */
    (void)ch;

    /* Call _done to advance to next channel */
    spdk_for_each_channel_continue(i, 0);
}

static void
drain_complete(struct spdk_io_channel_iter *i, int status)
{
    SPDK_NOTICELOG("All channels drained, status=%d\n", status);
}

static void
drain_all_channels(void *io_device)
{
    spdk_for_each_channel(io_device,
                          drain_channel,
                          NULL,       /* ctx passed to drain_channel */
                          drain_complete);
}
```

### Notes
- A channel obtained on thread A **must not** be used on thread B — doing so will trigger
  assertions in debug builds and cause data corruption in release builds.
- `spdk_put_io_channel()` is the counterpart to both `spdk_bdev_get_io_channel()` and the
  generic `spdk_get_io_channel()`.
- Always call `spdk_put_io_channel()` before `spdk_bdev_close()`.
- Channels hold references to underlying driver resources; failure to release them causes
  resource leaks and blocks bdev unregistration.
- Source reference: `examples/bdev/hello_world/hello_bdev.c`

---

## 8. RPC Method Registration Pattern

### When to Use
When adding a new JSON-RPC 2.0 method to SPDK — for configuring, querying, or controlling
your module at runtime. SPDK's RPC server accepts connections on a Unix domain socket
and dispatches registered methods.

### Complete Code Snippet

```c
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"

/*
 * --- Request/Response without parameters (getter) ---
 */
static void
rpc_get_my_info(struct spdk_jsonrpc_request *request,
                const struct spdk_json_val *params)
{
    struct spdk_json_write_ctx *w;

    if (params != NULL) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "No parameters expected");
        return;
    }

    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_object_begin(w);
    spdk_json_write_named_string(w, "name", "my_module");
    spdk_json_write_named_uint32(w, "version", 1);
    spdk_json_write_object_end(w);
    spdk_jsonrpc_end_result(request, w);
}

SPDK_RPC_REGISTER("get_my_info", rpc_get_my_info, SPDK_RPC_RUNTIME)

/*
 * --- Request with parameters (setter) ---
 *
 * Step 1: define a struct for the decoded params.
 * Step 2: define a decoder table mapping JSON keys to struct fields.
 * Step 3: decode with spdk_json_decode_object().
 * Step 4: call business logic, send response or error.
 */
struct rpc_set_config_params {
    char    *name;
    uint32_t value;
};

static const struct spdk_json_object_decoder rpc_set_config_decoders[] = {
    /* { "json_key", struct_offset, decoder_fn, optional } */
    {"name",  offsetof(struct rpc_set_config_params, name),  spdk_json_decode_string},
    {"value", offsetof(struct rpc_set_config_params, value), spdk_json_decode_uint32},
};

static void
free_rpc_set_config(struct rpc_set_config_params *p)
{
    free(p->name);
}

static void
rpc_set_my_config(struct spdk_jsonrpc_request *request,
                  const struct spdk_json_val *params)
{
    struct rpc_set_config_params req = {};
    int rc;

    /* Decode JSON params into the request struct */
    if (spdk_json_decode_object(params,
                                rpc_set_config_decoders,
                                SPDK_COUNTOF(rpc_set_config_decoders),
                                &req)) {
        SPDK_ERRLOG("spdk_json_decode_object failed\n");
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "Invalid parameters");
        return;
    }

    /* Apply configuration */
    rc = my_module_set_config(req.name, req.value);
    free_rpc_set_config(&req);

    if (rc != 0) {
        spdk_jsonrpc_send_error_response_fmt(request,
                                             SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                             "Set config failed: %s",
                                             spdk_strerror(-rc));
        return;
    }

    /* Send boolean true on success */
    spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("set_my_config", rpc_set_my_config, SPDK_RPC_RUNTIME)
```

### Notes
- `SPDK_RPC_REGISTER` is a macro that uses a constructor attribute to register the
  method at startup — no explicit registration call is needed.
- The second argument to `SPDK_RPC_REGISTER` is the state flag:
  - `SPDK_RPC_STARTUP` — method available only during subsystem initialization.
  - `SPDK_RPC_RUNTIME` — method available after SPDK is fully initialized (most common).
- Always free decoded string fields (`spdk_json_decode_string` allocates with `strdup`).
- For optional fields, add `true` as the fourth element in the decoder entry.
- Source reference: `lib/iscsi/iscsi_rpc.c`

---

## 9. Error Handling Patterns

### When to Use
Consistently throughout all SPDK code. SPDK uses negative `errno` return codes
(e.g., `-EINVAL`, `-ENOMEM`, `-EIO`) for synchronous errors, and the `bool success`
parameter in async completion callbacks for I/O errors.

### Complete Code Snippet

```c
#include "spdk/log.h"
#include "spdk/string.h"

/*
 * --- Pattern A: Synchronous return code check ---
 *
 * Negative = error (errno convention).
 * 0 = success.
 * Use spdk_strerror(-rc) to get the string (thread-safe strerror).
 */
static int
sync_error_example(void)
{
    int rc;

    rc = some_spdk_call();
    if (rc != 0) {
        SPDK_ERRLOG("some_spdk_call() failed: %s (%d)\n",
                    spdk_strerror(-rc), rc);
        return rc;
    }
    return 0;
}

/*
 * --- Pattern B: Resource allocation with cleanup chain ---
 *
 * Each allocation failure releases previously acquired resources
 * in reverse order before returning.
 */
static int
init_with_cleanup(struct my_ctx *ctx)
{
    int rc;

    rc = acquire_resource_a(ctx);
    if (rc != 0) {
        SPDK_ERRLOG("acquire_resource_a failed: %d\n", rc);
        return rc;
    }

    rc = acquire_resource_b(ctx);
    if (rc != 0) {
        SPDK_ERRLOG("acquire_resource_b failed: %d\n", rc);
        release_resource_a(ctx);   /* clean up A before returning */
        return rc;
    }

    rc = acquire_resource_c(ctx);
    if (rc != 0) {
        SPDK_ERRLOG("acquire_resource_c failed: %d\n", rc);
        release_resource_b(ctx);
        release_resource_a(ctx);
        return rc;
    }

    return 0;
}

/*
 * --- Pattern C: Async completion error check ---
 *
 * The `success` bool is the primary status indicator in bdev callbacks.
 * Always free the I/O object first, then check status.
 */
static void
io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct my_ctx *ctx = cb_arg;

    /* Step 1: always free I/O object */
    spdk_bdev_free_io(bdev_io);

    /* Step 2: check status */
    if (!success) {
        SPDK_ERRLOG("I/O failed\n");
        /* cleanup and stop */
        do_close_bdev(ctx);
        spdk_app_stop(-1);
        return;
    }

    /* Step 3: success path */
    SPDK_NOTICELOG("I/O completed successfully\n");
    do_close_bdev(ctx);
    spdk_app_stop(0);
}

/*
 * --- Pattern D: Assert for programmer errors ---
 *
 * Use assert() / SPDK_ASSERT() for conditions that must always be true
 * (programming errors, not runtime errors).
 */
static void
thread_local_operation(struct my_ctx *ctx)
{
    /* Verify we are on the correct thread — catches threading bugs early */
    assert(spdk_get_thread() == ctx->owner_thread);

    /* Safe to proceed */
}
```

### Notes
- Use `SPDK_ERRLOG()` for errors, `SPDK_WARNLOG()` for warnings, `SPDK_NOTICELOG()` for
  significant events, `SPDK_DEBUGLOG(module, ...)` for verbose debug output.
- `spdk_strerror(-rc)` is thread-safe; prefer it over `strerror(errno)`.
- Never silently swallow errors — at minimum log them with `SPDK_ERRLOG`.
- In app-level code, terminal errors should call `spdk_app_stop(-1)` to ensure clean shutdown.
- Source reference: `examples/bdev/hello_world/hello_bdev.c`, `lib/iscsi/iscsi_rpc.c`

---

## 10. Resource Cleanup Pattern (Reverse-Order)

### When to Use
At shutdown or on error paths. Resources must always be freed in the reverse order of
acquisition. For SPDK, the general order is: pollers -> I/O channels -> bdev descriptors ->
threads -> DMA memory -> subsystems.

### Complete Code Snippet

```c
#include "spdk/bdev.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/event.h"

struct full_ctx {
    /* Initialized in this order */
    struct spdk_thread     *worker_thread;      /* 1st created */
    struct spdk_bdev_desc  *bdev_desc;          /* 2nd */
    struct spdk_io_channel *io_channel;         /* 3rd */
    struct spdk_poller     *poller;             /* 4th */
    char                   *dma_buf;            /* 5th */
};

/*
 * Cleanup runs on the worker thread (reverse order of init).
 * Called via spdk_thread_send_msg if invoked from a different thread.
 */
static void
do_cleanup(void *arg)
{
    struct full_ctx *ctx = arg;

    /* 5th allocated -> free first */
    if (ctx->dma_buf) {
        spdk_dma_free(ctx->dma_buf);
        ctx->dma_buf = NULL;
    }

    /* 4th registered -> unregister */
    if (ctx->poller) {
        spdk_poller_unregister(&ctx->poller);   /* sets ctx->poller = NULL */
    }

    /* 3rd acquired -> put back */
    if (ctx->io_channel) {
        spdk_put_io_channel(ctx->io_channel);
        ctx->io_channel = NULL;
    }

    /* 2nd opened -> close */
    if (ctx->bdev_desc) {
        spdk_bdev_close(ctx->bdev_desc);
        ctx->bdev_desc = NULL;
    }

    /* 1st created thread -> signal it to exit */
    spdk_thread_exit(spdk_get_thread());

    /* Signal application shutdown */
    spdk_app_stop(0);
}

/*
 * Top-level shutdown trigger — may be called from any thread.
 */
static void
trigger_shutdown(struct full_ctx *ctx)
{
    if (ctx->worker_thread != NULL) {
        /* Marshal cleanup onto the worker thread */
        spdk_thread_send_msg(ctx->worker_thread, do_cleanup, ctx);
    } else {
        /* Worker never started — call spdk_app_stop directly */
        spdk_app_stop(0);
    }
}

/*
 * main()-level cleanup (after spdk_app_start returns):
 */
static void
post_reactor_cleanup(struct full_ctx *ctx)
{
    /* Free any remaining heap allocations not managed by SPDK */
    free(ctx->some_heap_alloc);

    /* spdk_app_fini() tears down SPDK subsystems */
    spdk_app_fini();
}
```

### Notes
- The rule is strict: **last in, first out**. Violating this order causes use-after-free,
  assertion failures, or hangs during shutdown.
- `spdk_put_io_channel()` must precede `spdk_bdev_close()`.
- `spdk_poller_unregister()` must run on the same thread the poller was registered on.
- `spdk_thread_exit()` requests the thread to exit; the reactor will destroy it after
  draining its message queue.
- `spdk_app_fini()` is always called from `main()` after `spdk_app_start()` returns,
  not from within a callback.
- Source reference: `examples/bdev/hello_world/hello_bdev.c` (read_complete and main)

---

## 11. NVMe Probe / Attach Pattern

### When to Use
When using the NVMe driver directly (bypassing the bdev layer) — for lowest-latency access,
custom queue pair configuration, or vendor-specific NVMe commands. Also used in standalone
tools that do not use `spdk_app_start`.

### Complete Code Snippet

```c
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/queue.h"

/* Linked list of discovered controllers */
struct ctrlr_entry {
    struct spdk_nvme_ctrlr *ctrlr;
    char                    name[1024];
    TAILQ_ENTRY(ctrlr_entry) link;
};

/* Linked list of active namespaces */
struct ns_entry {
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_nvme_ns    *ns;
    struct spdk_nvme_qpair *qpair;
    TAILQ_ENTRY(ns_entry)   link;
};

TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
TAILQ_HEAD(, ns_entry)    g_namespaces  = TAILQ_HEAD_INITIALIZER(g_namespaces);

/*
 * probe_cb: called for each discovered controller.
 * Return true to attach, false to skip.
 */
static bool
probe_cb(void *cb_ctx,
         const struct spdk_nvme_transport_id *trid,
         struct spdk_nvme_ctrlr_opts *opts)
{
    printf("Found controller at %s — attaching\n", trid->traddr);
    /* Optionally modify opts (queue depth, arb mechanism, etc.) */
    return true;  /* true = attach */
}

/*
 * attach_cb: called for each controller that was accepted by probe_cb.
 * Register namespaces here.
 */
static void
attach_cb(void *cb_ctx,
          const struct spdk_nvme_transport_id *trid,
          struct spdk_nvme_ctrlr *ctrlr,
          const struct spdk_nvme_ctrlr_opts *opts)
{
    const struct spdk_nvme_ctrlr_data *cdata;
    struct ctrlr_entry *entry;
    int nsid;

    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        perror("calloc ctrlr_entry");
        return;
    }

    cdata = spdk_nvme_ctrlr_get_data(ctrlr);
    snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)",
             cdata->mn, cdata->sn);
    entry->ctrlr = ctrlr;
    TAILQ_INSERT_TAIL(&g_controllers, entry, link);

    /* Enumerate active namespaces */
    for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
         nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
        struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
        struct ns_entry *nse;

        if (ns == NULL || !spdk_nvme_ns_is_active(ns)) {
            continue;
        }

        nse = calloc(1, sizeof(*nse));
        if (!nse) {
            continue;
        }
        nse->ctrlr = ctrlr;
        nse->ns    = ns;
        TAILQ_INSERT_TAIL(&g_namespaces, nse, link);
    }
}

static int
init_nvme(void)
{
    struct spdk_nvme_transport_id trid = {};
    int rc;

    /* Use PCIe transport for local NVMe devices */
    spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
    snprintf(trid.subnqn, sizeof(trid.subnqn), "%s",
             SPDK_NVMF_DISCOVERY_NQN);

    /*
     * Probe and attach all NVMe controllers found on the given transport.
     * Passing NULL for trid scans all PCIe NVMe devices.
     */
    rc = spdk_nvme_probe(&trid, NULL, probe_cb, attach_cb, NULL);
    if (rc != 0) {
        fprintf(stderr, "spdk_nvme_probe() failed: %d\n", rc);
        return rc;
    }

    if (TAILQ_EMPTY(&g_controllers)) {
        fprintf(stderr, "No NVMe controllers found\n");
        return -ENODEV;
    }

    return 0;
}

static void
cleanup_nvme(void)
{
    struct ns_entry    *ns_entry, *tmp_ns;
    struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr;
    struct spdk_nvme_detach_ctx *detach_ctx = NULL;

    /* Free namespace entries */
    TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns) {
        if (ns_entry->qpair) {
            spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
        }
        TAILQ_REMOVE(&g_namespaces, ns_entry, link);
        free(ns_entry);
    }

    /* Detach controllers (async to allow batching) */
    TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr) {
        TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
        spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
        free(ctrlr_entry);
    }

    /* Poll until all detachments complete */
    if (detach_ctx) {
        spdk_nvme_detach_poll(detach_ctx);
    }
}
```

### Notes
- `spdk_nvme_probe()` is synchronous — it blocks until enumeration completes.
- For fabric targets (NVMe-oF TCP, RDMA), set `trid.trtype`, `trid.traddr`, and
  `trid.trsvcid` before calling `spdk_nvme_probe()`.
- Queue pairs (`spdk_nvme_qpair`) must be allocated per thread with
  `spdk_nvme_ctrlr_alloc_io_qpair()` and freed with `spdk_nvme_ctrlr_free_io_qpair()`.
- Use `spdk_nvme_detach_async()` + `spdk_nvme_detach_poll()` in loops for efficient
  multi-controller detach — it avoids blocking on each controller individually.
- Source reference: `examples/nvme/hello_world/hello_world.c`

---

## 12. Configuration via JSON-RPC Pattern

### When to Use
For runtime configuration of a running SPDK target — creating bdevs, subsystems, listeners,
and other resources without restarting the application. The SPDK CLI tool `scripts/rpc.py`
uses this same mechanism.

### Complete Code Snippet

```c
/*
 * --- Server side: making your module configurable via RPC ---
 * (see Pattern 8 for full RPC method registration)
 *
 * Standard lifecycle for a "create" RPC:
 * 1. Decode params with spdk_json_decode_object()
 * 2. Call the module's internal create function
 * 3. Send bool true on success, error on failure
 * 4. Free all allocated params
 */

/* Example: rpc_bdev_malloc_create style */
struct rpc_create_resource {
    char     *name;
    uint64_t  num_blocks;
    uint32_t  block_size;
};

static void
free_rpc_create_resource(struct rpc_create_resource *r)
{
    free(r->name);
}

static const struct spdk_json_object_decoder rpc_create_resource_decoders[] = {
    {"name",        offsetof(struct rpc_create_resource, name),
     spdk_json_decode_string},
    {"num_blocks",  offsetof(struct rpc_create_resource, num_blocks),
     spdk_json_decode_uint64},
    {"block_size",  offsetof(struct rpc_create_resource, block_size),
     spdk_json_decode_uint32},
};

static void
rpc_create_resource(struct spdk_jsonrpc_request *request,
                    const struct spdk_json_val *params)
{
    struct rpc_create_resource req = {};
    struct spdk_json_write_ctx *w;
    int rc;

    if (params == NULL) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "Parameters required");
        return;
    }

    if (spdk_json_decode_object(params,
                                rpc_create_resource_decoders,
                                SPDK_COUNTOF(rpc_create_resource_decoders),
                                &req)) {
        SPDK_ERRLOG("Decoding RPC params failed\n");
        goto invalid;
    }

    rc = my_module_create(req.name, req.num_blocks, req.block_size);
    if (rc != 0) {
        spdk_jsonrpc_send_error_response_fmt(request,
                                             SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                             "Create failed: %s",
                                             spdk_strerror(-rc));
        free_rpc_create_resource(&req);
        return;
    }

    free_rpc_create_resource(&req);

    /* Return the created resource name as the result */
    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_string(w, req.name);
    spdk_jsonrpc_end_result(request, w);
    return;

invalid:
    spdk_jsonrpc_send_error_response(request,
                                     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                     "Invalid parameters");
    free_rpc_create_resource(&req);
}

SPDK_RPC_REGISTER("create_resource", rpc_create_resource, SPDK_RPC_RUNTIME)
```

```bash
# --- Client side: calling RPC methods ---

# Using scripts/rpc.py (Python client bundled with SPDK):
scripts/rpc.py bdev_malloc_create -b Malloc0 -s 64 -z 512
scripts/rpc.py bdev_get_bdevs
scripts/rpc.py bdev_malloc_delete Malloc0

# Custom socket path:
scripts/rpc.py -s /var/tmp/my_app.sock bdev_get_bdevs

# JSON output piped to jq:
scripts/rpc.py bdev_get_bdevs | jq '.[].name'

# Using curl directly (JSON-RPC 2.0 over Unix socket):
echo '{"jsonrpc":"2.0","method":"bdev_get_bdevs","id":1}' | \
    nc -U /var/tmp/spdk.sock
```

```json
// --- JSON config file loaded at startup via opts.json_config_file ---
// Executed in order during subsystem initialization (SPDK_RPC_STARTUP phase)
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_malloc_create",
          "params": {
            "name": "Malloc0",
            "num_blocks": 131072,
            "block_size": 512
          }
        }
      ]
    }
  ]
}
```

### Notes
- `opts.json_config_file` in `spdk_app_opts` causes the config to be replayed during
  startup before `app_start()` is called.
- Startup-phase RPCs (e.g., bdev creation) use `SPDK_RPC_STARTUP`; runtime RPCs that
  require subsystems to be fully initialized use `SPDK_RPC_RUNTIME`.
- `spdk_jsonrpc_send_bool_response(request, true)` is the idiomatic success response
  for mutation methods that do not return data.
- `spdk_jsonrpc_send_error_response_fmt()` is available for formatted error messages
  when you need to include dynamic content (error codes, names).
- Source reference: `lib/iscsi/iscsi_rpc.c`, SPDK documentation

---

## Quick Reference Table

| Pattern | Key Functions | Header |
|---|---|---|
| App init | `spdk_app_opts_init`, `spdk_app_start`, `spdk_app_stop`, `spdk_app_fini` | `spdk/event.h` |
| Bdev open/close | `spdk_bdev_open_ext`, `spdk_bdev_close`, `spdk_bdev_desc_get_bdev` | `spdk/bdev.h` |
| Bdev I/O | `spdk_bdev_read`, `spdk_bdev_write`, `spdk_bdev_free_io` | `spdk/bdev.h` |
| I/O retry | `spdk_bdev_queue_io_wait` | `spdk/bdev.h` |
| Thread | `spdk_thread_create`, `spdk_get_thread`, `spdk_thread_send_msg`, `spdk_thread_exit` | `spdk/thread.h` |
| Poller | `spdk_poller_register`, `spdk_poller_unregister`, `spdk_poller_pause`, `spdk_poller_resume` | `spdk/thread.h` |
| I/O channel | `spdk_bdev_get_io_channel`, `spdk_put_io_channel`, `spdk_for_each_channel` | `spdk/bdev.h` |
| RPC | `SPDK_RPC_REGISTER`, `spdk_json_decode_object`, `spdk_jsonrpc_begin_result`, `spdk_jsonrpc_end_result` | `spdk/rpc.h` |
| DMA memory | `spdk_dma_zmalloc`, `spdk_dma_free`, `spdk_bdev_get_buf_align` | `spdk/env.h` |
| NVMe probe | `spdk_nvme_probe`, `spdk_nvme_detach_async`, `spdk_nvme_detach_poll` | `spdk/nvme.h` |
| Logging | `SPDK_ERRLOG`, `SPDK_WARNLOG`, `SPDK_NOTICELOG`, `SPDK_DEBUGLOG` | `spdk/log.h` |
| Error string | `spdk_strerror` | `spdk/string.h` |

---

## Common Pitfalls

| Pitfall | Correct Approach |
|---|---|
| Using `malloc` for I/O buffers | Use `spdk_dma_zmalloc` with proper alignment from `spdk_bdev_get_buf_align` |
| Forgetting `spdk_bdev_free_io()` in completion callback | Always call it as the first line of every bdev completion callback |
| Using a channel from a different thread | Get and use channels on the same thread; send messages to cross thread boundaries |
| Calling `spdk_app_fini()` inside a callback | Call it only from `main()` after `spdk_app_start()` returns |
| Releasing resources in init order | Always reverse: poller -> channel -> descriptor -> thread -> memory |
| Ignoring `-ENOMEM` from bdev I/O | Always handle with `spdk_bdev_queue_io_wait` retry pattern |
| NULL `bdev_event_cb` in `spdk_bdev_open_ext` | Always provide a valid event callback function |
| Calling `spdk_nvme_probe` without `spdk_env_init` | Initialize DPDK environment first when not using `spdk_app_start` |

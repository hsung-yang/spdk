# Module 18: Thread Management

**Stage**: 2 (Implementation)
**Difficulty**: Intermediate
**Estimated Time**: 4 hours
**Prerequisites**: Modules 01-17

---

## Learning Objectives

- Understand the SPDK threading model and its design goals
- Create and destroy SPDK threads with proper lifecycle management
- Understand the reactor-thread relationship and how scheduling works
- Use inter-thread messaging with `spdk_thread_send_msg`
- Register and manage pollers (continuous and timed)
- Implement I/O channels and understand their thread binding
- Assign threads to specific CPU cores using cpumask
- Apply thread-local storage patterns correctly
- Monitor thread health with statistics APIs

---

## Core Concepts

### Concept 1: The SPDK Threading Model

SPDK threads are **not** OS threads. They are lightweight, stackless cooperative scheduling units that run on top of DPDK lcores (logical CPU cores). Understanding this distinction is fundamental to writing correct SPDK code.

```
┌─────────────────────────────────────────────────────────┐
│                    OS / Kernel                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│  │ pthread  │  │ pthread  │  │ pthread  │  (one per    │
│  │ (lcore 0)│  │ (lcore 1)│  │ (lcore 2)│   core)     │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘             │
│       │              │              │                    │
│  ┌────▼─────┐  ┌────▼─────┐  ┌────▼─────┐             │
│  │ Reactor 0│  │ Reactor 1│  │ Reactor 2│  SPDK Event  │
│  │ (polling)│  │ (polling)│  │ (polling)│  Framework   │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘             │
│       │              │              │                    │
│  ┌────▼─────┐  ┌────▼─────┐  ┌────▼─────┐             │
│  │ spdk_    │  │ spdk_    │  │ spdk_    │  SPDK        │
│  │ thread A │  │ thread B │  │ thread C │  Threads     │
│  └──────────┘  └──────────┘  └──────────┘             │
└─────────────────────────────────────────────────────────┘
```

**Key Properties**:
- Each SPDK thread runs exclusively on one reactor at a time
- No preemption: functions run to completion before yielding
- No mutexes needed for intra-thread state
- All cross-thread communication goes through the message passing API
- A reactor can host multiple SPDK threads (round-robin scheduling)

**Why no locks?** Because only one SPDK thread runs per reactor at a time, and an SPDK thread only runs on one reactor at a time, per-thread data needs no locking. This is the foundation of SPDK's lock-free performance.

---

### Concept 2: Thread Lifecycle State Machine

```
spdk_thread_create()
        │
        ▼
 ┌─────────────┐
 │   RUNNING   │◄──── spdk_thread_poll() drives execution
 └──────┬──────┘
        │ spdk_thread_exit()
        ▼
 ┌─────────────┐
 │   EXITING   │◄──── draining pollers, channels, messages
 └──────┬──────┘
        │ spdk_thread_is_exited() == true
        ▼
 ┌─────────────┐
 │   EXITED    │◄──── safe to call spdk_thread_destroy()
 └─────────────┘
```

From `lib/thread/thread.c`, the internal state enum:
```c
enum spdk_thread_state {
    /* The thread is processing pollers and messages. */
    SPDK_THREAD_STATE_RUNNING,

    /* The thread is in the process of termination.
     * It reaps unregistering pollers and releases I/O channels. */
    SPDK_THREAD_STATE_EXITING,

    /* The thread is exited. Ready to call spdk_thread_destroy(). */
    SPDK_THREAD_STATE_EXITED,
};
```

---

### Concept 3: Internal Thread Structure

The `struct spdk_thread` (from `lib/thread/thread.c`) reveals what an SPDK thread manages:

```c
struct spdk_thread {
    uint64_t                tsc_last;
    struct spdk_thread_stats stats;          /* busy_tsc / idle_tsc */

    /* Active pollers: run round-robin, no timer */
    TAILQ_HEAD(active_pollers_head, spdk_poller) active_pollers;

    /* Timed pollers: stored in a red-black tree by next run tick */
    RB_HEAD(timed_pollers_tree, spdk_poller)     timed_pollers;

    /* Paused pollers: waiting to be resumed or unregistered */
    TAILQ_HEAD(paused_pollers_head, spdk_poller) paused_pollers;

    struct spdk_ring         *messages;      /* lock-free ring for incoming messages */
    spdk_msg_fn              critical_msg;   /* single slot for critical messages */

    /* I/O channels: red-black tree keyed by io_device pointer */
    RB_HEAD(io_channel_tree, spdk_io_channel) io_channels;

    char                     name[SPDK_MAX_THREAD_NAME_LEN + 1];
    struct spdk_cpuset        cpumask;
    enum spdk_thread_state    state;
    bool                     is_bound;       /* pinned to current core */
    bool                     in_interrupt;   /* interrupt vs. poll mode */

    /* User context allocated at the end (flexible array) */
    uint8_t                  ctx[0];
};
```

The user context (`ctx[0]`) is a flexible array member — when `spdk_thread_lib_init()` is called with a `ctx_sz > 0`, each thread allocation includes that many extra bytes at the end. This is the scheduler's per-thread state.

---

## Thread Library Initialization

Before creating any threads, the threading library must be initialized. This is normally done by the SPDK event framework, but standalone applications must do it explicitly.

```c
#include "spdk/thread.h"

/*
 * Simple initialization — used when you don't need a custom scheduler.
 * new_thread_fn: called whenever a new thread is created; must call
 *                spdk_thread_poll() frequently on the provided thread.
 * ctx_sz:        extra bytes to allocate per thread for scheduler use.
 */
int
my_app_init_threads(void)
{
    int rc;

    rc = spdk_thread_lib_init(my_new_thread_cb, 0);
    if (rc != 0) {
        SPDK_ERRLOG("Failed to initialize thread library: %d\n", rc);
        return rc;
    }
    return 0;
}

/*
 * Extended initialization — supports thread rescheduling operations.
 * Required when using cpumask updates at runtime.
 */
int
my_app_init_threads_ext(void)
{
    return spdk_thread_lib_init_ext(
        my_thread_op_fn,           /* handles NEW and RESCHED operations */
        my_thread_op_supported_fn, /* returns true if op is supported */
        sizeof(struct my_scheduler_ctx),
        SPDK_DEFAULT_MSG_MEMPOOL_SIZE
    );
}
```

---

## Thread Creation

### Basic Thread Creation

```c
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/cpuset.h"

/*
 * Create a worker thread pinned to a specific CPU core.
 *
 * Real example pattern from lib/event/reactor.c and lib/ftl/ftl_init.c:
 *   cpumask specifies *preferred* cores — it is a hint to the scheduler,
 *   not a guarantee of exclusive use.
 */
static struct spdk_thread *
create_worker_thread(const char *name, uint32_t cpu_core)
{
    struct spdk_thread *thread;
    struct spdk_cpuset cpumask;

    /* Build a cpumask with a single core set */
    spdk_cpuset_zero(&cpumask);
    spdk_cpuset_set_cpu(&cpumask, cpu_core, true);

    thread = spdk_thread_create(name, &cpumask);
    if (thread == NULL) {
        SPDK_ERRLOG("Failed to create thread '%s' on core %u\n", name, cpu_core);
        return NULL;
    }

    SPDK_NOTICELOG("Created thread '%s' (id=%" PRIu64 ") on core %u\n",
                   spdk_thread_get_name(thread),
                   spdk_thread_get_id(thread),
                   cpu_core);
    return thread;
}

/*
 * Create a thread with no CPU affinity preference (scheduler decides).
 * Used in test/app/fuzz and similar tools.
 */
static struct spdk_thread *
create_any_thread(const char *name)
{
    return spdk_thread_create(name, NULL);
}
```

### Creating Threads Across All Active Cores

A common SPDK pattern: one worker thread per active lcore.

```c
/*
 * Pattern from lib/iscsi/iscsi_subsystem.c and module/event/subsystems/nvmf/nvmf_tgt.c
 */
static int
create_per_core_threads(void)
{
    uint32_t i;
    char thread_name[64];
    struct spdk_cpuset cpumask;

    SPDK_ENV_FOREACH_CORE(i) {
        snprintf(thread_name, sizeof(thread_name), "worker_%u", i);

        spdk_cpuset_zero(&cpumask);
        spdk_cpuset_set_cpu(&cpumask, i, true);

        g_workers[i].thread = spdk_thread_create(thread_name, &cpumask);
        if (g_workers[i].thread == NULL) {
            SPDK_ERRLOG("Failed to create worker thread for core %u\n", i);
            return -ENOMEM;
        }
        g_workers[i].core = i;
    }
    return 0;
}
```

The macro `SPDK_ENV_FOREACH_CORE(i)` expands to:
```c
for (i = spdk_env_get_first_core();
     i != UINT32_MAX;
     i = spdk_env_get_next_core(i))
```

---

## Thread Scheduling: The Reactor-Thread Relationship

### How Reactors Drive Threads

Reactors are the engine that drives SPDK threads. Each reactor runs an infinite polling loop:

```c
/*
 * Simplified reactor loop (from lib/event/reactor.c internals).
 * The real loop handles interrupt mode, scheduling, and multiple threads,
 * but the core concept is:
 */
static void
reactor_run(void *arg)
{
    struct spdk_reactor *reactor = arg;
    struct spdk_thread  *thread;

    while (!g_reactor_exit) {
        /* Poll each thread assigned to this reactor */
        TAILQ_FOREACH(thread, &reactor->threads, tailq) {
            /*
             * spdk_thread_poll() runs:
             *   1. All active (continuous) pollers once
             *   2. All expired timed pollers
             *   3. Up to max_msgs messages from the message ring
             *
             * Returns 1 if any work was done, 0 if idle.
             */
            spdk_thread_poll(thread, 0, 0);
        }
    }
}
```

### Thread Binding

Once pinned, a thread cannot be rescheduled to another core until unbound:

```c
/* Pin a thread to whatever core it currently runs on */
spdk_thread_bind(thread, true);

/* Check if a thread is pinned */
if (spdk_thread_is_bound(thread)) {
    SPDK_NOTICELOG("Thread '%s' is bound to its current core\n",
                   spdk_thread_get_name(thread));
}

/* Unpin — allows the scheduler to migrate the thread */
spdk_thread_bind(thread, false);
```

### Dynamic CPU Affinity Updates

Threads can request migration to different cores at runtime:

```c
/*
 * Pattern from lib/event/app_rpc.c — RPC handler to change thread affinity.
 * spdk_thread_set_cpumask() requires SPDK_THREAD_OP_RESCHED support.
 */
static void
_rpc_thread_set_cpumask(void *arg)
{
    struct rpc_set_cpumask_ctx *ctx = arg;
    struct spdk_cpuset new_mask;
    int rc;

    /* Parse the cpumask string into a spdk_cpuset */
    rc = spdk_cpuset_parse(&new_mask, ctx->cpumask_str);
    if (rc != 0) {
        ctx->status = rc;
        spdk_thread_send_msg(ctx->orig_thread, rpc_thread_set_cpumask_done, ctx);
        return;
    }

    /* Request rescheduling — the scheduler will migrate on the next poll */
    rc = spdk_thread_set_cpumask(&new_mask);
    ctx->status = rc;

    /* Notify the originating thread of the result */
    spdk_thread_send_msg(ctx->orig_thread, rpc_thread_set_cpumask_done, ctx);
}
```

---

## Inter-Thread Messaging

### The Message System

Messages are the only safe way to coordinate between SPDK threads. The underlying mechanism is a lock-free ring buffer (`spdk_ring`) with a small per-thread cache to avoid pool pressure.

```
Thread A                          Thread B
   │                                 │
   │  spdk_thread_send_msg(B, fn, ctx)
   │──────────────────────────────►  │
   │  (enqueue to B's ring)          │
   │                                 │
   │                   spdk_thread_poll(B, ...)
   │                                 │
   │                    dequeue msg  │
   │                    call fn(ctx) │
   │                                 ▼
```

### Basic Message Sending

```c
/*
 * spdk_thread_send_msg() is always asynchronous — fn() runs during the
 * next poll of `thread`, not immediately.
 *
 * The call is fire-and-forget: errors are fatal and handled internally.
 */

struct my_work {
    int value;
    struct spdk_thread *reply_thread;
};

static void
do_work_on_target(void *arg)
{
    struct my_work *work = arg;

    /* This runs on the target thread */
    SPDK_NOTICELOG("Processing value %d on thread '%s'\n",
                   work->value,
                   spdk_thread_get_name(spdk_get_thread()));

    /* When done, send a completion message back to the originator */
    spdk_thread_send_msg(work->reply_thread, work_complete_cb, work);
}

void
dispatch_work(struct spdk_thread *target, int value)
{
    struct my_work *work = calloc(1, sizeof(*work));
    if (work == NULL) {
        return;
    }

    work->value = value;
    work->reply_thread = spdk_get_thread(); /* capture current thread */

    spdk_thread_send_msg(target, do_work_on_target, work);
    /* Returns immediately — work runs later on 'target' */
}
```

### spdk_thread_exec_msg: Inline Optimization

When you don't know at call time whether you are already on the target thread:

```c
/*
 * spdk_thread_exec_msg() is inline and checks if thread == current thread.
 * If yes: calls fn(ctx) immediately (synchronously).
 * If no:  calls spdk_thread_send_msg() (asynchronously).
 *
 * Use this to avoid unnecessary message latency when the caller may
 * already be on the right thread.
 */
static inline int
spdk_thread_exec_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx)
{
    if (spdk_unlikely(spdk_get_thread() != thread)) {
        return spdk_thread_send_msg(thread, fn, ctx);
    }
    fn(ctx);
    return 0;
}
```

### Critical Messages

For signal handlers and other interrupt contexts, only one critical message can be outstanding at a time:

```c
/*
 * spdk_thread_send_critical_msg() uses a single atomic slot rather than
 * the ring. Only one critical message per thread can be queued at once.
 * Use for SIGINT handlers or other interrupt-driven shutdown triggers.
 */
static void
handle_shutdown(void *arg)
{
    /* Runs on app thread — initiates graceful shutdown */
    spdk_app_stop(0);
}

static void
signal_handler(int signum)
{
    struct spdk_thread *app_thread = spdk_thread_get_app_thread();

    /* Safe to call from signal handler context */
    spdk_thread_send_critical_msg(app_thread, handle_shutdown);
}
```

### Request-Response Pattern

Coordinating a response across threads without a condition variable:

```c
struct rpc_ctx {
    struct spdk_thread  *orig_thread;   /* thread that initiated the request */
    int                  result;
    bool                 completed;
    spdk_msg_fn          completion_cb;
    void                *completion_arg;
};

/* Step 2: runs on the worker thread */
static void
_do_rpc_work(void *arg)
{
    struct rpc_ctx *ctx = arg;

    ctx->result = do_actual_work();

    /* Step 3: reply to the originating thread */
    spdk_thread_send_msg(ctx->orig_thread, ctx->completion_cb, ctx);
}

/* Step 1: called from any thread */
void
dispatch_rpc_request(struct spdk_thread *worker,
                     spdk_msg_fn completion_cb,
                     void *completion_arg)
{
    struct rpc_ctx *ctx = calloc(1, sizeof(*ctx));

    ctx->orig_thread    = spdk_get_thread();
    ctx->completion_cb  = completion_cb;
    ctx->completion_arg = completion_arg;

    spdk_thread_send_msg(worker, _do_rpc_work, ctx);
}
```

---

## Pollers: The Heartbeat of SPDK Threads

Pollers are functions registered to run repeatedly on an SPDK thread. They are the primary mechanism for processing I/O completions, checking hardware queues, and performing background work.

### Poller States

From `lib/thread/thread.c`:

```c
enum spdk_poller_state {
    SPDK_POLLER_STATE_WAITING,      /* registered, not currently executing */
    SPDK_POLLER_STATE_RUNNING,      /* currently inside the poller's fn() */
    SPDK_POLLER_STATE_UNREGISTERED, /* unregistered while fn() was running */
    SPDK_POLLER_STATE_PAUSING,      /* pause requested, takes effect next run */
    SPDK_POLLER_STATE_PAUSED,       /* paused, on paused_pollers list */
};
```

### Continuous Pollers (Active Pollers)

Continuous pollers run on every call to `spdk_thread_poll()`. Use them for hot paths like NVMe queue completion processing.

```c
struct my_device_ctx {
    struct spdk_poller  *poll_poller;
    struct nvme_queue   *queue;
    uint64_t             completions_processed;
};

/*
 * Poller callback must return SPDK_POLLER_BUSY if it did work,
 * or SPDK_POLLER_IDLE if no work was available.
 * This return value drives CPU sleep decisions in interrupt mode.
 */
static int
device_poll(void *arg)
{
    struct my_device_ctx *ctx = arg;
    int completions;

    completions = process_nvme_completions(ctx->queue, 32);
    if (completions > 0) {
        ctx->completions_processed += completions;
        return SPDK_POLLER_BUSY;
    }

    return SPDK_POLLER_IDLE;
}

/* Register a continuous poller (period_microseconds = 0) */
static void
start_device_polling(void *arg)
{
    struct my_device_ctx *ctx = arg;

    ctx->poll_poller = SPDK_POLLER_REGISTER(device_poll, ctx, 0);
    if (ctx->poll_poller == NULL) {
        SPDK_ERRLOG("Failed to register device poller\n");
    }
}
```

`SPDK_POLLER_REGISTER` is a convenience macro that also captures the function name as the poller name for diagnostics:

```c
#define SPDK_POLLER_REGISTER(fn, arg, period_microseconds) \
    spdk_poller_register_named(fn, arg, period_microseconds, #fn)
```

### Timed Pollers

Timed pollers fire approximately every N microseconds. They are stored in a red-black tree sorted by `next_run_tick`.

```c
struct stats_ctx {
    struct spdk_poller  *stats_poller;
    uint64_t             last_busy_tsc;
    uint64_t             last_idle_tsc;
};

static int
print_thread_stats(void *arg)
{
    struct stats_ctx        *ctx = arg;
    struct spdk_thread_stats stats;

    spdk_thread_get_stats(&stats);

    SPDK_NOTICELOG("Thread '%s': busy_tsc_delta=%" PRIu64 " idle_tsc_delta=%" PRIu64 "\n",
                   spdk_thread_get_name(spdk_get_thread()),
                   stats.busy_tsc - ctx->last_busy_tsc,
                   stats.idle_tsc - ctx->last_idle_tsc);

    ctx->last_busy_tsc = stats.busy_tsc;
    ctx->last_idle_tsc = stats.idle_tsc;

    return SPDK_POLLER_BUSY;
}

/* Register a timed poller to fire every 1 second (1,000,000 us) */
static void
start_stats_poller(struct stats_ctx *ctx)
{
    ctx->stats_poller = SPDK_POLLER_REGISTER(print_thread_stats, ctx, 1000000);
}
```

### Pausing and Resuming Pollers

```c
/* Temporarily halt a poller without unregistering it */
spdk_poller_pause(ctx->poll_poller);

/* Re-activate the poller */
spdk_poller_resume(ctx->poll_poller);
```

### Unregistering Pollers

Always unregister pollers before calling `spdk_thread_exit()`:

```c
static void
stop_device_polling(struct my_device_ctx *ctx)
{
    if (ctx->poll_poller != NULL) {
        spdk_poller_unregister(&ctx->poll_poller);
        /* poll_poller is now NULL */
    }
}
```

---

## I/O Channels: Thread-Bound Device Contexts

I/O channels are the mechanism for giving each thread its own private context for interacting with a shared device. They solve the problem of per-thread resources (queue pairs, DMA buffers, statistics) without locking.

### The io_device / io_channel Model

```
┌─────────────────────────────────────────────────────┐
│  io_device (global, e.g., struct nvme_ctrlr *)      │
│                                                     │
│  Registered with: spdk_io_device_register()         │
│  create_cb: allocates per-channel context           │
│  destroy_cb: frees per-channel context              │
└──────────────────────┬──────────────────────────────┘
                       │  one channel per thread
         ┌─────────────┼─────────────┐
         ▼             ▼             ▼
   ┌──────────┐  ┌──────────┐  ┌──────────┐
   │ Thread 0 │  │ Thread 1 │  │ Thread 2 │
   │ channel  │  │ channel  │  │ channel  │
   │ (qpair 0)│  │ (qpair 1)│  │ (qpair 2)│
   └──────────┘  └──────────┘  └──────────┘
```

### Registering an I/O Device

```c
struct my_device {
    struct nvme_ctrlr   *ctrlr;
    int                  num_ns;
};

struct my_channel {
    struct nvme_qpair   *qpair;
    uint32_t             io_count;
};

/*
 * create_cb: called on the thread that calls spdk_get_io_channel().
 * io_device: the pointer passed to spdk_io_device_register().
 * ctx_buf:   pre-allocated buffer of size ctx_size from registration.
 */
static int
my_device_channel_create(void *io_device, void *ctx_buf)
{
    struct my_device  *dev = io_device;
    struct my_channel *ch  = ctx_buf;

    ch->qpair = nvme_ctrlr_alloc_io_qpair(dev->ctrlr);
    if (ch->qpair == NULL) {
        return -ENOMEM;
    }
    ch->io_count = 0;

    SPDK_DEBUGLOG(my_module, "Channel created for thread '%s'\n",
                  spdk_thread_get_name(spdk_get_thread()));
    return 0;
}

/*
 * destroy_cb: called on the thread that owns the channel.
 */
static void
my_device_channel_destroy(void *io_device, void *ctx_buf)
{
    struct my_channel *ch = ctx_buf;

    nvme_ctrlr_free_io_qpair(ch->qpair);
    ch->qpair = NULL;
}

/*
 * unregister_cb: called after all channels have been destroyed.
 * Safe to free the io_device at this point.
 */
static void
my_device_unregister_cb(void *io_device)
{
    struct my_device *dev = io_device;
    free(dev);
}

/* Register the device — called once during initialization */
void
my_device_init(struct my_device *dev)
{
    spdk_io_device_register(
        dev,                           /* io_device pointer (unique key) */
        my_device_channel_create,      /* per-thread create callback */
        my_device_channel_destroy,     /* per-thread destroy callback */
        sizeof(struct my_channel),     /* size of ctx_buf passed to callbacks */
        "my_device"                    /* name for diagnostics */
    );
}

/* Unregister — triggers destroy_cb on each thread that has a channel open */
void
my_device_fini(struct my_device *dev)
{
    spdk_io_device_unregister(dev, my_device_unregister_cb);
}
```

### Acquiring and Releasing Channels

```c
/*
 * spdk_get_io_channel() must be called on the thread that will use the channel.
 * It returns the existing channel if one exists for the current thread,
 * or calls create_cb to create a new one.
 */
static void
my_thread_start(void *arg)
{
    struct my_device        *dev = arg;
    struct spdk_io_channel  *ch;
    struct my_channel       *my_ch;

    /* Get or create a channel for the current thread */
    ch = spdk_get_io_channel(dev);
    if (ch == NULL) {
        SPDK_ERRLOG("Failed to get I/O channel\n");
        return;
    }

    /* Get the typed context buffer */
    my_ch = spdk_io_channel_get_ctx(ch);

    /* Use my_ch->qpair for I/O on this thread */
    submit_io(my_ch->qpair);

    /* Release the channel when done — triggers destroy_cb when refcount hits 0 */
    spdk_put_io_channel(ch);
}
```

### Iterating Channels Across All Threads

A common pattern for configuration updates or statistics collection:

```c
/*
 * spdk_for_each_channel() visits each open channel on its owning thread.
 * The msg callback runs on the correct thread for each channel.
 */

struct update_ctx {
    int new_queue_depth;
};

static void
update_one_channel(struct spdk_io_channel_iter *i)
{
    struct spdk_io_channel  *ch  = spdk_io_channel_iter_get_channel(i);
    struct my_channel       *my_ch = spdk_io_channel_get_ctx(ch);
    struct update_ctx       *ctx = spdk_io_channel_iter_get_ctx(i);

    /* This runs on the thread that owns 'ch' */
    my_ch->queue_depth = ctx->new_queue_depth;

    /* Signal that we're done with this channel — moves to the next */
    spdk_for_each_channel_continue(i, 0);
}

static void
update_all_done(struct spdk_io_channel_iter *i, int status)
{
    struct update_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
    SPDK_NOTICELOG("Updated all channels, status=%d\n", status);
    free(ctx);
}

void
update_all_channels(struct my_device *dev, int new_queue_depth)
{
    struct update_ctx *ctx = calloc(1, sizeof(*ctx));
    ctx->new_queue_depth = new_queue_depth;

    spdk_for_each_channel(
        dev,               /* io_device to iterate */
        update_one_channel, /* called on each channel's thread */
        ctx,               /* context passed through */
        update_all_done    /* called when all channels have been visited */
    );
}
```

---

## Thread-Local Storage and Per-Thread Resources

### Using spdk_thread_get_ctx()

When `spdk_thread_lib_init()` is called with `ctx_sz > 0`, each thread has private storage accessible via `spdk_thread_get_ctx()`. This is the scheduler's mechanism, but it illustrates the pattern.

For application code, the common approach is to pass context through the message and poller APIs:

```c
/*
 * Pattern: embed the spdk_thread pointer in your context struct.
 * The thread is then accessible wherever the context is passed.
 */
struct worker_ctx {
    struct spdk_thread  *thread;
    struct spdk_poller  *poller;
    uint64_t             processed;
    /* ... your state ... */
};

static int
worker_poller_fn(void *arg)
{
    struct worker_ctx *ctx = arg;

    /* ctx->thread is always this thread — no need to call spdk_get_thread() */
    assert(ctx->thread == spdk_get_thread());

    ctx->processed++;
    return SPDK_POLLER_BUSY;
}

static void
init_worker(void *arg)
{
    struct worker_ctx *ctx = arg;

    ctx->thread = spdk_get_thread(); /* capture on first entry */
    ctx->poller = SPDK_POLLER_REGISTER(worker_poller_fn, ctx, 0);
}
```

### Thread Identity APIs

```c
/* Get the currently executing thread */
struct spdk_thread *current = spdk_get_thread();

/* Get thread metadata */
const char *name  = spdk_thread_get_name(current);
uint64_t    id    = spdk_thread_get_id(current);

/* Look up a thread by ID (useful after serializing across an RPC) */
struct spdk_thread *found = spdk_thread_get_by_id(id);

/* Get the application main thread (the first thread created) */
struct spdk_thread *app = spdk_thread_get_app_thread();
bool is_app             = spdk_thread_is_app_thread(NULL); /* checks current */
```

---

## Thread Lifecycle Management

### Proper Thread Shutdown Sequence

The order matters: pollers must be unregistered and channels must be released before calling `spdk_thread_exit()`.

```c
struct my_app_thread {
    struct spdk_thread      *thread;
    struct spdk_poller      *work_poller;
    struct spdk_poller      *stats_poller;
    struct spdk_io_channel  *dev_channel;
    bool                     exiting;
};

/* Step 2: runs on the thread being shut down */
static void
thread_exit_fn(void *arg)
{
    struct my_app_thread *t = arg;

    /* Unregister all pollers first */
    if (t->work_poller) {
        spdk_poller_unregister(&t->work_poller);
    }
    if (t->stats_poller) {
        spdk_poller_unregister(&t->stats_poller);
    }

    /* Release all I/O channels */
    if (t->dev_channel) {
        spdk_put_io_channel(t->dev_channel);
        t->dev_channel = NULL;
    }

    /* Mark the thread as exiting — it will drain remaining work */
    spdk_thread_exit(t->thread);
}

/* Step 1: called from any thread to initiate shutdown */
void
shutdown_worker(struct my_app_thread *t)
{
    spdk_thread_send_msg(t->thread, thread_exit_fn, t);
}

/*
 * Step 3: poll the thread externally until it is fully exited,
 * then destroy it. This runs in the management/monitoring loop.
 */
void
wait_and_destroy_thread(struct my_app_thread *t)
{
    while (!spdk_thread_is_exited(t->thread)) {
        spdk_thread_poll(t->thread, 0, 0);
    }

    spdk_thread_destroy(t->thread);
    t->thread = NULL;
}
```

### Thread Health Monitoring

```c
/*
 * Monitor thread utilization using busy_tsc / idle_tsc statistics.
 * Values are cumulative TSC (timestamp counter) ticks.
 */
static void
log_thread_utilization(struct spdk_thread *thread)
{
    struct spdk_thread_stats stats;
    double utilization;

    if (spdk_thread_get_stats(&stats) != 0) {
        return;
    }

    uint64_t total = stats.busy_tsc + stats.idle_tsc;
    if (total > 0) {
        utilization = (double)stats.busy_tsc / (double)total * 100.0;
        SPDK_NOTICELOG("Thread '%s': %.1f%% busy\n",
                       spdk_thread_get_name(thread), utilization);
    }
}

/* Check if a thread has any work pending */
static void
check_thread_idle(struct spdk_thread *thread)
{
    if (spdk_thread_is_idle(thread)) {
        SPDK_DEBUGLOG(my_mod, "Thread '%s' is idle\n",
                      spdk_thread_get_name(thread));
    }

    if (spdk_thread_has_active_pollers(thread)) {
        SPDK_DEBUGLOG(my_mod, "Thread '%s' has active pollers\n",
                      spdk_thread_get_name(thread));
    }
}
```

---

## CPU Core Assignment and Affinity

### Understanding cpumask

```c
#include "spdk/cpuset.h"

/*
 * spdk_cpuset is a bitmask of CPU cores.
 * The thread scheduler uses it as a hint — not a hard binding.
 * For hard binding, use spdk_thread_bind() after creation.
 */

/* Create a mask for cores 2 and 3 */
struct spdk_cpuset mask;
spdk_cpuset_zero(&mask);
spdk_cpuset_set_cpu(&mask, 2, true);
spdk_cpuset_set_cpu(&mask, 3, true);

/* Parse a cpumask from a hex string (e.g., "0xc" = cores 2,3) */
struct spdk_cpuset parsed;
int rc = spdk_cpuset_parse(&parsed, "0xc");

/* Convert to string for logging */
char buf[SPDK_CPUSET_SIZE];
SPDK_NOTICELOG("cpumask: %s\n", spdk_cpuset_fmt(&mask));

/* Query current core */
uint32_t current_core = spdk_env_get_current_core();
```

### Real-World Pattern: NVMf Poll Group Threads

From `module/event/subsystems/nvmf/nvmf_tgt.c`:

```c
/*
 * NVMf creates one poll group thread per core to distribute
 * NVMe-oF connection processing across all available CPUs.
 */
static void
nvmf_tgt_create_poll_group_threads(void)
{
    uint32_t i;
    char thread_name[256];

    SPDK_ENV_FOREACH_CORE(i) {
        snprintf(thread_name, sizeof(thread_name),
                 "nvmf_tgt_poll_group_%u", i);

        /*
         * g_poll_groups_mask restricts threads to cores designated
         * for NVMf processing, set at startup via --cpumask argument.
         */
        g_poll_groups[i].thread = spdk_thread_create(thread_name,
                                                       g_poll_groups_mask);
        if (g_poll_groups[i].thread == NULL) {
            SPDK_ERRLOG("Failed to create poll group thread for core %u\n", i);
        }
    }
}
```

### App Thread Pattern

```c
/*
 * From lib/event/app.c: the app thread is the first thread created.
 * It handles RPC processing, subsystem initialization, and shutdown.
 */
static void
spdk_app_start(void *arg)
{
    struct spdk_cpuset tmp_cpumask;

    /* Create the app thread on the master lcore */
    spdk_cpuset_zero(&tmp_cpumask);
    spdk_cpuset_set_cpu(&tmp_cpumask, spdk_env_get_current_core(), true);
    spdk_thread_create("app_thread", &tmp_cpumask);

    /* All further initialization happens via messages to this thread */
    spdk_thread_send_msg(spdk_thread_get_app_thread(), bootstrap_fn, NULL);
}
```

---

## Complete Example: Multi-Thread Worker Pool

This example demonstrates all the concepts together: thread creation, pollers, inter-thread messaging, I/O channels, and lifecycle management.

```c
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/log.h"

#define MAX_WORKERS 16

struct work_request {
    uint64_t             request_id;
    struct spdk_thread  *reply_thread;
    void               (*complete_cb)(uint64_t id, int result, void *arg);
    void                *complete_arg;
};

struct worker_thread {
    struct spdk_thread  *thread;
    struct spdk_poller  *poller;
    uint32_t             core;
    uint64_t             requests_handled;
    bool                 stopping;
};

static struct worker_thread g_workers[MAX_WORKERS];
static int                  g_num_workers = 0;

/* ---- Poller function: runs on each worker thread ---- */

static int
worker_poller(void *arg)
{
    struct worker_thread *w = arg;

    if (w->stopping) {
        spdk_poller_unregister(&w->poller);
        spdk_thread_exit(w->thread);
        return SPDK_POLLER_BUSY;
    }

    /* In a real app, process hardware completions here */
    return SPDK_POLLER_IDLE;
}

/* ---- Message handlers ---- */

static void
handle_work_request(void *arg)
{
    struct work_request  *req    = arg;
    struct worker_thread *w;
    int result;

    /* Find this thread's worker context */
    uint32_t core = spdk_env_get_current_core();
    w = &g_workers[core];
    w->requests_handled++;

    /* Do the actual work */
    result = 0; /* placeholder */

    /* Reply to the requesting thread */
    spdk_thread_send_msg(req->reply_thread, (spdk_msg_fn)req->complete_cb, req);
    (void)result;
}

static void
worker_thread_init(void *arg)
{
    struct worker_thread *w = arg;

    w->poller = SPDK_POLLER_REGISTER(worker_poller, w, 0);
    if (w->poller == NULL) {
        SPDK_ERRLOG("Failed to register poller on core %u\n", w->core);
    }

    SPDK_NOTICELOG("Worker thread started on core %u (thread='%s')\n",
                   w->core, spdk_thread_get_name(spdk_get_thread()));
}

static void
worker_thread_stop(void *arg)
{
    struct worker_thread *w = arg;
    w->stopping = true;
    /* Poller will unregister itself and call spdk_thread_exit() */
}

/* ---- Public API ---- */

int
worker_pool_init(void)
{
    uint32_t i;
    char name[64];
    struct spdk_cpuset cpumask;

    SPDK_ENV_FOREACH_CORE(i) {
        if (g_num_workers >= MAX_WORKERS) {
            break;
        }

        snprintf(name, sizeof(name), "worker_%u", i);
        spdk_cpuset_zero(&cpumask);
        spdk_cpuset_set_cpu(&cpumask, i, true);

        g_workers[g_num_workers].core    = i;
        g_workers[g_num_workers].thread  = spdk_thread_create(name, &cpumask);
        if (g_workers[g_num_workers].thread == NULL) {
            SPDK_ERRLOG("Failed to create worker thread for core %u\n", i);
            return -ENOMEM;
        }

        /* Initialize the thread's poller via a message */
        spdk_thread_send_msg(g_workers[g_num_workers].thread,
                             worker_thread_init,
                             &g_workers[g_num_workers]);
        g_num_workers++;
    }

    SPDK_NOTICELOG("Worker pool initialized with %d threads\n", g_num_workers);
    return 0;
}

void
worker_pool_dispatch(struct work_request *req, uint32_t target_core)
{
    struct worker_thread *w;

    if (target_core >= (uint32_t)g_num_workers) {
        target_core = 0;
    }

    w = &g_workers[target_core];
    req->reply_thread = spdk_get_thread();
    spdk_thread_send_msg(w->thread, handle_work_request, req);
}

void
worker_pool_fini(void)
{
    int i;

    for (i = 0; i < g_num_workers; i++) {
        if (g_workers[i].thread != NULL) {
            spdk_thread_send_msg(g_workers[i].thread,
                                 worker_thread_stop,
                                 &g_workers[i]);
        }
    }

    /* In a real app, wait in the event loop for threads to exit */
    for (i = 0; i < g_num_workers; i++) {
        if (g_workers[i].thread == NULL) {
            continue;
        }
        while (!spdk_thread_is_exited(g_workers[i].thread)) {
            spdk_thread_poll(g_workers[i].thread, 0, 0);
        }
        spdk_thread_destroy(g_workers[i].thread);
        g_workers[i].thread = NULL;
    }
}
```

---

## Common Pitfalls and Diagnostics

### Pitfall 1: Accessing Shared State Without Messaging

```c
/* WRONG: thread A directly mutates state owned by thread B */
void bad_cross_thread_write(struct spdk_thread *b, struct shared_state *s)
{
    s->counter++;   /* Race condition — no lock, no message */
}

/* CORRECT: send a message so the mutation runs on the owning thread */
static void increment_counter(void *arg)
{
    struct shared_state *s = arg;
    s->counter++;   /* Safe: runs on the thread that owns s */
}

void good_cross_thread_write(struct spdk_thread *b, struct shared_state *s)
{
    spdk_thread_send_msg(b, increment_counter, s);
}
```

### Pitfall 2: Blocking Inside a Poller or Message Handler

SPDK threads are cooperative. Any call that blocks (sleep, mutex wait, synchronous I/O) stalls the entire reactor and all threads on it.

```c
/* WRONG: blocks the reactor for 1ms */
static int bad_poller(void *arg)
{
    usleep(1000);           /* Never do this */
    return SPDK_POLLER_IDLE;
}

/* WRONG: spinning on a result from another thread */
int bad_sync_call(struct spdk_thread *target)
{
    volatile bool done = false;
    spdk_thread_send_msg(target, set_done, (void *)&done);
    while (!done) {}        /* Deadlock if target is on same reactor */
    return 0;
}
```

### Pitfall 3: Calling spdk_thread_exit() Before Cleanup

```c
/* WRONG: exits without releasing resources */
static void bad_exit(void *arg)
{
    struct worker *w = arg;
    spdk_thread_exit(w->thread);  /* poller and channel still active */
}

/* CORRECT: clean up first */
static void good_exit(void *arg)
{
    struct worker *w = arg;

    spdk_poller_unregister(&w->poller);     /* unregister all pollers */
    spdk_put_io_channel(w->channel);        /* release all channels */
    spdk_thread_exit(w->thread);            /* now safe to exit */
}
```

### Pitfall 4: Getting an I/O Channel on the Wrong Thread

```c
/* WRONG: gets a channel on thread A but uses it on thread B */
struct spdk_io_channel *ch = spdk_get_io_channel(dev);   /* on thread A */
spdk_thread_send_msg(thread_b, use_channel, ch);          /* used on B */

/* CORRECT: get the channel on the thread that will use it */
static void get_and_use_channel(void *arg)
{
    struct my_device *dev = arg;
    struct spdk_io_channel *ch = spdk_get_io_channel(dev); /* on thread B */
    /* use ch here, then put it here */
    spdk_put_io_channel(ch);
}
spdk_thread_send_msg(thread_b, get_and_use_channel, dev);
```

---

## Thread Management Checklist

When writing code that uses SPDK threads, verify:

| Task | API | Notes |
|------|-----|-------|
| Library init | `spdk_thread_lib_init()` | Once per process, before any threads |
| Thread creation | `spdk_thread_create()` | Returns immediately; thread runs when polled |
| Poller registration | `SPDK_POLLER_REGISTER()` | Must be called on the owning thread |
| Cross-thread call | `spdk_thread_send_msg()` | Always async; use exec_msg if same-thread possible |
| I/O device registration | `spdk_io_device_register()` | Once per device, any thread |
| Get I/O channel | `spdk_get_io_channel()` | On the thread that will use it |
| Put I/O channel | `spdk_put_io_channel()` | On the same thread that got it |
| Thread shutdown | `spdk_thread_exit()` | After unregistering pollers and putting channels |
| Thread destruction | `spdk_thread_destroy()` | Only after `spdk_thread_is_exited()` returns true |
| I/O device unregister | `spdk_io_device_unregister()` | After all threads have put their channels |

---

## Practice Exercises

### Exercise 1: Timed Statistics Reporter

Create a module that:
1. Creates one SPDK thread per active lcore
2. Registers a timed poller on each thread that fires every 500ms
3. In the poller, logs the thread name, ID, and current `busy_tsc`/`idle_tsc` ratio
4. Provides a shutdown function that sends a stop message to each thread and waits for all to exit

Key APIs: `spdk_thread_create`, `SPDK_POLLER_REGISTER`, `spdk_thread_get_stats`, `spdk_thread_send_msg`, `spdk_thread_exit`, `spdk_thread_is_exited`, `spdk_thread_destroy`

### Exercise 2: Per-Thread Counter with Cross-Thread Query

Implement:
1. A context struct with a `uint64_t counter` per thread
2. A continuous poller that increments the counter
3. A `query_counter(thread, callback, arg)` function that sends a message to the target thread, reads its counter, then sends the result back via another message to the caller's thread

This exercise illustrates the request-response message pattern without any shared state or locking.

### Exercise 3: I/O Device with Per-Thread Queue

Implement:
1. An `io_device` with a per-thread queue (use a fixed-size array as the queue)
2. `create_cb` that initializes the queue
3. `destroy_cb` that asserts the queue is empty
4. A `submit_io(device, buffer, len, cb, cb_arg)` function that gets the channel for the current thread and enqueues the request
5. A poller that drains the queue and calls completions

---

## Key Takeaways

**Threading Model**:
- SPDK threads are lightweight cooperative units, not OS threads
- Each SPDK thread runs on exactly one reactor (OS thread) at a time
- No preemption: functions run to completion, so never block
- One reactor can run multiple SPDK threads in round-robin

**Communication**:
- `spdk_thread_send_msg()` is the only safe cross-thread mechanism
- Messages are enqueued in a lock-free ring and drained during `spdk_thread_poll()`
- `spdk_thread_exec_msg()` avoids a message round-trip when already on the target thread
- Critical messages use an atomic single slot for signal-safe delivery

**I/O Channels**:
- Each thread gets its own private channel context for each io_device
- Channels must be acquired and released on the same thread
- `spdk_for_each_channel()` visits channels on their owning threads safely

**Lifecycle**:
- Unregister all pollers and put all I/O channels before calling `spdk_thread_exit()`
- Poll the thread after `spdk_thread_exit()` until `spdk_thread_is_exited()` returns true
- Only then call `spdk_thread_destroy()`
- Unregister io_devices only after all threads have released their channels

**Performance**:
- Continuous pollers (`period_microseconds = 0`) are ideal for hot paths
- Return `SPDK_POLLER_IDLE` when no work was done (enables interrupt mode sleep)
- Avoid cross-thread messages on the critical I/O path; prefer per-thread state via I/O channels
- Use `spdk_thread_get_stats()` to measure busy vs. idle time per thread

---

## References

- `include/spdk/thread.h` — Full thread API with documentation
- `lib/thread/thread.c` — Thread, poller, and I/O channel implementation
- `lib/event/reactor.c` — Reactor loop and thread scheduling
- `lib/event/app.c` — App thread creation and bootstrap pattern
- `module/event/subsystems/nvmf/nvmf_tgt.c` — Per-core thread creation example
- `lib/ftl/ftl_init.c` — Single-thread creation with explicit cpumask
- `include/spdk/env.h` — CPU core enumeration APIs (`SPDK_ENV_FOREACH_CORE`)
- `include/spdk/cpuset.h` — CPU affinity mask APIs

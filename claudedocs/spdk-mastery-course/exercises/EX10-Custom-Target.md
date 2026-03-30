# Exercise 10: Custom Target Application (Capstone)

## Overview

| Field | Value |
|-------|-------|
| Exercise | 10 of 10 |
| Type | Capstone — builds on Exercises 1-9 |
| Estimated Time | 4-6 hours |
| Difficulty | Advanced |
| Modules Covered | 13-22 |

## Objective

Design and implement a production-grade custom NVMe-oF target application from scratch. Rather
than relying solely on the JSON-RPC configuration path used by the stock `nvmf_tgt`, you will
build a self-contained C application that programmatically constructs a target, attaches
subsystems, registers listeners, mounts namespaces, and injects custom business logic: access
logging, per-namespace I/O statistics, and token-bucket rate limiting.

By the end of this exercise you will have demonstrated end-to-end mastery of the SPDK event
framework, the NVMe-oF target API, bdev layer integration, and the reactor/poller model.

---

## Prerequisites

Complete all prior exercises and confirm the following before starting:

- [ ] SPDK built successfully (`make` in the SPDK root, no errors)
- [ ] Hugepages allocated (`scripts/setup.sh`)
- [ ] A null bdev or NVMe bdev available for namespace backing
- [ ] `nvme-cli` installed (`nvme version`)
- [ ] SPDK perf tool built (`build/bin/spdk_nvme_perf`)
- [ ] Familiarity with `spdk_app_start`, `spdk_nvmf_tgt_create`, reactor/poller concepts
- [ ] Exercises 7 (bdev layer), 8 (NVMe-oF basics), and 9 (subsystem management) completed

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    custom_tgt Process                           │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                  SPDK App Framework                      │   │
│  │   spdk_app_start() → custom_tgt_started() callback       │   │
│  └──────────────────────┬───────────────────────────────────┘   │
│                         │                                       │
│  ┌──────────────────────▼───────────────────────────────────┐   │
│  │              spdk_nvmf_tgt  (Target Core)                │   │
│  │                                                          │   │
│  │   ┌─────────────────────────────────────────────────┐    │   │
│  │   │           Subsystem: nqn.custom.disk0           │    │   │
│  │   │                                                 │    │   │
│  │   │  Listeners:                                     │    │   │
│  │   │    TCP  127.0.0.1:4420                          │    │   │
│  │   │    RDMA 192.168.100.1:4421  (bonus)             │    │   │
│  │   │                                                 │    │   │
│  │   │  Namespaces:                                    │    │   │
│  │   │    NSID 1 → Null Bdev  "Null0"  (64 GiB)       │    │   │
│  │   │    NSID 2 → Null Bdev  "Null1"  (64 GiB)       │    │   │
│  │   └─────────────────────────────────────────────────┘    │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │             Custom Business Logic Layer                  │   │
│  │                                                          │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐  │   │
│  │  │ Access Logger│  │  NS I/O Stats│  │  Rate Limiter │  │   │
│  │  │              │  │              │  │  (token bucket│  │   │
│  │  │ logs connect/│  │ read/write   │  │   per NS)     │  │   │
│  │  │ disconnect + │  │ IOPS, bytes, │  │               │  │   │
│  │  │ host NQN     │  │ latency hist │  │  IOPS cap     │  │   │
│  │  └──────────────┘  └──────────────┘  └───────────────┘  │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │          Stats Reporter Poller (1-second interval)       │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
         ▲                              ▲
         │  NVMe-oF / TCP               │  NVMe-oF / RDMA (bonus)
    ┌────┴────┐                    ┌────┴────┐
    │  Host A │                    │  Host B │
    │ nvme-cli│                    │  perf   │
    └─────────┘                    └─────────┘
```

---

## Part 1 — Project Setup (20 minutes)

### 1.1 Create the application directory

```bash
cd $SPDK_DIR
mkdir -p app/custom_tgt
```

### 1.2 Create the Makefile

```makefile
# app/custom_tgt/Makefile
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = custom_tgt

C_SRCS = custom_tgt_main.c

SPDK_LIB_LIST =  \
    nvmf          \
    bdev_null     \
    bdev          \
    accel         \
    thread        \
    util          \
    log           \
    sock          \
    trace

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
```

---

## Part 2 — Core Application Structure (30 minutes)

### 2.1 Header and global state

Create `app/custom_tgt/custom_tgt_main.c` and start with includes and global state:

```c
/* SPDX-License-Identifier: BSD-3-Clause
 * Custom NVMe-oF Target — SPDK Mastery Course Exercise 10
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"

/* ----------------------------------------------------------------
 * Tunables — change these to match your environment
 * ---------------------------------------------------------------- */
#define CUSTOM_TGT_NQN          "nqn.2024-01.io.spdk:cnode1"
#define CUSTOM_TGT_TCP_ADDR     "127.0.0.1"
#define CUSTOM_TGT_TCP_PORT     "4420"
#define CUSTOM_TGT_MAX_SUBSYS   16
#define CUSTOM_TGT_NUM_NS       2

/* Rate-limit: maximum read IOPS allowed per namespace (0 = unlimited) */
#define CUSTOM_TGT_READ_IOPS_LIMIT   50000
/* Token bucket refill interval in microseconds */
#define CUSTOM_TGT_BUCKET_INTERVAL_US  100000   /* 100 ms */

/* ----------------------------------------------------------------
 * Per-namespace I/O statistics
 * ---------------------------------------------------------------- */
struct ns_stats {
    uint64_t    read_ios;
    uint64_t    write_ios;
    uint64_t    read_bytes;
    uint64_t    write_bytes;
    uint64_t    read_latency_ticks;  /* sum — divide by read_ios for avg */
    uint64_t    write_latency_ticks;
    /* Snapshot taken at last stats report */
    uint64_t    prev_read_ios;
    uint64_t    prev_write_ios;
};

/* ----------------------------------------------------------------
 * Token-bucket rate limiter (per namespace)
 * ---------------------------------------------------------------- */
struct token_bucket {
    int64_t     tokens;          /* current token count */
    int64_t     capacity;        /* max tokens (burst) */
    int64_t     refill_tokens;   /* tokens added per interval */
    uint64_t    last_refill_tsc; /* TSC at last refill */
    uint64_t    tsc_per_interval;/* precomputed: tsc_rate * interval_us / 1e6 */
};

/* ----------------------------------------------------------------
 * Application global context
 * ---------------------------------------------------------------- */
struct custom_tgt_ctx {
    struct spdk_nvmf_tgt        *tgt;
    struct spdk_nvmf_subsystem  *subsystem;
    struct spdk_poller          *stats_poller;

    /* Per-namespace tracking (indexed 0..CUSTOM_TGT_NUM_NS-1) */
    struct ns_stats              ns_stats[CUSTOM_TGT_NUM_NS];
    struct token_bucket          rate_limiter[CUSTOM_TGT_NUM_NS];

    /* Build-up state machine */
    int                          setup_step;
};

static struct custom_tgt_ctx g_ctx = {};
```

---

## Part 3 — Rate Limiter Implementation (20 minutes)

### 3.1 Token bucket helpers

Add these functions after the global declarations:

```c
static void
token_bucket_init(struct token_bucket *tb, int64_t iops_limit,
                  uint64_t interval_us)
{
    uint64_t tsc_rate = spdk_get_ticks_hz();

    /* Burst = 2x the per-interval quota so short bursts are absorbed */
    tb->refill_tokens   = (int64_t)((double)iops_limit *
                                    interval_us / 1000000.0);
    tb->capacity        = tb->refill_tokens * 2;
    tb->tokens          = tb->capacity;
    tb->last_refill_tsc = spdk_get_ticks();
    tb->tsc_per_interval = tsc_rate * interval_us / 1000000UL;
}

/* Returns true if the I/O is allowed, false if it should be deferred/dropped */
static bool
token_bucket_consume(struct token_bucket *tb)
{
    uint64_t now = spdk_get_ticks();
    uint64_t elapsed = now - tb->last_refill_tsc;

    /* Refill proportional to elapsed time */
    if (elapsed >= tb->tsc_per_interval) {
        uint64_t intervals = elapsed / tb->tsc_per_interval;
        tb->tokens += (int64_t)(intervals * tb->refill_tokens);
        if (tb->tokens > tb->capacity) {
            tb->tokens = tb->capacity;
        }
        tb->last_refill_tsc += intervals * tb->tsc_per_interval;
    }

    if (tb->tokens <= 0) {
        return false;   /* over limit */
    }
    tb->tokens--;
    return true;
}
```

---

## Part 4 — Access Logger (15 minutes)

### 4.1 Connection and disconnection hooks

```c
/*
 * Called by the NVMe-oF layer when a new controller (host connection) is
 * established on our subsystem.  We receive a pointer to the controller so
 * we can interrogate it for diagnostic information.
 */
static void
custom_tgt_host_connected(void *ctx, struct spdk_nvmf_subsystem *subsystem,
                           struct spdk_nvmf_ctrlr *ctrlr)
{
    const char *hostnqn = spdk_nvmf_subsystem_get_nqn(subsystem);
    SPDK_NOTICELOG("[ACCESS] Host connected to subsystem '%s'\n", hostnqn);
    SPDK_NOTICELOG("[ACCESS]   Controller ID : %u\n",
                   spdk_nvmf_ctrlr_get_id(ctrlr));
}

static void
custom_tgt_host_disconnected(void *ctx, struct spdk_nvmf_subsystem *subsystem,
                               struct spdk_nvmf_ctrlr *ctrlr)
{
    SPDK_NOTICELOG("[ACCESS] Host disconnected from subsystem '%s'\n",
                   spdk_nvmf_subsystem_get_nqn(subsystem));
}
```

> **Note:** `spdk_nvmf_subsystem_register_ctrlr_callback()` accepts these hooks.
> See Part 6 for where to register them.

---

## Part 5 — Stats Reporter Poller (20 minutes)

### 5.1 Periodic stats dump

```c
static int
stats_reporter_poll(void *arg)
{
    struct custom_tgt_ctx *ctx = arg;
    uint64_t now_tsc = spdk_get_ticks();
    uint64_t hz = spdk_get_ticks_hz();
    (void)now_tsc;
    (void)hz;

    SPDK_NOTICELOG("=== NVMe-oF Target I/O Statistics ===\n");

    for (int i = 0; i < CUSTOM_TGT_NUM_NS; i++) {
        struct ns_stats *s = &ctx->ns_stats[i];
        uint64_t delta_reads  = s->read_ios  - s->prev_read_ios;
        uint64_t delta_writes = s->write_ios - s->prev_write_ios;

        SPDK_NOTICELOG("  NSID %d: read_IOPS=%-8" PRIu64
                       " write_IOPS=%-8" PRIu64
                       " read_MBps=%-6" PRIu64
                       " write_MBps=%-6" PRIu64 "\n",
                       i + 1,
                       delta_reads,       /* per second because poller fires at 1 Hz */
                       delta_writes,
                       s->read_bytes  / (1024 * 1024),
                       s->write_bytes / (1024 * 1024));

        /* Rolling average latency in microseconds */
        if (s->read_ios > 0) {
            uint64_t avg_lat_us = (s->read_latency_ticks / s->read_ios)
                                  * 1000000 / spdk_get_ticks_hz();
            SPDK_NOTICELOG("  NSID %d: avg_read_lat=%" PRIu64 " us\n",
                           i + 1, avg_lat_us);
        }

        /* Snapshot for next interval */
        s->prev_read_ios  = s->read_ios;
        s->prev_write_ios = s->write_ios;
    }

    return SPDK_POLLER_BUSY;
}
```

---

## Part 6 — Target Initialization State Machine (60 minutes)

This is the core of the exercise. SPDK's NVMe-oF subsystem API is asynchronous: each
operation supplies a callback that advances to the next step. We model this as a simple
state machine driven by a series of `_step_N` functions.

### 6.1 Forward declarations

```c
static void step_create_bdevs(struct custom_tgt_ctx *ctx);
static void step_create_transport(struct custom_tgt_ctx *ctx);
static void step_create_subsystem(struct custom_tgt_ctx *ctx);
static void step_add_listener(struct custom_tgt_ctx *ctx);
static void step_add_namespaces(struct custom_tgt_ctx *ctx);
static void step_start_subsystem(struct custom_tgt_ctx *ctx);
static void step_done(struct custom_tgt_ctx *ctx);
```

### 6.2 Step 0 — Application start callback

```c
static void
custom_tgt_started(void *arg1)
{
    struct custom_tgt_ctx *ctx = &g_ctx;
    struct spdk_nvmf_target_opts tgt_opts = {};

    SPDK_NOTICELOG("custom_tgt: application started\n");

    /* Create the NVMe-oF target instance */
    tgt_opts.size        = sizeof(tgt_opts);
    tgt_opts.max_subsystems = CUSTOM_TGT_MAX_SUBSYS;
    spdk_sprintf_s(tgt_opts.name, sizeof(tgt_opts.name), "custom_nvmf_tgt");

    ctx->tgt = spdk_nvmf_tgt_create(&tgt_opts);
    if (ctx->tgt == NULL) {
        SPDK_ERRLOG("Failed to create NVMe-oF target\n");
        spdk_app_stop(-1);
        return;
    }

    step_create_bdevs(ctx);
}
```

### 6.3 Step 1 — Create backing bdevs

```c
/*
 * We use the null bdev module so this exercise runs without physical NVMe
 * hardware.  Replace with spdk_bdev_open_ext() on a real NVMe bdev in
 * production.
 */
static void
step_create_bdevs(struct custom_tgt_ctx *ctx)
{
    struct spdk_bdev *bdev;
    char name[32];

    for (int i = 0; i < CUSTOM_TGT_NUM_NS; i++) {
        snprintf(name, sizeof(name), "Null%d", i);
        /*
         * spdk_bdev_create_null_bdev() is declared in bdev_null.h.
         * Parameters: name, num_blocks, block_size.
         * 64 GiB at 512 B/block = 134217728 blocks.
         */
        bdev = spdk_bdev_get_by_name(name);
        if (bdev == NULL) {
            SPDK_ERRLOG("Bdev '%s' not found. "
                        "Ensure bdev_null is registered via config.\n", name);
            spdk_app_stop(-1);
            return;
        }
        SPDK_NOTICELOG("  Found bdev '%s' (%"PRIu64" blocks x %u B)\n",
                       name,
                       spdk_bdev_get_num_blocks(bdev),
                       spdk_bdev_get_block_size(bdev));

        /* Initialize per-namespace rate limiter */
        if (CUSTOM_TGT_READ_IOPS_LIMIT > 0) {
            token_bucket_init(&ctx->rate_limiter[i],
                              CUSTOM_TGT_READ_IOPS_LIMIT,
                              CUSTOM_TGT_BUCKET_INTERVAL_US);
        }
    }

    step_create_transport(ctx);
}
```

### 6.4 Step 2 — Create TCP transport

```c
static void
transport_create_done_cb(void *cb_arg, int status)
{
    struct custom_tgt_ctx *ctx = cb_arg;
    if (status != 0) {
        SPDK_ERRLOG("Failed to create TCP transport: %d\n", status);
        spdk_app_stop(-1);
        return;
    }
    SPDK_NOTICELOG("  TCP transport created\n");
    step_create_subsystem(ctx);
}

static void
step_create_transport(struct custom_tgt_ctx *ctx)
{
    struct spdk_nvmf_transport_opts opts = {};

    if (!spdk_nvmf_transport_opts_init("TCP", &opts, sizeof(opts))) {
        SPDK_ERRLOG("Failed to initialize TCP transport opts\n");
        spdk_app_stop(-1);
        return;
    }

    /* Tune for low-latency workloads */
    opts.max_queue_depth        = 128;
    opts.max_qpairs_per_ctrlr   = 8;
    opts.in_capsule_data_size   = 4096;
    opts.max_io_size            = 131072;  /* 128 KiB */
    opts.io_unit_size           = 131072;
    opts.num_shared_buffers     = 4096;

    spdk_nvmf_transport_create_async("TCP", &opts,
                                     transport_create_done_cb, ctx);
}
```

### 6.5 Step 3 — Create subsystem

```c
static void
step_create_subsystem(struct custom_tgt_ctx *ctx)
{
    ctx->subsystem = spdk_nvmf_subsystem_create(
                         ctx->tgt,
                         CUSTOM_TGT_NQN,
                         SPDK_NVMF_SUBTYPE_NVME,
                         CUSTOM_TGT_NUM_NS);

    if (ctx->subsystem == NULL) {
        SPDK_ERRLOG("Failed to create NVMe subsystem '%s'\n", CUSTOM_TGT_NQN);
        spdk_app_stop(-1);
        return;
    }

    /* Allow any host to connect (open access).  For production, replace
     * with spdk_nvmf_subsystem_add_host() for allow-listing. */
    spdk_nvmf_subsystem_set_allow_any_host(ctx->subsystem, true);

    /* Register access-log callbacks */
    spdk_nvmf_subsystem_register_ctrlr_callback(
        ctx->subsystem,
        custom_tgt_host_connected,
        custom_tgt_host_disconnected,
        ctx);

    SPDK_NOTICELOG("  Subsystem '%s' created\n", CUSTOM_TGT_NQN);
    step_add_listener(ctx);
}
```

### 6.6 Step 4 — Add listener

```c
static void
listener_add_done_cb(struct spdk_nvmf_subsystem *subsystem,
                     void *cb_arg, int status)
{
    struct custom_tgt_ctx *ctx = cb_arg;
    if (status != 0) {
        SPDK_ERRLOG("Failed to add listener: %d\n", status);
        spdk_app_stop(-1);
        return;
    }
    SPDK_NOTICELOG("  Listener added: TCP %s:%s\n",
                   CUSTOM_TGT_TCP_ADDR, CUSTOM_TGT_TCP_PORT);
    step_add_namespaces(ctx);
}

static void
step_add_listener(struct custom_tgt_ctx *ctx)
{
    struct spdk_nvme_transport_id trid = {};
    struct spdk_nvmf_listen_opts  listen_opts = {};

    spdk_nvmf_transport_id_parse_trtype(&trid.trtype, "TCP");
    trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    snprintf(trid.traddr, sizeof(trid.traddr), "%s", CUSTOM_TGT_TCP_ADDR);
    snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", CUSTOM_TGT_TCP_PORT);

    spdk_nvmf_listen_opts_init(&listen_opts, sizeof(listen_opts));

    /*
     * Transition subsystem to INACTIVE so we can modify it, then add
     * the listener.  The subsystem must be paused before structural
     * changes (listeners, namespaces) can be applied.
     */
    int rc = spdk_nvmf_subsystem_add_listener(ctx->subsystem, &trid,
                                               listener_add_done_cb, ctx);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_nvmf_subsystem_add_listener failed: %d\n", rc);
        spdk_app_stop(-1);
    }
}
```

### 6.7 Step 5 — Add namespaces

```c
static void
step_add_namespaces(struct custom_tgt_ctx *ctx)
{
    for (int i = 0; i < CUSTOM_TGT_NUM_NS; i++) {
        char bdev_name[32];
        struct spdk_nvmf_ns_opts ns_opts = {};

        snprintf(bdev_name, sizeof(bdev_name), "Null%d", i);
        spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));

        /* Force a deterministic NSID for predictability */
        ns_opts.nsid = (uint32_t)(i + 1);

        uint32_t nsid = spdk_nvmf_subsystem_add_ns_ext(
                            ctx->subsystem, bdev_name, &ns_opts,
                            sizeof(ns_opts), NULL);
        if (nsid == 0) {
            SPDK_ERRLOG("Failed to add namespace for bdev '%s'\n", bdev_name);
            spdk_app_stop(-1);
            return;
        }
        SPDK_NOTICELOG("  Namespace NSID=%u backed by '%s'\n",
                       nsid, bdev_name);
    }

    step_start_subsystem(ctx);
}
```

### 6.8 Step 6 — Start subsystem

```c
static void
subsystem_start_done_cb(struct spdk_nvmf_subsystem *subsystem,
                        void *cb_arg, int status)
{
    struct custom_tgt_ctx *ctx = cb_arg;
    if (status != 0) {
        SPDK_ERRLOG("Failed to start subsystem: %d\n", status);
        spdk_app_stop(-1);
        return;
    }
    SPDK_NOTICELOG("  Subsystem started\n");
    step_done(ctx);
}

static void
step_start_subsystem(struct custom_tgt_ctx *ctx)
{
    int rc = spdk_nvmf_subsystem_start(ctx->subsystem,
                                        subsystem_start_done_cb, ctx);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_nvmf_subsystem_start failed: %d\n", rc);
        spdk_app_stop(-1);
    }
}
```

### 6.9 Step 7 — Target fully operational

```c
static void
step_done(struct custom_tgt_ctx *ctx)
{
    SPDK_NOTICELOG("======================================\n");
    SPDK_NOTICELOG("  custom_tgt is READY\n");
    SPDK_NOTICELOG("  NQN  : %s\n", CUSTOM_TGT_NQN);
    SPDK_NOTICELOG("  Addr : %s:%s (TCP)\n",
                   CUSTOM_TGT_TCP_ADDR, CUSTOM_TGT_TCP_PORT);
    SPDK_NOTICELOG("  NS   : %d namespace(s)\n", CUSTOM_TGT_NUM_NS);
    SPDK_NOTICELOG("======================================\n");

    /* Register 1-second stats reporter poller */
    ctx->stats_poller = SPDK_POLLER_REGISTER(stats_reporter_poll, ctx,
                                              1000000 /* 1 s in us */);
    if (ctx->stats_poller == NULL) {
        SPDK_WARNLOG("Failed to register stats poller\n");
    }
}
```

---

## Part 7 — main() and Shutdown (15 minutes)

### 7.1 Graceful shutdown

```c
static void
custom_tgt_shutdown(void)
{
    struct custom_tgt_ctx *ctx = &g_ctx;

    SPDK_NOTICELOG("custom_tgt: shutting down\n");

    if (ctx->stats_poller) {
        spdk_poller_unregister(&ctx->stats_poller);
    }

    if (ctx->subsystem) {
        /* Stop must be async; for brevity we let app cleanup handle it */
        spdk_nvmf_subsystem_destroy(ctx->subsystem, NULL, NULL);
        ctx->subsystem = NULL;
    }

    if (ctx->tgt) {
        spdk_nvmf_tgt_destroy(ctx->tgt, NULL, NULL);
        ctx->tgt = NULL;
    }
}
```

### 7.2 main()

```c
int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name            = "custom_tgt";
    opts.shutdown_cb     = custom_tgt_shutdown;
    /* JSON config is used only to declare bdev_null devices.
     * The NVMe-oF target is built programmatically above. */
    opts.json_config_file = argc > 1 ? argv[1] : NULL;

    rc = spdk_app_start(&opts, custom_tgt_started, NULL);
    spdk_app_fini();
    return rc;
}
```

---

## Part 8 — JSON Bdev Configuration (10 minutes)

Because we use the null bdev module for namespace backing, we need a minimal JSON config
to declare the bdev devices before the programmatic target setup runs.

Create `app/custom_tgt/bdev_null.json`:

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
            "num_blocks": 134217728,
            "block_size": 512
          }
        },
        {
          "method": "bdev_null_create",
          "params": {
            "name": "Null1",
            "num_blocks": 134217728,
            "block_size": 512
          }
        }
      ]
    }
  ]
}
```

---

## Part 9 — Build and Run (20 minutes)

### 9.1 Build

```bash
cd $SPDK_DIR
make -C app/custom_tgt
```

Expected output (last few lines):

```
  CC  custom_tgt_main.o
  LINK custom_tgt
```

The binary lands at `app/custom_tgt/custom_tgt`.

### 9.2 Allocate hugepages (if not already done)

```bash
sudo $SPDK_DIR/scripts/setup.sh
```

### 9.3 Run the target

```bash
sudo $SPDK_DIR/app/custom_tgt/custom_tgt \
    -c $SPDK_DIR/app/custom_tgt/bdev_null.json \
    -m 0x1        \   # reactor mask: core 0
    --iova-mode va    # virtual address IOVA for development environments
```

Expected startup log (abbreviated):

```
[NOTICE]: custom_tgt: application started
[NOTICE]:   Found bdev 'Null0' (134217728 blocks x 512 B)
[NOTICE]:   Found bdev 'Null1' (134217728 blocks x 512 B)
[NOTICE]:   TCP transport created
[NOTICE]:   Subsystem 'nqn.2024-01.io.spdk:cnode1' created
[NOTICE]:   Listener added: TCP 127.0.0.1:4420
[NOTICE]:   Namespace NSID=1 backed by 'Null0'
[NOTICE]:   Namespace NSID=2 backed by 'Null1'
[NOTICE]:   Subsystem started
[NOTICE]: ======================================
[NOTICE]:   custom_tgt is READY
[NOTICE]:   NQN  : nqn.2024-01.io.spdk:cnode1
[NOTICE]:   Addr : 127.0.0.1:4420 (TCP)
[NOTICE]:   NS   : 2 namespace(s)
[NOTICE]: ======================================
```

---

## Part 10 — Testing with nvme-cli (30 minutes)

### 10.1 Load kernel NVMe-TCP driver

```bash
sudo modprobe nvme-tcp
```

### 10.2 Discover the target

```bash
sudo nvme discover -t tcp -a 127.0.0.1 -s 4420
```

Expected output:

```
Discovery Log Number of Records 1, Generation counter 2
=====Discovery Log Entry 0======
trtype:  tcp
adrfam:  ipv4
subtype: nvme subsystem
treq:    not required
portid:  0
trsvcid: 4420
subnqn:  nqn.2024-01.io.spdk:cnode1
traddr:  127.0.0.1
```

### 10.3 Connect and verify namespaces

```bash
sudo nvme connect -t tcp -a 127.0.0.1 -s 4420 \
    -n nqn.2024-01.io.spdk:cnode1

# List the connected device
sudo nvme list

# Check namespace identifiers
sudo nvme id-ns /dev/nvme0n1
sudo nvme id-ns /dev/nvme0n2
```

### 10.4 Basic read/write test

```bash
# Write a pattern to NSID 1
sudo dd if=/dev/urandom of=/dev/nvme0n1 bs=4K count=1000

# Read it back and verify target stats poller output
# In the target terminal you should see:
#   NSID 1: write_IOPS=...  write_MBps=...
```

### 10.5 Verify access log

Observe target terminal output:

```
[NOTICE]: [ACCESS] Host connected to subsystem 'nqn.2024-01.io.spdk:cnode1'
[NOTICE]: [ACCESS]   Controller ID : 1
```

### 10.6 Disconnect

```bash
sudo nvme disconnect -n nqn.2024-01.io.spdk:cnode1
```

---

## Part 11 — Performance Testing with spdk_nvme_perf (30 minutes)

### 11.1 Sequential read baseline

```bash
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -r "trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 \
        subnqn:nqn.2024-01.io.spdk:cnode1" \
    -q 32  \   # queue depth
    -o 4096 \  # I/O size (bytes)
    -w read \  # workload
    -t 10      # duration (seconds)
```

### 11.2 Mixed read/write

```bash
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -r "trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 \
        subnqn:nqn.2024-01.io.spdk:cnode1" \
    -q 64 -o 4096 -w randrw -M 70 -t 30
```

### 11.3 Observe rate limiting

Set `CUSTOM_TGT_READ_IOPS_LIMIT` to `10000`, rebuild, and rerun the read test:

```bash
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -r "trtype:TCP ..." -q 128 -o 4096 -w read -t 10
```

Observe that reported IOPS in the perf output does not exceed ~10 000 IOPS, and the
target stats poller confirms the token bucket is actively throttling.

> **Checkpoint question:** Why does rate limiting with a token bucket at the target side
> increase observed latency on the initiator rather than reducing throughput uniformly?

---

## Part 12 — Checkpoint: Verify Business Logic (15 minutes)

After running the tests, answer the following in a short text file
`app/custom_tgt/NOTES.md`:

1. What did the access logger print when `spdk_nvme_perf` connected?
2. What is the per-interval token refill count for a 50 000 IOPS limit at 100 ms intervals?
3. Does the stats reporter poller run on the same reactor thread as the NVMe-oF I/O path?
   Why does this matter for lock-free stats accumulation?
4. What change would you make to support per-host rate limiting instead of per-namespace?

---

## Bonus A — Multi-Transport Support (TCP + RDMA)

Add RDMA transport support for environments with RDMA-capable NICs.

### B.1 Add a second listener

Extend `step_add_listener` (or add a new `step_add_rdma_listener`) to also register an
RDMA listener on a separate address/port:

```c
#define CUSTOM_TGT_RDMA_ADDR  "192.168.100.1"
#define CUSTOM_TGT_RDMA_PORT  "4421"

static void
step_add_rdma_listener(struct custom_tgt_ctx *ctx)
{
    struct spdk_nvme_transport_id trid = {};
    struct spdk_nvmf_listen_opts  listen_opts = {};

    spdk_nvmf_transport_id_parse_trtype(&trid.trtype, "RDMA");
    trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    snprintf(trid.traddr,  sizeof(trid.traddr),  "%s", CUSTOM_TGT_RDMA_ADDR);
    snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", CUSTOM_TGT_RDMA_PORT);

    spdk_nvmf_listen_opts_init(&listen_opts, sizeof(listen_opts));

    int rc = spdk_nvmf_subsystem_add_listener(ctx->subsystem, &trid,
                                               rdma_listener_add_done_cb, ctx);
    if (rc != 0) {
        SPDK_WARNLOG("RDMA listener not added (no RDMA hardware?): %d\n", rc);
        step_add_namespaces(ctx);  /* continue without RDMA */
    }
}
```

Add `nvmf_rdma` to `SPDK_LIB_LIST` in the Makefile.

### B.2 Test RDMA path

```bash
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -r "trtype:RDMA adrfam:IPv4 traddr:192.168.100.1 trsvcid:4421 \
        subnqn:nqn.2024-01.io.spdk:cnode1" \
    -q 128 -o 4096 -w read -t 30
```

Compare IOPS and latency vs the TCP path. Document the difference.

---

## Bonus B — Dynamic Namespace Addition via Signal

Implement SIGUSR1 handling to add a third namespace at runtime without restarting the
target. This demonstrates the SPDK subsystem pause/resume cycle required for hot namespace
addition.

```c
/* Signal handler runs on app thread via spdk_thread_send_msg */
static void
add_namespace_on_signal(void *arg)
{
    struct custom_tgt_ctx *ctx = arg;

    /* Must pause subsystem before modifying it */
    int rc = spdk_nvmf_subsystem_pause(ctx->subsystem,
                                        0,  /* all NSIDs */
                                        add_ns_paused_cb, ctx);
    if (rc != 0) {
        SPDK_ERRLOG("Failed to pause subsystem for hot-add: %d\n", rc);
    }
}

static void
add_ns_paused_cb(struct spdk_nvmf_subsystem *subsystem,
                 void *cb_arg, int status)
{
    struct custom_tgt_ctx *ctx = cb_arg;
    struct spdk_nvmf_ns_opts opts = {};

    spdk_nvmf_ns_opts_get_defaults(&opts, sizeof(opts));
    opts.nsid = 3;

    uint32_t nsid = spdk_nvmf_subsystem_add_ns_ext(subsystem, "Null2",
                                                    &opts, sizeof(opts), NULL);
    SPDK_NOTICELOG("Hot-added NSID %u\n", nsid);

    spdk_nvmf_subsystem_resume(subsystem, add_ns_resumed_cb, ctx);
}
```

Test by sending the signal while a perf run is in progress:

```bash
sudo kill -SIGUSR1 $(pgrep custom_tgt)
# Verify on the initiator side:
sudo nvme id-ctrl /dev/nvme0 | grep nn   # namespace count should increase
```

---

## Common Errors and Remediation

| Error | Likely Cause | Fix |
|-------|-------------|-----|
| `Bdev 'Null0' not found` | JSON config not loaded or bdev_null module missing | Pass `-c bdev_null.json`; add `bdev_null` to `SPDK_LIB_LIST` |
| `Failed to create TCP transport` | Port 4420 already in use (prior run) | `sudo fuser -k 4420/tcp` then retry |
| `spdk_nvmf_subsystem_add_listener failed: -22` | Transport not yet added to target | Ensure `transport_create_done_cb` completes before `step_add_listener` |
| `nvme discover` returns empty | Firewall blocking port 4420 | `sudo iptables -I INPUT -p tcp --dport 4420 -j ACCEPT` |
| Perf reports 0 IOPS | Subsystem not started, or wrong NQN | Verify `[NOTICE]: Subsystem started` in target log; check NQN spelling |
| Build: undefined `spdk_nvmf_ctrlr_get_id` | API mismatch with SPDK version | Replace with `spdk_nvmf_ctrlr_get_num_namespaces` or consult `include/spdk/nvmf.h` |

---

## Capstone Integration Checklist

This exercise draws on the following course modules. Tick each off as you complete the
corresponding part of the exercise:

| Module | Topic | Where Used |
|--------|-------|-----------|
| 13 | SPDK App Framework | `spdk_app_start`, `spdk_app_opts_init` |
| 14 | Reactor and Poller Model | `SPDK_POLLER_REGISTER`, stats reporter |
| 15 | Thread and Message Passing | `spdk_thread_send_msg` (Bonus B) |
| 16 | Bdev Layer | `spdk_bdev_get_by_name`, null bdev |
| 17 | NVMe-oF Target Creation | `spdk_nvmf_tgt_create` |
| 18 | Transports | `spdk_nvmf_transport_create_async` |
| 19 | Subsystems and Hosts | `spdk_nvmf_subsystem_create` |
| 20 | Listeners | `spdk_nvmf_subsystem_add_listener` |
| 21 | Namespaces | `spdk_nvmf_subsystem_add_ns_ext` |
| 22 | Controller Callbacks | `spdk_nvmf_subsystem_register_ctrlr_callback` |

---

## Summary

You have built a complete, production-inspired NVMe-oF target application that:

- Constructs a target, transport, subsystem, listeners, and namespaces programmatically
  using the `spdk_nvmf_*` API rather than relying on JSON-RPC at runtime
- Logs host connection and disconnection events with controller identity information
- Tracks per-namespace read/write IOPS, byte throughput, and average read latency via a
  lock-free stats structure updated on the I/O path
- Enforces a token-bucket read IOPS cap per namespace to demonstrate cooperative rate
  limiting without kernel involvement
- Reports live statistics every second via a reactor poller at zero additional thread cost
- Supports optional multi-transport (TCP + RDMA) and hot namespace addition (Bonus)

This capstone demonstrates that SPDK targets are not limited to the stock `nvmf_tgt` JSON
configuration model: the full target API is available to any C application that links the
SPDK libraries, enabling storage appliance firmware, hypervisor storage backends, and
custom NVMe-oF gateway implementations.

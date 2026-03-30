# Exercise 04: Null Bdev Module

## Overview

| Field | Value |
|-------|-------|
| Estimated Time | 3-4 hours |
| Difficulty | Intermediate |
| Module Path | `module/bdev/` |
| Reference | `module/bdev/null/bdev_null.c` |

## Objective

Implement a minimal block device module called `my_null` from scratch. The device discards all writes and returns zeroed buffers on reads — a clean foundation for understanding every mandatory hook in the bdev module interface before layering in real storage logic.

By the end of this exercise you will be able to:

- Register a bdev module with `SPDK_BDEV_MODULE_REGISTER`
- Implement `submit_request`, `io_type_supported`, `get_io_channel`, and `destruct`
- Create and delete device instances through JSON-RPC
- Build your module as a shared library alongside SPDK
- Measure I/O throughput against your module with `bdevperf`
- Add per-channel I/O counters and configurable latency injection (bonus)

## Prerequisites

- Completed EX01 (SPDK build), EX02 (reactor/thread model), EX03 (I/O channel pattern)
- Familiarity with SPDK's polled I/O model and `spdk_poller`
- C99, `TAILQ`, and basic pointer arithmetic

## Background: The bdev Module Interface

Every bdev module exposes two structures to the SPDK bdev layer.

**`struct spdk_bdev_module`** — registered once per module, describes lifecycle hooks:

```c
struct spdk_bdev_module {
    const char *name;
    int  (*module_init)(void);       /* called at subsystem startup        */
    void (*module_fini)(void);       /* called after all bdevs unregistered */
    int  (*get_ctx_size)(void);      /* per-I/O private storage size        */
    int  (*config_json)(struct spdk_json_write_ctx *w); /* config snapshot */
    bool async_fini;                 /* set true if module_fini is async    */
    /* ... additional optional fields ... */
};
```

**`struct spdk_bdev_fn_table`** — registered per device instance, describes per-I/O dispatch:

```c
struct spdk_bdev_fn_table {
    void (*submit_request)(struct spdk_io_channel *ch,
                           struct spdk_bdev_io *bdev_io);
    bool (*io_type_supported)(void *ctx,
                              enum spdk_bdev_io_type io_type);
    struct spdk_io_channel *(*get_io_channel)(void *ctx);
    int  (*destruct)(void *ctx);
    void (*write_config_json)(struct spdk_bdev *bdev,
                              struct spdk_json_write_ctx *w);
};
```

The bdev layer calls `submit_request` on the thread that owns the I/O channel. Your handler must either complete the I/O synchronously via `spdk_bdev_io_complete()` or queue it for deferred completion via a poller.

---

## Step 1 — Directory Layout

Create your module directory alongside the existing null bdev:

```
module/bdev/my_null/
    my_null_bdev.h        # internal types shared between .c files
    my_null_bdev.c        # module registration, I/O dispatch, channel mgmt
    my_null_bdev_rpc.c    # JSON-RPC handlers (create / delete)
    Makefile
```

```bash
mkdir -p /path/to/spdk/module/bdev/my_null
```

---

## Step 2 — Internal Header (`my_null_bdev.h`)

```c
/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"

/* Options passed to bdev_my_null_create() from the RPC handler. */
struct my_null_opts {
    char     *name;
    uint64_t  num_blocks;
    uint32_t  block_size;
};

/*
 * Create a new my_null bdev.
 *
 * On success *bdev_out is set to the registered bdev and 0 is returned.
 * The caller owns nothing; the bdev layer owns the lifetime.
 */
int bdev_my_null_create(struct spdk_bdev **bdev_out,
                        const struct my_null_opts *opts);

/* Asynchronous delete — cb is called on the calling thread once done. */
void bdev_my_null_delete(const char *name,
                         spdk_delete_callback cb, void *cb_arg);
```

---

## Step 3 — Core Implementation (`my_null_bdev.c`)

### 3.1 Includes and forward declarations

```c
/* SPDX-License-Identifier: BSD-3-Clause */
#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/bdev_module.h"

#include "my_null_bdev.h"

/* Forward declarations required by the module struct. */
static int  bdev_my_null_initialize(void);
static void bdev_my_null_finish(void);
```

### 3.2 Per-I/O context

The bdev layer allocates a fixed-size block of private storage per in-flight I/O. Your module declares how large that block must be via `get_ctx_size`. Use it to store whatever you need for deferred completion (a list link in this case).

```c
/*
 * Per-I/O private storage allocated by the bdev layer.
 * We embed a TAILQ link so the channel poller can walk pending I/Os.
 */
struct my_null_bdev_io {
    TAILQ_ENTRY(my_null_bdev_io) link;
};

static int
bdev_my_null_get_ctx_size(void)
{
    return sizeof(struct my_null_bdev_io);
}
```

### 3.3 Per-device struct

Embed `struct spdk_bdev` as the FIRST member. The bdev layer casts `ctx` (a `void *`) back to your struct, so the base address must be identical.

```c
struct my_null_bdev {
    struct spdk_bdev         bdev;   /* MUST be first */
    TAILQ_ENTRY(my_null_bdev) tailq;
};

static TAILQ_HEAD(, my_null_bdev) g_my_null_bdev_head =
    TAILQ_HEAD_INITIALIZER(g_my_null_bdev_head);
```

### 3.4 Per-channel struct and poller

Each I/O channel gets its own private state. The poller drains the pending queue once per reactor loop iteration (zero-latency completion — see the Bonus section to add real delay).

```c
struct my_null_io_channel {
    struct spdk_poller               *poller;
    TAILQ_HEAD(, my_null_bdev_io)     pending_io;
};

static int
bdev_my_null_channel_poll(void *arg)
{
    struct my_null_io_channel *ch = arg;
    struct my_null_bdev_io    *io;
    TAILQ_HEAD(, my_null_bdev_io) completed =
        TAILQ_HEAD_INITIALIZER(completed);

    /* Splice the whole queue atomically to avoid iterator invalidation. */
    TAILQ_SWAP(&completed, &ch->pending_io, my_null_bdev_io, link);

    if (TAILQ_EMPTY(&completed)) {
        return SPDK_POLLER_IDLE;
    }

    while ((io = TAILQ_FIRST(&completed)) != NULL) {
        TAILQ_REMOVE(&completed, io, link);
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io),
                              SPDK_BDEV_IO_STATUS_SUCCESS);
    }

    return SPDK_POLLER_BUSY;
}

static int
bdev_my_null_create_channel_cb(void *io_device, void *ctx_buf)
{
    struct my_null_io_channel *ch = ctx_buf;

    TAILQ_INIT(&ch->pending_io);
    ch->poller = SPDK_POLLER_REGISTER(bdev_my_null_channel_poll, ch, 0);
    if (!ch->poller) {
        SPDK_ERRLOG("Failed to register channel poller\n");
        return -ENOMEM;
    }

    return 0;
}

static void
bdev_my_null_destroy_channel_cb(void *io_device, void *ctx_buf)
{
    struct my_null_io_channel *ch = ctx_buf;

    spdk_poller_unregister(&ch->poller);
    /* Drain any remaining I/Os with failure so callers are not stuck. */
    struct my_null_bdev_io *io;
    while ((io = TAILQ_FIRST(&ch->pending_io)) != NULL) {
        TAILQ_REMOVE(&ch->pending_io, io, link);
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io),
                              SPDK_BDEV_IO_STATUS_FAILED);
    }
}
```

### 3.5 Module-level lifecycle

`module_init` registers the io_device that backs channel creation. `module_fini` undoes it.

```c
static int
bdev_my_null_initialize(void)
{
    /*
     * Register the io_device keyed on the global list head.
     * Any unique, stable address works — the list head is convenient.
     */
    spdk_io_device_register(&g_my_null_bdev_head,
                            bdev_my_null_create_channel_cb,
                            bdev_my_null_destroy_channel_cb,
                            sizeof(struct my_null_io_channel),
                            "my_null");
    return 0;
}

static void
bdev_my_null_finish(void)
{
    spdk_io_device_unregister(&g_my_null_bdev_head, NULL);
    spdk_bdev_module_fini_done();
}
```

### 3.6 Module registration

`SPDK_BDEV_MODULE_REGISTER` inserts the module struct into a linker set that the bdev subsystem iterates at startup — no explicit call needed from your application config.

```c
static struct spdk_bdev_module my_null_if = {
    .name        = "my_null",
    .module_init = bdev_my_null_initialize,
    .module_fini = bdev_my_null_finish,
    .async_fini  = true,     /* module_fini calls spdk_bdev_module_fini_done() */
    .get_ctx_size = bdev_my_null_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(my_null, &my_null_if)
```

### 3.7 I/O dispatch (`submit_request`)

`submit_request` is called on the reactor thread that owns the channel. For our null device all I/O types that we support simply get queued; the poller completes them on the next poll iteration. Unsupported types complete with failure immediately.

```c
static void
bdev_my_null_submit_request(struct spdk_io_channel *_ch,
                            struct spdk_bdev_io    *bdev_io)
{
    struct my_null_bdev_io    *io  = (struct my_null_bdev_io *)bdev_io->driver_ctx;
    struct my_null_io_channel *ch  = spdk_io_channel_get_ctx(_ch);

    switch (bdev_io->type) {
    case SPDK_BDEV_IO_TYPE_READ:
        /*
         * If the caller supplied a NULL iov_base the bdev layer expects
         * us to point it at a zeroed buffer.  We use the iov already
         * allocated by the bdev layer and zero-fill it here.
         */
        if (bdev_io->u.bdev.iovs[0].iov_base != NULL) {
            uint64_t len = bdev_io->u.bdev.num_blocks *
                           bdev_io->bdev->blocklen;
            memset(bdev_io->u.bdev.iovs[0].iov_base, 0, len);
        }
        /* fall through — queue for deferred completion */
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
    case SPDK_BDEV_IO_TYPE_RESET:
        TAILQ_INSERT_TAIL(&ch->pending_io, io, link);
        break;

    default:
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        break;
    }
}
```

### 3.8 Capability advertisement (`io_type_supported`)

Return `true` only for I/O types your `submit_request` handles successfully. The bdev layer rejects requests for unsupported types before they reach your handler.

```c
static bool
bdev_my_null_io_type_supported(void *ctx,
                               enum spdk_bdev_io_type io_type)
{
    switch (io_type) {
    case SPDK_BDEV_IO_TYPE_READ:
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
    case SPDK_BDEV_IO_TYPE_RESET:
        return true;
    default:
        return false;
    }
}
```

### 3.9 Channel acquisition

All bdevs that share the same io_device get a single channel per thread. Return a channel keyed on the global list head registered in `module_init`.

```c
static struct spdk_io_channel *
bdev_my_null_get_io_channel(void *ctx)
{
    return spdk_get_io_channel(&g_my_null_bdev_head);
}
```

### 3.10 Device destruction

`destruct` is called after the bdev layer has ensured no more I/O will be submitted. Remove from the global list and free. Return 0 for synchronous teardown.

```c
static int
bdev_my_null_destruct(void *ctx)
{
    struct my_null_bdev *bdev = ctx;

    TAILQ_REMOVE(&g_my_null_bdev_head, bdev, tailq);
    free(bdev->bdev.name);
    free(bdev);

    return 0;
}
```

### 3.11 Config JSON serialization

This callback is invoked when SPDK saves its running configuration. Emit the exact RPC call and parameters needed to recreate the device.

```c
static void
bdev_my_null_write_config_json(struct spdk_bdev        *bdev,
                               struct spdk_json_write_ctx *w)
{
    spdk_json_write_object_begin(w);
    spdk_json_write_named_string(w, "method", "bdev_my_null_create");
    spdk_json_write_named_object_begin(w, "params");
    spdk_json_write_named_string(w,  "name",       bdev->name);
    spdk_json_write_named_uint64(w,  "num_blocks",  bdev->blockcnt);
    spdk_json_write_named_uint32(w,  "block_size",  bdev->blocklen);
    spdk_json_write_object_end(w);
    spdk_json_write_object_end(w);
}
```

### 3.12 Function table and `bdev_my_null_create`

Wire everything together:

```c
static const struct spdk_bdev_fn_table my_null_fn_table = {
    .destruct          = bdev_my_null_destruct,
    .submit_request    = bdev_my_null_submit_request,
    .io_type_supported = bdev_my_null_io_type_supported,
    .get_io_channel    = bdev_my_null_get_io_channel,
    .write_config_json = bdev_my_null_write_config_json,
};

int
bdev_my_null_create(struct spdk_bdev **bdev_out,
                    const struct my_null_opts *opts)
{
    struct my_null_bdev *disk;
    int rc;

    if (!opts || !opts->name || opts->num_blocks == 0 ||
            opts->block_size == 0 || opts->block_size % 512 != 0) {
        SPDK_ERRLOG("Invalid options for my_null bdev\n");
        return -EINVAL;
    }

    disk = calloc(1, sizeof(*disk));
    if (!disk) {
        return -ENOMEM;
    }

    disk->bdev.name = strdup(opts->name);
    if (!disk->bdev.name) {
        free(disk);
        return -ENOMEM;
    }

    disk->bdev.product_name = "My Null Disk";
    disk->bdev.blocklen      = opts->block_size;
    disk->bdev.blockcnt      = opts->num_blocks;
    disk->bdev.ctxt          = disk;
    disk->bdev.fn_table      = &my_null_fn_table;
    disk->bdev.module        = &my_null_if;

    rc = spdk_bdev_register(&disk->bdev);
    if (rc != 0) {
        SPDK_ERRLOG("Failed to register my_null bdev '%s': %d\n",
                    opts->name, rc);
        free(disk->bdev.name);
        free(disk);
        return rc;
    }

    TAILQ_INSERT_TAIL(&g_my_null_bdev_head, disk, tailq);
    *bdev_out = &disk->bdev;
    return 0;
}
```

### 3.13 `bdev_my_null_delete`

```c
void
bdev_my_null_delete(const char *name,
                    spdk_delete_callback cb, void *cb_arg)
{
    int rc = spdk_bdev_unregister_by_name(name, &my_null_if, cb, cb_arg);
    if (rc != 0) {
        cb(cb_arg, rc);
    }
}
```

---

## Step 4 — JSON-RPC Handlers (`my_null_bdev_rpc.c`)

```c
/* SPDX-License-Identifier: BSD-3-Clause */
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "my_null_bdev.h"

/* ---- bdev_my_null_create ---- */

static const struct spdk_json_object_decoder rpc_create_decoders[] = {
    {"name",       offsetof(struct my_null_opts, name),       spdk_json_decode_string},
    {"num_blocks", offsetof(struct my_null_opts, num_blocks), spdk_json_decode_uint64},
    {"block_size", offsetof(struct my_null_opts, block_size), spdk_json_decode_uint32},
};

static void
rpc_bdev_my_null_create(struct spdk_jsonrpc_request *request,
                        const struct spdk_json_val  *params)
{
    struct my_null_opts        req = {};
    struct spdk_bdev          *bdev;
    struct spdk_json_write_ctx *w;
    int rc;

    if (spdk_json_decode_object(params, rpc_create_decoders,
                                SPDK_COUNTOF(rpc_create_decoders), &req)) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "Invalid parameters");
        goto cleanup;
    }

    rc = bdev_my_null_create(&bdev, &req);
    if (rc != 0) {
        spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
        goto cleanup;
    }

    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_string(w, bdev->name);
    spdk_jsonrpc_end_result(request, w);

cleanup:
    free(req.name);
}
SPDK_RPC_REGISTER("bdev_my_null_create", rpc_bdev_my_null_create,
                  SPDK_RPC_RUNTIME)

/* ---- bdev_my_null_delete ---- */

struct rpc_delete_req {
    char *name;
};

static const struct spdk_json_object_decoder rpc_delete_decoders[] = {
    {"name", offsetof(struct rpc_delete_req, name), spdk_json_decode_string},
};

static void
rpc_bdev_my_null_delete_cb(void *cb_arg, int bdeverrno)
{
    struct spdk_jsonrpc_request *request = cb_arg;

    if (bdeverrno == 0) {
        struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
        spdk_json_write_bool(w, true);
        spdk_jsonrpc_end_result(request, w);
    } else {
        spdk_jsonrpc_send_error_response(request, bdeverrno,
                                         spdk_strerror(-bdeverrno));
    }
}

static void
rpc_bdev_my_null_delete(struct spdk_jsonrpc_request *request,
                        const struct spdk_json_val  *params)
{
    struct rpc_delete_req req = {};

    if (spdk_json_decode_object(params, rpc_delete_decoders,
                                SPDK_COUNTOF(rpc_delete_decoders), &req)) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "Invalid parameters");
        return;
    }

    bdev_my_null_delete(req.name, rpc_bdev_my_null_delete_cb, request);
    free(req.name);
}
SPDK_RPC_REGISTER("bdev_my_null_delete", rpc_bdev_my_null_delete,
                  SPDK_RPC_RUNTIME)
```

---

## Step 5 — Makefile

Mirror the existing null bdev Makefile exactly, replacing the library name:

```makefile
#  SPDX-License-Identifier: BSD-3-Clause

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER   := 1
SO_MINOR := 0

C_SRCS  = my_null_bdev.c my_null_bdev_rpc.c
LIBNAME = bdev_my_null

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
```

---

## Step 6 — Hook into the Build System

SPDK's top-level `CONFIG` and `mk/` files control which bdev modules are compiled. The simplest approach is to add your library to the existing bdev module list.

Edit `mk/spdk.lib_deps.mk` — find the line that lists bdev modules and add `bdev_my_null`:

```makefile
# Before:
DEPDIRS-bdev_null = ...

# After (add your entry in alphabetical order):
DEPDIRS-bdev_my_null = log bdev
```

Then add the subdirectory to `module/bdev/Makefile`:

```makefile
# In the DIRS list, add:
my_null \
```

Rebuild:

```bash
cd /path/to/spdk
make -j$(nproc)
```

A successful build produces `build/lib/libspdk_bdev_my_null.so`.

---

## Step 7 — Runtime Test with bdevperf

### 7.1 Create an SPDK configuration JSON

Save as `my_null_test.json`:

```json
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_my_null_create",
          "params": {
            "name": "MyNull0",
            "num_blocks": 131072,
            "block_size": 4096
          }
        }
      ]
    }
  ]
}
```

This creates a 512 MiB device (131072 blocks * 4096 bytes).

### 7.2 Run bdevperf

```bash
sudo ./build/examples/bdevperf \
    --json my_null_test.json \
    -q 128 \
    -o 4096 \
    -t 10 \
    -w randread \
    -m 0x1
```

| Flag | Meaning |
|------|---------|
| `-q 128` | Queue depth per thread |
| `-o 4096` | I/O size in bytes |
| `-t 10` | Duration in seconds |
| `-w randread` | Workload: `randread`, `randwrite`, `randrw` |
| `-m 0x1` | CPU mask — one core |

Expected output (numbers will vary by hardware):

```
MyNull0 : 0.10 seconds (  0.10 total)    Read= 1250.00 MiB/s (  320000 IOPS)
```

A null bdev is CPU-bound. You should see millions of IOPS on modern hardware.

### 7.3 Verify via RPC at runtime

Start the SPDK application with `--rpc-socket /tmp/spdk.sock`, then in another terminal:

```bash
# List all registered bdevs
./scripts/rpc.py bdev_get_bdevs

# Create a second device at runtime
./scripts/rpc.py bdev_my_null_create --name MyNull1 --num-blocks 65536 --block-size 512

# Delete it
./scripts/rpc.py bdev_my_null_delete --name MyNull1
```

---

## Step 8 — Verification Checklist

Work through each item before moving to the bonus section:

- [ ] `make` completes without warnings on modified files
- [ ] `bdev_get_bdevs` lists `MyNull0` with correct `block_size` and `num_blocks`
- [ ] bdevperf completes a 10-second randread run without errors
- [ ] bdevperf completes a 10-second randwrite run without errors
- [ ] Creating a second device via RPC succeeds
- [ ] Deleting a device via RPC succeeds and it disappears from `bdev_get_bdevs`
- [ ] SPDK exits cleanly (no use-after-free from Valgrind or ASAN)

---

## Bonus A — Per-Channel I/O Statistics

Extend `struct my_null_io_channel` with counters and expose them via a new RPC.

### A.1 Extend the channel struct

```c
struct my_null_io_channel {
    struct spdk_poller               *poller;
    TAILQ_HEAD(, my_null_bdev_io)     pending_io;

    /* Statistics — updated on the owning reactor, no locking needed. */
    uint64_t  reads_completed;
    uint64_t  writes_completed;
    uint64_t  read_bytes;
    uint64_t  write_bytes;
};
```

### A.2 Update the poller to classify I/Os before completion

```c
static int
bdev_my_null_channel_poll(void *arg)
{
    struct my_null_io_channel *ch = arg;
    struct my_null_bdev_io    *io;
    TAILQ_HEAD(, my_null_bdev_io) done = TAILQ_HEAD_INITIALIZER(done);

    TAILQ_SWAP(&done, &ch->pending_io, my_null_bdev_io, link);
    if (TAILQ_EMPTY(&done)) {
        return SPDK_POLLER_IDLE;
    }

    while ((io = TAILQ_FIRST(&done)) != NULL) {
        struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(io);
        uint64_t bytes = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

        if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
            ch->reads_completed++;
            ch->read_bytes += bytes;
        } else {
            ch->writes_completed++;
            ch->write_bytes += bytes;
        }

        TAILQ_REMOVE(&done, io, link);
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
    }

    return SPDK_POLLER_BUSY;
}
```

### A.3 Aggregate across channels with `spdk_for_each_channel`

Statistics live per-channel (per-thread). To get a total you must ask each reactor to read its own channel. SPDK provides `spdk_for_each_channel` for exactly this pattern.

```c
struct my_null_stats_ctx {
    struct spdk_jsonrpc_request *request;
    uint64_t reads_completed;
    uint64_t writes_completed;
    uint64_t read_bytes;
    uint64_t write_bytes;
};

static void
_gather_stats(struct spdk_io_channel_iter *i)
{
    struct my_null_stats_ctx  *ctx = spdk_io_channel_iter_get_ctx(i);
    struct spdk_io_channel    *ch  = spdk_io_channel_iter_get_channel(i);
    struct my_null_io_channel *nch = spdk_io_channel_get_ctx(ch);

    ctx->reads_completed  += nch->reads_completed;
    ctx->writes_completed += nch->writes_completed;
    ctx->read_bytes        += nch->read_bytes;
    ctx->write_bytes       += nch->write_bytes;

    spdk_for_each_channel_continue(i, 0);
}

static void
_stats_done(struct spdk_io_channel_iter *i, int status)
{
    struct my_null_stats_ctx  *ctx = spdk_io_channel_iter_get_ctx(i);
    struct spdk_json_write_ctx *w   = spdk_jsonrpc_begin_result(ctx->request);

    spdk_json_write_object_begin(w);
    spdk_json_write_named_uint64(w, "reads_completed",  ctx->reads_completed);
    spdk_json_write_named_uint64(w, "writes_completed", ctx->writes_completed);
    spdk_json_write_named_uint64(w, "read_bytes",       ctx->read_bytes);
    spdk_json_write_named_uint64(w, "write_bytes",      ctx->write_bytes);
    spdk_json_write_object_end(w);
    spdk_jsonrpc_end_result(ctx->request, w);

    free(ctx);
}

static void
rpc_bdev_my_null_get_stats(struct spdk_jsonrpc_request *request,
                           const struct spdk_json_val  *params)
{
    struct my_null_stats_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        spdk_jsonrpc_send_error_response(request, -ENOMEM,
                                         spdk_strerror(ENOMEM));
        return;
    }

    ctx->request = request;
    spdk_for_each_channel(&g_my_null_bdev_head,
                          _gather_stats,
                          ctx,
                          _stats_done);
}
SPDK_RPC_REGISTER("bdev_my_null_get_stats", rpc_bdev_my_null_get_stats,
                  SPDK_RPC_RUNTIME)
```

Test it:

```bash
./scripts/rpc.py bdev_my_null_get_stats
# Output:
# {
#   "reads_completed": 3200000,
#   "writes_completed": 0,
#   "read_bytes": 13107200000,
#   "write_bytes": 0
# }
```

---

## Bonus B — Configurable Latency Injection

A zero-latency null bdev is useful for throughput benchmarks, but sometimes you want to simulate a real device. Add a per-device target latency that the channel poller enforces using a timer.

### B.1 Add latency to the device struct and opts

```c
/* In my_null_bdev.h */
struct my_null_opts {
    char     *name;
    uint64_t  num_blocks;
    uint32_t  block_size;
    uint64_t  latency_us;   /* 0 means complete immediately */
};

/* In my_null_bdev.c */
struct my_null_bdev {
    struct spdk_bdev         bdev;
    TAILQ_ENTRY(my_null_bdev) tailq;
    uint64_t                 latency_us;
};
```

### B.2 Tag each I/O with a deadline

```c
struct my_null_bdev_io {
    TAILQ_ENTRY(my_null_bdev_io) link;
    uint64_t complete_tsc;   /* TSC value at or after which I/O may complete */
};
```

### B.3 Set the deadline in `submit_request`

```c
static void
bdev_my_null_submit_request(struct spdk_io_channel *_ch,
                            struct spdk_bdev_io    *bdev_io)
{
    struct my_null_bdev_io *io   = (struct my_null_bdev_io *)bdev_io->driver_ctx;
    struct my_null_io_channel *ch = spdk_io_channel_get_ctx(_ch);
    struct my_null_bdev   *disk  = (struct my_null_bdev *)bdev_io->bdev->ctxt;

    switch (bdev_io->type) {
    case SPDK_BDEV_IO_TYPE_READ:
        if (bdev_io->u.bdev.iovs[0].iov_base != NULL) {
            memset(bdev_io->u.bdev.iovs[0].iov_base, 0,
                   bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
        }
        /* fall through */
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
    case SPDK_BDEV_IO_TYPE_RESET:
        if (disk->latency_us == 0) {
            io->complete_tsc = 0;
        } else {
            io->complete_tsc = spdk_get_ticks() +
                (spdk_get_ticks_hz() * disk->latency_us) / SPDK_SEC_TO_USEC;
        }
        TAILQ_INSERT_TAIL(&ch->pending_io, io, link);
        break;
    default:
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        break;
    }
}
```

### B.4 Gate completion on the deadline in the poller

```c
static int
bdev_my_null_channel_poll(void *arg)
{
    struct my_null_io_channel *ch  = arg;
    struct my_null_bdev_io    *io;
    uint64_t now = spdk_get_ticks();
    int completed = 0;

    /* Walk in FIFO order; stop at the first I/O not yet due. */
    while ((io = TAILQ_FIRST(&ch->pending_io)) != NULL) {
        if (io->complete_tsc != 0 && now < io->complete_tsc) {
            break;
        }
        TAILQ_REMOVE(&ch->pending_io, io, link);
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io),
                              SPDK_BDEV_IO_STATUS_SUCCESS);
        completed++;
    }

    return completed > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}
```

Test with 100 µs simulated latency:

```json
{
  "method": "bdev_my_null_create",
  "params": {
    "name": "SlowNull0",
    "num_blocks": 131072,
    "block_size": 4096,
    "latency_us": 100
  }
}
```

At 100 µs latency and queue depth 128, the theoretical maximum IOPS is `128 / 0.0001 = 1,280,000`. bdevperf should report close to this figure, confirming the latency injection is working.

---

## Common Mistakes

### 1. Not embedding `struct spdk_bdev` as the first member

The bdev layer does `(struct my_null_bdev *)bdev->ctxt`. If `spdk_bdev` is not at offset zero, the cast produces a garbage pointer.

```c
/* WRONG */
struct my_null_bdev {
    int                      some_field;
    struct spdk_bdev         bdev;   /* not at offset 0 */
};

/* CORRECT */
struct my_null_bdev {
    struct spdk_bdev         bdev;   /* must be first */
    int                      some_field;
};
```

### 2. Forgetting `spdk_bdev_module_fini_done()` when `async_fini = true`

If you set `async_fini = true` in your module struct but never call `spdk_bdev_module_fini_done()`, SPDK hangs on shutdown waiting for your module to finish.

```c
static void
bdev_my_null_finish(void)
{
    spdk_io_device_unregister(&g_my_null_bdev_head, NULL);
    spdk_bdev_module_fini_done();   /* MUST call this */
}
```

### 3. Completing I/O on the wrong thread

`spdk_bdev_io_complete()` must be called on the same thread that called `submit_request`. If you defer completion to another reactor (e.g., via a message), use `spdk_thread_send_msg` to hop back to the correct thread before calling `spdk_bdev_io_complete`.

### 4. Leaking memory on registration failure

`bdev_my_null_create` allocates the device before calling `spdk_bdev_register`. If registration fails, you must free both the name and the device struct before returning the error.

```c
rc = spdk_bdev_register(&disk->bdev);
if (rc != 0) {
    free(disk->bdev.name);  /* don't forget the strdup'd name */
    free(disk);
    return rc;
}
```

### 5. Returning `false` from `io_type_supported` but handling the type anyway

The bdev layer blocks I/O types you advertise as unsupported. If you later handle a type in `submit_request` that you returned `false` for, those code paths can never be reached. Keep the two functions in sync.

### 6. Registering the io_device with the wrong context size

`spdk_io_device_register` takes the channel context size as its fourth argument. If this does not match `sizeof(struct my_null_io_channel)`, your channel poller will read and write out-of-bounds memory.

```c
spdk_io_device_register(&g_my_null_bdev_head,
                        bdev_my_null_create_channel_cb,
                        bdev_my_null_destroy_channel_cb,
                        sizeof(struct my_null_io_channel), /* exact size */
                        "my_null");
```

---

## Summary

You have built a complete bdev module:

- **Module struct** with `SPDK_BDEV_MODULE_REGISTER` and lifecycle hooks
- **Per-device struct** wrapping `struct spdk_bdev`
- **Per-channel struct** with a polled completion queue
- **submit_request** dispatching all supported I/O types
- **JSON-RPC handlers** for create and delete
- **Makefile** that integrates with SPDK's build system
- **bdevperf** validation showing multi-million IOPS throughput
- **Bonus**: per-channel statistics aggregated with `spdk_for_each_channel`
- **Bonus**: TSC-based latency injection for device simulation

The same skeleton applies to any storage backend — replace the `submit_request` body with real DMA, NVMe command submission, or network I/O, and the rest of the infrastructure remains identical.

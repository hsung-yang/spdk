# Exercise 08: Error Injection

## Overview

| Field | Details |
|-------|---------|
| **Objective** | Extend the null bdev module with configurable error injection to simulate hardware faults, measure error propagation to the application layer, and understand how SPDK bdev completions signal failure. |
| **Prerequisites** | Exercise 04 (Custom Bdev Module), familiarity with `spdk_bdev_io_complete`, `SPDK_BDEV_IO_STATUS_*` constants, and the SPDK RPC system. |
| **Estimated Time** | 2-3 hours |
| **Difficulty** | Intermediate |

---

## Background

Real storage devices fail in a variety of ways: a random percentage of I/Os return errors, specific operation types (read vs. write) are more error-prone, and latent failures can be modeled as added latency. SPDK's bdev layer exposes `spdk_bdev_io_complete(bdev_io, status)` as the single completion path, making it straightforward to inject any failure status from inside a bdev driver.

The existing null bdev (`module/bdev/null/bdev_null.c`) is the ideal base: it accepts all I/O, queues it in a per-channel list, and drains the list from a poller, calling `spdk_bdev_io_complete(..., SPDK_BDEV_IO_STATUS_SUCCESS)` for every I/O. You will add a second completion path that fires `SPDK_BDEV_IO_STATUS_FAILED` based on configurable rules.

Key completion status values used in this exercise:

```c
SPDK_BDEV_IO_STATUS_SUCCESS   /*  0 - normal completion      */
SPDK_BDEV_IO_STATUS_FAILED    /* -1 - generic failure        */
SPDK_BDEV_IO_STATUS_ABORTED   /* -2 - I/O was aborted        */
SPDK_BDEV_IO_STATUS_NOMEM     /* -4 - out of memory          */
```

---

## Task Description

Extend `bdev_null` with three independently controllable injection modes, all governed by a per-bdev configuration structure:

1. **Error Rate Injection** - fail a configurable percentage (0-100) of all I/Os.
2. **I/O-Type Filter** - restrict injection to reads, writes, or both.
3. **Latency Injection** - defer completion of targeted I/Os by a configurable number of microseconds.

All three modes must be controllable at runtime through a new JSON-RPC method (`bdev_null_set_error_injection`) without restarting SPDK.

---

## Step-by-Step Instructions

### Step 1 - Understand the existing null bdev structure

Before writing a single line, read the key structs and the completion poller:

```c
/* module/bdev/null/bdev_null.c - structures you will modify */

struct null_bdev_io {
    TAILQ_ENTRY(null_bdev_io) link;
    /* You will add: uint64_t delay_tsc; */
};

struct null_bdev {
    struct spdk_bdev    bdev;
    TAILQ_ENTRY(null_bdev) tailq;
    /* You will add error injection config here */
};

struct null_io_channel {
    struct spdk_poller              *poller;
    TAILQ_HEAD(, null_bdev_io)       io;          /* pending I/Os */
    /* You will add a delayed I/O list here     */
};
```

The poller callback iterates `ch->io`, calls `spdk_bdev_io_complete(..., SPDK_BDEV_IO_STATUS_SUCCESS)` for each entry, and empties the list. Your injection logic intercepts completions in this poller.

### Step 2 - Add the error injection configuration structure

Open `module/bdev/null/bdev_null.c`. Add the following struct and extend `null_bdev` immediately after the existing struct definitions (around line 27):

```c
/* Bitmask of I/O types to inject errors into */
#define NULL_INJECT_IO_READ        (1u << SPDK_BDEV_IO_TYPE_READ)
#define NULL_INJECT_IO_WRITE       (1u << SPDK_BDEV_IO_TYPE_WRITE)
#define NULL_INJECT_IO_ALL         (NULL_INJECT_IO_READ | NULL_INJECT_IO_WRITE)

struct null_error_inject_config {
    bool     enabled;          /* master switch                          */
    uint32_t error_rate_pct;   /* 0-100: percentage of I/Os to fail      */
    uint32_t io_type_mask;     /* bitmask: which I/O types to affect     */
    uint64_t latency_us;       /* additional latency in microseconds (0 = none) */
    uint64_t io_counter;       /* rolling counter for deterministic rate */
};
```

Extend `null_bdev` to carry this config:

```c
struct null_bdev {
    struct spdk_bdev                  bdev;
    TAILQ_ENTRY(null_bdev)            tailq;
    struct null_error_inject_config   inject;    /* <-- new field */
};
```

Extend `null_bdev_io` to carry the absolute TSC deadline for delayed completions:

```c
struct null_bdev_io {
    TAILQ_ENTRY(null_bdev_io) link;
    uint64_t                  complete_tsc;  /* 0 = complete immediately */
};
```

Extend `null_io_channel` with a separate list for delayed I/Os:

```c
struct null_io_channel {
    struct spdk_poller              *poller;
    TAILQ_HEAD(, null_bdev_io)       io;          /* ready to complete     */
    TAILQ_HEAD(, null_bdev_io)       delayed_io;  /* waiting for deadline  */
};
```

Initialize the new list in `bdev_null_create_cb` (the io_channel create callback). Search for `TAILQ_INIT(&ch->io)` and add:

```c
TAILQ_INIT(&ch->delayed_io);
```

### Step 3 - Implement the injection decision helper

Add a static helper that decides whether a given I/O should be failed, placed after the struct definitions and before `bdev_null_submit_request`:

```c
/*
 * Returns true if this I/O should be injected with an error.
 * Uses a simple modular counter so the error rate is deterministic
 * rather than relying on rand(), which keeps tests reproducible.
 */
static bool
null_should_inject_error(struct null_bdev *nbdev, enum spdk_bdev_io_type io_type)
{
    struct null_error_inject_config *cfg = &nbdev->inject;

    if (!cfg->enabled) {
        return false;
    }

    /* Check I/O type filter */
    if (!(cfg->io_type_mask & (1u << io_type))) {
        return false;
    }

    if (cfg->error_rate_pct == 0) {
        return false;
    }

    /* Deterministic: fail every (100 / error_rate_pct)-th I/O */
    cfg->io_counter++;
    return ((cfg->io_counter * cfg->error_rate_pct) / 100) >
           (((cfg->io_counter - 1) * cfg->error_rate_pct) / 100);
}
```

### Step 4 - Modify `bdev_null_submit_request` to inject errors and latency

The submit path enqueues I/Os into `ch->io`. Replace the `TAILQ_INSERT_TAIL(&ch->io, null_io, link)` calls (READ, WRITE, WRITE_ZEROES, RESET cases) with a helper that routes to `ch->delayed_io` when latency is configured, or marks `complete_tsc = 0` for immediate completion after error decision:

```c
static void
null_enqueue_io(struct null_io_channel *ch, struct null_bdev *nbdev,
                struct null_bdev_io *null_io, struct spdk_bdev_io *bdev_io)
{
    struct null_error_inject_config *cfg = &nbdev->inject;

    if (null_should_inject_error(nbdev, bdev_io->type)) {
        /*
         * Error path: if latency injection is also set, delay before
         * signalling the error (simulates a slow failing device).
         * Otherwise complete immediately with FAILED status.
         */
        if (cfg->latency_us > 0) {
            null_io->complete_tsc = spdk_get_ticks() +
                                    cfg->latency_us * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
            /* Re-use the bit in the upper flag; mark as error-delayed */
            null_io->complete_tsc |= (1ULL << 63);  /* error sentinel bit */
            TAILQ_INSERT_TAIL(&ch->delayed_io, null_io, link);
        } else {
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        }
        return;
    }

    /* Success path */
    if (cfg->latency_us > 0 && (cfg->io_type_mask & (1u << bdev_io->type))) {
        null_io->complete_tsc = spdk_get_ticks() +
                                cfg->latency_us * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
        TAILQ_INSERT_TAIL(&ch->delayed_io, null_io, link);
    } else {
        null_io->complete_tsc = 0;
        TAILQ_INSERT_TAIL(&ch->io, null_io, link);
    }
}
```

Update `bdev_null_submit_request` for all queuing cases:

```c
case SPDK_BDEV_IO_TYPE_READ:
    /* ... existing DIF handling ... */
    null_enqueue_io(ch, spdk_io_channel_get_ctx(spdk_bdev_io_get_io_channel(bdev_io)),
                    null_io, bdev_io);
    /* Replace the direct TAILQ_INSERT_TAIL with the call above */
    break;
case SPDK_BDEV_IO_TYPE_WRITE:
    /* ... existing DIF handling ... */
    null_enqueue_io(ch, ..., null_io, bdev_io);
    break;
case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
case SPDK_BDEV_IO_TYPE_RESET:
    null_enqueue_io(ch, ..., null_io, bdev_io);
    break;
```

### Step 5 - Update the poller to drain delayed I/Os

The existing poller drains `ch->io` in a single pass. Extend it to also drain `ch->delayed_io` when deadlines have passed:

```c
static int
bdev_null_poll(void *arg)
{
    struct null_io_channel *ch = arg;
    uint32_t count;
    TAILQ_HEAD(, null_bdev_io) completed;
    struct null_bdev_io *null_io, *tmp;
    uint64_t now = spdk_get_ticks();

    TAILQ_INIT(&completed);
    TAILQ_SWAP(&completed, &ch->io, null_bdev_io, link);

    count = 0;
    TAILQ_FOREACH_SAFE(null_io, &completed, link, tmp) {
        TAILQ_REMOVE(&completed, null_io, link);
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(null_io),
                              SPDK_BDEV_IO_STATUS_SUCCESS);
        count++;
    }

    /* Drain delayed I/Os whose deadlines have passed */
    TAILQ_FOREACH_SAFE(null_io, &ch->delayed_io, link, tmp) {
        uint64_t deadline = null_io->complete_tsc & ~(1ULL << 63);
        bool is_error     = !!(null_io->complete_tsc & (1ULL << 63));

        if (now >= deadline) {
            TAILQ_REMOVE(&ch->delayed_io, null_io, link);
            spdk_bdev_io_complete(spdk_bdev_io_from_ctx(null_io),
                                  is_error ? SPDK_BDEV_IO_STATUS_FAILED
                                           : SPDK_BDEV_IO_STATUS_SUCCESS);
            count++;
        }
    }

    return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}
```

### Step 6 - Add the RPC method `bdev_null_set_error_injection`

Create a new file `module/bdev/null/bdev_null_rpc.c` (or append to the existing RPC file if one exists):

```c
/* module/bdev/null/bdev_null_rpc.c */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "bdev_null.h"   /* exposes bdev_null_set_error_injection() */

struct rpc_bdev_null_set_error_injection {
    char     *name;
    bool      enabled;
    uint32_t  error_rate_pct;
    bool      inject_reads;
    bool      inject_writes;
    uint64_t  latency_us;
};

static const struct spdk_json_object_decoder
rpc_bdev_null_set_error_injection_decoders[] = {
    {"name",            offsetof(struct rpc_bdev_null_set_error_injection, name),
     spdk_json_decode_string},
    {"enabled",         offsetof(struct rpc_bdev_null_set_error_injection, enabled),
     spdk_json_decode_bool, true},
    {"error_rate_pct",  offsetof(struct rpc_bdev_null_set_error_injection, error_rate_pct),
     spdk_json_decode_uint32, true},
    {"inject_reads",    offsetof(struct rpc_bdev_null_set_error_injection, inject_reads),
     spdk_json_decode_bool, true},
    {"inject_writes",   offsetof(struct rpc_bdev_null_set_error_injection, inject_writes),
     spdk_json_decode_bool, true},
    {"latency_us",      offsetof(struct rpc_bdev_null_set_error_injection, latency_us),
     spdk_json_decode_uint64, true},
};

static void
rpc_bdev_null_set_error_injection(struct spdk_jsonrpc_request *request,
                                   const struct spdk_json_val *params)
{
    struct rpc_bdev_null_set_error_injection req = {
        .enabled        = true,
        .error_rate_pct = 10,
        .inject_reads   = true,
        .inject_writes  = true,
        .latency_us     = 0,
    };
    struct null_error_inject_opts opts = {};
    int rc;

    if (spdk_json_decode_object(params,
                                rpc_bdev_null_set_error_injection_decoders,
                                SPDK_COUNTOF(rpc_bdev_null_set_error_injection_decoders),
                                &req)) {
        SPDK_ERRLOG("spdk_json_decode_object failed\n");
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                         "spdk_json_decode_object failed");
        goto cleanup;
    }

    opts.enabled        = req.enabled;
    opts.error_rate_pct = req.error_rate_pct;
    opts.io_type_mask   = 0;
    if (req.inject_reads)  { opts.io_type_mask |= NULL_INJECT_IO_READ;  }
    if (req.inject_writes) { opts.io_type_mask |= NULL_INJECT_IO_WRITE; }
    opts.latency_us     = req.latency_us;

    rc = bdev_null_set_error_injection(req.name, &opts);
    if (rc != 0) {
        spdk_jsonrpc_send_error_response_fmt(request, rc,
                                             "Failed to set error injection: %s",
                                             spdk_strerror(-rc));
        goto cleanup;
    }

    spdk_jsonrpc_send_bool_response(request, true);

cleanup:
    free(req.name);
}

SPDK_RPC_REGISTER("bdev_null_set_error_injection",
                  rpc_bdev_null_set_error_injection,
                  SPDK_RPC_RUNTIME)
```

Add the public API declaration to `module/bdev/null/bdev_null.h`:

```c
struct null_error_inject_opts {
    bool     enabled;
    uint32_t error_rate_pct;   /* 0-100                                   */
    uint32_t io_type_mask;     /* NULL_INJECT_IO_READ | NULL_INJECT_IO_WRITE */
    uint64_t latency_us;       /* 0 = no added latency                    */
};

/* Macros exported for use by RPC layer */
#define NULL_INJECT_IO_READ   (1u << SPDK_BDEV_IO_TYPE_READ)
#define NULL_INJECT_IO_WRITE  (1u << SPDK_BDEV_IO_TYPE_WRITE)

int bdev_null_set_error_injection(const char *bdev_name,
                                   const struct null_error_inject_opts *opts);
```

Implement `bdev_null_set_error_injection` in `bdev_null.c`:

```c
int
bdev_null_set_error_injection(const char *bdev_name,
                               const struct null_error_inject_opts *opts)
{
    struct null_bdev *nbdev;

    if (!opts || opts->error_rate_pct > 100) {
        return -EINVAL;
    }

    TAILQ_FOREACH(nbdev, &g_null_bdev_head, tailq) {
        if (strcmp(nbdev->bdev.name, bdev_name) == 0) {
            nbdev->inject.enabled        = opts->enabled;
            nbdev->inject.error_rate_pct = opts->error_rate_pct;
            nbdev->inject.io_type_mask   = opts->io_type_mask;
            nbdev->inject.latency_us     = opts->latency_us;
            nbdev->inject.io_counter     = 0;  /* reset counter on config change */
            return 0;
        }
    }

    return -ENODEV;
}
```

### Step 7 - Wire up the build system

Edit `module/bdev/null/CMakeLists.txt` (or the equivalent `Makefile`) to include the new RPC source:

```cmake
# CMakeLists.txt (if using CMake-based build)
target_sources(bdev_null PRIVATE
    bdev_null.c
    bdev_null_rpc.c   # <-- add this line
)
```

If building with the traditional SPDK Makefile, locate `module/bdev/null/Makefile` and append `bdev_null_rpc.c` to the `C_SRCS` variable:

```makefile
C_SRCS = bdev_null.c bdev_null_rpc.c
```

---

## Complete Solution Code

Below is the minimal, self-contained diff against `module/bdev/null/bdev_null.c` showing only the additions. Lines prefixed with `+` are new; context lines have no prefix.

```diff
--- a/module/bdev/null/bdev_null.c
+++ b/module/bdev/null/bdev_null.c
+#define NULL_INJECT_IO_READ   (1u << SPDK_BDEV_IO_TYPE_READ)
+#define NULL_INJECT_IO_WRITE  (1u << SPDK_BDEV_IO_TYPE_WRITE)
+#define NULL_INJECT_IO_ALL    (NULL_INJECT_IO_READ | NULL_INJECT_IO_WRITE)
+
+struct null_error_inject_config {
+    bool     enabled;
+    uint32_t error_rate_pct;
+    uint32_t io_type_mask;
+    uint64_t latency_us;
+    uint64_t io_counter;
+};

 struct null_bdev_io {
     TAILQ_ENTRY(null_bdev_io) link;
+    uint64_t complete_tsc;   /* 0 = complete immediately; bit63 = error flag */
 };

 struct null_bdev {
     struct spdk_bdev    bdev;
     TAILQ_ENTRY(null_bdev) tailq;
+    struct null_error_inject_config inject;
 };

 struct null_io_channel {
     struct spdk_poller              *poller;
     TAILQ_HEAD(, null_bdev_io)       io;
+    TAILQ_HEAD(, null_bdev_io)       delayed_io;
 };

+static bool
+null_should_inject_error(struct null_bdev *nbdev, enum spdk_bdev_io_type io_type)
+{
+    struct null_error_inject_config *cfg = &nbdev->inject;
+    if (!cfg->enabled || cfg->error_rate_pct == 0) { return false; }
+    if (!(cfg->io_type_mask & (1u << io_type)))     { return false; }
+    cfg->io_counter++;
+    return ((cfg->io_counter * cfg->error_rate_pct) / 100) >
+           (((cfg->io_counter - 1) * cfg->error_rate_pct) / 100);
+}
+
+static void
+null_enqueue_io(struct null_io_channel *ch, struct null_bdev *nbdev,
+                struct null_bdev_io *null_io, struct spdk_bdev_io *bdev_io)
+{
+    struct null_error_inject_config *cfg = &nbdev->inject;
+    bool inject_err = null_should_inject_error(nbdev, bdev_io->type);
+
+    if (inject_err && cfg->latency_us == 0) {
+        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
+        return;
+    }
+
+    if (cfg->latency_us > 0) {
+        uint64_t deadline = spdk_get_ticks() +
+                            cfg->latency_us * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
+        null_io->complete_tsc = inject_err ? (deadline | (1ULL << 63)) : deadline;
+        TAILQ_INSERT_TAIL(&ch->delayed_io, null_io, link);
+    } else {
+        null_io->complete_tsc = 0;
+        TAILQ_INSERT_TAIL(&ch->io, null_io, link);
+    }
+}

 /* In bdev_null_poll - extend existing drain loop: */
+    TAILQ_FOREACH_SAFE(null_io, &ch->delayed_io, link, tmp) {
+        uint64_t deadline = null_io->complete_tsc & ~(1ULL << 63);
+        bool     is_error = !!(null_io->complete_tsc & (1ULL << 63));
+        if (spdk_get_ticks() >= deadline) {
+            TAILQ_REMOVE(&ch->delayed_io, null_io, link);
+            spdk_bdev_io_complete(spdk_bdev_io_from_ctx(null_io),
+                                  is_error ? SPDK_BDEV_IO_STATUS_FAILED
+                                           : SPDK_BDEV_IO_STATUS_SUCCESS);
+            count++;
+        }
+    }

+int
+bdev_null_set_error_injection(const char *bdev_name,
+                               const struct null_error_inject_opts *opts)
+{
+    struct null_bdev *nbdev;
+    if (!opts || opts->error_rate_pct > 100) { return -EINVAL; }
+    TAILQ_FOREACH(nbdev, &g_null_bdev_head, tailq) {
+        if (strcmp(nbdev->bdev.name, bdev_name) == 0) {
+            nbdev->inject.enabled        = opts->enabled;
+            nbdev->inject.error_rate_pct = opts->error_rate_pct;
+            nbdev->inject.io_type_mask   = opts->io_type_mask;
+            nbdev->inject.latency_us     = opts->latency_us;
+            nbdev->inject.io_counter     = 0;
+            return 0;
+        }
+    }
+    return -ENODEV;
+}
```

---

## Build Instructions

```bash
# From the SPDK root directory:
cd /path/to/spdk

# Configure (if not already done):
./configure --with-shared

# Build only the bdev_null module and its dependencies:
make -j$(nproc) DPDKDIR=/path/to/dpdk

# Alternatively, build the full tree:
make -j$(nproc)
```

Expected output: no errors in `module/bdev/null/`. Warnings about unused variables indicate you forgot to call `null_enqueue_io` in one of the switch cases.

---

## Run Instructions

### 1 - Start SPDK with the null bdev application

```bash
sudo ./build/bin/spdk_tgt -c /path/to/config.json &
```

Example minimal JSON config (`null_ei_test.json`):

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
            "num_blocks": 204800,
            "block_size": 512
          }
        }
      ]
    }
  ]
}
```

### 2 - Enable 20% error injection on reads only, no latency

```bash
./scripts/rpc.py bdev_null_set_error_injection \
    --name Null0 \
    --enabled true \
    --error-rate-pct 20 \
    --inject-reads true \
    --inject-writes false \
    --latency-us 0
```

### 3 - Run bdevperf to observe failures

```bash
sudo ./build/examples/bdevperf \
    -c null_ei_test.json \
    -q 64 -o 4096 -w read -t 10 \
    -r /var/tmp/spdk.sock
```

Expected: bdevperf reports non-zero `io_errors`; approximately 20% of read I/Os fail.

### 4 - Enable latency injection (500 us) on writes

```bash
./scripts/rpc.py bdev_null_set_error_injection \
    --name Null0 \
    --enabled true \
    --error-rate-pct 0 \
    --inject-reads false \
    --inject-writes true \
    --latency-us 500
```

Expected: write IOPS drops significantly, read IOPS unaffected.

### 5 - Disable injection

```bash
./scripts/rpc.py bdev_null_set_error_injection \
    --name Null0 \
    --enabled false
```

---

## Testing: Verifying Error Propagation to the Application Layer

### Unit test approach

SPDK ships with a unit test framework under `test/unit/`. Create `test/unit/lib/bdev/null/bdev_null_error_inject.c`:

```c
#include "spdk_cunit.h"
#include "spdk/bdev.h"
#include "bdev_null.c"   /* white-box test: include the .c file directly */

static int g_last_status;

static void
test_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    g_last_status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
    spdk_bdev_free_io(bdev_io);
}

static void
test_error_rate_100pct(void)
{
    struct null_bdev nbdev = {};
    struct null_error_inject_opts opts = {
        .enabled        = true,
        .error_rate_pct = 100,
        .io_type_mask   = NULL_INJECT_IO_READ | NULL_INJECT_IO_WRITE,
        .latency_us     = 0,
    };

    /* Apply config to the bdev */
    snprintf(nbdev.bdev.name, sizeof(nbdev.bdev.name), "TestNull");
    TAILQ_INSERT_TAIL(&g_null_bdev_head, &nbdev, tailq);
    CU_ASSERT(bdev_null_set_error_injection("TestNull", &opts) == 0);

    /* All I/Os for the targeted types must return false from should_inject */
    for (int i = 0; i < 100; i++) {
        CU_ASSERT(null_should_inject_error(&nbdev, SPDK_BDEV_IO_TYPE_READ) == true);
    }

    TAILQ_REMOVE(&g_null_bdev_head, &nbdev, tailq);
}

static void
test_error_rate_0pct(void)
{
    struct null_bdev nbdev = {};
    struct null_error_inject_opts opts = {
        .enabled        = true,
        .error_rate_pct = 0,
        .io_type_mask   = NULL_INJECT_IO_ALL,
        .latency_us     = 0,
    };

    snprintf(nbdev.bdev.name, sizeof(nbdev.bdev.name), "TestNull2");
    TAILQ_INSERT_TAIL(&g_null_bdev_head, &nbdev, tailq);
    CU_ASSERT(bdev_null_set_error_injection("TestNull2", &opts) == 0);

    for (int i = 0; i < 100; i++) {
        CU_ASSERT(null_should_inject_error(&nbdev, SPDK_BDEV_IO_TYPE_READ) == false);
    }

    TAILQ_REMOVE(&g_null_bdev_head, &nbdev, tailq);
}

static void
test_io_type_filter(void)
{
    struct null_bdev nbdev = {};
    struct null_error_inject_opts opts = {
        .enabled        = true,
        .error_rate_pct = 100,
        .io_type_mask   = NULL_INJECT_IO_READ,  /* reads only */
        .latency_us     = 0,
    };

    snprintf(nbdev.bdev.name, sizeof(nbdev.bdev.name), "TestNull3");
    TAILQ_INSERT_TAIL(&g_null_bdev_head, &nbdev, tailq);
    CU_ASSERT(bdev_null_set_error_injection("TestNull3", &opts) == 0);

    CU_ASSERT(null_should_inject_error(&nbdev, SPDK_BDEV_IO_TYPE_READ)  == true);
    nbdev.inject.io_counter = 0;
    CU_ASSERT(null_should_inject_error(&nbdev, SPDK_BDEV_IO_TYPE_WRITE) == false);

    TAILQ_REMOVE(&g_null_bdev_head, &nbdev, tailq);
}

int
main(int argc, char **argv)
{
    CU_pSuite suite = NULL;
    unsigned int num_failures;

    CU_initialize_registry();
    suite = CU_add_suite("bdev_null_error_inject", NULL, NULL);
    CU_ADD_TEST(suite, test_error_rate_100pct);
    CU_ADD_TEST(suite, test_error_rate_0pct);
    CU_ADD_TEST(suite, test_io_type_filter);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    num_failures = CU_get_number_of_failures();
    CU_cleanup_registry();
    return num_failures;
}
```

Run the unit test:

```bash
make -C test/unit/lib/bdev/null
./test/unit/lib/bdev/null/bdev_null_error_inject
```

### Integration verification with bdevperf

Script to assert non-zero error count:

```bash
#!/usr/bin/env bash
set -e

SPDK_ROOT="$(pwd)"
SOCK="/var/tmp/spdk.sock"

# Start target
sudo "${SPDK_ROOT}/build/bin/spdk_tgt" -c null_ei_test.json &
SPDK_PID=$!
sleep 2

# Enable 50% error injection
"${SPDK_ROOT}/scripts/rpc.py" -s "${SOCK}" bdev_null_set_error_injection \
    --name Null0 --enabled true --error-rate-pct 50 \
    --inject-reads true --inject-writes true --latency-us 0

# Run bdevperf for 5 seconds
OUTPUT=$(sudo "${SPDK_ROOT}/build/examples/bdevperf" \
    -r "${SOCK}" -q 32 -o 4096 -w read -t 5 2>&1)

# Assert errors were reported
if echo "${OUTPUT}" | grep -q "io_errors.*[1-9]"; then
    echo "PASS: errors propagated to application layer"
else
    echo "FAIL: no errors observed"
    kill "${SPDK_PID}"
    exit 1
fi

kill "${SPDK_PID}"
```

---

## Bonus Challenges

### Bonus A - Bit-Rot Simulation (corrupt read data)

Real SSDs can return success but silently corrupt data (bit-rot). Add a `corrupt_reads` flag to `null_error_inject_config`. When set, after copying the read buffer, XOR a single byte at a configurable offset with `0xFF`:

```c
if (cfg->corrupt_reads && bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
    uint8_t *buf = bdev_io->u.bdev.iovs[0].iov_base;
    if (buf && bdev_io->u.bdev.iovs[0].iov_len > cfg->corrupt_offset) {
        buf[cfg->corrupt_offset] ^= 0xFF;
    }
    /* Still complete with SUCCESS - that is what makes bit-rot insidious */
    spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
    return;
}
```

Extend the RPC decoder with `"corrupt_reads"` (bool) and `"corrupt_offset"` (uint64) fields.

To verify, have the application write a known pattern, enable bit-rot, read it back, and compare checksums.

### Bonus B - Timeout Injection (I/O never completes)

A hung device stops completing I/Os entirely. Add a `timeout_inject_pct` field. When triggered, insert the I/O into `ch->delayed_io` with `complete_tsc = UINT64_MAX` - the poller will never drain it. This will exercise the abort path: after the application's timeout fires, it calls `spdk_bdev_abort`, which must find and drain the stuck I/O from `delayed_io`.

```c
if (null_should_inject_timeout(nbdev, bdev_io->type)) {
    null_io->complete_tsc = UINT64_MAX;  /* never expires naturally */
    TAILQ_INSERT_TAIL(&ch->delayed_io, null_io, link);
    return;
}
```

Verify with:
```bash
./scripts/rpc.py bdev_null_set_error_injection \
    --name Null0 --enabled true --error-rate-pct 5 \
    --timeout-inject-pct 5 --latency-us 0
```

Then confirm `spdk_bdev_abort` successfully recovers stuck I/Os by watching the bdevperf abort counter increment.

---

## Common Mistakes

| Mistake | Symptom | Fix |
|---------|---------|-----|
| Calling `spdk_bdev_io_complete` twice for the same I/O | Segfault or assertion in the bdev layer | Ensure every code path reaches exactly one completion call. Use `return` immediately after `spdk_bdev_io_complete`. |
| Forgetting to initialize `TAILQ_INIT(&ch->delayed_io)` | Random crash when first delayed I/O is queued | Add the `TAILQ_INIT` call inside the io_channel create callback alongside the existing `TAILQ_INIT(&ch->io)`. |
| Using `rand()` for error rate | Flaky tests, hard to reproduce failure rates | Use the deterministic counter-based approach shown in `null_should_inject_error`. |
| Setting the error sentinel bit on a TSC value that already has bit 63 set | I/O is treated as success when it should fail, or deadline is corrupted | Verify TSC values: on typical x86 hardware bit 63 will not be set for hundreds of years, but add an assertion: `assert(deadline < (1ULL << 63))`. |
| Not resetting `io_counter` after config change | Error rate shifts unexpectedly after an `rpc.py` update | Always reset `io_counter = 0` inside `bdev_null_set_error_injection`. |
| Modifying `null_bdev.inject` from outside the SPDK thread that owns the bdev | Data race, occasional wrong behavior | In the RPC handler, verify you are running on the SPDK app thread with `spdk_thread_is_app_thread()` or use `spdk_thread_send_msg` to marshal the config update. |
| Forgetting to handle aborts for I/Os in `delayed_io` | Abort RPC never completes; memory leak | Extend `bdev_null_abort_io` to search both `ch->io` and `ch->delayed_io`. |
| Error injection active on RESET type I/Os | Device can never reset, all subsequent I/Os fail | Default `io_type_mask` should exclude `SPDK_BDEV_IO_TYPE_RESET`. Only add RESET to the mask when explicitly testing reset-failure scenarios. |

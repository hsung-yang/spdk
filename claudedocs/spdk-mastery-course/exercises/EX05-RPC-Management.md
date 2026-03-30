# Exercise 05: RPC Management

## Overview

| Field | Value |
|---|---|
| **Objective** | Add custom JSON-RPC methods to an SPDK application to expose runtime configuration and statistics |
| **Estimated Time** | 2-3 hours |
| **Prerequisites** | EX01 (Build SPDK), EX02 (Hello World app), familiarity with C structs and JSON |
| **Key APIs** | `SPDK_RPC_REGISTER`, `spdk_json_decode_object`, `spdk_jsonrpc_begin_result`, `spdk_jsonrpc_end_result` |

---

## Background

SPDK exposes a JSON-RPC 2.0 server over a Unix domain socket. Every SPDK subsystem
registers its RPC methods at startup using the `SPDK_RPC_REGISTER` macro, which arranges
for the registration function to run as a C constructor before `main()`. At runtime the
application calls `spdk_rpc_server_accept()` in its poller loop to dispatch incoming requests.

There are two state masks that control when a method is callable:

| Mask | When callable |
|---|---|
| `SPDK_RPC_STARTUP` | Only before the app finishes initializing |
| `SPDK_RPC_RUNTIME` | Only after initialization completes |
| `SPDK_RPC_STARTUP \| SPDK_RPC_RUNTIME` | Always |

A handler function has the signature:

```c
static void my_rpc_handler(struct spdk_jsonrpc_request *request,
                            const struct spdk_json_val *params);
```

Inside the handler you either:
- Decode `params` with `spdk_json_decode_object()`, do work, then send a response, or
- Send an error with `spdk_jsonrpc_send_error_response()`.

---

## Task Description

You will build a small SPDK application called `rpc_demo` that maintains a simple
configuration and runtime counters. You will expose three RPC methods:

| Method | State | Description |
|---|---|---|
| `rpc_demo_get_config` | RUNTIME | Return current configuration as a JSON object |
| `rpc_demo_set_config` | RUNTIME | Update one or more configuration fields |
| `rpc_demo_get_stats` | RUNTIME | Return runtime counters (requests served, uptime) |

**Bonus tasks** (section 8):
- Input validation with a descriptive error on bad values
- `rpc_demo_reset_stats` — zero the runtime counters

---

## Step 1: Project Layout

Create the following files under `app/rpc_demo/`:

```
app/rpc_demo/
├── Makefile
├── rpc_demo.c      # app skeleton, poller loop
└── rpc_demo_rpc.c  # all RPC handler registrations
```

---

## Step 2: Application State

In `rpc_demo.c` define the global state your RPC methods will read and write.
Keep it simple: a config struct and a stats struct, both protected by the SPDK
app thread (single-threaded access is guaranteed for handlers called from the
poller loop).

```c
/* rpc_demo.c */
#include "spdk/stdinc.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/string.h"

/* ------------------------------------------------------------------ */
/* Application state shared with rpc_demo_rpc.c via this header-like  */
/* extern declarations (or a small internal header).                   */
/* ------------------------------------------------------------------ */

struct rpc_demo_config {
    uint32_t queue_depth;   /* max outstanding I/Os */
    uint32_t timeout_us;    /* per-request timeout in microseconds */
    bool     debug_mode;    /* verbose logging toggle */
    char     target_name[64];
};

struct rpc_demo_stats {
    uint64_t requests_served;
    uint64_t errors;
    uint64_t uptime_ticks; /* updated by the poller */
};

/* Globals – only accessed on the SPDK app thread. */
struct rpc_demo_config g_config = {
    .queue_depth  = 128,
    .timeout_us   = 10000,
    .debug_mode   = false,
    .target_name  = "default",
};

struct rpc_demo_stats g_stats = {0};

static struct spdk_rpc_server *g_rpc_server;
static struct spdk_poller     *g_poller;

/* Forward declarations */
static int  demo_poller(void *arg);
static void demo_start(void *arg1);
static void demo_shutdown(void);

/* ------------------------------------------------------------------ */
/* Poller – called by SPDK reactor every ~1 ms                         */
/* ------------------------------------------------------------------ */
static int
demo_poller(void *arg)
{
    g_stats.uptime_ticks++;

    /* Accept pending RPC connections and dispatch requests. */
    if (g_rpc_server) {
        spdk_rpc_server_accept(g_rpc_server);
    }

    return SPDK_POLLER_BUSY;
}

/* ------------------------------------------------------------------ */
/* App start callback                                                  */
/* ------------------------------------------------------------------ */
static void
demo_start(void *arg1)
{
    const char *rpc_addr = "/var/tmp/rpc_demo.sock";

    SPDK_NOTICELOG("rpc_demo starting\n");

    g_rpc_server = spdk_rpc_server_listen(rpc_addr);
    if (!g_rpc_server) {
        SPDK_ERRLOG("Failed to start RPC server on %s\n", rpc_addr);
        spdk_app_stop(-1);
        return;
    }

    /* Allow runtime RPCs now that we are fully initialized. */
    spdk_rpc_set_state(SPDK_RPC_RUNTIME);

    g_poller = spdk_poller_register(demo_poller, NULL, 1000 /* 1 ms */);
    if (!g_poller) {
        SPDK_ERRLOG("Failed to register poller\n");
        spdk_app_stop(-1);
    }

    SPDK_NOTICELOG("RPC server listening on %s\n", rpc_addr);
}

/* ------------------------------------------------------------------ */
/* App shutdown                                                        */
/* ------------------------------------------------------------------ */
static void
demo_shutdown(void)
{
    spdk_poller_unregister(&g_poller);
    if (g_rpc_server) {
        spdk_rpc_server_close(g_rpc_server);
        g_rpc_server = NULL;
    }
    spdk_app_stop(0);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name            = "rpc_demo";
    opts.shutdown_cb     = demo_shutdown;
    /* Disable the default SPDK RPC server; we start our own. */
    opts.rpc_addr        = NULL;

    rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    rc = spdk_app_start(&opts, demo_start, NULL);
    spdk_app_fini();
    return rc;
}
```

---

## Step 3: Internal Header

Create `rpc_demo_internal.h` so `rpc_demo_rpc.c` can reference the globals:

```c
/* rpc_demo_internal.h */
#pragma once

#include "spdk/stdinc.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"
#include "spdk/log.h"
#include "spdk/util.h"   /* SPDK_COUNTOF */

struct rpc_demo_config {
    uint32_t queue_depth;
    uint32_t timeout_us;
    bool     debug_mode;
    char     target_name[64];
};

struct rpc_demo_stats {
    uint64_t requests_served;
    uint64_t errors;
    uint64_t uptime_ticks;
};

extern struct rpc_demo_config g_config;
extern struct rpc_demo_stats  g_stats;
```

> **Note:** In a real SPDK module you would put this header in the module's
> private include directory. For this exercise a single header at the same
> level as the source files is fine.

---

## Step 4: Implement `rpc_demo_get_config`

This method takes no parameters and returns the current configuration.

```c
/* rpc_demo_rpc.c */
#include "rpc_demo_internal.h"

/* ------------------------------------------------------------------ */
/* rpc_demo_get_config                                                 */
/*   No parameters.                                                    */
/*   Returns: { queue_depth, timeout_us, debug_mode, target_name }    */
/* ------------------------------------------------------------------ */
static void
rpc_demo_get_config(struct spdk_jsonrpc_request *request,
                    const struct spdk_json_val *params)
{
    struct spdk_json_write_ctx *w;

    /*
     * Methods that accept no parameters should reject any params the
     * caller passes.  This is a JSON-RPC 2.0 best practice.
     */
    if (params != NULL) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "rpc_demo_get_config requires no parameters");
        return;
    }

    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_object_begin(w);

    spdk_json_write_named_uint32(w, "queue_depth", g_config.queue_depth);
    spdk_json_write_named_uint32(w, "timeout_us",  g_config.timeout_us);
    spdk_json_write_named_bool  (w, "debug_mode",  g_config.debug_mode);
    spdk_json_write_named_string(w, "target_name", g_config.target_name);

    spdk_json_write_object_end(w);
    spdk_jsonrpc_end_result(request, w);

    g_stats.requests_served++;
}

SPDK_RPC_REGISTER("rpc_demo_get_config", rpc_demo_get_config, SPDK_RPC_RUNTIME)
```

**What the pattern does:**

1. `spdk_jsonrpc_begin_result()` opens the `"result"` key in the JSON-RPC
   response envelope and returns a write context.
2. `spdk_json_write_object_begin/end()` wrap your fields in a JSON object `{}`.
3. `spdk_json_write_named_*()` emit `"key": value` pairs.
4. `spdk_jsonrpc_end_result()` finalizes and sends the response.

---

## Step 5: Implement `rpc_demo_set_config`

This method accepts a partial update — all fields are optional. The decoder
array's last column (`true`) marks a field as optional.

```c
/* ------------------------------------------------------------------ */
/* rpc_demo_set_config                                                 */
/*   Parameters (all optional):                                        */
/*     queue_depth  uint32                                             */
/*     timeout_us   uint32                                             */
/*     debug_mode   bool                                               */
/*     target_name  string                                             */
/*   Returns: true on success                                          */
/* ------------------------------------------------------------------ */

/*
 * Intermediate struct for decoding.  We decode into this first, then
 * copy valid fields into g_config so the live state is never partially
 * updated if decoding fails halfway through.
 */
struct rpc_set_config_req {
    uint32_t queue_depth;
    uint32_t timeout_us;
    bool     debug_mode;
    char    *target_name; /* heap-allocated by spdk_json_decode_string */
};

/*
 * Decoder table.  Each entry maps a JSON key to a struct field.
 * Format: { "json_key", offsetof(struct, field), decoder_fn, optional }
 * optional = true  -> field may be absent in the request
 * optional = false -> field is required; decode fails if absent
 */
static const struct spdk_json_object_decoder rpc_set_config_decoders[] = {
    {
        "queue_depth",
        offsetof(struct rpc_set_config_req, queue_depth),
        spdk_json_decode_uint32,
        true   /* optional */
    },
    {
        "timeout_us",
        offsetof(struct rpc_set_config_req, timeout_us),
        spdk_json_decode_uint32,
        true
    },
    {
        "debug_mode",
        offsetof(struct rpc_set_config_req, debug_mode),
        spdk_json_decode_bool,
        true
    },
    {
        "target_name",
        offsetof(struct rpc_set_config_req, target_name),
        spdk_json_decode_string, /* allocates heap memory */
        true
    },
};

static void
free_rpc_set_config_req(struct rpc_set_config_req *req)
{
    free(req->target_name);
}

static void
rpc_demo_set_config(struct spdk_jsonrpc_request *request,
                    const struct spdk_json_val *params)
{
    /*
     * Zero-initialize so optional fields not present in the request
     * remain at zero/NULL, letting us detect which were actually set.
     */
    struct rpc_set_config_req req = {
        /* Mirror current config as defaults so omitted fields keep
         * their existing values.  spdk_json_decode_object will
         * overwrite only the fields present in params.            */
        .queue_depth  = g_config.queue_depth,
        .timeout_us   = g_config.timeout_us,
        .debug_mode   = g_config.debug_mode,
        .target_name  = NULL,
    };

    if (params == NULL) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "Parameters required");
        return;
    }

    if (spdk_json_decode_object(params,
                                 rpc_set_config_decoders,
                                 SPDK_COUNTOF(rpc_set_config_decoders),
                                 &req)) {
        SPDK_ERRLOG("spdk_json_decode_object failed for rpc_demo_set_config\n");
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "Invalid parameters");
        free_rpc_set_config_req(&req);
        return;
    }

    /* Apply to live config. */
    g_config.queue_depth = req.queue_depth;
    g_config.timeout_us  = req.timeout_us;
    g_config.debug_mode  = req.debug_mode;

    if (req.target_name != NULL) {
        snprintf(g_config.target_name, sizeof(g_config.target_name),
                 "%s", req.target_name);
    }

    free_rpc_set_config_req(&req);
    g_stats.requests_served++;

    spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("rpc_demo_set_config", rpc_demo_set_config, SPDK_RPC_RUNTIME)
```

---

## Step 6: Implement `rpc_demo_get_stats`

```c
/* ------------------------------------------------------------------ */
/* rpc_demo_get_stats                                                  */
/*   No parameters.                                                    */
/*   Returns: { requests_served, errors, uptime_ticks }               */
/* ------------------------------------------------------------------ */
static void
rpc_demo_get_stats(struct spdk_jsonrpc_request *request,
                   const struct spdk_json_val *params)
{
    struct spdk_json_write_ctx *w;

    if (params != NULL) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "rpc_demo_get_stats requires no parameters");
        return;
    }

    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_object_begin(w);

    spdk_json_write_named_uint64(w, "requests_served", g_stats.requests_served);
    spdk_json_write_named_uint64(w, "errors",          g_stats.errors);
    spdk_json_write_named_uint64(w, "uptime_ticks",    g_stats.uptime_ticks);

    spdk_json_write_object_end(w);
    spdk_jsonrpc_end_result(request, w);

    g_stats.requests_served++;
}

SPDK_RPC_REGISTER("rpc_demo_get_stats", rpc_demo_get_stats, SPDK_RPC_RUNTIME)
```

---

## Step 7: Makefile

```makefile
# app/rpc_demo/Makefile
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = rpc_demo

C_SRCS = rpc_demo.c rpc_demo_rpc.c

SPDK_LIB_LIST = event log util jsonrpc json rpc thread

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
```

Build from the SPDK root:

```bash
cd /path/to/spdk
make -C app/rpc_demo
```

---

## Step 8: Build and Run

### 8.1 Configure and build SPDK (if not done)

```bash
cd /path/to/spdk
./configure --with-shared
make -j$(nproc)
```

### 8.2 Build the demo app

```bash
make -C app/rpc_demo
```

### 8.3 Run the app

```bash
sudo ./app/rpc_demo/rpc_demo \
    --no-shconf \
    -m 0x1 \
    --log-level=rpc_demo:DEBUG
```

The app prints:
```
rpc_demo: RPC server listening on /var/tmp/rpc_demo.sock
```

---

## Step 9: Testing with `scripts/rpc.py`

Open a second terminal. SPDK ships `scripts/rpc.py` as the standard RPC client.

### 9.1 Get current configuration

```bash
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_get_config
```

Expected response:
```json
{
  "queue_depth": 128,
  "timeout_us": 10000,
  "debug_mode": false,
  "target_name": "default"
}
```

### 9.2 Update configuration (partial update)

```bash
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock \
    rpc_demo_set_config \
    --queue-depth 256 \
    --debug-mode true
```

> `rpc.py` converts `--queue-depth` to `"queue_depth"` automatically.

Verify the change:

```bash
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_get_config
```

```json
{
  "queue_depth": 256,
  "timeout_us": 10000,
  "debug_mode": true,
  "target_name": "default"
}
```

### 9.3 Get runtime statistics

```bash
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_get_stats
```

```json
{
  "requests_served": 3,
  "errors": 0,
  "uptime_ticks": 12847
}
```

### 9.4 Testing with `curl` (raw JSON-RPC 2.0)

If `rpc.py` is not available, you can use `curl` against the Unix socket:

```bash
curl --unix-socket /var/tmp/rpc_demo.sock \
     -X POST \
     -H "Content-Type: application/json" \
     -d '{
           "jsonrpc": "2.0",
           "id": 1,
           "method": "rpc_demo_get_config"
         }' \
     http://localhost/
```

Updating config with `curl`:

```bash
curl --unix-socket /var/tmp/rpc_demo.sock \
     -X POST \
     -H "Content-Type: application/json" \
     -d '{
           "jsonrpc": "2.0",
           "id": 2,
           "method": "rpc_demo_set_config",
           "params": {
             "queue_depth": 64,
             "target_name": "nvme0"
           }
         }' \
     http://localhost/
```

Expected:
```json
{"jsonrpc":"2.0","id":2,"result":true}
```

### 9.5 Testing error handling

Pass an unexpected parameter to a no-param method:

```bash
curl --unix-socket /var/tmp/rpc_demo.sock \
     -X POST \
     -H "Content-Type: application/json" \
     -d '{
           "jsonrpc": "2.0",
           "id": 3,
           "method": "rpc_demo_get_config",
           "params": {"bad_field": 1}
         }' \
     http://localhost/
```

Expected error response:
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "error": {
    "code": -32602,
    "message": "rpc_demo_get_config requires no parameters"
  }
}
```

---

## Bonus Tasks

### Bonus A: Parameter validation in `rpc_demo_set_config`

After decoding, add range checks before applying to `g_config`:

```c
/* Inside rpc_demo_set_config, after spdk_json_decode_object succeeds */

if (req.queue_depth == 0 || req.queue_depth > 4096) {
    spdk_jsonrpc_send_error_response(request,
                                     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                     "queue_depth must be between 1 and 4096");
    free_rpc_set_config_req(&req);
    g_stats.errors++;
    return;
}

if (req.timeout_us < 100 || req.timeout_us > 1000000) {
    spdk_jsonrpc_send_error_response(request,
                                     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                     "timeout_us must be between 100 and 1000000");
    free_rpc_set_config_req(&req);
    g_stats.errors++;
    return;
}

if (req.target_name != NULL && strlen(req.target_name) == 0) {
    spdk_jsonrpc_send_error_response(request,
                                     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                     "target_name must not be empty");
    free_rpc_set_config_req(&req);
    g_stats.errors++;
    return;
}
```

Test the validation:

```bash
curl --unix-socket /var/tmp/rpc_demo.sock \
     -X POST \
     -H "Content-Type: application/json" \
     -d '{"jsonrpc":"2.0","id":4,"method":"rpc_demo_set_config",
          "params":{"queue_depth": 0}}' \
     http://localhost/
```

Expected:
```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "error": {
    "code": -32602,
    "message": "queue_depth must be between 1 and 4096"
  }
}
```

### Bonus B: Implement `rpc_demo_reset_stats`

Add a method that zeroes the stats counters.

```c
/* ------------------------------------------------------------------ */
/* rpc_demo_reset_stats                                                */
/*   No parameters.                                                    */
/*   Returns: true                                                     */
/* ------------------------------------------------------------------ */
static void
rpc_demo_reset_stats(struct spdk_jsonrpc_request *request,
                     const struct spdk_json_val *params)
{
    if (params != NULL) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "rpc_demo_reset_stats requires no parameters");
        return;
    }

    memset(&g_stats, 0, sizeof(g_stats));
    SPDK_NOTICELOG("Stats reset via RPC\n");

    spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("rpc_demo_reset_stats", rpc_demo_reset_stats, SPDK_RPC_RUNTIME)
```

Test the full flow:

```bash
# View stats before reset
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_get_stats

# Reset
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_reset_stats

# Confirm zeroed
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_get_stats
```

---

## Common Mistakes

### 1. Forgetting to free heap memory from `spdk_json_decode_string`

`spdk_json_decode_string` calls `malloc`. Every early-return error path must
call your free function, or you leak memory on every bad request.

```c
/* Wrong: leaks req.target_name if decode succeeds but validation fails */
if (spdk_json_decode_object(...)) { return; }
if (req.queue_depth > 4096)      { return; } /* leak! */
free(req.target_name);

/* Right: always free before returning */
if (spdk_json_decode_object(...)) { goto invalid; }
if (req.queue_depth > 4096)      { goto invalid; }
free_rpc_set_config_req(&req);
spdk_jsonrpc_send_bool_response(request, true);
return;
invalid:
    free_rpc_set_config_req(&req);
    spdk_jsonrpc_send_error_response(...);
```

### 2. Calling RPC methods before `spdk_rpc_set_state(SPDK_RPC_RUNTIME)`

`SPDK_RPC_RUNTIME` methods are silently rejected until the application calls
`spdk_rpc_set_state(SPDK_RPC_RUNTIME)`. Always set the state after your init
completes, not before.

### 3. Using wrong state mask for initialization-only methods

A method that modifies a static configuration (e.g., listening address) should
use `SPDK_RPC_STARTUP`, not `SPDK_RPC_RUNTIME`. Calling it after startup would
have no effect or cause an error. Think carefully about which mask each method
deserves.

### 4. Not calling `spdk_jsonrpc_end_result` after `spdk_jsonrpc_begin_result`

Once you call `spdk_jsonrpc_begin_result()`, you **must** call
`spdk_jsonrpc_end_result()` on all code paths. Failing to do so leaves the
request dangling and the client waiting forever. There is no RAII in C — trace
every branch.

### 5. Mixing up `SPDK_COUNTOF` and `sizeof`

The decoder array argument must use `SPDK_COUNTOF(decoder_array)` (number of
elements), not `sizeof(decoder_array)`. Passing `sizeof` is a runtime bug that
will decode garbage fields.

```c
/* Wrong */
spdk_json_decode_object(params, decoders, sizeof(decoders), &req);

/* Right */
spdk_json_decode_object(params, decoders, SPDK_COUNTOF(decoders), &req);
```

### 6. Modifying global state before decoding completes

Never apply partial config before `spdk_json_decode_object` returns success.
Decode into a temporary struct, validate, then copy to the live global.

### 7. Calling RPC handlers from a non-app thread

The SPDK RPC server dispatches on the app thread inside `spdk_rpc_server_accept()`.
If you call `spdk_rpc_server_accept()` from a reactor thread other than the
app thread, you will have a data race. Keep the accept call in your main-thread
poller.

---

## Summary

In this exercise you:

- Registered three custom RPC methods using `SPDK_RPC_REGISTER`
- Decoded JSON parameters with `spdk_json_decode_object` and a decoder table
- Built JSON responses using `spdk_jsonrpc_begin_result` / `spdk_json_write_named_*` / `spdk_jsonrpc_end_result`
- Sent boolean and error responses with `spdk_jsonrpc_send_bool_response` and `spdk_jsonrpc_send_error_response`
- Tested all methods with both `scripts/rpc.py` and raw `curl`
- (Bonus) Added input validation and a `reset_stats` method

The same pattern — decoder table, decode, validate, respond — applies to every
RPC method across all SPDK subsystems. Mastering it unlocks the ability to add
runtime observability and control to any SPDK application.

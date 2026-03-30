# Module 14: NVMe Driver

**Stage**: 2 (Implementation)
**Difficulty**: Intermediate
**Estimated Time**: 4 hours
**Prerequisites**: Modules 01-13

---

## Learning Objectives

- Initialize and use NVMe controllers
- Manage namespaces and queue pairs
- Submit NVMe commands directly
- Handle NVMe-specific errors
- Use admin commands
- Understand transport abstraction

---

## Core Concepts

### Concept 1: NVMe Architecture

```mermaid
graph TD
    A[NVMe Controller] --> B[Admin Queue Pair]
    A --> C[I/O Queue Pair 1]
    A --> D[I/O Queue Pair 2]
    A --> E[I/O Queue Pair N]

    A --> F[Namespace 1]
    A --> G[Namespace 2]

    C --> H[Thread 1 I/O]
    D --> I[Thread 2 I/O]

    style A fill:#e1f5ff
    style B fill:#fff4e1
    style C fill:#e1ffe1
    style F fill:#ffe1f5
```

**Components**:
- **Controller**: Physical NVMe device
- **Namespace**: Logical volume on controller
- **Queue Pair**: Submission + Completion queues
- **Admin QP**: Special queue for management commands

---

### Concept 2: Queue Pair Model

```mermaid
sequenceDiagram
    participant App
    participant SQ as Submission Queue
    participant CQ as Completion Queue
    participant Ctrl as Controller

    App->>SQ: Submit command
    Note over SQ: Doorbell write
    SQ->>Ctrl: Process command
    Ctrl->>CQ: Post completion
    App->>CQ: Poll for completion
    CQ-->>App: Completion entry
```

---

## NVMe Driver API

### API 1: Probing and Initialization

**spdk_nvme_probe**:
```c
#include "spdk/nvme.h"

struct nvme_context {
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_nvme_ns *ns;
    struct spdk_nvme_qpair *qpair;
};

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
         struct spdk_nvme_ctrlr_opts *opts)
{
    SPDK_NOTICELOG("Probing: %s\n", trid->traddr);
    return true;  // Attach to this device
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
          struct spdk_nvme_ctrlr *ctrlr,
          const struct spdk_nvme_ctrlr_opts *opts)
{
    struct nvme_context *ctx = cb_ctx;
    int nsid;

    SPDK_NOTICELOG("Attached to %s\n", trid->traddr);

    ctx->ctrlr = ctrlr;

    // Get first active namespace
    nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
    if (nsid == 0) {
        SPDK_ERRLOG("No active namespaces\n");
        return;
    }

    ctx->ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
    if (!spdk_nvme_ns_is_active(ctx->ns)) {
        SPDK_ERRLOG("Namespace %d not active\n", nsid);
        return;
    }

    SPDK_NOTICELOG("Namespace ID: %d, Size: %lu sectors\n",
                   nsid, spdk_nvme_ns_get_num_sectors(ctx->ns));
}

int
probe_nvme_devices(struct nvme_context *ctx)
{
    int rc;

    rc = spdk_nvme_probe(NULL, ctx, probe_cb, attach_cb, NULL);
    if (rc != 0) {
        SPDK_ERRLOG("NVMe probe failed\n");
        return rc;
    }

    return 0;
}
```

---

### API 2: Queue Pair Management

**spdk_nvme_ctrlr_alloc_io_qpair**:
```c
struct spdk_nvme_qpair *
allocate_qpair(struct spdk_nvme_ctrlr *ctrlr)
{
    struct spdk_nvme_io_qpair_opts opts;
    struct spdk_nvme_qpair *qpair;

    spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));

    // Customize options
    opts.qprio = SPDK_NVME_QPRIO_URGENT;  // High priority
    opts.io_queue_size = 128;              // Queue depth
    opts.io_queue_requests = 256;          // Request pool size

    qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
    if (qpair == NULL) {
        SPDK_ERRLOG("Failed to allocate queue pair\n");
        return NULL;
    }

    return qpair;
}

void
free_qpair(struct spdk_nvme_qpair *qpair)
{
    int rc = spdk_nvme_ctrlr_free_io_qpair(qpair);
    if (rc != 0) {
        SPDK_ERRLOG("Failed to free qpair\n");
    }
}
```

---

### API 3: I/O Operations

**spdk_nvme_ns_cmd_read/write**:
```c
struct io_request {
    void *buf;
    uint64_t lba;
    uint32_t lba_count;
    bool completed;
    int status;
};

static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
    struct io_request *req = arg;

    if (spdk_nvme_cpl_is_error(completion)) {
        SPDK_ERRLOG("Read error: SC %02x SCT %02x\n",
                    completion->status.sc, completion->status.sct);
        req->status = -1;
    } else {
        req->status = 0;
    }

    req->completed = true;
}

int
do_nvme_read(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
             void *buf, uint64_t lba, uint32_t lba_count)
{
    struct io_request req = { .buf = buf, .lba = lba,
                             .lba_count = lba_count,
                             .completed = false };
    int rc;

    rc = spdk_nvme_ns_cmd_read(ns, qpair, buf, lba, lba_count,
                              read_complete, &req, 0);
    if (rc != 0) {
        SPDK_ERRLOG("Failed to submit read\n");
        return rc;
    }

    // Poll for completion
    while (!req.completed) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }

    return req.status;
}

int
do_nvme_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
              void *buf, uint64_t lba, uint32_t lba_count)
{
    struct io_request req = { .completed = false };
    int rc;

    rc = spdk_nvme_ns_cmd_write(ns, qpair, buf, lba, lba_count,
                               read_complete, &req, 0);
    if (rc != 0) {
        return rc;
    }

    while (!req.completed) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }

    return req.status;
}
```

**Async I/O with Poller**:
```c
struct io_context {
    struct spdk_nvme_qpair *qpair;
    struct spdk_poller *poller;
    uint64_t outstanding_ios;
};

static int
completion_poller(void *arg)
{
    struct io_context *ctx = arg;
    int32_t num_completions;

    num_completions = spdk_nvme_qpair_process_completions(ctx->qpair, 0);

    if (num_completions < 0) {
        SPDK_ERRLOG("Error processing completions\n");
        return SPDK_POLLER_IDLE;
    }

    return num_completions > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

void
setup_async_io(struct io_context *ctx)
{
    // Register poller to check completions
    ctx->poller = SPDK_POLLER_REGISTER(completion_poller, ctx, 0);
}
```

---

### API 4: Admin Commands

**Get Log Page**:
```c
struct spdk_nvme_health_information_page {
    // ... health data fields
};

void
get_health_info(struct spdk_nvme_ctrlr *ctrlr)
{
    struct spdk_nvme_health_information_page health;
    int rc;

    rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr,
                                          SPDK_NVME_LOG_HEALTH_INFORMATION,
                                          SPDK_NVME_GLOBAL_NS_TAG,
                                          &health, sizeof(health),
                                          0, NULL, NULL);
    if (rc == 0) {
        SPDK_NOTICELOG("Temperature: %u K\n", health.temperature);
        SPDK_NOTICELOG("Available Spare: %u%%\n", health.available_spare);
    }
}
```

**Identify Controller**:
```c
void
print_controller_info(struct spdk_nvme_ctrlr *ctrlr)
{
    const struct spdk_nvme_ctrlr_data *cdata;

    cdata = spdk_nvme_ctrlr_get_data(ctrlr);

    SPDK_NOTICELOG("Controller Information:\n");
    SPDK_NOTICELOG("  Model Number: %.40s\n", cdata->mn);
    SPDK_NOTICELOG("  Serial Number: %.20s\n", cdata->sn);
    SPDK_NOTICELOG("  Firmware: %.8s\n", cdata->fr);
    SPDK_NOTICELOG("  Max Data Transfer: %u\n", cdata->mdts);
}
```

**Identify Namespace**:
```c
void
print_namespace_info(struct spdk_nvme_ns *ns)
{
    const struct spdk_nvme_ns_data *nsdata;
    uint64_t size_bytes;
    uint32_t sector_size;

    nsdata = spdk_nvme_ns_get_data(ns);
    sector_size = spdk_nvme_ns_get_sector_size(ns);
    size_bytes = spdk_nvme_ns_get_size(ns);

    SPDK_NOTICELOG("Namespace Information:\n");
    SPDK_NOTICELOG("  ID: %u\n", spdk_nvme_ns_get_id(ns));
    SPDK_NOTICELOG("  Size: %lu bytes (%lu sectors)\n",
                   size_bytes, spdk_nvme_ns_get_num_sectors(ns));
    SPDK_NOTICELOG("  Sector Size: %u bytes\n", sector_size);
    SPDK_NOTICELOG("  Optimal I/O Boundary: %u blocks\n",
                   spdk_nvme_ns_get_optimal_io_boundary(ns));
}
```

---

## Common Patterns

### Pattern 1: Complete NVMe Application

```c
#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/env.h"

struct nvme_app {
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_nvme_ns *ns;
    struct spdk_nvme_qpair *qpair;
    void *buf;
};

static void
cleanup(struct nvme_app *app)
{
    if (app->qpair) {
        spdk_nvme_ctrlr_free_io_qpair(app->qpair);
    }
    if (app->ctrlr) {
        spdk_nvme_detach(app->ctrlr);
    }
    if (app->buf) {
        spdk_free(app->buf);
    }
    free(app);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
         struct spdk_nvme_ctrlr_opts *opts)
{
    return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
          struct spdk_nvme_ctrlr *ctrlr,
          const struct spdk_nvme_ctrlr_opts *opts)
{
    struct nvme_app *app = cb_ctx;
    int nsid;

    app->ctrlr = ctrlr;

    nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
    app->ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

    print_controller_info(ctrlr);
    print_namespace_info(app->ns);
}

int
main(int argc, char **argv)
{
    struct nvme_app *app;
    struct spdk_env_opts opts;
    int rc;

    app = calloc(1, sizeof(*app));

    // Initialize SPDK environment
    spdk_env_opts_init(&opts);
    opts.name = "nvme_app";

    rc = spdk_env_init(&opts);
    if (rc != 0) {
        free(app);
        return 1;
    }

    // Probe NVMe devices
    rc = spdk_nvme_probe(NULL, app, probe_cb, attach_cb, NULL);
    if (rc != 0 || app->ctrlr == NULL) {
        fprintf(stderr, "No NVMe controllers found\n");
        cleanup(app);
        return 1;
    }

    // Allocate queue pair
    app->qpair = allocate_qpair(app->ctrlr);
    if (app->qpair == NULL) {
        cleanup(app);
        return 1;
    }

    // Allocate I/O buffer
    app->buf = spdk_zmalloc(4096, 4096, NULL, SPDK_ENV_SOCKET_ID_ANY,
                            SPDK_MALLOC_DMA);

    // Do some I/O
    rc = do_nvme_read(app->ns, app->qpair, app->buf, 0, 1);
    if (rc == 0) {
        printf("Read successful\n");
    }

    // Cleanup
    cleanup(app);

    return rc;
}
```

---

## Error Handling

### NVMe Status Codes

```c
static void
handle_completion(const struct spdk_nvme_cpl *cpl)
{
    if (spdk_nvme_cpl_is_error(cpl)) {
        uint8_t sct = cpl->status.sct;  // Status Code Type
        uint8_t sc = cpl->status.sc;    // Status Code

        switch (sct) {
        case SPDK_NVME_SCT_GENERIC:
            switch (sc) {
            case SPDK_NVME_SC_SUCCESS:
                break;
            case SPDK_NVME_SC_INVALID_OPCODE:
                SPDK_ERRLOG("Invalid opcode\n");
                break;
            case SPDK_NVME_SC_INVALID_FIELD:
                SPDK_ERRLOG("Invalid field\n");
                break;
            case SPDK_NVME_SC_LBA_OUT_OF_RANGE:
                SPDK_ERRLOG("LBA out of range\n");
                break;
            default:
                SPDK_ERRLOG("Generic error: SC %02x\n", sc);
                break;
            }
            break;

        case SPDK_NVME_SCT_COMMAND_SPECIFIC:
            SPDK_ERRLOG("Command-specific error: SC %02x\n", sc);
            break;

        case SPDK_NVME_SCT_MEDIA_ERROR:
            SPDK_ERRLOG("Media error: SC %02x\n", sc);
            break;

        default:
            SPDK_ERRLOG("Unknown SCT: %02x SC: %02x\n", sct, sc);
            break;
        }
    }
}
```

---

## Summary

**Key Points**:
1. NVMe uses queue pairs for I/O submission
2. Each thread should have its own queue pair
3. Admin queue for management commands
4. Namespaces are logical volumes
5. Completions must be polled

**Best Practices**:
- One queue pair per thread
- Poll completions regularly
- Handle errors properly
- Use aligned buffers
- Check namespace properties

**Next Steps**:
- Module 15: JSON-RPC Interface
- Module 16: Custom Bdev Module

---

## References

- `lib/nvme/nvme.c` - NVMe driver core
- `include/spdk/nvme.h` - NVMe API
- `include/spdk/nvme_spec.h` - NVMe specification
- `examples/nvme/hello_world/` - Simple example
- NVMe specification at nvmexpress.org

# SPDK API 빠른 레퍼런스

**버전**: SPDK main 브랜치 (2024)
**최종 업데이트**: 2026-03-30

이 레퍼런스는 주요 SPDK 서브시스템의 가장 흔히 사용되는 API를 다룹니다.
각 항목에는 함수 시그니처, 간략한 설명, 반환 값, 일반적인 사용 컨텍스트가
포함됩니다. 전체 문서는 Doxygen 출력 또는 해당 헤더 파일을 참조하세요.

---

## 목차

1. [애플리케이션 프레임워크](#1-application-framework)
2. [스레드와 폴러](#2-thread-and-poller)
3. [I/O 채널](#3-io-channel)
4. [블록 장치 (Bdev)](#4-block-device-bdev)
5. [NVMe 드라이버](#5-nvme-driver)
6. [NVMe-oF 타겟](#6-nvme-of-target)
7. [메모리와 환경](#7-memory-and-environment)
8. [JSON-RPC](#8-json-rpc)
9. [주요 타입과 콜백](#9-key-types-and-callbacks)

---

## 1. 애플리케이션 프레임워크

**헤더**: `spdk/event.h`

애플리케이션 프레임워크는 DPDK EAL을 초기화하고, SPDK 리액터를 설정하고,
JSON-RPC 서버를 시작하며, 메인 이벤트 루프를 구동합니다. 대부분의 애플리케이션은
`spdk_app_opts_init`만 호출하고, `spdk_app_opts`를 채운 후 `spdk_app_start`를 호출합니다.

---

### `spdk_app_opts_init`

```c
void spdk_app_opts_init(struct spdk_app_opts *opts, size_t opts_size);
```

**설명**: `*opts`를 안전한 기본값으로 채웁니다. 구조체 끝에 새 필드가 추가되고
`opts_size` 인수가 ABI 불일치를 방지하므로, 개별 필드를 설정하기 전에
항상 이 함수를 호출하세요.

**매개변수**:
- `opts` - 초기화할 옵션 구조체에 대한 포인터.
- `opts_size` - `sizeof(struct spdk_app_opts)`.

**반환값**: void

**사용법**:
```c
struct spdk_app_opts opts = {};
spdk_app_opts_init(&opts, sizeof(opts));
opts.name = "my_app";
opts.json_config_file = "config.json";
```

---

### `spdk_app_start`

```c
int spdk_app_start(struct spdk_app_opts *opts_user,
                   spdk_msg_fn start_fn,
                   void *arg1);
```

**설명**: SPDK를 초기화하고, 리액터 루프를 시작한 후 `start_fn`을 호출합니다
on the app thread. Blocks until the application shuts down. This is the main entry
point for SPDK applications.

**매개변수**:
- `opts_user` - Options populated after calling `spdk_app_opts_init`.
- `start_fn` - Function called on the app thread once initialization completes.
- `arg1` - Opaque argument passed to `start_fn`.

**반환값**: 0 on clean shutdown, non-zero on error.

**사용법**:
```c
static void app_start(void *arg) {
    /* application work begins here */
    spdk_app_stop(0);
}

int main(int argc, char **argv) {
    struct spdk_app_opts opts = {};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "hello_spdk";
    return spdk_app_start(&opts, app_start, NULL);
}
```

---

### `spdk_app_stop`

```c
void spdk_app_stop(int rc);
```

**설명**: Requests graceful application shutdown. The reactor loop will
drain in-flight work, call registered cleanup functions, and then return from
`spdk_app_start` with the given `rc`.

**매개변수**:
- `rc` - Exit code returned by `spdk_app_start`.

**반환값**: void

---

### `spdk_app_start_shutdown`

```c
void spdk_app_start_shutdown(void);
```

**설명**: Initiates the application shutdown sequence. Similar to
`spdk_app_stop(0)` but does not set a return code. Typically called from a
signal handler or an asynchronous context.

**반환값**: void

---

### `spdk_app_fini`

```c
void spdk_app_fini(void);
```

**설명**: Releases all resources allocated by `spdk_app_start`. Must be
called after `spdk_app_start` returns. Usually called immediately after
`spdk_app_start` in `main()`.

**반환값**: void

---

### `struct spdk_app_opts` — Key Fields

```c
struct spdk_app_opts {
    const char *name;                   /* Application name (required) */
    const char *json_config_file;       /* Path to JSON config file */
    bool        json_config_ignore_errors;
    const char *rpc_addr;               /* UNIX socket or IP:port for RPC */
    const char *reactor_mask;           /* CPU mask for reactors, e.g. "0x3" */
    int         shm_id;                 /* Shared memory ID (-1 = private) */
    spdk_app_shutdown_cb shutdown_cb;   /* Custom shutdown callback */
    int         mem_size;               /* Hugepage MB to reserve (-1 = auto) */
    bool        no_pci;                 /* Disable PCI device probing */
    bool        no_huge;                /* Disable hugepages (testing only) */
    int         main_core;             /* lcore for the app/main thread */
    enum spdk_log_level print_level;   /* Log verbosity */
};
```

---

## 2. 스레드와 폴러

**헤더**: `spdk/thread.h`

SPDK threads are lightweight, stackless units of execution pinned to a reactor
(lcore). Work is submitted via messages or executed by pollers. All SPDK
subsystem APIs must be called from an SPDK thread.

---

### `spdk_thread_create`

```c
struct spdk_thread *spdk_thread_create(const char *name,
                                       const struct spdk_cpuset *cpumask);
```

**설명**: Creates a new SPDK thread. The thread is not associated with
an OS thread; it is scheduled by the reactor that picks it up according to
`cpumask`.

**매개변수**:
- `name` - Human-readable name for debugging.
- `cpumask` - Set of lcores that may run this thread. NULL means any lcore.

**반환값**: Pointer to the new thread, or NULL on failure.

**사용법**:
```c
struct spdk_thread *thread = spdk_thread_create("io_thread", NULL);
```

---

### `spdk_get_thread`

```c
struct spdk_thread *spdk_get_thread(void);
```

**설명**: Returns the SPDK thread currently running on the calling OS
thread. Returns NULL if called from a non-SPDK context.

**반환값**: Current `spdk_thread *`, or NULL.

---

### `spdk_thread_get_app_thread`

```c
struct spdk_thread *spdk_thread_get_app_thread(void);
```

**설명**: Returns the application (main) thread created by
`spdk_app_start`. This is the thread on which initialization callbacks run.

**반환값**: App thread pointer, or NULL if called before `spdk_app_start`.

---

### `spdk_thread_exit`

```c
int spdk_thread_exit(struct spdk_thread *thread);
```

**설명**: Marks a thread for exit. The thread will stop polling and be
eligible for destruction once all active pollers have been unregistered and all
I/O channels have been released.

**매개변수**:
- `thread` - Thread to exit.

**반환값**: 0 on success, -EBUSY if the thread still has active resources.

---

### `spdk_thread_destroy`

```c
void spdk_thread_destroy(struct spdk_thread *thread);
```

**설명**: Frees memory associated with a thread that has already exited
(i.e., `spdk_thread_exit` returned 0 and all cleanup is complete).

**매개변수**:
- `thread` - Thread to destroy. Must be in the exited state.

**반환값**: void

---

### `spdk_thread_send_msg`

```c
int spdk_thread_send_msg(const struct spdk_thread *thread,
                         spdk_msg_fn fn,
                         void *ctx);
```

**설명**: Schedules `fn(ctx)` to run on the specified thread. This is
the primary mechanism for cross-thread communication in SPDK. The call is
lock-free and thread-safe.

**매개변수**:
- `thread` - Target thread.
- `fn` - Function to call (`void fn(void *ctx)`).
- `ctx` - Argument passed to `fn`.

**반환값**: 0 on success, -ENOMEM if the message ring is full.

**사용법**:
```c
spdk_thread_send_msg(target_thread, my_callback, my_ctx);
```

---

### `spdk_poller_register`

```c
struct spdk_poller *spdk_poller_register(spdk_poller_fn fn,
                                         void *arg,
                                         uint64_t period_microseconds);
```

**설명**: Registers a polling function on the current SPDK thread.
The function is called repeatedly at the specified interval. Use
`period_microseconds = 0` for a tight busy-poll loop.

**매개변수**:
- `fn` - Poller callback (`int fn(void *arg)`). Must return `SPDK_POLLER_BUSY`
  if work was done, `SPDK_POLLER_IDLE` otherwise.
- `arg` - Argument passed to `fn` on each invocation.
- `period_microseconds` - Minimum interval between calls. 0 = every reactor loop.

**반환값**: Opaque poller handle, or NULL on failure.

**사용법**:
```c
static int my_poller(void *arg) {
    /* check for completions */
    return SPDK_POLLER_IDLE;
}

struct spdk_poller *p = spdk_poller_register(my_poller, ctx, 0);
```

---

### `spdk_poller_register_named`

```c
struct spdk_poller *spdk_poller_register_named(spdk_poller_fn fn,
                                               void *arg,
                                               uint64_t period_microseconds,
                                               const char *name);
```

**설명**: Same as `spdk_poller_register` but attaches a human-readable
name to the poller. The name is visible in RPC output (`rpc.py thread_get_pollers`).

**반환값**: Opaque poller handle, or NULL on failure.

---

### `spdk_poller_unregister`

```c
void spdk_poller_unregister(struct spdk_poller **ppoller);
```

**설명**: Stops and frees a poller. The pointer is set to NULL after
unregistration. Must be called from the same SPDK thread that registered the
poller.

**매개변수**:
- `ppoller` - Address of the poller pointer. Set to NULL on return.

**반환값**: void

---

### `spdk_poller_register_interrupt`

```c
void spdk_poller_register_interrupt(struct spdk_poller *poller,
                                    spdk_poller_set_interrupt_mode_cb cb_fn,
                                    void *cb_arg);
```

**설명**: Marks a poller as capable of operating in interrupt mode.
`cb_fn` is invoked when the thread transitions between poll and interrupt modes.
Pass NULL for `cb_fn` if no transition callback is needed.

**반환값**: void

---

## 3. I/O 채널

**헤더**: `spdk/thread.h`

I/O channels are per-thread handles to a shared resource (e.g., a bdev or NVMe
controller). They allow lock-free I/O submission from any SPDK thread.

---

### `spdk_io_channel_get_ctx`

```c
void *spdk_io_channel_get_ctx(struct spdk_io_channel *ch);
```

**설명**: Returns the driver-private context buffer associated with an
I/O channel. The size of this buffer is specified when registering the I/O device.

**반환값**: Pointer to the per-channel context.

---

### `spdk_put_io_channel`

```c
void spdk_put_io_channel(struct spdk_io_channel *ch);
```

**설명**: Releases a reference to an I/O channel. When the reference
count reaches zero the channel's destroy callback is invoked. Must be called
from the thread that called `spdk_get_io_channel`.

**반환값**: void

---

## 4. 블록 장치 (Bdev)

**헤더**: `spdk/bdev.h`

The bdev layer provides a uniform I/O interface over heterogeneous storage
backends. Applications interact through a descriptor (`spdk_bdev_desc`) and
submit I/O via a per-thread I/O channel.

---

### `spdk_bdev_open_ext`

```c
int spdk_bdev_open_ext(const char *bdev_name,
                       bool write,
                       spdk_bdev_event_cb_t event_cb,
                       void *event_ctx,
                       struct spdk_bdev_desc **desc);
```

**설명**: Opens a block device by name and returns a descriptor. The
`event_cb` is called asynchronously when device events occur (removal, resize).
The descriptor must be closed with `spdk_bdev_close` when no longer needed.

**매개변수**:
- `bdev_name` - Name of the bdev as registered with the bdev layer.
- `write` - true to open read-write, false for read-only.
- `event_cb` - Callback for device events (required; must not be NULL).
- `event_ctx` - Opaque context passed to `event_cb`.
- `desc` - Output: filled with the opened descriptor on success.

**반환값**: 0 on success, negative errno on failure.

**사용법**:
```c
static void event_cb(enum spdk_bdev_event_type type,
                     struct spdk_bdev *bdev, void *ctx) {
    if (type == SPDK_BDEV_EVENT_REMOVE) { /* handle hot-unplug */ }
}

struct spdk_bdev_desc *desc;
int rc = spdk_bdev_open_ext("Nvme0n1", true, event_cb, NULL, &desc);
```

---

### `spdk_bdev_close`

```c
void spdk_bdev_close(struct spdk_bdev_desc *desc);
```

**설명**: Closes a bdev descriptor. All I/O channels obtained from
this descriptor must be released before calling this function.

**반환값**: void

---

### `spdk_bdev_get_io_channel`

```c
struct spdk_io_channel *spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc);
```

**설명**: Returns a per-thread I/O channel for the given bdev descriptor.
I/O submission functions require this channel. Must be called and used on the
same SPDK thread. Release with `spdk_put_io_channel`.

**반환값**: I/O channel pointer, or NULL on failure.

**사용법**:
```c
struct spdk_io_channel *ch = spdk_bdev_get_io_channel(desc);
/* submit I/O using ch */
spdk_put_io_channel(ch);
```

---

### `spdk_bdev_read`

```c
int spdk_bdev_read(struct spdk_bdev_desc *desc,
                   struct spdk_io_channel *ch,
                   void *buf,
                   uint64_t offset,
                   uint64_t nbytes,
                   spdk_bdev_io_completion_cb cb,
                   void *cb_arg);
```

**설명**: Submits an asynchronous read in byte units. `buf` must be
DMA-safe memory (allocated with `spdk_zmalloc` / `spdk_malloc`).

**매개변수**:
- `desc` - Open bdev descriptor.
- `ch` - Per-thread I/O channel from `spdk_bdev_get_io_channel`.
- `buf` - Destination buffer (DMA-capable).
- `offset` - Byte offset into the device.
- `nbytes` - Number of bytes to read.
- `cb` - Completion callback.
- `cb_arg` - Opaque argument passed to `cb`.

**반환값**: 0 if the I/O was queued, -ENOMEM if no I/O resources available,
other negative errno on error.

---

### `spdk_bdev_read_blocks`

```c
int spdk_bdev_read_blocks(struct spdk_bdev_desc *desc,
                          struct spdk_io_channel *ch,
                          void *buf,
                          uint64_t offset_blocks,
                          uint64_t num_blocks,
                          spdk_bdev_io_completion_cb cb,
                          void *cb_arg);
```

**설명**: Same as `spdk_bdev_read` but parameters are in block units
(block size from `spdk_bdev_get_block_size`). Preferred for block-aligned I/O.

**반환값**: 0 on success, negative errno on error.

---

### `spdk_bdev_write`

```c
int spdk_bdev_write(struct spdk_bdev_desc *desc,
                    struct spdk_io_channel *ch,
                    void *buf,
                    uint64_t offset,
                    uint64_t nbytes,
                    spdk_bdev_io_completion_cb cb,
                    void *cb_arg);
```

**설명**: Submits an asynchronous write in byte units. `buf` must be
DMA-safe memory and must remain valid until the completion callback fires.

**반환값**: 0 if queued, -ENOMEM or other negative errno on error.

---

### `spdk_bdev_write_blocks`

```c
int spdk_bdev_write_blocks(struct spdk_bdev_desc *desc,
                           struct spdk_io_channel *ch,
                           void *buf,
                           uint64_t offset_blocks,
                           uint64_t num_blocks,
                           spdk_bdev_io_completion_cb cb,
                           void *cb_arg);
```

**설명**: Same as `spdk_bdev_write` but parameters are in block units.

**반환값**: 0 on success, negative errno on error.

---

### `spdk_bdev_unmap`

```c
int spdk_bdev_unmap(struct spdk_bdev_desc *desc,
                    struct spdk_io_channel *ch,
                    uint64_t offset,
                    uint64_t nbytes,
                    spdk_bdev_io_completion_cb cb,
                    void *cb_arg);
```

**설명**: Submits a TRIM/UNMAP command in byte units. The device may
reclaim the storage. Not all bdev backends support unmap; check with
`spdk_bdev_io_type_supported`.

**반환값**: 0 on success, -ENOTSUP if unsupported, other negative errno on error.

---

### `spdk_bdev_write_zeroes`

```c
int spdk_bdev_write_zeroes(struct spdk_bdev_desc *desc,
                           struct spdk_io_channel *ch,
                           uint64_t offset,
                           uint64_t nbytes,
                           spdk_bdev_io_completion_cb cb,
                           void *cb_arg);
```

**설명**: Writes zeros to the specified byte range. More efficient than
a buffer-fill write on devices that support native zero-fill commands.

**반환값**: 0 on success, negative errno on error.

---

### `spdk_bdev_flush`

```c
int spdk_bdev_flush(struct spdk_bdev_desc *desc,
                    struct spdk_io_channel *ch,
                    uint64_t offset,
                    uint64_t length,
                    spdk_bdev_io_completion_cb cb,
                    void *cb_arg);
```

**설명**: Issues a flush (cache writeback) for the given byte range.
Completion signals that the data is persistent on the device.

**반환값**: 0 on success, negative errno on error.

---

### Bdev Information Accessors

```c
/* Header: spdk/bdev.h */

const char *spdk_bdev_get_name(const struct spdk_bdev *bdev);
/* Returns the registered name string of the bdev. */

uint32_t spdk_bdev_get_block_size(const struct spdk_bdev *bdev);
/* Returns block size in bytes (e.g., 512 or 4096). */

uint64_t spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev);
/* Returns total number of logical blocks on the device. */

uint32_t spdk_bdev_get_optimal_io_boundary(const struct spdk_bdev *bdev);
/* Returns the preferred I/O boundary in blocks (0 if no preference). */

bool spdk_bdev_io_type_supported(struct spdk_bdev *bdev,
                                  enum spdk_bdev_io_type io_type);
/* Returns true if the bdev supports the given I/O type. */

struct spdk_bdev *spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc);
/* Returns the spdk_bdev handle from an open descriptor. */
```

---

### Bdev I/O Completion Callback Pattern

```c
static void io_complete(struct spdk_bdev_io *bdev_io,
                        bool success,
                        void *cb_arg)
{
    struct my_ctx *ctx = cb_arg;
    if (!success) {
        /* retrieve detailed status */
        int sct, sc;
        uint32_t cdw0;
        spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);
    }
    spdk_bdev_free_io(bdev_io);  /* always free the I/O */
}
```

### `spdk_bdev_free_io`

```c
void spdk_bdev_free_io(struct spdk_bdev_io *bdev_io);
```

**설명**: Returns a completed bdev I/O back to the pool. Must be called
exactly once from within the completion callback.

**반환값**: void

---

## 5. NVMe 드라이버

**헤더**: `spdk/nvme.h`

The SPDK NVMe driver provides direct, lock-free access to NVMe devices bypassing
the kernel. It manages controllers (`spdk_nvme_ctrlr`), namespaces
(`spdk_nvme_ns`), and I/O queue pairs (`spdk_nvme_qpair`).

---

### `spdk_nvme_probe`

```c
int spdk_nvme_probe(const struct spdk_nvme_transport_id *trid,
                    void *cb_ctx,
                    spdk_nvme_probe_cb probe_cb,
                    spdk_nvme_attach_cb attach_cb,
                    spdk_nvme_remove_cb remove_cb);
```

**설명**: Scans for NVMe devices matching `trid`. For each candidate
device `probe_cb` is called; if it returns true the driver attaches the device
and calls `attach_cb` with the controller handle. Pass NULL for `trid` to probe
all local PCIe NVMe devices.

**매개변수**:
- `trid` - Transport ID filter. NULL for all PCIe devices.
- `cb_ctx` - Opaque context passed to all callbacks.
- `probe_cb` - Called before attach: `bool probe_cb(void *ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)`. Return true to attach.
- `attach_cb` - Called after successful attach: `void attach_cb(void *ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)`.
- `remove_cb` - Called on device removal (may be NULL).

**반환값**: 0 on success, negative errno on error.

**사용법**:
```c
spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
```

---

### `spdk_nvme_detach`

```c
int spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr);
```

**설명**: Detaches from an NVMe controller and frees all associated
resources. All I/O queue pairs must be freed before calling this.

**반환값**: 0 on success, negative errno on error.

---

### `spdk_nvme_ctrlr_get_num_ns`

```c
uint32_t spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr);
```

**설명**: Returns the number of namespaces reported by the controller.
Namespace IDs are 1-based (1 through `num_ns`).

**반환값**: Number of namespaces.

---

### `spdk_nvme_ctrlr_get_ns`

```c
struct spdk_nvme_ns *spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr,
                                             uint32_t nsid);
```

**설명**: Returns the namespace handle for the given 1-based namespace
ID. Returns NULL if the namespace does not exist or is not active.

**반환값**: Namespace pointer or NULL.

---

### `spdk_nvme_ctrlr_alloc_io_qpair`

```c
struct spdk_nvme_qpair *spdk_nvme_ctrlr_alloc_io_qpair(
    struct spdk_nvme_ctrlr *ctrlr,
    const struct spdk_nvme_io_qpair_opts *opts,
    size_t opts_size);
```

**설명**: Allocates an I/O submission/completion queue pair on the
controller. Each SPDK thread that submits I/O should have its own queue pair.
Pass NULL for `opts` to use defaults.

**매개변수**:
- `ctrlr` - Controller to allocate on.
- `opts` - Queue pair options (NULL = defaults).
- `opts_size` - `sizeof(*opts)`, ignored when `opts` is NULL.

**반환값**: Queue pair pointer, or NULL on failure.

---

### `spdk_nvme_ctrlr_free_io_qpair`

```c
int spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair);
```

**설명**: Destroys an I/O queue pair and frees its resources. Must be
called from the same thread that allocated the queue pair.

**반환값**: 0 on success, negative errno on error.

---

### `spdk_nvme_ns_cmd_read`

```c
int spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns,
                          struct spdk_nvme_qpair *qpair,
                          void *payload,
                          uint64_t lba,
                          uint32_t lba_count,
                          spdk_nvme_cmd_cb cb_fn,
                          void *cb_arg,
                          uint32_t io_flags);
```

**설명**: Submits a read command to the namespace. `payload` must be
physically contiguous DMA memory. The completion callback receives the CQE.

**매개변수**:
- `ns` - Target namespace.
- `qpair` - I/O queue pair for submission.
- `payload` - DMA buffer to receive data.
- `lba` - Starting logical block address.
- `lba_count` - Number of LBAs to read.
- `cb_fn` - Completion callback `void cb(void *arg, const struct spdk_nvme_cpl *cpl)`.
- `cb_arg` - Argument for `cb_fn`.
- `io_flags` - NVMe I/O flags (e.g., `SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS`).

**반환값**: 0 if submitted, negative errno on error.

---

### `spdk_nvme_ns_cmd_write`

```c
int spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns,
                           struct spdk_nvme_qpair *qpair,
                           void *payload,
                           uint64_t lba,
                           uint32_t lba_count,
                           spdk_nvme_cmd_cb cb_fn,
                           void *cb_arg,
                           uint32_t io_flags);
```

**설명**: Submits a write command to the namespace. Same parameter
conventions as `spdk_nvme_ns_cmd_read`.

**반환값**: 0 if submitted, negative errno on error.

---

### `spdk_nvme_ns_cmd_flush`

```c
int spdk_nvme_ns_cmd_flush(struct spdk_nvme_ns *ns,
                           struct spdk_nvme_qpair *qpair,
                           spdk_nvme_cmd_cb cb_fn,
                           void *cb_arg);
```

**설명**: Submits an NVMe Flush command to force pending data to
non-volatile media.

**반환값**: 0 if submitted, negative errno on error.

---

### `spdk_nvme_qpair_process_completions`

```c
int32_t spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair,
                                             uint32_t max_completions);
```

**설명**: Polls the completion queue and invokes callbacks for completed
commands. Call this repeatedly from an SPDK poller. Pass `max_completions = 0`
to process all available completions.

**반환값**: Number of completions processed (>= 0), or negative errno on error.

---

### `spdk_nvme_ctrlr_process_admin_completions`

```c
int32_t spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr);
```

**설명**: Polls the admin completion queue. Must be called periodically
when using admin commands or the keep-alive feature.

**반환값**: Number of completions processed (>= 0), or negative errno on error.

---

### `spdk_nvme_ctrlr_cmd_admin_raw`

```c
int spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
                                  struct spdk_nvme_cmd *cmd,
                                  void *buf,
                                  uint32_t len,
                                  spdk_nvme_cmd_cb cb_fn,
                                  void *cb_arg);
```

**설명**: Submits a raw admin command. Use for vendor-specific or
non-standard admin operations. `cmd` is a populated NVMe command structure;
the driver fills in the queue and completion handling fields.

**반환값**: 0 if submitted, negative errno on error.

---

### NVMe Namespace Accessors

```c
/* Header: spdk/nvme.h */

uint64_t spdk_nvme_ns_get_num_sectors(struct spdk_nvme_ns *ns);
/* Total number of logical blocks in the namespace. */

uint32_t spdk_nvme_ns_get_sector_size(struct spdk_nvme_ns *ns);
/* Logical block size in bytes (data only, excluding metadata). */

uint32_t spdk_nvme_ns_get_max_io_xfer_size(struct spdk_nvme_ns *ns);
/* Maximum bytes per single I/O command. */

bool spdk_nvme_ns_is_active(struct spdk_nvme_ns *ns);
/* Returns true if the namespace is in the active (usable) state. */

uint32_t spdk_nvme_ns_get_id(struct spdk_nvme_ns *ns);
/* Returns the 1-based namespace identifier. */
```

---

## 6. NVMe-oF 타겟

**헤더**: `spdk/nvmf.h`

The NVMe-oF target exposes NVMe namespaces over a fabric transport (TCP, RDMA,
FC). The key objects are a target (`spdk_nvmf_tgt`), subsystems
(`spdk_nvmf_subsystem`), and transports (`spdk_nvmf_transport`).

---

### `spdk_nvmf_tgt_create`

```c
struct spdk_nvmf_tgt *spdk_nvmf_tgt_create(struct spdk_nvmf_target_opts *opts);
```

**설명**: Creates a new NVMe-oF target. The target is the top-level
container for subsystems and transports.

**매개변수**:
- `opts` - Target options. Pass a zero-initialized struct for defaults.
  The `name` field distinguishes multiple targets.

**반환값**: Target pointer, or NULL on failure.

---

### `spdk_nvmf_tgt_destroy`

```c
void spdk_nvmf_tgt_destroy(struct spdk_nvmf_tgt *tgt,
                            spdk_nvmf_tgt_destroy_done_fn *cb_fn,
                            void *cb_arg);
```

**설명**: Asynchronously destroys a target. The callback is invoked
once all subsystems and transports have been torn down.

**반환값**: void

---

### `spdk_nvmf_subsystem_create`

```c
struct spdk_nvmf_subsystem *spdk_nvmf_subsystem_create(
    struct spdk_nvmf_tgt *tgt,
    const char *nqn,
    enum spdk_nvmf_subtype type,
    uint32_t num_ns);
```

**설명**: Creates a new NVMe-oF subsystem with the given NQN (NVMe
Qualified Name). Use `SPDK_NVMF_SUBTYPE_NVME` for storage subsystems and
`SPDK_NVMF_SUBTYPE_DISCOVERY` for discovery subsystems.

**매개변수**:
- `tgt` - Parent target.
- `nqn` - Subsystem NQN, e.g. `"nqn.2024-01.io.spdk:cnode1"`.
- `type` - Subsystem type.
- `num_ns` - Initial namespace capacity (can grow dynamically).

**반환값**: Subsystem pointer, or NULL on failure.

---

### `spdk_nvmf_subsystem_start`

```c
int spdk_nvmf_subsystem_start(struct spdk_nvmf_subsystem *subsystem,
                               spdk_nvmf_subsystem_state_change_done cb_fn,
                               void *cb_arg);
```

**설명**: Transitions a subsystem from the INACTIVE state to ACTIVE,
making it accessible to initiators. The callback fires once the transition
completes (or fails).

**반환값**: 0 if the transition was initiated, negative errno on error.

---

### `spdk_nvmf_subsystem_stop`

```c
int spdk_nvmf_subsystem_stop(struct spdk_nvmf_subsystem *subsystem,
                              spdk_nvmf_subsystem_state_change_done cb_fn,
                              void *cb_arg);
```

**설명**: Transitions a subsystem from ACTIVE to INACTIVE, disconnecting
all connected initiators. The callback fires on completion.

**반환값**: 0 if the transition was initiated, negative errno on error.

---

### `spdk_nvmf_subsystem_add_listener`

```c
void spdk_nvmf_subsystem_add_listener(
    struct spdk_nvmf_subsystem *subsystem,
    struct spdk_nvme_transport_id *trid,
    spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
    void *cb_arg);
```

**설명**: Adds a listen address to a subsystem. Initiators connect to
this address to reach the subsystem. `trid` specifies transport type, address,
and port (e.g., TCP on `192.168.1.1:4420`).

**반환값**: void (result delivered via `cb_fn`).

---

### `spdk_nvmf_subsystem_add_ns_ext`

```c
uint32_t spdk_nvmf_subsystem_add_ns_ext(
    struct spdk_nvmf_subsystem *subsystem,
    const char *bdev_name,
    const struct spdk_nvmf_ns_opts *opts,
    size_t opts_size,
    const char *ptpl_file);
```

**설명**: Adds a bdev as a namespace to the subsystem. The NSID is
auto-assigned (or specified via `opts->nsid`). The subsystem must be PAUSED
or INACTIVE when calling this.

**매개변수**:
- `subsystem` - Target subsystem.
- `bdev_name` - Name of the bdev to expose.
- `opts` - Namespace options (NULL for defaults).
- `opts_size` - `sizeof(*opts)`.
- `ptpl_file` - Persist-through-power-loss reservation file path (or NULL).

**반환값**: Assigned NSID (>= 1) on success, 0 on failure.

---

### `spdk_nvmf_transport_create_async`

```c
int spdk_nvmf_transport_create_async(
    const char *transport_name,
    struct spdk_nvmf_transport_opts *opts,
    spdk_nvmf_transport_create_done_cb cb_fn,
    void *cb_arg);
```

**설명**: Creates a transport of the given type (e.g., `"TCP"` or
`"RDMA"`) asynchronously. The transport must be added to a target via
`spdk_nvmf_tgt_add_transport` before use.

**반환값**: 0 if creation was initiated, negative errno on error.

---

### `spdk_nvmf_tgt_add_transport`

```c
void spdk_nvmf_tgt_add_transport(struct spdk_nvmf_tgt *tgt,
                                  struct spdk_nvmf_transport *transport,
                                  spdk_nvmf_tgt_add_transport_done_fn cb_fn,
                                  void *cb_arg);
```

**설명**: Attaches a transport to a target. After this completes
(via callback), listeners can be added and the target can accept connections.

**반환값**: void

---

### NVMe-oF Subsystem Host Access

```c
/* Header: spdk/nvmf.h */

int spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem,
                                  const char *hostnqn,
                                  const struct spdk_nvmf_host_opts *opts);
/* Adds an allowed initiator NQN. If no hosts are added, any initiator can connect. */

int spdk_nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem,
                                     const char *hostnqn);
/* Removes a previously added host NQN. */

void spdk_nvmf_subsystem_set_allow_any_host(struct spdk_nvmf_subsystem *subsystem,
                                             bool allow_any_host);
/* Enables or disables open access (any initiator NQN accepted). */
```

---

## 7. 메모리와 환경

**헤더**: `spdk/env.h`

SPDK requires DMA-capable memory for all I/O buffers. The env layer wraps DPDK's
hugepage allocator and provides named mempools for fixed-size object pools.

---

### `spdk_malloc`

```c
void *spdk_malloc(size_t size,
                  size_t align,
                  uint64_t *unused,
                  int numa_id,
                  uint32_t flags);
```

**설명**: Allocates DMA-capable or shared memory. `flags` controls the
allocation type: use `SPDK_MALLOC_DMA` for I/O buffers, `SPDK_MALLOC_SHARE`
for cross-process buffers. At least one flag must be set. `unused` must be NULL.

**매개변수**:
- `size` - Size in bytes.
- `align` - Alignment (power of two, or 0 for cache-line alignment).
- `unused` - Must be NULL.
- `numa_id` - NUMA node (`SPDK_ENV_NUMA_ID_ANY` for any node).
- `flags` - `SPDK_MALLOC_DMA` and/or `SPDK_MALLOC_SHARE`.

**반환값**: Pointer to the allocated buffer, or NULL on failure.

**사용법**:
```c
void *buf = spdk_malloc(4096, 4096, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
```

---

### `spdk_zmalloc`

```c
void *spdk_zmalloc(size_t size,
                   size_t align,
                   uint64_t *unused,
                   int numa_id,
                   uint32_t flags);
```

**설명**: Same as `spdk_malloc` but the returned buffer is zeroed.
Preferred when the buffer will be used as a read destination or initialized
to zero.

**반환값**: Pointer to zeroed buffer, or NULL on failure.

---

### `spdk_realloc`

```c
void *spdk_realloc(void *buf, size_t size, size_t align);
```

**설명**: Resizes a buffer previously allocated with `spdk_malloc` or
`spdk_zmalloc`. Existing data is preserved. The buffer may be moved.

**반환값**: Pointer to resized buffer, or NULL on failure.

---

### `spdk_free`

```c
void spdk_free(void *buf);
```

**설명**: Frees a buffer allocated with `spdk_malloc`, `spdk_zmalloc`,
or `spdk_realloc`. Do not use the standard C `free()` for SPDK-allocated
memory.

**반환값**: void

---

### `spdk_dma_free`

```c
void spdk_dma_free(void *buf);
```

**설명**: Alias for `spdk_free`. Frees DMA memory returned by legacy
`spdk_dma_malloc` or `spdk_dma_zmalloc` allocators.

**반환값**: void

---

### `spdk_env_opts_init`

```c
void spdk_env_opts_init(struct spdk_env_opts *opts);
```

**설명**: Initializes `spdk_env_opts` with default values. Use when
calling `spdk_env_init` directly (without the app framework).

**반환값**: void

---

### `spdk_mempool_create`

```c
struct spdk_mempool *spdk_mempool_create(const char *name,
                                         size_t count,
                                         size_t ele_size,
                                         size_t cache_size,
                                         int numa_id);
```

**설명**: Creates a named pool of fixed-size objects backed by hugepage
memory. Mempools are useful for pre-allocating I/O request structures or
metadata buffers.

**매개변수**:
- `name` - Unique pool name (max `SPDK_MAX_MEMPOOL_NAME_LEN` chars).
- `count` - Number of elements in the pool.
- `ele_size` - Size of each element in bytes.
- `cache_size` - Per-core cache size (`SPDK_MEMPOOL_DEFAULT_CACHE_SIZE` recommended).
- `numa_id` - NUMA node for allocation, or `SPDK_ENV_NUMA_ID_ANY`.

**반환값**: Pool pointer, or NULL on failure.

---

### `spdk_mempool_free`

```c
void spdk_mempool_free(struct spdk_mempool *mp);
```

**설명**: Destroys a mempool and frees its memory. All elements must
have been returned to the pool before calling this.

**반환값**: void

---

### `spdk_mempool_get`

```c
void *spdk_mempool_get(struct spdk_mempool *mp);
```

**설명**: Gets one element from the pool. Returns immediately without
blocking. Returns NULL if the pool is empty.

**반환값**: Element pointer, or NULL if pool is empty.

---

### `spdk_mempool_put`

```c
void spdk_mempool_put(struct spdk_mempool *mp, void *ele);
```

**설명**: Returns one element to the pool.

**반환값**: void

---

### `spdk_mempool_get_bulk` / `spdk_mempool_put_bulk`

```c
int spdk_mempool_get_bulk(struct spdk_mempool *mp,
                          void **ele_arr,
                          size_t count);

void spdk_mempool_put_bulk(struct spdk_mempool *mp,
                           void **ele_arr,
                           size_t count);
```

**설명**: Batch variants for getting/returning multiple elements at once.
`get_bulk` returns 0 on success, -1 if there are fewer than `count` elements
available (no partial fill).

---

### Memory Flags Reference

| Flag                  | Value  | Meaning                                |
|-----------------------|--------|----------------------------------------|
| `SPDK_MALLOC_DMA`     | `0x01` | Memory is DMA-capable (IOVA-mapped)    |
| `SPDK_MALLOC_SHARE`   | `0x02` | Memory shared across process boundaries|
| `SPDK_ENV_NUMA_ID_ANY`| `-1`   | Allocate on any NUMA node              |

---

## 8. JSON-RPC

**헤더**: `spdk/rpc.h`, `spdk/jsonrpc.h`

SPDK's management plane is exposed through a JSON-RPC 2.0 server. Applications
register method handlers with `SPDK_RPC_REGISTER`; the framework dispatches
incoming requests to the appropriate handler.

---

### `SPDK_RPC_REGISTER` (macro)

```c
#define SPDK_RPC_REGISTER(method, func, state_mask)
```

**설명**: Registers `func` as the handler for the JSON-RPC method named
`method`. The registration happens at program startup via a constructor attribute.
`state_mask` restricts when the method is available:
- `SPDK_RPC_STARTUP` — available only during startup (before app is running).
- `SPDK_RPC_RUNTIME` — available only after full initialization.
- `SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME` — always available.

**Handler Signature**:
```c
void my_rpc_handler(struct spdk_jsonrpc_request *request,
                    const struct spdk_json_val *params);
```

**사용법**:
```c
static void rpc_get_version(struct spdk_jsonrpc_request *request,
                             const struct spdk_json_val *params)
{
    struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_string(w, "1.0");
    spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_version", rpc_get_version, SPDK_RPC_RUNTIME)
```

---

### `spdk_jsonrpc_begin_result`

```c
struct spdk_json_write_ctx *spdk_jsonrpc_begin_result(
    struct spdk_jsonrpc_request *request);
```

**설명**: Begins writing the result field of a JSON-RPC response.
Returns a write context that accepts JSON values. Must be paired with
`spdk_jsonrpc_end_result`.

**반환값**: JSON write context pointer.

---

### `spdk_jsonrpc_end_result`

```c
void spdk_jsonrpc_end_result(struct spdk_jsonrpc_request *request,
                              struct spdk_json_write_ctx *w);
```

**설명**: Finalizes and sends the JSON-RPC response. After this call
`request` and `w` are invalid.

**반환값**: void

---

### `spdk_jsonrpc_send_bool_response`

```c
void spdk_jsonrpc_send_bool_response(struct spdk_jsonrpc_request *request,
                                     bool value);
```

**설명**: Convenience wrapper that sends a JSON-RPC result of `true`
or `false`. Equivalent to `begin_result` + `write_bool` + `end_result`.

**반환값**: void

---

### `spdk_jsonrpc_send_error_response`

```c
void spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request,
                                      int error_code,
                                      const char *msg);
```

**설명**: Sends a JSON-RPC error response with the given error code and
message string. Standard error codes are defined in `spdk/jsonrpc.h`
(e.g., `SPDK_JSONRPC_ERROR_INVALID_PARAMS`).

**반환값**: void

---

### `spdk_rpc_register_method`

```c
void spdk_rpc_register_method(const char *method,
                               spdk_rpc_method_handler func,
                               uint32_t state_mask);
```

**설명**: Programmatic (non-macro) method registration. Prefer the
`SPDK_RPC_REGISTER` macro for static registrations; use this function only
when the method name or handler is determined at runtime.

**반환값**: void

---

### `spdk_rpc_server_listen`

```c
struct spdk_rpc_server *spdk_rpc_server_listen(const char *listen_addr);
```

**설명**: Starts an RPC server listening on the given address (UNIX
domain socket path or `ip:port`). The app framework calls this automatically
using `opts.rpc_addr`; direct use is needed only for secondary RPC servers.

**반환값**: Server handle, or NULL on failure.

---

### `spdk_rpc_server_accept`

```c
void spdk_rpc_server_accept(struct spdk_rpc_server *server);
```

**설명**: Accepts and processes pending RPC connections. Must be called
repeatedly from a poller when managing the server manually.

**반환값**: void

---

### `spdk_rpc_set_state`

```c
void spdk_rpc_set_state(uint32_t state_mask);
```

**설명**: Transitions the RPC server's state. Methods whose `state_mask`
includes the new state become active. The app framework calls this after
initialization is complete to enable `SPDK_RPC_RUNTIME` methods.

**반환값**: void

---

### RPC Error Codes

| Constant                              | Value   | Meaning                      |
|---------------------------------------|---------|------------------------------|
| `SPDK_JSONRPC_ERROR_PARSE_ERROR`      | `-32700`| Malformed JSON               |
| `SPDK_JSONRPC_ERROR_INVALID_REQUEST`  | `-32600`| Not a valid JSON-RPC request |
| `SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND` | `-32601`| Unknown method               |
| `SPDK_JSONRPC_ERROR_INVALID_PARAMS`   | `-32602`| Bad parameters               |
| `SPDK_JSONRPC_ERROR_INTERNAL_ERROR`   | `-32603`| Internal server error        |

---

## 9. 주요 타입과 콜백

### Callback Signatures Summary

```c
/* Application */
typedef void (*spdk_msg_fn)(void *ctx);
typedef void (*spdk_app_shutdown_cb)(void);

/* Poller */
typedef int  (*spdk_poller_fn)(void *ctx);
/* Must return SPDK_POLLER_BUSY or SPDK_POLLER_IDLE */

/* Bdev */
typedef void (*spdk_bdev_event_cb_t)(enum spdk_bdev_event_type type,
                                      struct spdk_bdev *bdev,
                                      void *event_ctx);
typedef void (*spdk_bdev_io_completion_cb)(struct spdk_bdev_io *bdev_io,
                                           bool success,
                                           void *cb_arg);

/* NVMe */
typedef bool (*spdk_nvme_probe_cb)(void *cb_ctx,
                                    const struct spdk_nvme_transport_id *trid,
                                    struct spdk_nvme_ctrlr_opts *opts);
typedef void (*spdk_nvme_attach_cb)(void *cb_ctx,
                                     const struct spdk_nvme_transport_id *trid,
                                     struct spdk_nvme_ctrlr *ctrlr,
                                     const struct spdk_nvme_ctrlr_opts *opts);
typedef void (*spdk_nvme_cmd_cb)(void *ctx,
                                  const struct spdk_nvme_cpl *cpl);

/* JSON-RPC */
typedef void (*spdk_rpc_method_handler)(struct spdk_jsonrpc_request *request,
                                         const struct spdk_json_val *params);
```

---

### Poller Return Values

```c
enum spdk_thread_poller_rc {
    SPDK_POLLER_IDLE,   /* No work was done this iteration */
    SPDK_POLLER_BUSY,   /* Work was done; do not sleep    */
};
```

---

### Bdev Event Types

```c
enum spdk_bdev_event_type {
    SPDK_BDEV_EVENT_REMOVE,           /* Device is being removed     */
    SPDK_BDEV_EVENT_RESIZE,           /* Device capacity has changed */
    SPDK_BDEV_EVENT_MEDIA_MANAGEMENT, /* Media event (e.g., error)   */
};
```

---

### NVMe Transport ID

```c
/* Defined in spdk/nvme_spec.h, used throughout spdk/nvme.h */
struct spdk_nvme_transport_id {
    char trstring[SPDK_NVMF_TRSVCID_MAX_LEN]; /* "PCIE", "TCP", "RDMA" */
    enum spdk_nvme_transport_type trtype;
    enum spdk_nvmf_adrfam adrfam;              /* Address family        */
    char traddr[SPDK_NVMF_TRADDR_MAX_LEN];    /* PCI addr or IP        */
    char trsvcid[SPDK_NVMF_TRSVCID_MAX_LEN];  /* Port number           */
    char subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];   /* Subsystem NQN         */
    /* ... */
};
```

---

### Header File Index

| Subsystem             | Primary Header         | Secondary Headers                  |
|-----------------------|------------------------|------------------------------------|
| Application Framework | `spdk/event.h`         | `spdk/init.h`                      |
| Thread / Poller       | `spdk/thread.h`        | `spdk/cpuset.h`                    |
| Block Device (Bdev)   | `spdk/bdev.h`          | `spdk/bdev_module.h`               |
| NVMe Driver           | `spdk/nvme.h`          | `spdk/nvme_spec.h`, `spdk/nvme_zns.h` |
| NVMe-oF Target        | `spdk/nvmf.h`          | `spdk/nvmf_spec.h`, `spdk/nvmf_transport.h` |
| Memory / Environment  | `spdk/env.h`           | —                                  |
| JSON-RPC              | `spdk/rpc.h`           | `spdk/jsonrpc.h`, `spdk/json.h`    |

---

*This reference is generated from SPDK source headers. Always consult the
authoritative Doxygen documentation (`make doxygen`) for the most complete
and up-to-date information.*

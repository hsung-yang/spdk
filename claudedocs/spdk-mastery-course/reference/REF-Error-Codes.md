# SPDK Error Code Reference

**Source files**: `include/spdk/bdev_module.h`, `include/spdk/nvme_spec.h`, `include/spdk/string.h`

---

## Table of Contents

1. [Bdev I/O Status Codes](#1-bdev-io-status-codes)
2. [NVMe Status Code Types (SCT)](#2-nvme-status-code-types-sct)
3. [NVMe Generic Command Status Codes](#3-nvme-generic-command-status-codes)
4. [NVMe Command-Specific Status Codes](#4-nvme-command-specific-status-codes)
5. [NVMe Media Error Status Codes](#5-nvme-media-error-status-codes)
6. [NVMe Path Status Codes](#6-nvme-path-status-codes)
7. [POSIX errno Values Used in SPDK](#7-posix-errno-values-used-in-spdk)
8. [Error Utility Functions](#8-error-utility-functions)
9. [Common Error Scenarios](#9-common-error-scenarios)
10. [Quick Lookup Table](#10-quick-lookup-table)
11. [Debugging Tips by Error Category](#11-debugging-tips-by-error-category)

---

## 1. Bdev I/O Status Codes

Defined in `include/spdk/bdev_module.h` as `enum spdk_bdev_io_status`.

These values appear in the `status` field of `struct spdk_bdev_io` and are passed to the
bdev I/O completion callback (`spdk_bdev_io_completion_cb`).

| Constant | Value | Meaning |
|---|---|---|
| `SPDK_BDEV_IO_STATUS_SUCCESS` | `1` | I/O completed successfully |
| `SPDK_BDEV_IO_STATUS_PENDING` | `0` | I/O is still in progress (not a final status) |
| `SPDK_BDEV_IO_STATUS_FAILED` | `-1` | Generic failure — use when no more specific code applies |
| `SPDK_BDEV_IO_STATUS_NVME_ERROR` | `-2` | NVMe-level error; check `bdev_io->internal.error.nvme` for SCT/SC |
| `SPDK_BDEV_IO_STATUS_SCSI_ERROR` | `-3` | SCSI-level error; check `bdev_io->internal.error.scsi` for sense data |
| `SPDK_BDEV_IO_STATUS_NOMEM` | `-4` | Insufficient resources (DMA buffers, queue depth, etc.) — I/O will be retried automatically |
| `SPDK_BDEV_IO_STATUS_MISCOMPARE` | `-5` | Data miscompare during a compare-and-write or verify operation |
| `SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED` | `-6` | First command of a fused operation failed |
| `SPDK_BDEV_IO_STATUS_ABORTED` | `-7` | I/O was explicitly aborted (e.g., queue drain, reset) |
| `SPDK_BDEV_IO_STATUS_AIO_ERROR` | `-8` | Linux AIO operation returned an error |
| `SPDK_MIN_BDEV_IO_STATUS` | `-8` | Sentinel — lowest valid status value |

### Key Rules

- Values `<= 0` that are not `PENDING` represent errors.
- `NOMEM` is **not** a permanent failure. The bdev layer retries the I/O automatically after
  other I/Os on the same channel complete and free resources. Bdev modules must not return
  `NOMEM` for `RESET` type I/Os.
- `NVME_ERROR` and `SCSI_ERROR` carry additional detail in the `bdev_io->internal.error`
  union. Always inspect that union before logging a generic error message.
- `ABORTED` is the expected status when calling `spdk_bdev_abort()` on in-flight I/Os.

### Completion Callback Pattern

```c
static void
my_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    if (!success) {
        enum spdk_bdev_io_status status = bdev_io->internal.status;
        switch (status) {
        case SPDK_BDEV_IO_STATUS_NVME_ERROR:
            /* Inspect bdev_io->internal.error.nvme.sct and .sc */
            break;
        case SPDK_BDEV_IO_STATUS_NOMEM:
            /* Should not reach here — bdev layer retries automatically */
            break;
        case SPDK_BDEV_IO_STATUS_ABORTED:
            /* Expected during reset or shutdown */
            break;
        default:
            break;
        }
    }
    spdk_bdev_free_io(bdev_io);
}
```

---

## 2. NVMe Status Code Types (SCT)

Defined in `include/spdk/nvme_spec.h` as `enum spdk_nvme_status_code_type`.

The SCT field is 3 bits in the NVMe completion queue entry status field and selects which
namespace of status codes the SC field belongs to.

| Constant | Value | Description |
|---|---|---|
| `SPDK_NVME_SCT_GENERIC` | `0x0` | Generic command status — applies to all commands |
| `SPDK_NVME_SCT_COMMAND_SPECIFIC` | `0x1` | Status specific to the issued command type |
| `SPDK_NVME_SCT_MEDIA_ERROR` | `0x2` | Media and data integrity errors |
| `SPDK_NVME_SCT_PATH` | `0x3` | Path-related errors (multi-path, ANA) |
| `SPDK_NVME_SCT_VENDOR_SPECIFIC` | `0x7` | Vendor-defined status codes |

### Completion Checking Macros

```c
/* True if completion contains any error */
spdk_nvme_cpl_is_error(cpl)

/* True if completion is successful */
spdk_nvme_cpl_is_success(cpl)

/* True if error is a media/integrity error */
spdk_nvme_cpl_is_media_error(cpl)

/* True if error is a path-related error */
spdk_nvme_cpl_is_path_error(cpl)

/* True if error indicates namespace not ready */
spdk_nvme_cpl_is_ns_not_ready(cpl)
```

A completion is considered successful only when `sct == SPDK_NVME_SCT_GENERIC` and
`sc == SPDK_NVME_SC_SUCCESS`. Any other combination is an error.

---

## 3. NVMe Generic Command Status Codes

Defined as `enum spdk_nvme_generic_command_status_code` (`SCT = 0x0`).

### Success

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_SUCCESS` | `0x00` | Command completed without error |

### Command Parameter Errors

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_INVALID_OPCODE` | `0x01` | Opcode is not supported or reserved |
| `SPDK_NVME_SC_INVALID_FIELD` | `0x02` | A command field contains an invalid value |
| `SPDK_NVME_SC_COMMAND_ID_CONFLICT` | `0x03` | CID is already in use in the submission queue |
| `SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT` | `0x0b` | NSID is invalid or namespace format is unsupported |
| `SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR` | `0x0c` | Command issued out of required sequence |
| `SPDK_NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED` | `0x14` | Write crosses atomic boundary |
| `SPDK_NVME_SC_OPERATION_DENIED` | `0x15` | Operation not permitted by policy or reservation |

### Data Transfer and SGL Errors

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_DATA_TRANSFER_ERROR` | `0x04` | Host-to-device or device-to-host data transfer failed |
| `SPDK_NVME_SC_INVALID_SGL_SEG_DESCRIPTOR` | `0x0d` | SGL segment descriptor is invalid |
| `SPDK_NVME_SC_INVALID_NUM_SGL_DESCIRPTORS` | `0x0e` | Number of SGL descriptors exceeds limit |
| `SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID` | `0x0f` | Data SGL length does not match transfer length |
| `SPDK_NVME_SC_METADATA_SGL_LENGTH_INVALID` | `0x10` | Metadata SGL length is invalid |
| `SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID` | `0x11` | SGL descriptor type not supported |
| `SPDK_NVME_SC_INVALID_CONTROLLER_MEM_BUF` | `0x12` | Controller memory buffer address is invalid |
| `SPDK_NVME_SC_INVALID_PRP_OFFSET` | `0x13` | PRP entry offset is invalid |
| `SPDK_NVME_SC_INVALID_SGL_OFFSET` | `0x16` | SGL offset is not supported |
| `SPDK_NVME_SC_SGL_DATA_BLOCK_GRANULARITY_INVALID` | `0x1e` | SGL data block granularity violation |

### Abort and Power Events

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_ABORTED_POWER_LOSS` | `0x05` | Command aborted due to power loss notification |
| `SPDK_NVME_SC_INTERNAL_DEVICE_ERROR` | `0x06` | Internal device error; no data transferred |
| `SPDK_NVME_SC_ABORTED_BY_REQUEST` | `0x07` | Command aborted by an Abort command |
| `SPDK_NVME_SC_ABORTED_SQ_DELETION` | `0x08` | Command aborted because its SQ was deleted |
| `SPDK_NVME_SC_ABORTED_FAILED_FUSED` | `0x09` | Fused command aborted because the other half failed |
| `SPDK_NVME_SC_ABORTED_MISSING_FUSED` | `0x0a` | Fused command aborted because companion was missing |
| `SPDK_NVME_SC_ABORTED_PREEMPT` | `0x1b` | Command aborted due to reservation preempt-and-abort |
| `SPDK_NVME_SC_COMMAND_INTERRUPTED` | `0x21` | Command was interrupted |

### Fabric / Keep-Alive / Host

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_HOSTID_INCONSISTENT_FORMAT` | `0x18` | Host Identifier format is inconsistent |
| `SPDK_NVME_SC_KEEP_ALIVE_EXPIRED` | `0x19` | Keep-alive timer expired |
| `SPDK_NVME_SC_KEEP_ALIVE_INVALID` | `0x1a` | Keep-alive timer value is invalid or not supported |
| `SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR` | `0x22` | Transient transport error; may succeed on retry |
| `SPDK_NVME_SC_COMMAND_PROHIBITED_BY_LOCKDOWN` | `0x23` | Command not permitted in current lockdown mode |
| `SPDK_NVME_SC_ADMIN_COMMAND_MEDIA_NOT_READY` | `0x24` | Admin command issued but media not ready |

### Sanitize

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_SANITIZE_FAILED` | `0x1c` | Sanitize operation failed |
| `SPDK_NVME_SC_SANITIZE_IN_PROGRESS` | `0x1d` | Sanitize in progress; most commands are blocked |

### Namespace / Capacity Errors (SC >= 0x80)

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_LBA_OUT_OF_RANGE` | `0x80` | LBA exceeds the size of the namespace |
| `SPDK_NVME_SC_CAPACITY_EXCEEDED` | `0x81` | Namespace capacity exceeded (thin provisioning) |
| `SPDK_NVME_SC_NAMESPACE_NOT_READY` | `0x82` | Namespace not ready; retry after a short delay |
| `SPDK_NVME_SC_RESERVATION_CONFLICT` | `0x83` | Reservation held by another host conflicts |
| `SPDK_NVME_SC_FORMAT_IN_PROGRESS` | `0x84` | Format NVM in progress on this namespace |

### FDP (Flexible Data Placement)

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_FDP_DISABLED` | `0x29` | FDP is not enabled on this namespace |
| `SPDK_NVME_SC_INVALID_PLACEMENT_HANDLE_LIST` | `0x2a` | Placement handle list is invalid |

---

## 4. NVMe Command-Specific Status Codes

Defined as `enum spdk_nvme_command_specific_status_code` (`SCT = 0x1`).

These apply only to specific admin or I/O commands. The SC value meaning depends on which
opcode was submitted.

### Queue Management (Create/Delete I/O SQ/CQ)

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_COMPLETION_QUEUE_INVALID` | `0x00` | Specified CQ does not exist |
| `SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER` | `0x01` | Queue ID is invalid or already in use |
| `SPDK_NVME_SC_INVALID_QUEUE_SIZE` | `0x02` | Queue size exceeds controller maximum |
| `SPDK_NVME_SC_ABORT_COMMAND_LIMIT_EXCEEDED` | `0x03` | Number of outstanding aborts exceeded |
| `SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED` | `0x05` | AER limit reached |
| `SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR` | `0x08` | Interrupt vector is invalid |
| `SPDK_NVME_SC_INVALID_QUEUE_DELETION` | `0x0c` | Cannot delete queue in current state |

### Firmware and Log

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_INVALID_FIRMWARE_SLOT` | `0x06` | Firmware slot number is invalid |
| `SPDK_NVME_SC_INVALID_FIRMWARE_IMAGE` | `0x07` | Firmware image failed validation |
| `SPDK_NVME_SC_INVALID_LOG_PAGE` | `0x09` | Log page identifier is not supported |
| `SPDK_NVME_SC_FIRMWARE_REQ_CONVENTIONAL_RESET` | `0x0b` | Firmware activation requires conventional reset |
| `SPDK_NVME_SC_FIRMWARE_REQ_NVM_RESET` | `0x10` | Firmware activation requires NVM subsystem reset |
| `SPDK_NVME_SC_FIRMWARE_REQ_RESET` | `0x11` | Firmware activation requires controller reset |
| `SPDK_NVME_SC_FIRMWARE_REQ_MAX_TIME_VIOLATION` | `0x12` | Firmware activation would exceed time limit |
| `SPDK_NVME_SC_FIRMWARE_ACTIVATION_PROHIBITED` | `0x13` | Firmware activation not permitted at this time |

### Feature Management

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_INVALID_FORMAT` | `0x0a` | Format NVM parameters are invalid |
| `SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE` | `0x0d` | Feature ID does not support Save |
| `SPDK_NVME_SC_FEATURE_NOT_CHANGEABLE` | `0x0e` | Feature is not changeable |
| `SPDK_NVME_SC_FEATURE_NOT_NAMESPACE_SPECIFIC` | `0x0f` | Feature applies to controller, not namespace |

### Namespace Management

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_OVERLAPPING_RANGE` | `0x14` | Directive range overlaps an existing range |
| `SPDK_NVME_SC_NAMESPACE_INSUFFICIENT_CAPACITY` | `0x15` | NVM capacity insufficient for new namespace |
| `SPDK_NVME_SC_NAMESPACE_ID_UNAVAILABLE` | `0x16` | No NSID available for allocation |
| `SPDK_NVME_SC_NAMESPACE_ALREADY_ATTACHED` | `0x18` | Namespace is already attached to this controller |
| `SPDK_NVME_SC_NAMESPACE_IS_PRIVATE` | `0x19` | Namespace is private and cannot be shared |
| `SPDK_NVME_SC_NAMESPACE_NOT_ATTACHED` | `0x1a` | Namespace is not attached to this controller |
| `SPDK_NVME_SC_THINPROVISIONING_NOT_SUPPORTED` | `0x1b` | Thin provisioning not supported |
| `SPDK_NVME_SC_CONTROLLER_LIST_INVALID` | `0x1c` | Controller list has an invalid entry |
| `SPDK_NVME_SC_NAMESPACE_ATTACH_LIMIT_EXCEEDED` | `0x27` | Too many controllers attached to namespace |

### ANA (Asymmetric Namespace Access)

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_ANA_GROUP_IDENTIFIER_INVALID` | `0x24` | ANA Group ID is invalid |
| `SPDK_NVME_SC_ANA_ATTACH_FAILED` | `0x25` | ANA attach failed |

### I/O Command Specific

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_CONFLICTING_ATTRIBUTES` | `0x80` | Conflicting attributes in command |
| `SPDK_NVME_SC_INVALID_PROTECTION_INFO` | `0x81` | Protection information settings are invalid |
| `SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE` | `0x82` | Write attempted to a read-only LBA range |

### Zoned Namespace (ZNS)

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_ZONED_BOUNDARY_ERROR` | `0xb8` | Write crosses zone boundary |
| `SPDK_NVME_SC_ZONE_IS_FULL` | `0xb9` | Target zone is full |
| `SPDK_NVME_SC_ZONE_IS_READ_ONLY` | `0xba` | Target zone is read-only |
| `SPDK_NVME_SC_ZONE_IS_OFFLINE` | `0xbb` | Target zone is offline |
| `SPDK_NVME_SC_ZONE_INVALID_WRITE` | `0xbc` | Write address does not match zone write pointer |
| `SPDK_NVME_SC_TOO_MANY_ACTIVE_ZONES` | `0xbd` | Active zone limit exceeded |
| `SPDK_NVME_SC_TOO_MANY_OPEN_ZONES` | `0xbe` | Open zone limit exceeded |
| `SPDK_NVME_SC_INVALID_ZONE_STATE_TRANSITION` | `0xbf` | Requested zone state transition is not allowed |

---

## 5. NVMe Media Error Status Codes

Defined as `enum spdk_nvme_media_error_status_code` (`SCT = 0x2`).

These indicate data integrity or physical media failures. They are the most critical category
from a data durability perspective.

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_WRITE_FAULTS` | `0x80` | Write operation encountered unrecoverable physical faults |
| `SPDK_NVME_SC_UNRECOVERED_READ_ERROR` | `0x81` | Read data is unrecoverable (uncorrectable ECC) |
| `SPDK_NVME_SC_GUARD_CHECK_ERROR` | `0x82` | Guard (CRC) check of end-to-end data failed |
| `SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR` | `0x83` | Application tag in protection information mismatched |
| `SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR` | `0x84` | Reference tag in protection information mismatched |
| `SPDK_NVME_SC_COMPARE_FAILURE` | `0x85` | Compare command data mismatch (not a media error per se) |
| `SPDK_NVME_SC_ACCESS_DENIED` | `0x86` | Access to the LBA range is denied |
| `SPDK_NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK` | `0x87` | Read returned deallocated or unwritten logical block data |
| `SPDK_NVME_SC_END_TO_END_STORAGE_TAG_CHECK_ERROR` | `0x88` | Storage tag check error in end-to-end protection |

### Media Error Handling Note

Media errors in `SPDK_NVME_SC_UNRECOVERED_READ_ERROR` or `SPDK_NVME_SC_WRITE_FAULTS`
typically indicate failing NAND. The drive may report the location via an AER (Asynchronous
Event Request). In SPDK, use `spdk_nvme_cpl_is_media_error()` to quickly test for this
category before inspecting SC.

---

## 6. NVMe Path Status Codes

Defined as `enum spdk_nvme_path_status_code` (`SCT = 0x3`).

Path errors appear in multi-path (ANA) configurations or when the fabric transport fails.

| Constant | SC | Description |
|---|---|---|
| `SPDK_NVME_SC_INTERNAL_PATH_ERROR` | `0x00` | General internal path error |
| `SPDK_NVME_SC_ASYMMETRIC_ACCESS_PERSISTENT_LOSS` | `0x01` | ANA state is Persistent Loss — path is permanently unavailable |
| `SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE` | `0x02` | ANA state is Inaccessible — retry on a different path |
| `SPDK_NVME_SC_ASYMMETRIC_ACCESS_TRANSITION` | `0x03` | ANA state is transitioning — retry after state stabilises |
| `SPDK_NVME_SC_CONTROLLER_PATH_ERROR` | `0x60` | Error in the controller path |
| `SPDK_NVME_SC_HOST_PATH_ERROR` | `0x70` | Error in the host path (e.g., fabric transport) |
| `SPDK_NVME_SC_ABORTED_BY_HOST` | `0x71` | Command aborted by the host (host-initiated) |

Use `spdk_nvme_cpl_is_path_error()` to test for `SCT = 0x3` before inspecting SC.

---

## 7. POSIX errno Values Used in SPDK

SPDK API functions return negative errno values (`-errno`) on failure. These follow the
standard Linux conventions but the most commonly encountered values in SPDK code are listed
below.

| errno | Value | Macro | Meaning in SPDK context |
|---|---|---|---|
| `EINVAL` | `22` | `-EINVAL` | Invalid argument — bad parameter, unsupported config, or format error |
| `ENOMEM` | `12` | `-ENOMEM` | Out of memory — huge page allocation failed, pool exhausted |
| `ENODEV` | `19` | `-ENODEV` | No such device — NVMe controller not found, device detached |
| `EBUSY` | `16` | `-EBUSY` | Device or resource busy — queue full, reactor not idle, device resetting |
| `EEXIST` | `17` | `-EEXIST` | Already exists — duplicate bdev name, duplicate endpoint |
| `ENOTSUP` | `95` | `-ENOTSUP` | Not supported — feature not implemented for this backend |
| `EIO` | `5` | `-EIO` | I/O error — transport-level failure, vhost socket error |
| `EAGAIN` | `11` | `-EAGAIN` | Try again — resource temporarily unavailable, back-pressure |
| `ETIMEDOUT` | `110` | `-ETIMEDOUT` | Timed out — NVMe admin command timeout, keep-alive expiry |
| `ENOENT` | `2` | `-ENOENT` | No such file or directory — configuration file not found |
| `EPERM` | `1` | `-EPERM` | Operation not permitted — insufficient privilege |
| `ERANGE` | `34` | `-ERANGE` | Value out of range — LBA range check, buffer too small |
| `EFAULT` | `14` | `-EFAULT` | Bad address — null pointer, invalid DMA mapping |
| `EACCES` | `13` | `-EACCES` | Permission denied — vfio group access, hugepage mmap |
| `ENOSPC` | `28` | `-ENOSPC` | No space left — thin provisioning capacity hit |

### Decoding errno in SPDK

```c
int rc = spdk_some_api_call(...);
if (rc != 0) {
    SPDK_ERRLOG("API failed: %s\n", spdk_strerror(-rc));
    /* or thread-safe version: */
    char errbuf[128];
    spdk_strerror_r(-rc, errbuf, sizeof(errbuf));
    SPDK_ERRLOG("API failed: %s\n", errbuf);
}
```

### Typical errno Return Patterns

```c
/* From lib/event/reactor.c */
if (core >= RTE_MAX_LCORE) {
    return -EINVAL;   /* caller passed bad CPU mask */
}
if (reactor == NULL) {
    return -ENOMEM;   /* rte_zmalloc returned NULL */
}
if (reactor->in_interrupt) {
    return -EBUSY;    /* reactor is in interrupt mode */
}
if (!spdk_interrupt_mode_is_enabled()) {
    return -ENOTSUP;  /* feature disabled at runtime */
}
```

---

## 8. Error Utility Functions

### `spdk_strerror()` / `spdk_strerror_r()`

Declared in `include/spdk/string.h`.

```c
/* Returns a pointer to a thread-local string — not thread-safe across threads */
const char *spdk_strerror(int errnum);

/* Thread-safe version — writes into caller-provided buffer */
void spdk_strerror_r(int errnum, char *buf, size_t buflen);
```

Pass the **positive** errno value (e.g., `spdk_strerror(ENOMEM)` or `spdk_strerror(-rc)`
when `rc` is negative).

### `spdk_nvme_cpl_get_status_string()`

Declared in `include/spdk/nvme.h`.

```c
const char *spdk_nvme_cpl_get_status_string(const struct spdk_nvme_status *status);
```

Returns a human-readable string for an NVMe completion status. Useful for logging:

```c
if (spdk_nvme_cpl_is_error(&cpl)) {
    SPDK_ERRLOG("NVMe error: %s\n",
                spdk_nvme_cpl_get_status_string(&cpl.status));
}
```

### Completion Checking Macros (from `nvme_spec.h`)

```c
/* Primary error check — true if any error */
spdk_nvme_cpl_is_error(cpl)          /* sct != 0 || sc != 0 */

/* Inverse */
spdk_nvme_cpl_is_success(cpl)

/* Category checks */
spdk_nvme_cpl_is_media_error(cpl)    /* SCT == 0x2 */
spdk_nvme_cpl_is_path_error(cpl)     /* SCT == 0x3 */
spdk_nvme_cpl_is_ns_not_ready(cpl)   /* SCT=0, SC=0x82 */
```

---

## 9. Common Error Scenarios

### Scenario 1: NVMe Device Not Found at Probe

**Symptom**: `spdk_nvme_probe()` returns 0 but no controller is attached.

**Related codes**: `-ENODEV`, probe callback never fires.

**Causes**:
- PCI address string is malformed (triggers `-EINVAL` in transport layer).
- VFIO not configured — device still bound to kernel driver.
- Device was hot-removed between probe and attach.

**Debug**:
```bash
# Verify VFIO binding
ls /sys/bus/pci/devices/<BDF>/driver
# Should show vfio-pci, not nvme
```

---

### Scenario 2: Bdev I/O Returns NOMEM

**Symptom**: I/O completion callback receives `success = false`,
`bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM`.

**Causes**:
- DMA-safe memory pool (`spdk_mempool`) is exhausted.
- NVMe queue pair is full (submission queue depth exceeded).
- Too many concurrent I/Os for the configured queue depth.

**Resolution**: This is automatically handled — the bdev layer queues the I/O and retries
it after in-flight I/Os complete. If `NOMEM` is persistent, increase queue depth
(`num_io_queues`, `queue_depth` in bdev NVMe config) or reduce application concurrency.

---

### Scenario 3: NVMe Keep-Alive Expired (NVMf)

**Symptom**: `SPDK_NVME_SC_KEEP_ALIVE_EXPIRED` (SC `0x19`, SCT `0x0`) in completion or
host-side disconnection.

**Causes**:
- NVMf target became overloaded; polling loop stalled.
- Keep-alive timeout (`-k` in NVMf connect) set too low for the RTT.
- SPDK reactor was paused for too long (e.g., huge page allocation contention).

**Debug**:
```bash
# Check target reactor utilization
rpc.py framework_get_reactors
# Look for reactors near 100% busy
```

---

### Scenario 4: LBA Out of Range

**Symptom**: `SPDK_NVME_SC_LBA_OUT_OF_RANGE` (SC `0x80`, SCT `0x0`).

**Causes**:
- Application computed wrong offset — off-by-one past namespace end.
- Namespace was re-sized after device was opened.
- Stale cached `num_blocks` value.

**Debug**: Call `spdk_nvme_ns_get_num_sectors()` to get current namespace size.
Always validate: `start_lba + num_blocks <= spdk_nvme_ns_get_num_sectors(ns)`.

---

### Scenario 5: Zoned Namespace Write Pointer Mismatch

**Symptom**: `SPDK_NVME_SC_ZONE_INVALID_WRITE` (SC `0xbc`, SCT `0x1`).

**Causes**:
- Write LBA does not match the zone write pointer (SLBA must equal WP).
- Multiple writers competing on the same zone without synchronization.
- Zone not in Open or Implicit Open state.

**Debug**: Issue a `Zone Management Receive` command (or use `spdk_nvme_zns_report_zones()`)
to read the current write pointer for the affected zone.

---

### Scenario 6: Reactor Returns `-ENOTSUP`

**Symptom**: `spdk_env_thread_launch_master()` or scheduler API returns `-ENOTSUP`.

**Causes**:
- Interrupt mode requested but not compiled in.
- Scheduler type not registered (plugin not loaded).
- Feature requires DPDK version not present.

**Resolution**: Check `spdk_interrupt_mode_is_enabled()` before calling interrupt-mode APIs.
Verify all required shared libraries are loaded.

---

### Scenario 7: ANA Path Error on NVMf

**Symptom**: Path error completion `SCT = 0x3`, `SC = 0x02`
(`SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE`).

**Causes**:
- Active-Optimized path failed; I/O was routed to an Inaccessible path.
- Controller has not yet reported the new ANA state.

**Resolution**: The NVMe multipath layer should automatically retry on an Optimized path.
In SPDK, `spdk_nvme_cpl_is_path_error()` returns true; the caller should failover to
another `qpair` on a different controller.

---

## 10. Quick Lookup Table

### By Symptom

| Symptom | Likely Code | Category |
|---|---|---|
| I/O silently fails with no data | `SPDK_NVME_SC_INTERNAL_DEVICE_ERROR` (0x06) | Generic |
| Write rejected — wrong LBA | `SPDK_NVME_SC_LBA_OUT_OF_RANGE` (0x80) | Generic |
| Read returns zeros unexpectedly | `SPDK_NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK` (0x87) | Media |
| Data corruption detected | `SPDK_NVME_SC_GUARD_CHECK_ERROR` (0x82) | Media |
| I/O retried automatically | `SPDK_BDEV_IO_STATUS_NOMEM` (-4) | Bdev |
| I/O cancelled during reset | `SPDK_BDEV_IO_STATUS_ABORTED` (-7) | Bdev |
| Linux AIO failure | `SPDK_BDEV_IO_STATUS_AIO_ERROR` (-8) | Bdev |
| Device not found | `-ENODEV` | errno |
| Bad API parameter | `-EINVAL` | errno |
| Pool/memory exhausted | `-ENOMEM` | errno |
| Feature not available | `-ENOTSUP` | errno |
| Device resetting | `-EBUSY` | errno |
| Zone write pointer mismatch | `SPDK_NVME_SC_ZONE_INVALID_WRITE` (0xbc) | Cmd-specific |
| Namespace not attached | `SPDK_NVME_SC_NAMESPACE_NOT_ATTACHED` (0x1a) | Cmd-specific |
| Fabric disconnection | `SPDK_NVME_SC_KEEP_ALIVE_EXPIRED` (0x19) | Generic |
| Multi-path failover needed | `SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE` (0x02) | Path |

### By NVMe SC Value (Common Subset)

| SC (hex) | SCT | Constant |
|---|---|---|
| `0x00` | Generic | `SPDK_NVME_SC_SUCCESS` |
| `0x01` | Generic | `SPDK_NVME_SC_INVALID_OPCODE` |
| `0x02` | Generic | `SPDK_NVME_SC_INVALID_FIELD` |
| `0x04` | Generic | `SPDK_NVME_SC_DATA_TRANSFER_ERROR` |
| `0x06` | Generic | `SPDK_NVME_SC_INTERNAL_DEVICE_ERROR` |
| `0x07` | Generic | `SPDK_NVME_SC_ABORTED_BY_REQUEST` |
| `0x19` | Generic | `SPDK_NVME_SC_KEEP_ALIVE_EXPIRED` |
| `0x1c` | Generic | `SPDK_NVME_SC_SANITIZE_FAILED` |
| `0x80` | Generic | `SPDK_NVME_SC_LBA_OUT_OF_RANGE` |
| `0x82` | Generic | `SPDK_NVME_SC_NAMESPACE_NOT_READY` |
| `0x80` | Media | `SPDK_NVME_SC_WRITE_FAULTS` |
| `0x81` | Media | `SPDK_NVME_SC_UNRECOVERED_READ_ERROR` |
| `0x82` | Media | `SPDK_NVME_SC_GUARD_CHECK_ERROR` |
| `0x87` | Media | `SPDK_NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK` |
| `0x00` | Path | `SPDK_NVME_SC_INTERNAL_PATH_ERROR` |
| `0x02` | Path | `SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE` |
| `0xbc` | Cmd | `SPDK_NVME_SC_ZONE_INVALID_WRITE` |

---

## 11. Debugging Tips by Error Category

### Bdev I/O Status Errors

1. **Always inspect the specific status value**, not just the `success` boolean. The boolean
   is `false` for all errors; the status code tells you which one.

2. **For `NVME_ERROR`**, always read both `sct` and `sc` from
   `bdev_io->internal.error.nvme`. Log both fields and call
   `spdk_nvme_cpl_get_status_string()`.

3. **For `SCSI_ERROR`**, read the sense key, ASC, and ASCQ from
   `bdev_io->internal.error.scsi`. SCSI errors are only relevant for bdev modules that
   implement a SCSI backend (e.g., virtio-scsi, iSCSI).

4. **Enable SPDK trace** for a detailed timeline:
   ```bash
   spdk_trace_record -s <shm_id> -o trace.out
   spdk_trace -f trace.out | grep BDEV
   ```

### NVMe Generic Errors

5. **`INVALID_FIELD` (0x02)** almost always means a driver bug — a field in the NVMe
   command struct was set to an unsupported value. Enable `SPDK_LOG_NVMe` at DEBUG level
   to log raw command structures.

6. **`NAMESPACE_NOT_READY` (0x82)** is transient. Back off and retry. If persistent,
   check if a Format NVM or Sanitize is in progress.

7. **`KEEP_ALIVE_EXPIRED` (0x19)** on an NVMf connection means the fabric RTT or target
   processing latency exceeded the KAT. Tune with `spdk_nvme_ctrlr_cmd_set_feature()`
   `KEEP_ALIVE_TIMER` or pass `-k <ms>` to `nvme connect`.

### Media Errors

8. **Media errors are device-side permanent failures**. Log the NSID and LBA range
   immediately. Do not retry media errors — retries will return the same result and wear
   flash unnecessarily.

9. **`DEALLOCATED_OR_UNWRITTEN_BLOCK` (0x87)** is not a failure — it is the device
   reporting that a deallocated block was read and a deterministic value returned
   (controlled by the `DLFEAT` field in the Identify Namespace data). Check
   `spdk_nvme_ns_get_dealloc_logical_block_fill_value()`.

### Path Errors

10. **Path errors should trigger failover**, not be surfaced as I/O errors to the
    application. In SPDK's bdev_nvme multipath module, path errors cause the I/O to be
    retried on the next available path automatically.

11. **`ASYMMETRIC_ACCESS_INACCESSIBLE`** means the ANA group is in Inaccessible state —
    a controller change is in progress. Poll `spdk_nvme_ctrlr_get_ana_log_page()` until
    an Optimized path is available.

### POSIX errno

12. **`-ENOMEM` from API init functions** usually means hugepage allocation failed. Check
    that `--huge-unlink` is not set and that enough 2 MB or 1 GB pages are reserved:
    ```bash
    cat /proc/meminfo | grep HugePage
    ```

13. **`-EINVAL` from bdev or NVMe APIs** — enable `SPDK_LOG` at DEBUG level. SPDK logs
    the specific invalid field before returning `-EINVAL` in most code paths.

14. **`-EBUSY` from reactor APIs** — the reactor is in interrupt mode or currently
    processing. Use `spdk_thread_send_msg()` to schedule work on the correct thread
    rather than calling reactor APIs from an arbitrary context.

15. **`-ENOTSUP`** indicates a compile-time or runtime feature gate. Check
    `./configure --help` for the relevant feature flag (e.g., `--with-rdma`,
    `--with-fio-plugin`).

---

*Reference built from SPDK source: `include/spdk/bdev_module.h`, `include/spdk/nvme_spec.h`,
`include/spdk/nvme.h`, `include/spdk/string.h`, and representative samples from `lib/`.*

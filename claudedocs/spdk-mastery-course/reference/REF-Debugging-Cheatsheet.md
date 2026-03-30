# SPDK Debugging Cheatsheet

**Course**: SPDK Mastery
**Reference**: REF-Debugging-Cheatsheet
**Audience**: SPDK developers and system integrators

---

## Table of Contents

1. [GDB Quick Reference](#1-gdb-quick-reference)
2. [Log Levels and Log Flags](#2-log-levels-and-log-flags)
3. [Trace Point Usage](#3-trace-point-usage)
4. [Common Error Messages and Causes](#4-common-error-messages-and-causes)
5. [Memory Debugging](#5-memory-debugging)
6. [Core Dump Analysis](#6-core-dump-analysis)
7. [Performance Debugging](#7-performance-debugging)
8. [Quick Diagnosis Flowchart](#8-quick-diagnosis-flowchart)
9. [Useful RPC Commands](#9-useful-rpc-commands)
10. [Environment Variables](#10-environment-variables)

---

## 1. GDB Quick Reference

### Build with Debug Symbols

```bash
# Configure with debug symbols (disables optimizations)
./configure --enable-debug

# Or add to an existing build
make CFLAGS="-g -O0"

# Confirm debug info is embedded
file build/bin/spdk_tgt
readelf -S build/bin/spdk_tgt | grep debug
```

### Attach GDB to a Running SPDK Process

```bash
# Find the PID
pgrep -a spdk_tgt

# Attach (requires ptrace permissions or root)
gdb -p <PID>

# Or launch directly
gdb --args ./build/bin/spdk_tgt -c spdk.json

# Inside GDB: run until first useful stop
(gdb) set follow-fork-mode child
(gdb) set detach-on-fork off
(gdb) run
```

### Thread Inspection

SPDK runs one reactor thread per CPU core. Each reactor is a pthread.

```gdb
# List all threads
(gdb) info threads

# Switch to a specific thread
(gdb) thread 3

# Print backtrace for every thread
(gdb) thread apply all bt

# Print backtrace for every thread (compact, one frame per line)
(gdb) thread apply all bt 1

# Find the reactor threads by name
(gdb) thread find reactor
```

### Breakpoints Inside Pollers

SPDK pollers run in tight loops. Breaking inside them freezes the whole reactor.

```gdb
# Set a conditional breakpoint to avoid flooding
(gdb) break nvmf_poll_group_poll if g_num_aborts > 0

# Break in a specific source file
(gdb) break lib/nvmf/tcp.c:nvmf_tcp_poll_group_poll

# Break on a specific NVMe command opcode
(gdb) break nvmf_request_exec if cmd->opc == SPDK_NVME_OPC_READ

# Disable a breakpoint without deleting it
(gdb) disable 2

# One-shot breakpoint (auto-deleted after first hit)
(gdb) tbreak spdk_bdev_io_complete
```

### Inspecting SPDK Structures

```gdb
# Print a spdk_bdev_io structure
(gdb) p *bdev_io

# Print the I/O channel list for a bdev
(gdb) p *spdk_io_channel

# Walk a TAILQ (example: pending I/Os)
(gdb) p *((struct spdk_bdev_io *)bdev->internal.io_stat)

# Pretty-print a structure
(gdb) set print pretty on
(gdb) p *channel

# Find where a pointer lives (heap vs stack vs BSS)
(gdb) info symbol 0xADDRESS
```

### Watchpoints

```gdb
# Watch a variable for any write
(gdb) watch bdev->internal.status

# Watch a memory address range
(gdb) watch -l buf[0]

# Read watchpoint (fires on read)
(gdb) rwatch channel->ref_count
```

### Useful GDB Init File (~/.gdbinit for SPDK)

```
set print pretty on
set print array on
set print array-indexes on
set pagination off
set confirm off
handle SIGTERM nostop noprint
# Avoid stopping on DPDK signals
handle SIGRTMIN nostop noprint pass
handle SIGRTMIN+1 nostop noprint pass
```

---

## 2. Log Levels and Log Flags

### Log Level Hierarchy

| Level | Enum Constant | Meaning |
|-------|--------------|---------|
| -1 | `SPDK_LOG_DISABLED` | All messages suppressed |
| 0 | `SPDK_LOG_ERROR` | Fatal and non-recoverable errors |
| 1 | `SPDK_LOG_WARN` | Recoverable problems, degraded state |
| 2 | `SPDK_LOG_NOTICE` | Normal significant events (default) |
| 3 | `SPDK_LOG_INFO` | Detailed subsystem information |
| 4 | `SPDK_LOG_DEBUG` | Per-request / per-poll debug output |

### Log Macros in Source Code

```c
/* Always printed at the given level */
SPDK_ERRLOG("Failed to allocate IO channel: %d\n", rc);
SPDK_WARNLOG("Queue depth %u exceeds recommended limit\n", qd);
SPDK_NOTICELOG("NVMe-oF target started on %s\n", addr);

/* Gated by a named flag (must be enabled at startup or via RPC) */
SPDK_INFOLOG(nvmf, "Connection accepted from %s\n", addr_str);
SPDK_DEBUGLOG(bdev, "bdev_io %p submitted to %s\n", bdev_io, bdev->name);

/* Binary dump gated by a flag */
SPDK_LOGDUMP(nvme, "NVMe command", cmd_buf, sizeof(*cmd));

/* Rate-limited error (at most once per second) */
SPDK_ERRLOG_RATELIMIT("CRC mismatch on LBA %" PRIu64 "\n", lba);
```

### Registering a Log Flag in a Subsystem

```c
/* In your .c file — one per translation unit */
SPDK_LOG_REGISTER_COMPONENT(my_module)

/* Then use it */
SPDK_DEBUGLOG(my_module, "Processing request %p\n", req);
```

> `SPDK_DEBUGLOG` and `SPDK_LOGDUMP` are compiled out unless the binary is built with `--enable-debug`. `SPDK_INFOLOG` is always compiled in but gated by the flag's runtime state.

### Startup: Enabling Log Flags via CLI

```bash
# Enable a single flag
spdk_tgt -c spdk.json --logflag nvmf

# Enable multiple flags
spdk_tgt -c spdk.json --logflag nvmf --logflag bdev

# Enable all flags (very verbose)
spdk_tgt -c spdk.json --logflag all

# Set the minimum log level to DEBUG
spdk_tgt -c spdk.json -L debug --logflag nvme
```

### Runtime: Changing Log Level via RPC

```bash
# Raise log level to DEBUG at runtime
scripts/rpc.py log_set_level DEBUG

# Lower back to NOTICE
scripts/rpc.py log_set_level NOTICE

# Enable a log flag at runtime
scripts/rpc.py log_set_flag nvmf

# Disable a log flag
scripts/rpc.py log_clear_flag nvmf

# List all available flags and their current state
scripts/rpc.py log_get_flags

# Query current log level
scripts/rpc.py log_get_level
```

### Redirecting Log Output

```bash
# Log to a file (--msg-mempool-size avoids losing early messages)
spdk_tgt -c spdk.json 2>spdk.log

# Use syslog
spdk_tgt -c spdk.json --use-syslog spdk_tgt

# Tail live output with timestamps
tail -f spdk.log | ts '[%Y-%m-%d %H:%M:%.S]'
```

---

## 3. Trace Point Usage

### Architecture Overview

SPDK tracepoints use a lock-free shared-memory ring buffer (`/dev/shm/_trace.<app_name>.<pid>`). Each entry records:
- 64-bit TSC timestamp
- Tpoint ID (group × 64 + index)
- Owner ID (e.g., NVMe-oF connection number)
- Object ID (e.g., bdev_io pointer)
- Up to 8 bytes of inline arguments

### Enabling Trace at Startup

```bash
# Enable all tracepoints for nvmf group
spdk_tgt -c spdk.json --num-trace-entries 131072

# The shared memory file appears at:
ls /dev/shm/_trace.*
```

### Enabling / Disabling at Runtime via RPC

```bash
# List all tpoint groups
scripts/rpc.py trace_get_tpoint_group_mask

# Enable a group by name
scripts/rpc.py trace_set_tpoint_group_mask --tpoint-group-mask nvmf_tcp

# Enable individual tpoints by numeric mask
scripts/rpc.py trace_set_tpoint_group_mask --tpoint-group-mask 0x4

# Disable all tracing
scripts/rpc.py trace_set_tpoint_group_mask --tpoint-group-mask 0x0
```

### Collecting a Trace Snapshot

```bash
# Dump from the live shared memory into a file
spdk_trace -s spdk_tgt -p <PID> -f trace_output.bin

# Or dump the shm directly
cp /dev/shm/_trace.spdk_tgt.<PID> trace_output.bin
```

### Analyzing with spdk_trace

```bash
# Print human-readable timeline (stdout)
build/bin/spdk_trace -f trace_output.bin

# Filter to a single object (e.g., one bdev_io)
build/bin/spdk_trace -f trace_output.bin -o 0xADDRESS

# Filter by owner (connection/channel ID)
build/bin/spdk_trace -f trace_output.bin -w 3

# Show only events in a time window (TSC units)
build/bin/spdk_trace -f trace_output.bin -b 1000000 -e 5000000

# JSON output for external tooling
build/bin/spdk_trace -f trace_output.bin --json > trace.json
```

### Adding a Custom Tracepoint

```c
#include "spdk/trace.h"

/* Define the group and tpoint IDs once per module */
#define TRACE_GROUP_MY_MOD  15
#define TRACE_MY_MOD_START  SPDK_TPOINT_ID(TRACE_GROUP_MY_MOD, 0)
#define TRACE_MY_MOD_DONE   SPDK_TPOINT_ID(TRACE_GROUP_MY_MOD, 1)

/* Record a tracepoint with one integer argument */
spdk_trace_record(TRACE_MY_MOD_START, 0, 0, (uint64_t)req, req->length);
```

---

## 4. Common Error Messages and Causes

### NVMe / bdev Layer

| Error Message | Likely Cause | Fix |
|--------------|-------------|-----|
| `NVMe read/write error: SC 0x02` | Medium error (bad block) | Check drive SMART; replace or reallocate |
| `Timeout occurred for CID xx` | Controller unresponsive or reset pending | Increase `nvme_timeout_ms`; check PCIe link |
| `bdev_io_split failed - no memory` | IO split pool exhausted | Increase `--bdev-io-pool-size` |
| `bdev_get_io_channel failed` | Max channels per bdev reached | Reduce `num_shared_buffers` or increase limit |
| `Failed to allocate large buffer` | Hugepage memory exhausted | Add `-m` hugepage flag or reduce queue depth |

### NVMe-oF Target

| Error Message | Likely Cause | Fix |
|--------------|-------------|-----|
| `Could not create CQ/SQ, out of resources` | RDMA MR or CQ exhausted | Increase `num_shared_buffers`; check ulimits |
| `Poll group poller exited abnormally` | Unhandled exception in poller | Check full backtrace; often a null deref |
| `transport_create failed` | Port/address already in use | Check `ss -tlnp`; kill conflicting process |
| `Subsystem not found` | NQN mismatch or subsystem not added | Verify `nvmf_get_subsystems` RPC output |
| `Connect request rejected: FABRICS error 0x02` | Invalid host NQN or no allow-hostnqn | Add host NQN to subsystem allowed list |

### DPDK / Hugepages / Memory

| Error Message | Likely Cause | Fix |
|--------------|-------------|-----|
| `Cannot get hugepage information` | Hugepages not configured in kernel | `echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages` |
| `Cannot mmap memory for rte_config` | Stale `/dev/hugepages` files from crashed run | `rm /dev/hugepages/rte_*; rm /dev/shm/spdk_*` |
| `EAL: No NUMA socket memory allocated` | NUMA mismatch between NIC/NVMe and memory | Pin hugepages to the correct NUMA node |
| `Memory hotplug is not supported` | RTE version incompatibility | Ensure DPDK version matches SPDK build |

### Reactor / Thread

| Error Message | Likely Cause | Fix |
|--------------|-------------|-----|
| `Reactor X is not responding` | Infinite loop or blocking syscall in poller | Profile with `perf top -p <PID>`; find stuck function |
| `spdk_thread_poll: deferred msgs overflow` | Deferred message queue exhausted | Increase `msg_mempool_size` |
| `Cannot run without DPDK EAL threads` | CPU mask misconfigured | Check `-c` CPU mask covers available cores |

---

## 5. Memory Debugging

### AddressSanitizer (ASan) Build

```bash
# Configure with ASan
./configure --enable-asan

make -j$(nproc)

# Run with ASan options
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:log_path=/tmp/asan \
  ./build/bin/spdk_tgt -c spdk.json

# Suppress known DPDK false positives
export LSAN_OPTIONS=suppressions=/path/to/spdk/test/common/asan.supp
```

ASan detects: heap buffer overflows, use-after-free, stack overflows, use-after-return, memory leaks.

> ASan is incompatible with hugepages in some configurations. Use `--no-huge --iova-mode=va` during ASan testing.

### UndefinedBehaviorSanitizer

```bash
./configure --enable-ubsan
make -j$(nproc)

UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ./build/bin/spdk_tgt -c spdk.json
```

### Valgrind

Valgrind is slower but works without recompilation.

```bash
# Basic memcheck (heap errors and leaks)
valgrind --tool=memcheck \
         --leak-check=full \
         --track-origins=yes \
         --suppressions=test/common/valgrind.supp \
         ./build/bin/spdk_tgt -c spdk.json --no-huge

# Massif heap profiler
valgrind --tool=massif \
         --pages-as-heap=yes \
         ./build/bin/spdk_tgt -c spdk.json --no-huge
ms_print massif.out.<PID> | less
```

### Writing a Valgrind Suppression

```
{
   dpdk_mmap_suppression
   Memcheck:Addr8
   fun:rte_eal_init
   ...
}
```

Save to `my_suppressions.supp` and pass `--suppressions=my_suppressions.supp`.

### SPDK Memory Pool Introspection via RPC

```bash
# Dump memory pool utilization
scripts/rpc.py spdk_get_memory_pools

# Check bdev buffer pool
scripts/rpc.py bdev_get_iostat
```

---

## 6. Core Dump Analysis

### Enable Core Dumps

```bash
# Unlimited core size for current session
ulimit -c unlimited

# Persist across reboots (add to /etc/security/limits.conf)
echo "* soft core unlimited" >> /etc/security/limits.conf
echo "* hard core unlimited" >> /etc/security/limits.conf

# Control where cores are written (systemd systems)
echo "/tmp/core.%e.%p.%t" > /proc/sys/kernel/core_pattern

# For systemd-coredump, retrieve with:
coredumpctl list
coredumpctl gdb <PID>
```

### Load Core with GDB

```bash
# Load binary + core file
gdb ./build/bin/spdk_tgt /tmp/core.spdk_tgt.12345.1711800000

# Inside GDB
(gdb) bt               # backtrace of crashed thread
(gdb) info threads     # list all threads at time of crash
(gdb) thread 2         # switch to thread 2
(gdb) bt full          # full backtrace with local variables
(gdb) frame 3          # select stack frame 3
(gdb) info locals      # print all locals in frame
(gdb) p *bdev_io       # dereference a pointer
```

### Core Analysis Checklist

1. Run `bt full` on the crashed thread — identify the faulting function and line.
2. Run `thread apply all bt` — check if other threads are in suspect states.
3. Examine locals and arguments in the top few frames.
4. Check for NULL dereference: the address in a SIGSEGV is usually printed by GDB as `0x0` or a small offset.
5. Check for stack corruption: if `bt` shows garbage frames, the stack was overwritten — look for buffer overflows with ASan.
6. Correlate with the last SPDK log lines before the crash.

### Extracting the Log Buffer from a Core

If logging was redirected to stderr/file, cross-reference `dmesg` and your log file with the core's timestamp.

```bash
# GDB can print the in-memory log ring if you know the symbol
(gdb) p spdk_log_get_print_level()
```

---

## 7. Performance Debugging

### spdk_top — Live Reactor Utilization

`spdk_top` provides a top-like view of SPDK reactor threads, pollers, and I/O channels.

```bash
# Start spdk_top (connects via RPC socket)
scripts/spdk_top.py

# With a specific RPC socket
scripts/spdk_top.py -s /var/tmp/spdk.sock

# Useful key bindings inside spdk_top:
#   r   - sort by reactor utilization
#   p   - sort by poller busy count
#   c   - show per-channel I/O stats
#   q   - quit
```

Output columns:
- **Busy %** — fraction of poll cycles where the poller found work
- **Idle %** — fraction of cycles with no work (poller returned 0)
- **Poller count** — number of active pollers on the reactor

### Reactor Utilization via RPC

```bash
# Get reactor statistics
scripts/rpc.py framework_get_reactors | python3 -m json.tool

# Check thread stats (poller counts, msg queue depths)
scripts/rpc.py thread_get_stats | python3 -m json.tool

# Poll group stats for NVMe-oF
scripts/rpc.py nvmf_get_stats | python3 -m json.tool
```

### I/O Statistics

```bash
# Per-bdev IOPS, bandwidth, latency
scripts/rpc.py bdev_get_iostat

# Continuous polling (1-second interval)
watch -n1 'scripts/rpc.py bdev_get_iostat | python3 -m json.tool'

# NVMe controller stats
scripts/rpc.py bdev_nvme_get_controller_health_info --name Nvme0
```

### Linux perf for Hotspot Analysis

```bash
# Record 30 seconds of CPU samples for a running SPDK process
perf record -F 99 -p <PID> -g -- sleep 30
perf report --sort comm,dso,sym

# flamegraph (requires Brendan Gregg's FlameGraph scripts)
perf script | stackcollapse-perf.pl | flamegraph.pl > spdk_flame.svg

# Count cache misses
perf stat -e cache-misses,cache-references,instructions,cycles \
  -p <PID> sleep 10
```

### Latency Histogram

```bash
# Use SPDK's built-in histogram (if compiled in)
scripts/rpc.py bdev_enable_histogram --name Nvme0n1 --enable true

# After a workload, retrieve histogram
scripts/rpc.py bdev_get_histogram --name Nvme0n1
```

### Common Performance Anti-Patterns

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| One reactor at 100%, others idle | Unbalanced bdev/namespace assignment | Distribute namespaces across CPUs |
| High I/O latency variance | Memory copy on I/O path | Use zero-copy with `--enable-rdma` or DIF |
| Low IOPS despite low CPU | Queue depth too small | Increase `qd` in config; check `num_io_queues` |
| CPU busy but no I/O progress | Spinlock contention | Profile with `perf`; check lock ordering |
| Memory bandwidth saturated | Multiple threads accessing same NUMA node | Pin threads and memory to same NUMA |

---

## 8. Quick Diagnosis Flowchart

```
SYMPTOM: SPDK process crashes immediately
    |
    +-- Check: Is there a core file?
    |       Yes --> Section 6: Core Dump Analysis
    |       No  --> ulimit -c unlimited and reproduce
    |
    +-- Check: Any SPDK_ERRLOG lines?
            Yes --> Section 4: Common Error Messages
            No  --> Enable --logflag all, reproduce

SYMPTOM: SPDK process hangs / stops processing I/O
    |
    +-- Check: Any reactor showing 100% utilization?
    |       Yes --> Infinite loop in poller (perf top -p <PID>)
    |       No  --> Dead reactor (no work dispatched)
    |
    +-- Check: RPC still responsive?
            Yes --> scripts/rpc.py thread_get_stats (find stuck thread)
            No  --> GDB attach, thread apply all bt

SYMPTOM: Low I/O performance
    |
    +-- Check: spdk_top reactor busy %
    |       < 50% --> I/O not reaching reactor (check submission path)
    |       ~ 100% --> CPU bound (perf profile, check queue depth)
    |
    +-- Check: bdev_get_iostat latency
            High average --> congestion, check queue depth / # CPUs
            High p99/p999 --> periodic stall (GC, GDB, jitter)

SYMPTOM: NVMe-oF clients cannot connect
    |
    +-- Check: Is the transport listening?
    |       scripts/rpc.py nvmf_get_transports
    |
    +-- Check: Is the subsystem present?
    |       scripts/rpc.py nvmf_get_subsystems
    |
    +-- Check: Is the host NQN allowed?
            Look for "Connect request rejected" in log

SYMPTOM: Memory errors / corrupted data
    |
    +-- Rebuild with --enable-asan, reproduce
    +-- Run with valgrind --tool=memcheck
    +-- Check hugepage configuration (stale /dev/hugepages files?)
```

---

## 9. Useful RPC Commands for Debugging

All commands use `scripts/rpc.py` (default socket: `/var/tmp/spdk.sock`).

```bash
# Custom socket path
scripts/rpc.py -s /var/tmp/my_app.sock <command>

# JSON output for scripting
scripts/rpc.py --json <command>
```

### Framework / Reactor

| Command | Purpose |
|---------|---------|
| `framework_get_reactors` | List reactors, their CPU cores, and thread assignments |
| `thread_get_stats` | Per-thread poller counts, I/O channel counts, msg queue depth |
| `thread_get_pollers` | List all active pollers on each thread |
| `framework_get_config` | Dump current JSON configuration |
| `spdk_kill_instance` | Send a signal to the SPDK process (e.g., `SIGTERM`) |

### Block Devices (bdev)

| Command | Purpose |
|---------|---------|
| `bdev_get_bdevs` | List all block devices with their configuration |
| `bdev_get_iostat` | IOPS, bandwidth, and latency per bdev |
| `bdev_enable_histogram` | Enable latency histograms on a bdev |
| `bdev_get_histogram` | Retrieve histogram data |
| `bdev_nvme_get_controllers` | List NVMe controllers |
| `bdev_nvme_get_controller_health_info` | SMART data for an NVMe controller |

### NVMe-oF Target

| Command | Purpose |
|---------|---------|
| `nvmf_get_transports` | List active transports (TCP, RDMA, etc.) |
| `nvmf_get_subsystems` | List subsystems, namespaces, and allowed hosts |
| `nvmf_get_stats` | Poll group and connection statistics |
| `nvmf_subsystem_get_controllers` | List connected controllers per subsystem |

### Logging

| Command | Purpose |
|---------|---------|
| `log_get_level` | Get current log level |
| `log_set_level` | Set log level (ERROR/WARN/NOTICE/INFO/DEBUG) |
| `log_get_flags` | List all flags and their enabled state |
| `log_set_flag` | Enable a log flag at runtime |
| `log_clear_flag` | Disable a log flag at runtime |

### Tracing

| Command | Purpose |
|---------|---------|
| `trace_get_tpoint_group_mask` | Get current tpoint group mask |
| `trace_set_tpoint_group_mask` | Enable/disable trace groups |

### iSCSI / Vhost (when applicable)

| Command | Purpose |
|---------|---------|
| `iscsi_get_connections` | List active iSCSI connections |
| `iscsi_get_options` | Show iSCSI configuration |
| `vhost_get_controllers` | List vhost controllers |

---

## 10. Environment Variables for Debugging

### DPDK / EAL Variables

| Variable | Effect | Example |
|----------|--------|---------|
| `RTE_LOG_LEVEL` | Set DPDK internal log verbosity (0=emerg … 8=debug) | `RTE_LOG_LEVEL=8` |
| `RTE_SDK` | Override DPDK SDK path | `RTE_SDK=/opt/dpdk` |
| `RTE_TARGET` | Override DPDK build target | `RTE_TARGET=x86_64-native-linuxapp-gcc` |

### ASan / Sanitizer Variables

| Variable | Effect | Example |
|----------|--------|---------|
| `ASAN_OPTIONS` | AddressSanitizer behavior | `detect_leaks=1:abort_on_error=1` |
| `LSAN_OPTIONS` | Leak sanitizer (subset of ASan) | `suppressions=/path/to/asan.supp` |
| `UBSAN_OPTIONS` | UBSan behavior | `print_stacktrace=1:halt_on_error=1` |
| `TSAN_OPTIONS` | ThreadSanitizer behavior | `halt_on_error=1:log_path=/tmp/tsan` |

### SPDK-Specific Variables

| Variable | Effect | Example |
|----------|--------|---------|
| `HUGEMEM` | Hugepage memory size in MB passed to setup scripts | `HUGEMEM=4096` |
| `NRHUGE` | Number of 2 MB hugepages (used by `setup.sh`) | `NRHUGE=1024` |
| `PCI_ALLOWED` | Restrict NVMe device binding to specific PCI addresses | `PCI_ALLOWED=0000:01:00.0` |
| `PCI_BLOCKED` | Block specific PCI devices from being bound | `PCI_BLOCKED=0000:02:00.0` |
| `SPDK_COREDUMP_PATH` | Override default core dump path | `SPDK_COREDUMP_PATH=/data/cores` |

### Linux Kernel Debug Variables

| Variable / Path | Effect |
|-----------------|--------|
| `/proc/sys/kernel/core_pattern` | Core dump file naming pattern |
| `/proc/sys/kernel/perf_event_paranoid` | `echo -1` to allow perf without root |
| `/proc/sys/vm/nr_hugepages` | Set 2 MB hugepage count |
| `/proc/sys/vm/nr_overcommit_hugepages` | Additional hugepages usable by SPDK |
| `/sys/bus/pci/devices/<BDF>/numa_node` | Check NUMA affinity of a PCIe device |

### Useful One-Liners

```bash
# Check hugepage allocation
cat /proc/meminfo | grep -i huge

# Show NUMA topology
numactl --hardware

# Check IOMMU is enabled (required for VFIO)
dmesg | grep -i iommu

# List VFIO-bound devices
ls /dev/vfio/

# Check current UIO/VFIO binding for NVMe devices
scripts/setup.sh status

# Verify DPDK device binding
dpdk-devbind.py --status

# Monitor SPDK process file descriptors
ls /proc/<PID>/fd | wc -l

# Watch hugepage consumption in real time
watch -n1 'grep -i huge /proc/meminfo'
```

---

## Summary Card

| Task | Command |
|------|---------|
| Enable debug logs | `--logflag all -L debug` |
| Change log level live | `scripts/rpc.py log_set_level DEBUG` |
| Dump trace | `spdk_trace -s spdk_tgt -p <PID> -f out.bin` |
| Analyze trace | `build/bin/spdk_trace -f out.bin` |
| Live reactor view | `scripts/spdk_top.py` |
| I/O stats | `scripts/rpc.py bdev_get_iostat` |
| List bdevs | `scripts/rpc.py bdev_get_bdevs` |
| List NVMe-oF subsystems | `scripts/rpc.py nvmf_get_subsystems` |
| Attach GDB | `gdb -p <PID>` |
| Backtrace all threads | `(gdb) thread apply all bt` |
| ASan build | `./configure --enable-asan && make` |
| Core dump analysis | `gdb ./build/bin/spdk_tgt /tmp/core.*` |
| Check hugepages | `grep -i huge /proc/meminfo` |
| Hotspot profiling | `perf record -F 99 -p <PID> -g -- sleep 30` |

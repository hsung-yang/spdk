# REF: SPDK Performance Tuning Guide

**Course**: SPDK Mastery
**Module**: Reference Materials
**Audience**: Engineers deploying SPDK in production or benchmarking environments

---

## Table of Contents

1. [Pre-Tuning Checklist](#1-pre-tuning-checklist)
2. [CPU Affinity Tuning](#2-cpu-affinity-tuning)
3. [Memory Tuning](#3-memory-tuning)
4. [I/O Tuning](#4-io-tuning)
5. [NVMe-oF Specific Tuning](#5-nvme-of-specific-tuning)
6. [Benchmarking Methodology](#6-benchmarking-methodology)
7. [Performance Metrics to Collect](#7-performance-metrics-to-collect)
8. [Common Performance Pitfalls and Fixes](#8-common-performance-pitfalls-and-fixes)
9. [Quick-Reference Parameter Table](#9-quick-reference-parameter-table)

---

## 1. Pre-Tuning Checklist

Before starting SPDK or running any benchmark, verify the following system-level settings. Skipping these steps is the single most common source of unexpectedly poor results.

### 1.1 BIOS Settings

| Setting | Required Value | Why It Matters |
|---|---|---|
| Hyper-Threading (HT) | Disabled (for latency), Enabled (for throughput) | HT shares L1/L2 cache between logical siblings; SPDK pollers fight for cache lines |
| CPU C-States | Disabled (C1E/C3/C6/C7 all off) | C-state wake latency adds 10-100 µs jitter to polling loops |
| Turbo Boost / Intel Speed Step | Disabled for repeatability, optional for throughput | Frequency variation makes benchmark runs non-comparable |
| NUMA Interleaving | Disabled | Allow SPDK to make explicit NUMA-local allocations |
| PCIe ASPM | Disabled | Active State Power Management adds latency on PCIe transactions |
| SR-IOV | Enabled if using VF passthrough | Required for NVMe-oF or vhost SR-IOV workloads |

```
# Verify C-states from Linux after boot
cat /sys/devices/system/cpu/cpu*/cpuidle/state*/disable
# All should show "1" (disabled) for C2 and above
```

### 1.2 Kernel Parameters

Add the following to `/etc/default/grub` `GRUB_CMDLINE_LINUX` and run `grub2-mkconfig`:

```
# Core isolation and NUMA
isolcpus=2-15,18-31          # Reserve cores for SPDK (adjust to your topology)
nohz_full=2-15,18-31         # Disable timer ticks on isolated cores
rcu_nocbs=2-15,18-31         # Move RCU callbacks off isolated cores
nosoftlockup                  # Disable soft lockup watchdog (interferes with polling)
intel_idle.max_cstate=0       # Disable Intel-specific C-states
processor.max_cstate=1        # Hard limit C-states at OS level
default_hugepagesz=1G         # Set 1G as default hugepage size
hugepagesz=1G
hugepages=32                  # Pre-allocate 32 x 1G pages (adjust to memory budget)
iommu=pt                      # Passthrough IOMMU for lowest DMA overhead
intel_iommu=on                # Enable IOMMU (required for VFIO)
```

Apply and reboot:
```bash
sudo grub2-mkconfig -o /boot/grub2/grub.cfg
sudo reboot
```

### 1.3 Hugepage Configuration

SPDK uses DPDK-style hugepages for all DMA buffers. Without pre-allocated hugepages, SPDK will fail to start.

```bash
# Check current hugepage allocation
cat /proc/meminfo | grep Huge

# Allocate 1G hugepages at runtime (loses on reboot)
echo 32 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages

# NUMA-aware allocation (recommended for multi-socket systems)
echo 16 > /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages
echo 16 > /sys/devices/system/node/node1/hugepages/hugepages-1048576kB/nr_hugepages

# Mount hugetlbfs if not already mounted
mount -t hugetlbfs nodev /mnt/huge

# Make allocation persistent via /etc/sysctl.conf
echo "vm.nr_hugepages = 32" >> /etc/sysctl.conf
```

**Rule of thumb**: Allocate at least 2x the memory SPDK will use for I/O buffers. A target serving 4 NVMe drives with queue depth 128 and 128 KB I/O size needs roughly:
```
4 drives x 128 QD x 128 KB = 64 MB minimum; allocate 4–8 GB to be safe.
```

### 1.4 CPU Isolation Verification

```bash
# Verify isolcpus took effect
cat /sys/devices/system/cpu/isolated

# Check no kernel threads are scheduled on isolated cores
ps -eLo psr,pid,comm | awk '$1 >= 2 && $1 <= 15'   # should be nearly empty

# Verify nohz_full
cat /sys/devices/system/cpu/nohz_full
```

### 1.5 NVMe Drive Pre-Checks

```bash
# Bind NVMe device to vfio-pci before starting SPDK
sudo scripts/setup.sh          # SPDK helper: unbinds from kernel driver, binds to vfio-pci

# Verify binding
lspci -k -d :0108              # NVMe class; driver should show vfio-pci

# Check PCIe link speed (Gen4 x4 = ~7 GB/s; verify you are not link-limited)
sudo lspci -vv -s <BDF> | grep -i "lnksta\|width"
```

---

## 2. CPU Affinity Tuning

### 2.1 The Reactor Mask

The `reactor_mask` (also exposed as `-m` / `--cpumask` on the CLI) tells SPDK which CPU cores to spawn reactors on. Every reactor runs a tight polling loop. Assigning reactors to the wrong cores is the most impactful configuration mistake.

**In `spdk_app_opts`** (programmatic):
```c
struct spdk_app_opts opts = {};
spdk_app_opts_init(&opts, sizeof(opts));
opts.reactor_mask = "0x3C";   // hex bitmask: cores 2, 3, 4, 5
```

**On the command line** (for bdevperf, nvmf_tgt, etc.):
```bash
./build/bin/nvmf_tgt -m 0x3C             # hex mask
./build/bin/nvmf_tgt -m [2-5]            # range notation
./build/bin/nvmf_tgt -m "[2,3,4,5]"      # list notation
```

**Format options** (defined in `doc/applications.md §cpu_mask`):
- Hex string: `0x1F07`
- Comma-separated: `[0,1,2,8-12]`
- All formats are case-insensitive

### 2.2 NUMA Awareness

SPDK performs NUMA-local allocation by default, but only if reactors are placed correctly.

```bash
# Check NVMe device NUMA node
cat /sys/bus/pci/devices/0000:01:00.0/numa_node

# Check NIC NUMA node (for NVMe-oF)
cat /sys/bus/pci/devices/0000:82:00.0/numa_node

# Rule: reactor_mask should cover cores local to both the NVMe device
# and the NIC on the same NUMA node.

# Example: NVMe on NUMA 0 (cores 0-15), NIC on NUMA 0
./build/bin/nvmf_tgt -m [2-7]            # cores 2-7 on NUMA 0: optimal

# Counter-example: NVMe on NUMA 0, reactors on NUMA 1
./build/bin/nvmf_tgt -m [18-23]          # cross-NUMA DMA: ~100 ns extra latency per I/O
```

**Check NUMA topology**:
```bash
numactl --hardware
lstopo --of ascii    # if hwloc is installed
```

### 2.3 isolcpus and SPDK Cores

Cores listed in `isolcpus` are invisible to the Linux scheduler. SPDK should use these cores for its reactors so that no kernel threads interrupt the polling loop.

```
# Kernel cmdline
isolcpus=2-15,18-31

# SPDK reactor_mask matches isolated cores
-m [2-15,18-31]
```

Leave at least 1–2 cores per socket for the OS, management threads, and NIC IRQ handling.

### 2.4 IRQ Affinity for NICs (NVMe-oF)

For NVMe-oF, the NIC should have its interrupt vectors pinned to cores adjacent to (but not the same as) the SPDK reactors:

```bash
# Find NIC IRQs
cat /proc/interrupts | grep <iface>

# Pin IRQs to core 1 (adjacent to SPDK cores 2-7)
echo 2 > /proc/irq/<IRQ_NUMBER>/smp_affinity_list

# Use the SPDK irq scripts if present
scripts/irq_set_affinity.sh <iface> <cpumask>
```

---

## 3. Memory Tuning

### 3.1 Hugepage Sizes: 2M vs 1G

| Hugepage Size | Pros | Cons | Best For |
|---|---|---|---|
| 2M (default) | Easier to allocate, flexible sizing | More TLB entries needed for large allocations | Development, small deployments |
| 1G | Single TLB entry per GB, lowest DMA overhead | Must be pre-allocated at boot, inflexible | Production NVMe-oF targets, high IOPS |

The kernel parameter `default_hugepagesz=1G hugepagesz=1G hugepages=N` pre-allocates 1G pages at boot, before memory fragmentation occurs.

### 3.2 SPDK Memory Size (`-s` / `mem_size`)

The `-s` option (maps to `spdk_app_opts.mem_size`) controls how much hugepage memory DPDK will register. Default is 0, which means DPDK uses all available hugepages.

```bash
# Limit SPDK to 8 GB of hugepage memory
./build/bin/nvmf_tgt -m [2-7] -s 8192    # value in MB

# In opts struct
opts.mem_size = 8192;   // MB
```

**Sizing guidance**:
- I/O buffer pool: `num_reactors x max_queue_depth x max_io_size`
- NVMe-oF transport buffers: `num_connections x in_capsule_data_size x 16`
- Add 20% headroom for metadata, control structures, and DPDK internals

### 3.3 NUMA-Local Memory Allocation

```bash
# Force DPDK to allocate per-socket (socket_mem in MB, comma-separated per socket)
# Passed via EAL options in SPDK config or --eal-args
./build/bin/nvmf_tgt -m [2-7] --eal-args="--socket-mem=4096,4096"

# Prefer single segment hugepages (reduces TLB pressure further)
# opts.hugepage_single_segments = true
```

### 3.4 I/O Buffer Pool Sizing

SPDK pre-allocates a pool of DMA-capable buffers at startup. The pool size directly limits achievable queue depth:

```bash
# In nvmf target: set via RPC after startup
rpc.py nvmf_set_config --io_unit_size 131072    # 128 KB I/O units
rpc.py bdev_nvme_set_options --io_queue_requests 512
```

If the buffer pool is exhausted, I/Os will queue internally and latency spikes. Watch for `buf_cache_count` dropping to zero in `bdev_get_bdevs` stats.

---

## 4. I/O Tuning

### 4.1 Queue Depth

Queue depth (QD) is the primary lever for IOPS throughput. Each NVMe namespace supports up to `io_queue_size` outstanding commands per queue.

**In `spdk_nvme_ctrlr_opts`** (programmatic):
```c
struct spdk_nvme_ctrlr_opts opts;
spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts));
opts.io_queue_size = 1024;       // commands per queue
opts.io_queue_requests = 2048;   // in-flight request objects (>= io_queue_size)
opts.num_io_queues = 4;          // number of I/O queue pairs per controller
```

**Benchmark sweep** to find optimal QD:
```bash
for qd in 1 2 4 8 16 32 64 128 256; do
    ./build/bin/spdk_nvme_perf -q $qd -o 4096 -w randread -t 30 -c 0x4 \
      -r "trtype:PCIe traddr:0000:01:00.0" 2>&1 | grep "IOPS\|Latency"
done
```

Typical NVMe SSD saturates at QD=32–64 for 4K random reads. Going higher adds latency without IOPS gain.

### 4.2 I/O Size

```bash
# 4K random read (max IOPS, tests controller queue)
./build/bin/spdk_nvme_perf -q 64 -o 4096 -w randread -t 60 -c 0x4 \
  -r "trtype:PCIe traddr:0000:01:00.0"

# 128K sequential read (max bandwidth, tests PCIe/NAND bandwidth)
./build/bin/spdk_nvme_perf -q 8 -o 131072 -w read -t 60 -c 0x4 \
  -r "trtype:PCIe traddr:0000:01:00.0"
```

**I/O size guidelines**:
- 4K: maximum IOPS, latency-sensitive workloads
- 16K–64K: mixed OLTP-style workloads
- 128K–512K: streaming, backup, sequential scan
- Align I/O size to the NVMe optimal I/O boundary (check `Optimal Write Size` from `nvme id-ns`)

### 4.3 Batch Count (I/O Completions)

SPDK processes completions in batches. The `--io-count` (`-N`) option in `spdk_nvme_perf` limits total I/Os per run; for continuous workloads leave it unset.

For the bdev layer, the completion batch size is controlled internally per poller iteration. Larger batch sizes improve throughput at the cost of tail latency:

```bash
# bdevperf: adjust queue depth to implicitly control batch behavior
./build/examples/bdevperf -q 256 -o 4096 -t 60 -w randread -m 0x4 \
  -b Nvme0n1
```

### 4.4 Write Back vs Write Through

For NVMe bdev with caching layers (OCF):
```bash
# Write-back: higher throughput, risk of data loss on crash
rpc.py bdev_ocf_create Cache0 wb NvmeCache0n1 NvmeCore0n1

# Write-through: safe, lower throughput
rpc.py bdev_ocf_create Cache0 wt NvmeCache0n1 NvmeCore0n1
```

---

## 5. NVMe-oF Specific Tuning

### 5.1 Poll Group Architecture

Each NVMe-oF poll group (`spdk_nvmf_poll_group`) runs on one reactor core and handles a set of connections. The number of poll groups equals the number of reactor cores assigned to NVMe-oF.

```
reactor_mask = [2,3,4,5]  →  4 poll groups created
                               connections distributed round-robin
```

```bash
# Verify poll group distribution at runtime
rpc.py nvmf_get_stats
# Look at "poll_groups" array: each entry shows thread name and connection count
```

**Sizing rule**: 1 reactor core per 4–8 NVMe namespaces, or 1 core per 4 Gbps of target bandwidth (TCP), or 1 core per 25 Gbps (RDMA).

### 5.2 Connection Distribution

Connections are assigned to poll groups at accept time, round-robin. To influence distribution:

```bash
# Create a dedicated subsystem with explicit CPU affinity
rpc.py nvmf_create_subsystem nqn.2024-01.io.spdk:cnode1 \
  --allow-any-host \
  --mn "SPDK Target"

# Add listener on specific transport
rpc.py nvmf_subsystem_add_listener nqn.2024-01.io.spdk:cnode1 \
  -t TCP -a 192.168.1.100 -s 4420
```

For RDMA, pin the RDMA completion queue threads to the same NUMA node as the HCA:
```bash
# Set RDMA transport options
rpc.py nvmf_create_transport -t RDMA \
  --max-queue-depth 128 \
  --max-io-size 131072 \
  --in-capsule-data-size 4096 \
  --num-shared-buffers 4096
```

### 5.3 Transport-Specific Options

**TCP Transport**:
```bash
rpc.py nvmf_create_transport -t TCP \
  --max-queue-depth 128 \
  --max-io-size 131072 \
  --in-capsule-data-size 8192 \   # inline data threshold; match expected I/O size for small I/Os
  --num-shared-buffers 8192 \     # shared buffer pool size
  --sock-priority 6               # socket priority (requires CAP_NET_ADMIN)
```

**RDMA Transport**:
```bash
rpc.py nvmf_create_transport -t RDMA \
  --max-queue-depth 128 \
  --max-io-size 131072 \
  --in-capsule-data-size 4096 \
  --num-shared-buffers 4096 \
  --max-srq-depth 4096            # shared receive queue depth
```

### 5.4 Acceptor Poll Rate

The acceptor thread rate controls how quickly new connections are accepted. Default is 10,000 µs (10 ms):

```bash
rpc.py nvmf_set_config --acceptor-poll-rate 1000    # 1 ms: faster connection setup
# Note: lower values consume more CPU on the acceptor thread
```

### 5.5 In-Capsule Data Threshold

`in_capsule_data_size` determines whether I/O data is sent inline with the NVMe command capsule (zero-copy path) or via a separate data transfer. Setting this equal to the expected small I/O size eliminates one round-trip:

```
Workload: 4K random read
Set in_capsule_data_size = 4096 for read responses (TCP)
Result: data returned in single capsule, no extra RDMA WRITE
```

---

## 6. Benchmarking Methodology

### 6.1 Tool Selection

| Tool | Best For | Transport Support |
|---|---|---|
| `spdk_nvme_perf` | NVMe local and NVMe-oF end-to-end latency/IOPS | PCIe, TCP, RDMA |
| `bdevperf` | Bdev layer overhead, bdev module comparison | All bdev types |
| `fio` + SPDK plugin | Realistic multi-job workloads, FIO job files | All bdev types via ioengine |

### 6.2 spdk_nvme_perf

```bash
# 4K random read, QD=64, 60-second run, latency histogram enabled
./build/bin/spdk_nvme_perf \
  -q 64 \
  -o 4096 \
  -w randread \
  -t 60 \
  -c 0x4 \
  -L \                             # enable latency tracking
  -r "trtype:PCIe traddr:0000:01:00.0"

# NVMe-oF TCP target
./build/bin/spdk_nvme_perf \
  -q 128 \
  -o 4096 \
  -w randread \
  -t 300 \
  -c 0xFF \
  -L \
  -r "trtype:TCP adrfam:IPv4 traddr:192.168.1.100 trsvcid:4420"

# With warmup period (10 seconds before measuring)
./build/bin/spdk_nvme_perf -q 64 -o 4096 -w randread -t 60 -a 10 \
  -r "trtype:PCIe traddr:0000:01:00.0"

# Key options summary:
#   -q  queue depth
#   -o  I/O size (bytes, or suffix: 4k, 128k)
#   -w  pattern: read, write, randread, randwrite, randrw
#   -M  read percentage for randrw (e.g., -M 70 = 70% read)
#   -t  duration in seconds
#   -c  core mask
#   -L  enable latency histogram
#   -a  warmup time in seconds
#   -r  transport specification
```

### 6.3 bdevperf

```bash
# Start bdevperf against a local NVMe bdev
sudo ./build/examples/bdevperf \
  -c /path/to/bdev.json \
  -m 0x4 \
  -z &                            # -z: wait for RPC to start tests

# Trigger test via RPC
sudo python3 examples/bdev/bdevperf/bdevperf.py perform_tests \
  -q 64 \
  -o 4096 \
  -t 60 \
  -w randread

# Using a config file (FIO-style)
cat > /tmp/bdevperf.conf << 'EOF'
[global]
filename=Nvme0n1
bs=4096
rw=randread
iodepth=64

[job0]
cpumask=0x4
EOF

sudo ./build/examples/bdevperf -c bdev.json -j /tmp/bdevperf.conf -m 0x4
```

### 6.4 FIO with SPDK Plugin

```bash
# Build SPDK with fio plugin support
./configure --with-fio=/path/to/fio/source
make

# FIO job file
cat > /tmp/spdk_fio.job << 'EOF'
[global]
ioengine=/path/to/spdk/build/fio/spdk_bdev
spdk_json_conf=/path/to/bdev.json
thread=1
group_reporting=1
direct=1
bs=4k
rw=randread
iodepth=64
time_based=1
runtime=60

[job0]
filename=Nvme0n1
cpus_allowed=2
EOF

sudo fio /tmp/spdk_fio.job
```

### 6.5 Benchmark Discipline

1. **Warmup**: Always run 10–30 seconds of warmup before collecting measurements. NVMe TLC drives have write caches that affect initial numbers.
2. **Steady state**: Run for at least 60 seconds; 300 seconds for storage-class workloads.
3. **Repeat**: Run each configuration 3+ times and report median.
4. **Isolation**: Stop all other workloads on the system. Check `mpstat -P ALL 1` to confirm CPU utilization is clean.
5. **Document**: Record BIOS version, kernel version, SPDK version, drive firmware, and all tuning parameters alongside results.
6. **Baseline first**: Always measure with kernel NVMe driver (`fio --ioengine=libaio`) before SPDK to establish a reference delta.

---

## 7. Performance Metrics to Collect

### 7.1 Primary Metrics

| Metric | Collection Method | Notes |
|---|---|---|
| IOPS | spdk_nvme_perf output, bdevperf RPC | Report as average over steady-state interval |
| Throughput (MB/s) | spdk_nvme_perf output | IOPS x block size |
| Mean latency (µs) | spdk_nvme_perf `-L` flag | Less useful than percentiles |
| P50 latency | spdk_nvme_perf histogram | Typical latency |
| P99 latency | spdk_nvme_perf histogram | Service-level agreement metric |
| P99.9 latency | spdk_nvme_perf histogram | Outlier detection |
| P99.99 latency | spdk_nvme_perf histogram | Tail latency for latency-critical apps |

`spdk_nvme_perf` with `-L` reports latency at the following cutoffs automatically:
`1%, 10%, 25%, 50%, 75%, 90%, 95%, 98%, 99%, 99.5%, 99.9%, 99.99%, 99.999%, 99.9999%, 99.99999%`

### 7.2 CPU Utilization

```bash
# Per-core utilization during test (run in separate terminal)
mpstat -P ALL 1 60

# SPDK reactor busy vs idle ratio
# Available via RPC during test
rpc.py framework_get_reactors
# Look for "busy" and "idle" TSC counts per reactor

# busy_tsc / (busy_tsc + idle_tsc) = CPU utilization
# Target: 95-100% busy on reactor cores (polling is the design intent)
# Warning: if < 80% busy with room for more IOPS, the bottleneck is elsewhere
```

### 7.3 SPDK-Specific Metrics

```bash
# Bdev I/O statistics
rpc.py bdev_get_iostat -b Nvme0n1
# Returns: bytes_read, bytes_written, num_read_ops, num_write_ops,
#          read_latency_ticks, write_latency_ticks

# NVMe-oF target statistics
rpc.py nvmf_get_stats
# Returns per poll-group: io_count, pending_data_buffer_count,
#                         transport-specific counters

# Thread statistics
rpc.py thread_get_stats
# Returns per-thread: idle_tsc, busy_tsc, poller counts

# Convert TSC ticks to microseconds
# latency_us = latency_ticks / (tsc_rate / 1e6)
# Get tsc_rate from: rpc.py framework_get_reactors | grep tsc_rate
```

### 7.4 System-Level Metrics

```bash
# PCIe bandwidth (requires pcm or turbostat)
sudo pcm-pcie 1

# Memory bandwidth (NUMA cross-traffic is a red flag)
sudo numastat -m
sudo pcm-memory 1

# NIC utilization (for NVMe-oF)
sar -n DEV 1 60

# Interrupt counts (high count on SPDK cores = IRQ affinity problem)
watch -n 1 cat /proc/interrupts
```

---

## 8. Common Performance Pitfalls and Fixes

### Pitfall 1: SPDK Reactors Sharing Cores with OS Threads

**Symptom**: IOPS variance between runs, latency spikes, `mpstat` shows `%sys` time on SPDK cores.

**Diagnosis**:
```bash
ps -eLo psr,pid,comm | awk '$1 == 2'   # check core 2 for non-SPDK threads
```

**Fix**: Add cores to `isolcpus`, `nohz_full`, and `rcu_nocbs` kernel parameters. Ensure `reactor_mask` only covers isolated cores.

---

### Pitfall 2: NUMA-Mismatched Allocation

**Symptom**: Throughput plateau at ~50–60% of expected peak, high memory latency in `pcm-memory`.

**Diagnosis**:
```bash
numastat -p $(pgrep nvmf_tgt)    # look for cross-node memory access
```

**Fix**: Ensure `reactor_mask` covers only cores on the NUMA node that owns the NVMe device and NIC. If devices span NUMA nodes, assign dedicated reactors per node.

---

### Pitfall 3: Insufficient Hugepages

**Symptom**: SPDK fails to start with `Cannot get hugepage information` or `Unable to allocate DMA memory`.

**Fix**:
```bash
echo 32 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
# Then restart SPDK with -s option matching available memory
```

---

### Pitfall 4: Queue Depth Too Low

**Symptom**: IOPS far below drive specification; `spdk_nvme_perf` shows near-zero idle time but low IOPS.

**Diagnosis**: Run IOPS vs QD sweep (see §4.1). If IOPS scales linearly with QD up to the drive's rated max, QD is the constraint.

**Fix**: Increase `-q` in benchmarks. In production, increase `io_queue_size` and `io_queue_requests` in `spdk_nvme_ctrlr_opts`.

---

### Pitfall 5: I/O Size Mismatch with in_capsule_data_size (NVMe-oF)

**Symptom**: NVMe-oF TCP latency is 2x higher than expected for small I/Os.

**Diagnosis**: Run `tcpdump` and observe whether data arrives in the same TCP segment as the command capsule.

**Fix**: Set `in_capsule_data_size` to match your workload's I/O size:
```bash
rpc.py nvmf_create_transport -t TCP --in-capsule-data-size 4096
```

---

### Pitfall 6: PCIe Bandwidth Saturation

**Symptom**: Throughput caps at ~3.5 GB/s on a Gen3 x4 link even with queue depth scaled up.

**Diagnosis**:
```bash
lspci -vv -s <BDF> | grep LnkSta   # should show "Speed 16GT/s, Width x4" for Gen4
```

**Fix**: Verify the NVMe drive is seated in a Gen4-capable slot. Re-seat the drive. Check motherboard documentation for slot bifurcation settings.

---

### Pitfall 7: CPU Frequency Scaling During Test

**Symptom**: IOPS numbers vary significantly between test runs, especially early in the run.

**Diagnosis**:
```bash
watch -n 0.5 "cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq | sort -u"
```

**Fix**: Set CPU governor to `performance`:
```bash
cpupower frequency-set -g performance
# or
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

---

### Pitfall 8: Buffer Pool Exhaustion

**Symptom**: Latency increases sharply under sustained load; `nvmf_get_stats` shows `pending_data_buffer_count` growing.

**Fix**: Increase `num_shared_buffers` when creating the transport:
```bash
rpc.py nvmf_create_transport -t TCP --num-shared-buffers 16384
```

Alternatively increase the `-s` (mem_size) argument to give SPDK more hugepage memory.

---

### Pitfall 9: Hyper-Threading Interference

**Symptom**: Latency spikes every few milliseconds; cache miss rate elevated.

**Diagnosis**: Check if SPDK reactor cores have HT siblings that are not isolated:
```bash
cat /sys/devices/system/cpu/cpu2/topology/thread_siblings_list
# e.g., returns "2,18" meaning core 2 and logical core 18 share a physical core
```

**Fix**: Either disable HT in BIOS, or include both logical siblings in `isolcpus` and `reactor_mask`.

---

## 9. Quick-Reference Parameter Table

| Parameter | Where Set | Recommended Value | Impact |
|---|---|---|---|
| `reactor_mask` | CLI `-m`, `spdk_app_opts` | Isolated cores on same NUMA as devices | Core assignment for all SPDK reactors |
| `mem_size` | CLI `-s`, `spdk_app_opts` | 2–4x expected I/O buffer footprint (MB) | Total hugepage memory SPDK registers |
| `hugepage_single_segments` | `spdk_app_opts` | `true` for production | Reduces TLB pressure |
| `io_queue_size` | `spdk_nvme_ctrlr_opts` | 1024 (check drive max) | Commands per NVMe queue |
| `io_queue_requests` | `spdk_nvme_ctrlr_opts` | 2048 (>= io_queue_size) | In-flight request objects |
| `num_io_queues` | `spdk_nvme_ctrlr_opts` | 1 per reactor polling that device | Parallelism to NVMe controller |
| Queue depth (`-q`) | `spdk_nvme_perf`, `bdevperf` | 64–128 for 4K random; 8–16 for 128K seq | Primary IOPS/latency trade-off |
| I/O size (`-o`) | `spdk_nvme_perf`, `bdevperf` | 4096 (IOPS test); 131072 (BW test) | Block size for benchmark |
| `in_capsule_data_size` | `nvmf_create_transport` | Match workload I/O size | Eliminates extra round-trip for small I/Os |
| `num_shared_buffers` | `nvmf_create_transport` | 8192–16384 (TCP), 4096 (RDMA) | NVMe-oF DMA buffer pool size |
| `max_queue_depth` | `nvmf_create_transport` | 128 | NVMe-oF max SQ depth per connection |
| `max_io_size` | `nvmf_create_transport` | 131072 (128 KB) | Maximum I/O size target will accept |
| `acceptor_poll_rate` | `nvmf_set_config` | 1000–10000 µs | Connection setup latency |
| `isolcpus` (kernel) | `/etc/default/grub` | All SPDK reactor cores | Removes kernel scheduler from SPDK cores |
| `nohz_full` (kernel) | `/etc/default/grub` | Same as `isolcpus` | Disables timer tick on SPDK cores |
| `rcu_nocbs` (kernel) | `/etc/default/grub` | Same as `isolcpus` | Moves RCU callbacks off SPDK cores |
| `intel_idle.max_cstate` | Kernel cmdline | `0` | Prevents C-state latency on wakeup |
| `scaling_governor` | `/sys/…/cpufreq/` | `performance` | Prevents frequency scaling during tests |
| `nr_hugepages` (1G) | `/sys/kernel/mm/hugepages/` | 16–64 depending on workload | Pre-allocates contiguous 1G pages |
| NIC IRQ affinity | `/proc/irq/*/smp_affinity_list` | Adjacent non-SPDK core on same NUMA | Prevents NIC IRQs interrupting reactors |

---

## Appendix: Diagnostic Command Reference

```bash
# System overview
numactl --hardware                              # NUMA topology
lstopo --of ascii                               # CPU/cache/memory topology
lspci -k | grep -A2 "NVM"                      # NVMe PCIe bindings

# Hugepages
cat /proc/meminfo | grep -i huge
numastat -m | grep -i huge

# CPU state
cat /sys/devices/system/cpu/isolated
cat /sys/devices/system/cpu/nohz_full
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort -u

# SPDK runtime
rpc.py framework_get_reactors                  # reactor busy/idle ratio
rpc.py thread_get_stats                        # per-thread stats
rpc.py bdev_get_iostat                         # bdev I/O counters
rpc.py nvmf_get_stats                          # NVMe-oF poll group stats

# Drive health / queue depth
nvme id-ctrl /dev/nvme0 | grep -i "mdts\|oacs"   # max data transfer size
nvme id-ns /dev/nvme0n1 | grep -i "npdg\|nows"   # optimal I/O granularity

# Live monitoring during test
watch -n 1 "rpc.py bdev_get_iostat -b Nvme0n1 2>/dev/null | python3 -c \
  \"import sys,json; s=json.load(sys.stdin)['bdevs'][0]; \
  print('IOPS:', s['num_read_ops'], 'BW(MB/s):', s['bytes_read']//1048576)\""
```

---

*This guide reflects SPDK as shipped in the repository at the time of authoring. Transport RPC parameters and option names may evolve across releases; always cross-reference with `rpc.py --help` and the generated `doc/jsonrpc.md` for the version you are running.*

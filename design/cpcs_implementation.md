# CPCS Implementation Details

This document tracks implementation details for CPCS core support and the CPCS-RT foundation in SPDK. It combines the CPCS-scenario implementation summary with the CPCS-RT components introduced in feature/add-first-document.

## 1. CPCS Core (Compute Namespaces, Programs, MRS, Execute)

### 1.1 Core modules

- Compute namespace management: `lib/nvmf/cpcs/nvmf_cpcs.c` and `lib/nvmf/cpcs/nvmf_cpcs.h`
- Program lifecycle: `lib/nvmf/cpcs/program.c` and `lib/nvmf/cpcs/program.h`
- Program activation: `lib/nvmf/cpcs/program_activation.c` and `lib/nvmf/cpcs/program_activation.h`
- Execute handler: `lib/nvmf/cpcs/execute.c` and `lib/nvmf/cpcs/execute.h`
- Runtime interface: `lib/nvmf/cpcs/runtime.h` and `lib/nvmf/cpcs/runtime_stub.c`
- Optional eBPF runtime: `lib/nvmf/cpcs/ebpf_runtime.c` and `lib/nvmf/cpcs/ebpf_runtime.h`

### 1.2 Memory management

- Memory Range Sets (MRS): `lib/nvmf/cpcs/memory_range_set.c` and `lib/nvmf/cpcs/memory_range_set.h`
- Reachability groups: `lib/nvmf/cpcs/reachability.c` and `lib/nvmf/cpcs/reachability.h`

### 1.3 Command handling

- Admin command dispatch: `lib/nvmf/cpcs/admin_cmd.c`
- I/O command dispatch: `lib/nvmf/cpcs/cpcs_cmd.c`
- NVMe-oF integration: `lib/nvmf/ctrlr.c` routes CPCS admin and execute commands

### 1.4 RPC management

- `cpcs_ns_create`, `cpcs_ns_delete`, `cpcs_program_list`, `cpcs_mrs_list` in `lib/nvmf/cpcs/cpcs_rpc.c`

### 1.5 SLM bdev

- Module: `module/bdev/slm/vbdev_slm.c` (dispatch) and `module/bdev/slm/vbdev_pslm.c` / `module/bdev/slm/vbdev_pslm.h` (pSLM provider)
- RPC: `module/bdev/slm/vbdev_slm_rpc.c`
- Public API: `include/spdk/bdev_slm.h`

### 1.6 Headers and constants

- CPCS constants and status codes: `include/spdk/nvme_spec.h`
- CPCS command/data structures: `include/spdk/nvme_cpcs_spec.h`

### 1.7 Completed capabilities

- Program load/unload with chunked loading (LOFF/NUMB semantics)
- Program activation and activation limits
- MRS create/delete with overlap detection and granularity validation
- Execute Program command with MRS acquisition and return value propagation
- Reachability groups and reference counting for safe concurrent access
- RPC management for compute namespaces and monitoring
- SLM memory namespace bdev with zero-copy buffer access

## 2. CPCS-RT Foundation (CPCS-RT/MT building blocks)

The CPCS-RT components are currently standalone utilities intended to support the staged-execution pipeline described in `design/cpcs_rt.md`.

### 2.1 Decode

- `lib/nvmf/cpcsrt_decode.c` parses a vendor-specific descriptor payload into `struct cpcs_decoded`.
- Descriptor format: `include/spdk/nvme_cpcs_rt.h` defines `struct spdk_nvme_cpcs_desc` and CPCS-RT opcodes.

### 2.2 Epoch manager

- `lib/nvmf/cpcsrt_epoch.c` maintains `(range_id -> epoch)` state.
- Range id uses `(nsid << 48) | range_index` with `range_size_bytes` partitioning.
- `cpcsrt_epoch_bump_extent` updates all ranges covered by an LBA extent.

### 2.3 Lease manager

- `lib/nvmf/cpcsrt_lease.c` provides read and write leases on SLM buffers.
- Readers and a single writer are tracked; write conflicts return `-EAGAIN`.

### 2.4 Cache

- `lib/nvmf/cpcsrt_cache.c` implements a simple LRU cache keyed by `(range_id, epoch, layout_id)`.
- Eviction triggers a callback to free or scrub buffers.

### 2.5 Program registry

- `lib/nvmf/cpcsrt_program.c` provides an allowlist registry for program functions.

### 2.6 Data structures and config

- `lib/nvmf/cpcsrt.h` defines job, tenant, buffer, lease, epoch, and cache structures.

## 3. Integration Notes

- CPCS admin and execute commands are handled by `lib/nvmf/cpcs` and integrated in `lib/nvmf/ctrlr.c`.
- Identify I/O command set specific (CNS 05h) supports CPCS namespace data via `spdk_nvmf_cpcs_ns_identify`.
- CPCS-RT modules are included in the build but not yet wired into the NVMe-oF request path.

## 4. Tests

- CPCS SLM unit tests: `test/unit/lib/bdev/slm`
- CPCS integration tests: `test/cpcs`

# CPCS Core Design (SPDK NVMe-oF Target)

## 1. Overview

This document describes the SPDK implementation of the NVMe Computational Programs Command Set (CPCS) and its Subsystem Local Memory (SLM) integration. The implementation provides compute namespace management, program lifecycle, memory range sets, and execute command handling in the NVMe-oF target.

## 2. Architecture

```
NVMe-oF Target (SPDK)
  - Compute Namespace (CSI 0x04)
    - Programs: load, activate, execute
    - Memory Range Sets (MRS)
    - Reachability groups
    - Runtime interface (stub or eBPF)
  - SLM bdev (memory namespace)
```

## 3. Components

### 3.1 Core CPCS

- `lib/nvmf/cpcs/nvmf_cpcs.c`: compute namespace management
- `lib/nvmf/cpcs/program.c`: program load/unload and metadata
- `lib/nvmf/cpcs/program_activation.c`: activation and limits
- `lib/nvmf/cpcs/execute.c`: execute program handler
- `lib/nvmf/cpcs/runtime.h`: runtime interface
- `lib/nvmf/cpcs/runtime_stub.c`: stub runtime
- `lib/nvmf/cpcs/ebpf_runtime.c`: optional eBPF runtime

### 3.2 Memory Management

- `lib/nvmf/cpcs/memory_range_set.c`: MRS create/delete/validate
- `lib/nvmf/cpcs/reachability.c`: reachability groups

### 3.3 Integration

- `lib/nvmf/cpcs/cpcs_cmd.c`: CPCS admin and I/O command dispatch
- `lib/nvmf/cpcs/cpcs_rpc.c`: RPC management commands
- `include/spdk/nvme_spec.h`: CPCS opcodes and status codes
- `include/spdk/nvme_cpcs_spec.h`: CPCS command and data structures

### 3.4 SLM bdev

- `module/bdev/slm/vbdev_slm.c`: SLM bdev implementation
- `module/bdev/slm/vbdev_slm_rpc.c`: SLM RPCs
- `include/spdk/bdev_slm.h`: SLM public API

## 4. Command Handling

### 4.1 Admin Commands

- Load Program (0xC0): chunked program loading
- Program Activation (0xC1): activate, deactivate, or deactivate all
- MRS Management (0xC2): create or delete memory range sets

### 4.2 I/O Commands

- Execute Program (0x01): run an activated program with memory ranges

## 5. Compute Namespace Options

```
struct spdk_nvmf_cpcs_ns_opts {
  uint32_t nsid;
  uint16_t max_activated;
  uint16_t max_mrs;
  uint8_t  max_ranges_per_mrs;
  uint8_t  mrs_granularity;
  uint64_t max_program_bytes;
  uint8_t  load_program_gran;
  uint16_t reach_group_id;
};
```

## 6. RPC Management

- `cpcs_ns_create`: create a compute namespace
- `cpcs_ns_delete`: delete a compute namespace
- `cpcs_program_list`: list programs
- `cpcs_mrs_list`: list memory range sets

## 7. Status Codes

CPCS-specific status codes are defined in `include/spdk/nvme_spec.h` and returned for program and MRS errors (invalid index, program not activated, overlapping ranges, and so on).

## 8. Runtime Interface

- `runtime_stub.c` supports functional testing without external dependencies.
- `ebpf_runtime.c` provides optional uBPF execution and helper functions when available.

## 9. Testing

- Unit tests: `test/unit/lib/bdev/slm`
- Integration tests: `test/cpcs`


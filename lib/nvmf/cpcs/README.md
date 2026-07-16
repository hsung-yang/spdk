# NVMe Computational Programs Command Set (CPCS) Implementation

This directory contains the SPDK implementation of NVMe Computational Programs Command Set (CPCS) Specification Revision 1.1.

Design and documentation are in the project-level `docs/` directory:
- [`docs/wiki/cpcs-overview.md`](../../../../../docs/wiki/cpcs-overview.md) — CPCS overview and concepts
- [`docs/wiki/cpcs-architecture.md`](../../../../../docs/wiki/cpcs-architecture.md) — CPCS architecture deep dive
- [`docs/components/spdk-cpcs-target.md`](../../../../../docs/components/spdk-cpcs-target.md) — SPDK CPCS target component guide

## Overview

CPCS enables computational offload to NVMe devices, allowing programs (e.g., eBPF bytecode) to execute on the storage device with direct access to memory regions. This reduces data movement and improves performance for data-intensive workloads.

## Architecture

```
┌─────────────────────────────────────────────────┐
│            NVMe-oF Target (SPDK)                │
│                                                 │
│  ┌──────────────────────────────────────────┐  │
│  │         Compute Namespace (CSI 0x04)     │  │
│  │                                          │  │
│  │  ┌────────────┐    ┌─────────────────┐  │  │
│  │  │  Programs  │    │ Memory Range    │  │  │
│  │  │            │    │ Sets (MRS)      │  │  │
│  │  │  Load      │    │                 │  │  │
│  │  │  Activate  │◄───┤  Reachability   │  │  │
│  │  │  Execute   │    │  Groups         │  │  │
│  │  └────────────┘    └─────────────────┘  │  │
│  │         │                    │          │  │
│  │         ▼                    ▼          │  │
│  │  ┌────────────────────────────────────┐ │  │
│  │  │       Runtime (eBPF VM)            │ │  │
│  │  └────────────────────────────────────┘ │  │
│  └──────────────────────────────────────────┘  │
│                     │                           │
│                     ▼                           │
│  ┌──────────────────────────────────────────┐  │
│  │    Subsystem Local Memory (SLM) Bdev    │  │
│  │         (Byte-addressable memory)        │  │
│  └──────────────────────────────────────────┘  │
└─────────────────────────────────────────────────┘
```

## Components

### Core Components

- **nvmf_cpcs.c/h**: Compute namespace management
- **program.c/h**: Program lifecycle (Load/Unload)
- **program_activation.c/h**: Program activation management
- **execute.c/h**: Execute Program command handler
- **runtime.h**: Runtime interface for program execution
- **runtime_stub.c**: Stub runtime (placeholder for eBPF)

### Memory Management

- **memory_range_set.c/h**: Memory Range Set (MRS) management
- **reachability.c/h**: Reachability groups for namespace access control

### Integration

- **cpcs_cmd.c/h**: NVMe-oF command handler integration

## Key Features

### Program Management

- **Load/Unload**: Upload program bytecode in chunks
- **Activate/Deactivate**: Initialize runtime (JIT compile) and manage activation limits
- **Validation**: Program data and format validation
- **Built-in Programs**: A small set of device-defined programs (fixed PIND) are installed at namespace creation to support experiments without requiring program download

### Memory Access

- **Memory Range Sets**: Define memory regions accessible to programs
- **Reachability Groups**: Control which compute namespaces can access which memory namespaces
- **Granularity Control**: Configurable memory access granularity (4B to 4KB)

### Execution

- **Execute Program**: Run activated programs with parameter passing
- **Memory Access**: Direct access to SLM through MRS
- **Return Values**: 64-bit return values in completion
- **Concurrency**: Safe concurrent execution with reference counting

## Commands

### Admin Commands

- **Load Program (0xC0)**: Load program data
  - Fields: PIND, PTYPE, PIT, PUID, PSIZE, LOFF, NUMB
  - Supports chunked loading

- **Program Activation (0xC1)**: Activate/Deactivate programs
  - Actions: 0x00 (Activate), 0x01 (Deactivate), 0x02 (Deactivate All)
  - Fields: PIND, ACT

- **MRS Management (0xC2)**: Create/Delete Memory Range Sets
  - Actions: 0x00 (Create), 0x01 (Delete)
  - Fields: RSID, MACT, NUMR
  - Returns RSID in DW0 for Create

### I/O Commands

- **Execute Program (0x01)**: Execute activated program
  - Fields: PIND, RSID, NUMR, DLEN, CPARAM1, CPARAM2
  - Returns 64-bit value in DW0-DW1

## RPC Commands

CPCS provides RPC commands for management and monitoring:

### Namespace Management

**cpcs_ns_create** - Create CPCS namespace
```bash
rpc.py cpcs_ns_create \
  --subsystem-nqn nqn.2024-01.io.spdk:cnode1 \
  --nsid 200 \
  --max-activated 16 \
  --max-mrs 64 \
  --max-ranges-per-mrs 8 \
  --mrs-granularity 12 \
  --max-program-bytes 16 \
  --load-program-gran 12 \
  --reach-group-id 1
```

**cpcs_ns_delete** - Delete CPCS namespace
```bash
rpc.py cpcs_ns_delete \
  --subsystem-nqn nqn.2024-01.io.spdk:cnode1 \
  --nsid 200
```

### Monitoring

**cpcs_program_list** - List programs in namespace
```bash
rpc.py cpcs_program_list \
  --subsystem-nqn nqn.2024-01.io.spdk:cnode1 \
  --nsid 200
```

Returns:
```json
[
  {
    "pind": 0,
    "ptype": 192,
    "puid": 12345,
    "total_size": 4096,
    "loaded_bytes": 4096,
    "activated": true,
    "exec_count": 0
  }
]
```

**cpcs_program_install_builtins** - Install device-defined built-in programs
```bash
rpc.py cpcs_program_install_builtins \
  --subsystem-nqn nqn.2024-01.io.spdk:cnode1 \
  --nsid 200
```

**cpcs_mrs_list** - List Memory Range Sets
```bash
rpc.py cpcs_mrs_list \
  --subsystem-nqn nqn.2024-01.io.spdk:cnode1 \
  --nsid 200
```

Returns:
```json
[
  {
    "rsid": 1,
    "range_count": 2,
    "ref_count": 0
  }
]
```

## Status Codes

CPCS-specific status codes (defined in `spdk/nvme_spec.h`):

- `INVALID_PROGRAM_INDEX`: Program index out of range
- `NO_PROGRAM`: No program at specified index
- `PROGRAM_IN_USE`: Program currently in use
- `INVALID_PROGRAM_DATA`: Invalid program data or format
- `PROGRAM_TOO_BIG`: Program exceeds size limits
- `MAX_PROGRAMS_ACTIVATED`: Activation limit reached
- `PROGRAM_NOT_ACTIVATED`: Program not activated for execution
- `INVALID_MEMORY_RANGE_SET_ID`: Invalid MRS ID
- `MAX_MEMORY_RANGES_EXCEEDED`: Too many memory ranges
- And more...

## Configuration

### Compute Namespace Options

```c
struct spdk_nvmf_cpcs_ns_opts {
    uint32_t nsid;                 // Namespace ID
    uint16_t max_activated;        // Max concurrent activated programs
    uint16_t max_mrs;              // Max Memory Range Sets
    uint16_t max_ranges_per_mrs;   // Max ranges per MRS
    uint8_t  mrs_granularity;      // MRS granularity (2^N bytes)
    uint32_t max_program_bytes;    // Max total program bytes (MiB)
    uint8_t  load_program_gran;    // Load granularity (2^N bytes)
    uint16_t reach_group_id;       // Reachability group ID
};
```

### SLM Bdev Options

- **size**: Total memory size in MiB
- **granularity**: Access granularity in bytes
- **nsid**: Namespace ID for reachability

## Implementation Status

✅ **Phase 1: Foundation**
- CPCS header files and constants
- SLM bdev module
- Test infrastructure

✅ **Phase 2: Compute Namespace Core**
- Namespace management
- Reachability groups
- Memory Range Sets

✅ **Phase 3: Program Management**
- Program Load/Unload
- Program Activate/Deactivate
- Runtime infrastructure (stub)

✅ **Phase 4: Execute Program**
- Execute command handler
- Memory access setup
- Runtime execution

🚧 **Phase 5: Integration & Verification** (In Progress)
- Command handler integration
- End-to-end testing
- Documentation

## Runtime Implementation

### eBPF Runtime

The implementation includes a full eBPF runtime with conditional uBPF support:

**Features**:
- ✅ **Dual Mode**: Full uBPF integration when available, simulation mode otherwise
- ✅ **Helper Functions**: SLM read/write, parameter access, logging
- ✅ **JIT Compilation**: Automatic JIT when uBPF library is available
- ✅ **Graceful Fallback**: Interpreter mode if JIT compilation fails
- ✅ **Validation**: eBPF bytecode validation on program load

**Helper Functions**:
1. `slm_read(mr_id, offset, len, buf_ptr)` - Read from SLM memory
2. `slm_write(mr_id, offset, len, buf_ptr)` - Write to SLM memory
3. `get_param(param_id)` - Get command parameters (CPARAM1, CPARAM2)
4. `log(level, msg_ptr, msg_len)` - Log messages from eBPF program

**Building with uBPF**:
```bash
# Install uBPF library
git clone https://github.com/iovisor/ubpf.git
cd ubpf
make
sudo make install

# Build SPDK with uBPF support
./configure --with-ubpf
make
```

**Without uBPF**:
The runtime will operate in simulation mode, allowing testing and development
without the uBPF library. Programs will execute with a simulated environment.

### Performance Optimization

- **Zero-copy execution**: Minimize memory copies
- **Batch processing**: Support for batch execution
- **Async execution**: Asynchronous program execution
- **NUMA awareness**: NUMA-aware memory allocation

### Additional Features

- **Program caching**: Cache compiled programs
- **Statistics**: Execution statistics and monitoring
- **Debugging**: Debug support for programs
- **Multi-language support**: Support for WASM, etc.

## Testing

Run tests:
```bash
# Unit tests
cd test/unit/lib/bdev/slm
make test

# Integration tests
cd test/cpcs
make test
```

## References

- NVMe Computational Programs Command Set Specification v1.1
- SPDK NVMe-oF Target Documentation
- uBPF: Userspace eBPF VM

## License

BSD-3-Clause (see SPDX headers in source files)

## Contributors

Implementation by CPCS Implementation Team (2024)

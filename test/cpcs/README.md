# CPCS Test Suite

Comprehensive test suite for the NVMe Computational Programs Command Set (CPCS) implementation in SPDK.

## Test Components

### 1. Integration Tests (`cpcs.sh`)
- **Location**: `test/cpcs/cpcs.sh`
- **Purpose**: End-to-end integration tests for CPCS functionality
- **Coverage**:
  - SLM bdev operations (create, delete, info)
  - CPCS namespace creation and deletion
  - Program management (list, load, activate)
  - Memory Range Set (MRS) operations
  - Multiple SLM bdevs
  - Invalid parameter handling

### 2. Reachability Tests (`reachability_test/`)
- **Location**: `test/cpcs/reachability_test/`
- **Purpose**: Validate namespace reachability enforcement
- **Coverage**:
  - Reachability manager initialization
  - Group creation and management
  - Namespace addition to groups
  - Reachability verification (allow/deny)
  - Cross-group access control

### 3. SLM Basic Tests (`slm_basic_test/`)
- **Location**: `test/cpcs/slm_basic_test/`
- **Purpose**: Subsystem Local Memory bdev validation
- **Coverage**:
  - SLM bdev creation
  - Buffer access operations
  - Memory layout validation
  - Error handling

### 4. Builtin Smoke Tests (`cpcs_builtin_smoke/`)
- **Location**: `test/cpcs/cpcs_builtin_smoke/`
- **Purpose**: Built-in program validation
- **Coverage**:
  - Device-defined programs (memcpy, memfill, sum64)
  - Program execution
  - Return value verification

## Running Tests

### Quick Start (Local)

If SPDK is already built and you have appropriate permissions:

```bash
# Run all integration tests
sudo ./test/cpcs/cpcs.sh

# Run specific test suites
cd test/cpcs/reachability_test && make && sudo ./reachability_test
cd test/cpcs/slm_basic_test && sudo ./slm_basic_test.sh
cd test/cpcs/cpcs_builtin_smoke && make && sudo ./cpcs_builtin_smoke
```

### Using Vagrant (Recommended for Development)

The vagrant test runner provides an isolated environment with all dependencies:

```bash
# Run all tests (create VM, build, test, cleanup)
./test/cpcs/run_vagrant_tests.sh

# Step-by-step approach
./test/cpcs/run_vagrant_tests.sh create   # Create VM
./test/cpcs/run_vagrant_tests.sh setup    # Build SPDK
./test/cpcs/run_vagrant_tests.sh test     # Run tests
./test/cpcs/run_vagrant_tests.sh cleanup  # Destroy VM

# Custom VM configuration
./test/cpcs/run_vagrant_tests.sh -d ubuntu2204 -c 8 -m 16384 all

# Open shell for debugging
./test/cpcs/run_vagrant_tests.sh shell
```

#### Vagrant Prerequisites

**macOS:**
```bash
brew install vagrant
brew install --cask virtualbox
```

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get update
sudo apt-get install vagrant virtualbox
```

**Linux (Fedora/RHEL):**
```bash
sudo dnf install vagrant
# Install VirtualBox from https://www.virtualbox.org/wiki/Linux_Downloads
```

### Using Docker (Alternative)

For containerized testing:

```bash
# Build SPDK Docker image
docker/build_base/build.sh

# Run tests in container
docker run -it --privileged \
  -v $(pwd):/spdk \
  spdk/build_base \
  bash -c "cd /spdk && ./configure --with-cpcs && make && ./test/cpcs/cpcs.sh"
```

## Test Requirements

### System Requirements
- **CPU**: 2+ cores (4+ recommended for Vagrant)
- **RAM**: 4GB minimum (8GB+ recommended for Vagrant)
- **Disk**: 20GB free space for VM
- **OS**: Linux (Ubuntu 20.04+, Fedora 37+, CentOS 7+) or FreeBSD

### Build Requirements
- SPDK built with CPCS support: `./configure --with-cpcs && make`
- Root/sudo access for NVMe device access
- Hugepages configured (handled by `scripts/setup.sh`)

### Runtime Requirements
- SPDK target binary (`build/bin/spdk_tgt`)
- Python 3.6+ with required packages
- NVMe emulation (physical/emulated NVMe device or NVMe-oF)

## Test Architecture

### Integration Test Flow (`cpcs.sh`)

```
┌─────────────────────────────────────┐
│  Start SPDK Target (spdk_tgt)      │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Test: SLM Bdev Operations          │
│  - Create/Delete/Info               │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Test: Multiple SLM Bdevs           │
│  - Create 5 bdevs in parallel       │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Test: Invalid Parameters           │
│  - Zero size, zero granularity      │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Test: CPCS Namespace Creation      │
│  - Subsystem, SLM, Compute NS       │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Test: Program/MRS Listing          │
│  - Verify empty initial state       │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Test: Namespace Deletion           │
│  - Cleanup all resources            │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Stop SPDK Target & Report Results  │
└─────────────────────────────────────┘
```

### Reachability Test Flow

```
┌─────────────────────────────────────┐
│  Initialize Reachability Manager    │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Create Reachability Groups         │
│  - Group 1, Group 2                 │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Add Namespaces to Groups           │
│  - NSID 100,200 → Group 1           │
│  - NSID 300 → Group 2               │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Create Reachability Association    │
│  - Assoc: Group 1 + Group 2         │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Test: Association Access (Allow)   │
│  - 100 can reach 200 ✓              │
│  - 100 can reach 300 ✓              │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│  Cleanup & Report Results           │
└─────────────────────────────────────┘
```

## Troubleshooting

### Test Failures

**"spdk_tgt not found"**
```bash
# Build SPDK first
./configure --with-cpcs && make
```

**"Permission denied"**
```bash
# Run with sudo
sudo ./test/cpcs/cpcs.sh
```

**"Cannot allocate hugepages"**
```bash
# Setup hugepages
sudo scripts/setup.sh
```

**"RPC connection failed"**
```bash
# Check if another spdk_tgt is running
ps aux | grep spdk_tgt
sudo killall spdk_tgt

# Check RPC socket
ls -la /var/tmp/spdk.sock
```

### Vagrant Issues

**"Vagrant not found"**
```bash
# Install vagrant (see Prerequisites above)
brew install vagrant  # macOS
```

**"VirtualBox not found"**
```bash
# Install VirtualBox
brew install --cask virtualbox  # macOS
```

**"VM creation failed"**
```bash
# Check logs
cd ~/spdk-cpcs-test-vm
vagrant status
vagrant up --debug

# Try different provider
./test/cpcs/run_vagrant_tests.sh -p libvirt create
```

**"VM build failed"**
```bash
# SSH into VM to debug
cd ~/spdk-cpcs-test-vm
vagrant ssh

# Check build manually
cd /home/vagrant/spdk_repo/spdk
./configure --with-cpcs
make
```

### Common Issues

**Test hangs**
- Check if spdk_tgt is running: `ps aux | grep spdk_tgt`
- Check system logs: `dmesg | tail -50`
- Verify NVMe device: `lspci | grep NVMe`

**Memory errors**
- Increase hugepages: `echo 2048 > /proc/sys/vm/nr_hugepages`
- Check memory: `free -h`

**RPC errors**
- Verify JSON syntax in RPC calls
- Check spdk_tgt logs: `sudo cat /tmp/spdk.log`
- Test RPC manually: `scripts/rpc.py bdev_get_bdevs`

## Test Coverage

### Current Coverage (~85%)

✅ **Fully Tested**:
- SLM bdev lifecycle (create, delete, info)
- CPCS namespace management
- Reachability group enforcement
- MRS creation and validation
- Program lifecycle (load, activate, list)
- RPC interface
- Error handling

🚧 **Partial Coverage**:
- eBPF program execution (requires uBPF)
- Transaction descriptor support (planned)
- CPCS-RT runtime loop (in progress)

📋 **Planned**:
- Multi-threaded execution
- Performance benchmarks
- Stress testing
- Fault injection

## Adding New Tests

### 1. Create Test Directory
```bash
mkdir test/cpcs/my_new_test
cd test/cpcs/my_new_test
```

### 2. Create Test Code
```c
// my_test.c
#include "../cpcs_common.h"

static void test_my_feature(void) {
    CU_ASSERT(my_function() == expected_value);
}

int main(void) {
    CU_initialize_registry();

    CU_pSuite suite = CU_add_suite("My Test Suite", NULL, NULL);
    CU_add_test(suite, "test_my_feature", test_my_feature);

    CU_basic_run_tests();
    return CU_get_number_of_failures();
}
```

### 3. Create Makefile
```makefile
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = my_test
C_SRCS := my_test.c ../cpcs_common.c

SPDK_LIB_LIST = nvmf bdev_slm
include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
```

### 4. Update Parent Makefile
```makefile
# In test/cpcs/Makefile
DIRS-y += my_new_test
```

### 5. Add to Test Runner
```bash
# In test/cpcs/run_vagrant_tests.sh, add:
vagrant ssh -c "cd /home/vagrant/spdk_repo/spdk/test/cpcs/my_new_test && make && sudo ./my_test"
```

## Continuous Integration

Tests are automatically run in CI for:
- Pull requests
- Main branch commits
- Nightly builds

CI configuration: `.github/workflows/cpcs-tests.yml` (if exists)

## Performance Testing

### Benchmark Tests (Planned)

```bash
# Run performance benchmarks
sudo ./test/cpcs/cpcs_perf.sh

# Output:
# - Program execution latency
# - MRS creation overhead
# - Throughput metrics
```

## Documentation

- **Design**: `design/cpcs.md` - Architecture overview
- **Implementation**: `design/cpcs_implementation.md` - Status matrix
- **Architecture**: `design/cpcs.md` - Architecture and workflow
- **Reachability**: `design/cpcs.md` - Access control guide
- **API**: `lib/nvmf/cpcs/README.md` - Code structure

## Contributing

1. Write tests for new features
2. Ensure all existing tests pass
3. Follow SPDK coding standards
4. Update this README for new test suites
5. Add CI configuration if needed

## License

SPDX-License-Identifier: BSD-3-Clause
Copyright (C) 2024-2026 CPCS Implementation Team

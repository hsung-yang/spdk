# Module 10: Environment Setup

**Stage**: 2 (Implementation)
**Difficulty**: Intermediate
**Estimated Time**: 2 hours
**Prerequisites**: Stage 1 complete

**Version History**:
- v1.0 (2026-03-30): Initial version
- v1.1 (2026-03-30): Expanded with full dependency, build, device, and troubleshooting detail

---

## Learning Objectives

By the end of this module, you will be able to:
- Assess hardware and software requirements for SPDK development
- Install all required and optional dependencies using `pkgdep.sh`
- Configure and build SPDK with the correct options for your use case
- Bind NVMe and other PCI devices to userspace drivers (VFIO/UIO)
- Configure hugepages correctly for single-node and NUMA systems
- Run the SPDK test suite to verify a working environment
- Set up an effective IDE and debugging workflow
- Diagnose and fix the most common build and runtime problems
- Develop SPDK inside a virtual machine or container

---

## Overview

Setting up an SPDK development environment involves more steps than a typical userspace library because SPDK bypasses the kernel storage stack entirely. Correct hugepage allocation, PCI device binding, and IOMMU configuration are all prerequisites before a single SPDK application can run. This module walks through each step in the order they must be completed, explains why each step is necessary, and documents how to verify that each step succeeded.

---

## 1. System Requirements

### 1.1 Hardware Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| CPU architecture | x86_64 or ARM64 | x86_64 with AVX-512 |
| RAM | 8 GB | 16 GB or more |
| Storage | Any block device | NVMe SSD |
| IOMMU | Not required | Required for VFIO |
| CPU cores | 2 | 4 or more |

SPDK's polled-mode drivers dedicate CPU cores entirely to I/O. A dedicated core means no other process should be scheduled on it during a production workload. For development and testing, sharing cores is acceptable.

For VFIO-based device access (the recommended path), the CPU and motherboard must both support IOMMU: Intel VT-d or AMD-Vi. This is verified at runtime by `dmesg | grep -i iommu`.

NVMe hardware is not strictly required for learning. SPDK includes a null block device and an AIO bdev that work on any block device or regular file.

### 1.2 Operating System Requirements

SPDK supports Linux and FreeBSD. This module focuses on Linux.

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| Kernel version | 4.0 | 5.15 LTS or newer |
| Distribution | Any with glibc 2.17+ | Ubuntu 22.04, Fedora 38+ |
| GCC | 7.0 | 11.0+ |
| Clang | 6.0 | 14.0+ |
| Python | 3.6 | 3.10+ |
| Git | 2.0 | Any recent version |
| nasm | 2.13 | Any recent version |

Newer kernels are preferred because:
- `vfio-pci` improvements reduce setup friction
- `io_uring` support is available for the uring bdev
- IOMMU passthrough mode (`iommu=pt`) performs better

To check your kernel version:
```bash
uname -r
# Example output: 6.8.0-45-generic
```

### 1.3 BIOS/UEFI Settings

Before starting, verify these settings in your BIOS/UEFI if you plan to use VFIO:

- **Intel VT-d** or **AMD-Vi**: Must be enabled
- **SR-IOV** (optional): Enable if using SR-IOV NVMe devices
- **Above 4G decoding**: Enable for systems with many PCIe devices
- **Secure Boot**: May need to be disabled for unsigned kernel modules

---

## 2. Cloning the Repository

```bash
# Clone SPDK (includes submodules reference, but not their content yet)
git clone https://github.com/spdk/spdk
cd spdk

# Initialize and fetch all submodules (DPDK, ISA-L, OCF, etc.)
# This can take several minutes on first run
git submodule update --init

# To update submodules after a git pull:
git submodule update
```

The `--init` flag is required on first clone because submodule URLs are recorded in `.gitmodules` but the directories are not populated until this command runs.

Key submodules and what they provide:

| Submodule | Purpose |
|-----------|---------|
| `dpdk/` | DPDK: EAL, memory, PCI, ring buffers |
| `isa-l/` | Intel ISA-L: erasure coding, CRC |
| `isa-l-crypto/` | Intel ISA-L crypto: AES-GCM acceleration |
| `ocf/` | Open CAS Framework: caching tier |
| `libvfio-user/` | vfio-user: mediated device protocol |

---

## 3. Installing Dependencies

### 3.1 Using pkgdep.sh

SPDK provides `scripts/pkgdep.sh` to automate package installation. The script detects the Linux distribution (Ubuntu/Debian, Fedora/RHEL, openSUSE, Alpine, Arch) and installs the correct packages.

```bash
# Must be run as root or with sudo
# Installs bare minimum build dependencies
sudo scripts/pkgdep.sh
```

The script accepts flags to install optional dependency groups:

```bash
# Show all available options
sudo scripts/pkgdep.sh --help

# Install developer tools (clang-format, lcov, gdb, valgrind, etc.)
sudo scripts/pkgdep.sh --developer-tools

# Install RDMA/RoCE dependencies (libibverbs, librdmacm)
sudo scripts/pkgdep.sh --rdma

# Install io_uring support (liburing-dev)
sudo scripts/pkgdep.sh --uring

# Install RBD (Ceph) bdev dependencies
sudo scripts/pkgdep.sh --rbd

# Install documentation build dependencies (doxygen, graphviz, etc.)
sudo scripts/pkgdep.sh --docs

# Install IDXD (Intel Data Streaming Accelerator) dependencies
sudo scripts/pkgdep.sh --idxd

# Install all optional dependencies at once
sudo scripts/pkgdep.sh --all
```

### 3.2 Core Dependencies (Installed by Default)

When you run `pkgdep.sh` without flags, it installs:

- **Build tools**: gcc, g++, make, pkg-config, nasm
- **Python packages**: pyelftools (needed for DPDK build)
- **Libraries**: libaio-dev, libssl-dev, libjson-c-dev, libcunit1-dev, uuid-dev
- **Kernel headers**: linux-headers for UIO module builds

On Ubuntu/Debian:
```bash
# What pkgdep.sh installs (representative, not exhaustive)
sudo apt-get install -y \
    gcc g++ make pkg-config git nasm \
    python3 python3-pip python3-pyelftools \
    libaio-dev libssl-dev libjson-c-dev \
    libcunit1-dev uuid-dev libncurses5-dev \
    libcmocka-dev
```

### 3.3 Verifying Dependencies

After running `pkgdep.sh`, verify key tools are present:

```bash
gcc --version         # Should print GCC 7+
nasm --version        # Required for DPDK crypto
python3 --version     # Should print Python 3.6+
pkg-config --version  # Used during configure
```

---

## 4. Configuring the Build

### 4.1 Basic Configuration

The `./configure` script generates build configuration. It is a bash script (not autoconf-generated), so options are SPDK-specific.

```bash
# Default configuration: builds with DPDK, vhost, and virtio enabled
./configure

# See all available options
grep -E '^\s+echo " --' configure | head -60
```

### 4.2 Common Configure Options

| Option | Effect |
|--------|--------|
| `--enable-debug` | Add `-g -O0`, enable assertions, disable optimizations |
| `--enable-asan` | Enable AddressSanitizer (memory error detection) |
| `--enable-ubsan` | Enable UndefinedBehaviorSanitizer |
| `--enable-coverage` | Enable gcov code coverage instrumentation |
| `--enable-lto` | Enable link-time optimization (slower build, faster binary) |
| `--enable-werror` | Treat all compiler warnings as errors |
| `--disable-tests` | Skip building functional tests |
| `--disable-unit-tests` | Skip building unit tests |
| `--disable-examples` | Skip building example programs |
| `--disable-apps` | Skip building application binaries |
| `--with-shared` | Build shared (.so) libraries instead of static (.a) |
| `--with-rdma` | Enable RDMA transport for NVMe-oF |
| `--with-uring` | Enable io_uring bdev |
| `--with-fio[=DIR]` | Build fio plugin for benchmarking |
| `--with-crypto` | Build ISA-L crypto vbdev module |
| `--with-rbd` | Build Ceph RBD bdev module |
| `--with-dpdk=DIR` | Use a custom DPDK installation instead of the bundled one |
| `--with-vfio-user=DIR` | Enable vfio-user transport |
| `--with-usdt` | Enable userspace DTrace/SystemTap probes |
| `--with-ocf` | Build Open CAS Framework caching module |

### 4.3 Configuration Examples

```bash
# Debug build for development (most common for active development)
./configure --enable-debug

# Debug build with memory error detection
./configure --enable-debug --enable-asan --enable-ubsan

# Production-like build with optimizations
./configure --enable-lto

# NVMe-oF development with RDMA transport
./configure --with-rdma

# Minimal build for faster iteration (no tests, no examples)
./configure --enable-debug --disable-tests --disable-examples

# Build with io_uring bdev support
./configure --with-uring

# Build with benchmarking support (requires fio source)
./configure --with-fio=/path/to/fio/source

# Build shared libraries (needed for dynamic linking)
./configure --with-shared
```

### 4.4 After configure Runs

`configure` generates `mk/config.mk` which records all selected options. You can inspect it:

```bash
# View selected configuration
cat mk/config.mk | grep -v '^#' | grep -v '^$'
```

If you change configure options, always run `make clean` first to avoid stale object files from a previous configuration:

```bash
./configure --enable-debug
make clean
make -j$(nproc)
```

---

## 5. Building SPDK

### 5.1 Standard Build

```bash
# Build using all available CPU cores
# Takes 5-15 minutes depending on hardware and options
make -j$(nproc)
```

`$(nproc)` expands to the number of logical CPU cores. On a 8-core machine this becomes `make -j8`.

### 5.2 Build Outputs

After a successful build:

```
build/
├── lib/
│   ├── libspdk_nvme.a        # NVMe driver library
│   ├── libspdk_bdev.a        # Block device abstraction layer
│   ├── libspdk_env_dpdk.a    # DPDK environment
│   └── ...                   # ~50+ static libraries
├── examples/
│   ├── nvme/hello_world      # Basic NVMe probe example
│   └── ...
└── app/
    ├── spdk_tgt              # Multi-protocol storage target
    ├── nvmf_tgt              # NVMe-oF target
    └── ...
```

Verify the build produced libraries:

```bash
# List built libraries (should be 40-60 files)
ls build/lib/libspdk_*.a | wc -l

# Check a specific library was built
ls -lh build/lib/libspdk_nvme.a
```

### 5.3 Incremental Builds

After changing source files, you do not need `make clean`. A plain `make` rebuilds only changed files:

```bash
# Edit a source file, then:
make -j$(nproc)
```

`make clean` is needed when:
- Switching between `--enable-debug` and release configurations
- Changing configure flags that affect compilation flags
- After `git pull` introduces changes to makefiles or headers

### 5.4 Building Specific Targets

```bash
# Build only the NVMe example
make -j$(nproc) examples/nvme/hello_world/hello_world

# Build only libraries (no examples or tests)
make -j$(nproc) libs

# Build and run unit tests
make -j$(nproc) unittest
```

---

## 6. Hugepage Configuration

### 6.1 Why SPDK Needs Hugepages

DPDK (SPDK's memory subsystem) uses hugepages to:
1. Reduce TLB pressure: fewer page table entries for the same memory range
2. Enable DMA: physical addresses of hugepages are stable and contiguous
3. Avoid page faults: hugepages are pinned in physical memory

Without hugepages, SPDK applications fail at startup with errors like:
```
EAL: Not enough memory available on socket 0!
```

### 6.2 Hugepage Sizes

Linux supports two hugepage sizes on x86_64:
- **2 MB** (default): Standard choice, always available
- **1 GB** (gigantic pages): Better performance, requires kernel boot parameter

For most development work, 2 MB hugepages are sufficient.

### 6.3 Configuring 2 MB Hugepages

```bash
# Check current hugepage status
grep -i huge /proc/meminfo

# Allocate 2048 hugepages (4 GB total)
sudo sh -c 'echo 2048 > /proc/sys/vm/nr_hugepages'

# Verify allocation succeeded
grep HugePages_Total /proc/meminfo
# Expected: HugePages_Total:     2048

# Make persistent across reboots
echo 'vm.nr_hugepages = 2048' | sudo tee /etc/sysctl.d/99-spdk-hugepages.conf
sudo sysctl -p /etc/sysctl.d/99-spdk-hugepages.conf
```

How many hugepages you need depends on your workload. SPDK defaults to 2048 MB (1024 pages of 2 MB). For development, 1024 pages (2 GB) is usually enough.

### 6.4 Configuring 1 GB Hugepages

1 GB hugepages must be reserved at boot time. They cannot be allocated after the kernel has started:

```bash
# Add to GRUB_CMDLINE_LINUX in /etc/default/grub:
# hugepagesz=1G hugepages=4
sudo vim /etc/default/grub

# Update GRUB and reboot
sudo update-grub   # Ubuntu/Debian
# or
sudo grub2-mkconfig -o /boot/grub2/grub.cfg  # Fedora/RHEL

sudo reboot

# After reboot, verify:
grep -i huge /proc/meminfo
# HugePages_Total:       4
# Hugepagesize:    1048576 kB
```

### 6.5 NUMA-Aware Hugepage Allocation

On NUMA systems (multi-socket servers), allocate hugepages on specific nodes using `setup.sh` environment variables:

```bash
# Allocate 1024 hugepages on NUMA node 0 only
sudo HUGENODE=0 NRHUGE=1024 scripts/setup.sh

# Allocate different amounts per node
sudo HUGENODE='nodes_hp[0]=2048,nodes_hp[1]=512,2' scripts/setup.sh
# node0: 2048 pages, node1: 512 pages, node2: default (NRHUGE)
```

Check NUMA topology:
```bash
# Show NUMA nodes and their memory
numactl --hardware

# Show which hugepages are on which node
cat /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
```

### 6.6 Hugetlbfs Mount

DPDK requires hugetlbfs to be mounted. `setup.sh` handles this automatically, but you can also mount it manually:

```bash
# Check if already mounted
mount | grep hugetlbfs

# Mount manually (if not mounted)
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge

# Persist in /etc/fstab
echo 'nodev /mnt/huge hugetlbfs defaults 0 0' | sudo tee -a /etc/fstab
```

---

## 7. IOMMU Configuration

### 7.1 Why IOMMU Matters

VFIO (Virtual Function I/O) is the preferred driver for binding PCI devices to userspace because it provides:
- DMA protection via IOMMU: prevents a userspace driver from accessing memory it should not
- No kernel module required beyond `vfio-pci`
- Works with containers and VMs

Without IOMMU, SPDK falls back to `uio_pci_generic`, which provides no DMA isolation. For development this is acceptable; for production use VFIO.

### 7.2 Enabling IOMMU

Edit the GRUB configuration:

```bash
sudo vim /etc/default/grub
```

For Intel CPUs, find `GRUB_CMDLINE_LINUX` and add `intel_iommu=on iommu=pt`:
```
GRUB_CMDLINE_LINUX="quiet splash intel_iommu=on iommu=pt"
```

For AMD CPUs:
```
GRUB_CMDLINE_LINUX="quiet splash amd_iommu=on iommu=pt"
```

The `iommu=pt` flag enables passthrough mode, which improves performance for devices not assigned to VMs.

```bash
# Apply GRUB changes
sudo update-grub   # Ubuntu/Debian
# or
sudo grub2-mkconfig -o /boot/grub2/grub.cfg  # Fedora/RHEL

sudo reboot
```

### 7.3 Verifying IOMMU

```bash
# Check kernel recognized IOMMU
dmesg | grep -i iommu | head -10

# Expected for Intel:
# DMAR: IOMMU enabled
# DMAR-IR: Enabled IRQ remapping in xapic mode

# Check IOMMU groups exist
ls /sys/kernel/iommu_groups/ | wc -l
# Should print a nonzero number
```

---

## 8. Binding Devices with setup.sh

### 8.1 What setup.sh Does

`scripts/setup.sh` is the primary tool for preparing the system to run SPDK applications. It:
1. Allocates hugepages (via HUGEMEM or NRHUGE environment variables)
2. Loads required kernel modules (`vfio-pci` or `uio_pci_generic`)
3. Detects NVMe, I/OAT, VMD, and Virtio PCI devices
4. Unbinds them from their kernel drivers
5. Binds them to `vfio-pci` (or `uio_pci_generic` as fallback)

### 8.2 Running setup.sh

```bash
# Default: bind all compatible devices, allocate 2048 MB hugepages
sudo scripts/setup.sh

# Check status of all SPDK-compatible devices
sudo scripts/setup.sh status

# Reset: rebind all devices back to kernel drivers
sudo scripts/setup.sh reset

# Cleanup orphaned SPDK files after a crash
sudo scripts/setup.sh cleanup
```

### 8.3 setup.sh Environment Variables

```bash
# Control hugepage memory size (in MB, default: 2048)
sudo HUGEMEM=4096 scripts/setup.sh

# Control number of hugepages directly (overrides HUGEMEM)
sudo NRHUGE=1024 scripts/setup.sh

# Allocate hugepages on a specific NUMA node
sudo HUGENODE=0 scripts/setup.sh

# Control hugepage size (in kB)
sudo HUGEPGSZ=2048 scripts/setup.sh   # 2 MB pages
sudo HUGEPGSZ=1048576 scripts/setup.sh # 1 GB pages

# Bind only specific PCI devices
sudo PCI_ALLOWED="0000:01:00.0 0000:02:00.0" scripts/setup.sh

# Block specific devices (keep them kernel-owned)
sudo PCI_BLOCKED="0000:01:00.0" scripts/setup.sh

# Force a specific driver instead of auto-selecting vfio-pci/uio_pci_generic
sudo DRIVER_OVERRIDE=uio_pci_generic scripts/setup.sh

# Set ownership of hugepage mountpoint and vfio groups
sudo TARGET_USER=$USER scripts/setup.sh

# Bind only NVMe devices (skip I/OAT, VMD, Virtio)
sudo DEV_TYPE=NVME scripts/setup.sh
```

### 8.4 Checking Device Binding

```bash
# List NVMe devices before binding
lspci | grep -i nvme

# Check device status after setup.sh
sudo scripts/setup.sh status

# Example output:
# BDF         Vendor  Device  NUMA  Driver     Device name
# 0000:01:00.0 8086    0953    0     vfio-pci   -

# Manually check a device's driver
cat /sys/bus/pci/devices/0000:01:00.0/driver/module/drivers
```

### 8.5 Manual Device Binding (Advanced)

When you need fine-grained control over individual devices:

```bash
# Find the PCI address of your NVMe device
lspci | grep NVMe
# 0000:01:00.0 Non-Volatile memory controller: ...

# Load vfio-pci module
sudo modprobe vfio-pci

# Get device vendor:device ID
cat /sys/bus/pci/devices/0000:01:00.0/vendor
cat /sys/bus/pci/devices/0000:01:00.0/device

# Unbind from current driver
echo "0000:01:00.0" | sudo tee /sys/bus/pci/devices/0000:01:00.0/driver/unbind

# Bind to vfio-pci
echo "0000:01:00.0" | sudo tee /sys/bus/pci/drivers/vfio-pci/bind

# Verify binding
ls -la /sys/bus/pci/devices/0000:01:00.0/driver
```

---

## 9. Verifying the Environment

### 9.1 Quick Verification with hello_world

The `hello_world` example probes NVMe devices and reports what it finds:

```bash
# Run hello_world (requires bound NVMe device)
sudo ./build/examples/nvme/hello_world

# Expected output (with one NVMe device):
# Initializing NVMe Controllers
# Attached to 0000:01:00.0
# Using controller INTEL SSDPED... (BTPY...)
# Namespace ID: 1 size: 375GB
# Initialization complete.
# Hello World!
```

If no NVMe devices are bound, hello_world exits immediately without error. Use `setup.sh status` to confirm devices are bound.

### 9.2 Using the Null Block Device (No NVMe Required)

SPDK includes a null bdev for testing without real hardware:

```bash
# Run bdevperf with null bdev (no hardware needed)
sudo ./build/examples/bdev/bdevperf/bdevperf \
    -z \
    -q 128 \
    -o 4096 \
    -w randrw \
    -M 50 \
    -t 5 \
    -b Null0 \
    --json <(echo '{"subsystems":[{"subsystem":"bdev","config":[{"method":"bdev_null_create","params":{"name":"Null0","num_blocks":204800,"block_size":512}}]}]}')
```

### 9.3 Running the Full Test Suite

```bash
# Run unit tests (no hardware required, ~5-10 minutes)
sudo ./test/unit/unittest.sh

# Run functional tests (requires NVMe hardware, ~30-60 minutes)
sudo ./test/nvme/nvme.sh

# Run the complete test suite (slow, requires hardware)
sudo ./test/run_tests.sh
```

The unit test suite can be run without root on some configurations, but hardware-dependent tests always require root because they access PCI devices directly.

---

## 10. Development Environment Setup

### 10.1 VSCode Configuration

Create a VSCode C/C++ configuration for IntelliSense to find SPDK and DPDK headers:

```json
// .vscode/c_cpp_properties.json
{
    "configurations": [
        {
            "name": "SPDK Linux",
            "includePath": [
                "${workspaceFolder}/include",
                "${workspaceFolder}/include/spdk",
                "${workspaceFolder}/include/spdk_internal",
                "${workspaceFolder}/dpdk/lib/eal/include",
                "${workspaceFolder}/dpdk/lib/eal/linux/include",
                "${workspaceFolder}/dpdk/lib/eal/x86/include",
                "${workspaceFolder}/dpdk/config",
                "${workspaceFolder}/lib"
            ],
            "defines": [
                "SPDK_CONFIG_DEBUG",
                "__linux__"
            ],
            "compilerPath": "/usr/bin/gcc",
            "cStandard": "c11",
            "intelliSenseMode": "gcc-x64",
            "compileCommands": "${workspaceFolder}/compile_commands.json"
        }
    ],
    "version": 4
}
```

Generate `compile_commands.json` for accurate IntelliSense:

```bash
# Install bear (Build EAR)
sudo apt-get install bear   # Ubuntu
# or
sudo dnf install bear       # Fedora

# Generate compile_commands.json
bear -- make -j$(nproc)
```

With `compile_commands.json`, VSCode understands the exact compiler flags used for each file, making Go-to-Definition and Find-References accurate.

Recommended VSCode extensions for SPDK development:
- **C/C++** (Microsoft): IntelliSense, debugging
- **clangd**: Alternative to Microsoft C/C++, faster indexing
- **GitLens**: Blame annotations, history
- **Hex Editor**: Useful when inspecting raw I/O buffers

### 10.2 Vim/Neovim Configuration

For Vim/Neovim users, clangd with `compile_commands.json` provides the same capabilities:

```bash
# Install clangd
sudo apt-get install clangd   # Ubuntu

# ~/.config/nvim/init.vim or similar
# Use nvim-lspconfig to configure clangd:
# lua require('lspconfig').clangd.setup{}
```

Create a `.clangd` file at the SPDK root to provide per-project clangd configuration:

```yaml
# .clangd
CompileFlags:
  CompilationDatabase: .
  Add: [-Wno-error]
```

### 10.3 Code Formatting

SPDK enforces a strict coding style checked by CI. Run the formatter before committing:

```bash
# Install clang-format (via pkgdep.sh --developer-tools or manually)
sudo apt-get install clang-format

# Check formatting of a file
./scripts/check_format.sh include/spdk/nvme.h

# Check entire working tree
./scripts/check_format.sh
```

SPDK uses a custom `.clang-format` file at the repository root. It is based on Linux kernel style with specific SPDK adjustments.

### 10.4 Debugging with GDB

```bash
# Build with debug symbols (required for useful GDB output)
./configure --enable-debug
make clean && make -j$(nproc)

# Launch an SPDK application under GDB
sudo gdb --args ./build/examples/nvme/hello_world

# Useful GDB commands for SPDK development:
(gdb) set follow-fork-mode child   # Follow forked processes
(gdb) break spdk_nvme_probe        # Set breakpoint at SPDK function
(gdb) break nvme_pcie_ctrlr_construct  # Set breakpoint in driver internals
(gdb) run
(gdb) backtrace                    # Print call stack on crash
(gdb) info threads                 # List all threads (reactors)
(gdb) thread 2                     # Switch to reactor thread
(gdb) frame 3                      # Inspect a specific stack frame
(gdb) print *ctrlr                 # Print a structure's contents
(gdb) watch ctrlr->state           # Watchpoint on a field
```

For multi-threaded SPDK applications, each reactor runs on its own thread. When debugging a crash, `info threads` and `thread N` let you inspect each reactor's state.

### 10.5 Debugging with AddressSanitizer

For memory bugs, AddressSanitizer is more effective than GDB:

```bash
# Build with ASan
./configure --enable-debug --enable-asan
make clean && make -j$(nproc)

# Run normally (ASan intercepts bad memory accesses automatically)
sudo ./build/examples/nvme/hello_world

# ASan output on a use-after-free:
# ==12345==ERROR: AddressSanitizer: heap-use-after-free
# READ of size 8 at 0x602000000010 thread T0
# ...
```

Note: ASan adds significant overhead (~2x slowdown). Do not use it for performance measurements.

### 10.6 Perf and Tracing

```bash
# Profile an SPDK application with perf
sudo perf record -g -p $(pgrep spdk_tgt) -- sleep 30
sudo perf report

# Record CPU cycles for a specific function
sudo perf stat -e cache-misses,cache-references,instructions \
    ./build/examples/nvme/hello_world

# SPDK supports USDT probes when built with --with-usdt
./configure --with-usdt
make -j$(nproc)

# Trace SPDK events with bpftrace (requires --with-usdt build)
sudo bpftrace -e 'usdt:/path/to/app:spdk:*{ printf("%s\n", probe); }'
```

---

## 11. Common Build Issues and Troubleshooting

### 11.1 Submodule Not Initialized

**Symptom**: `make` fails with missing header errors from dpdk/ or isa-l/ directories.

```
fatal error: rte_config.h: No such file or directory
```

**Solution**:
```bash
git submodule update --init
```

### 11.2 Python pyelftools Missing

**Symptom**: DPDK build fails with a Python import error.

```
ModuleNotFoundError: No module named 'elftools'
```

**Solution**:
```bash
pip3 install pyelftools
# or via package manager:
sudo apt-get install python3-pyelftools
```

### 11.3 nasm Not Found

**Symptom**: Build fails when compiling ISA-L or DPDK crypto code.

```
/bin/sh: 1: nasm: not found
```

**Solution**:
```bash
sudo apt-get install nasm   # Ubuntu
sudo dnf install nasm       # Fedora
```

### 11.4 No Hugepages Available

**Symptom**: SPDK application exits immediately.

```
EAL: Not enough memory available on socket 0! Requested: 2048MB
DPDK init failed with error -12
```

**Solution**:
```bash
# Allocate hugepages
sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'

# Verify allocation (Free must equal Total)
grep HugePages /proc/meminfo

# If allocation fails (not enough contiguous memory), reboot and try again
```

### 11.5 hugepages Allocated but Not Free

**Symptom**: Hugepages are allocated but an application fails to start.

```
grep HugePages /proc/meminfo
HugePages_Total:    1024
HugePages_Free:        0   # All pages are in use by another process
```

**Solution**: Find and terminate the process holding hugepages:
```bash
# Find processes using hugepages
sudo fuser /mnt/huge/*

# Or look for SPDK processes
pgrep -a spdk
pgrep -a dpdk
```

### 11.6 Permission Denied on VFIO Device

**Symptom**: Application cannot open `/dev/vfio/N`.

```
Failed to open /dev/vfio/1: Permission denied
```

**Solution**:
```bash
# Option 1: Run with sudo
sudo ./build/examples/nvme/hello_world

# Option 2: Set ownership with TARGET_USER
sudo TARGET_USER=$USER scripts/setup.sh

# Option 3: Add user to vfio group (if group exists)
sudo usermod -aG vfio $USER
newgrp vfio
```

### 11.7 Device is Busy (Mounted Filesystem)

**Symptom**: `setup.sh` skips an NVMe device.

```
NVMe 0000:01:00.0 has active mountpoints, will not bind
```

**Solution**:
```bash
# Find and unmount filesystems on the device
lsblk /dev/nvme0n1
sudo umount /dev/nvme0n1p1

# Then re-run setup
sudo scripts/setup.sh
```

Never bind an NVMe device that contains a mounted filesystem. The kernel driver is managing it, and forcibly rebinding will corrupt data.

### 11.8 IOMMU Not Enabled

**Symptom**: `setup.sh` falls back to `uio_pci_generic` instead of `vfio-pci`.

```
IOMMU not available, falling back to uio_pci_generic
```

**Solution**: Enable IOMMU in BIOS and add kernel boot parameters as described in Section 7.2. After enabling, verify:

```bash
dmesg | grep -i "iommu enabled"
```

### 11.9 Linker Errors When Building Applications Against SPDK

**Symptom**: Linking a custom application fails with undefined symbols.

```
undefined reference to `spdk_nvme_probe'
```

**Solution**: SPDK libraries have internal dependencies that must be listed in the correct order. Use `pkg-config` or the SPDK linker flags helper:

```bash
# Get the correct linker flags
SPDK_ROOT=/path/to/spdk
$(SPDK_ROOT)/scripts/pkgdep/get_ldflags.sh

# Or use pkg-config if libraries were installed
pkg-config --libs spdk_nvme spdk_env_dpdk
```

---

## 12. Virtual Machine and Container Environments

### 12.1 Developing in a VM

VMs are viable for SPDK development when physical NVMe hardware is unavailable. There are two approaches:

**Approach 1: Null/AIO bdev (no hardware passthrough)**

Most SPDK subsystems can be tested without real NVMe hardware using the null bdev or AIO bdev (which wraps a file or block device):

```bash
# Inside VM: no special setup needed
sudo scripts/setup.sh        # Allocates hugepages
make -j$(nproc)              # Build normally

# Use null bdev in tests (no hardware required)
sudo ./build/examples/bdev/bdevperf/bdevperf -z -q 128 -o 4096 -w randread -t 5
```

**Approach 2: NVMe passthrough (real hardware performance)**

Pass an NVMe device from the host directly into the VM using VFIO:

```bash
# On host: bind NVMe to vfio-pci
sudo PCI_ALLOWED="0000:01:00.0" scripts/setup.sh

# QEMU invocation with NVMe passthrough
sudo qemu-system-x86_64 \
    -device vfio-pci,host=01:00.0 \
    -m 8G \
    -cpu host \
    -enable-kvm \
    ...

# Inside VM: SPDK sees the device natively
sudo scripts/setup.sh
sudo ./build/examples/nvme/hello_world
```

### 12.2 SPDK in Docker

Docker containers can run SPDK for development and CI, with some constraints: hugepages must be available on the host and shared with the container.

```dockerfile
# Dockerfile for SPDK development environment
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    git gcc g++ make python3 python3-pip \
    pkg-config nasm libaio-dev libssl-dev \
    libjson-c-dev libcunit1-dev uuid-dev \
    python3-pyelftools \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /spdk
COPY . .
RUN git submodule update --init
RUN ./configure --enable-debug
RUN make -j$(nproc)
```

Run the container with hugepage and device access:

```bash
# Run container with access to hugepages and VFIO
docker run -it \
    --privileged \
    --cap-add SYS_ADMIN \
    -v /dev/hugepages:/dev/hugepages \
    -v /dev/vfio:/dev/vfio \
    spdk-dev:latest \
    bash

# Inside container: hugepages must already be allocated on host
sudo scripts/setup.sh
./build/examples/nvme/hello_world
```

The `--privileged` flag is needed for PCI device access. In CI environments, a more restrictive approach uses specific capability flags and bind-mounts only the required devices.

### 12.3 Vagrant Development Environment

SPDK includes Vagrant configuration for a reproducible development VM:

```bash
# Install Vagrant and VirtualBox or libvirt
# Then from the SPDK source directory:
cd scripts/vagrant
vagrant up

# Connect to the VM
vagrant ssh

# Inside VM: SPDK source is at /spdk
cd /spdk
sudo scripts/setup.sh
make -j$(nproc)
./build/examples/nvme/hello_world
```

The Vagrant environment pre-configures hugepages and provides a stable Ubuntu base image that matches SPDK CI.

---

## 13. Environment Checklist

Use this checklist to verify your environment is fully configured before proceeding to Module 11.

```
System Requirements
  [ ] x86_64 or ARM64 CPU
  [ ] 8+ GB RAM
  [ ] Linux kernel 4.0+ (5.x recommended)
  [ ] GCC 7+ installed

Repository
  [ ] git clone https://github.com/spdk/spdk
  [ ] git submodule update --init completed successfully

Dependencies
  [ ] sudo scripts/pkgdep.sh completed without errors
  [ ] python3 -c "import elftools" exits without error
  [ ] nasm --version works

Build
  [ ] ./configure completed (check mk/config.mk exists)
  [ ] make -j$(nproc) completed without errors
  [ ] ls build/lib/libspdk_nvme.a returns a file

Hugepages
  [ ] grep HugePages_Free /proc/meminfo shows > 0

Device Access (if using real NVMe)
  [ ] sudo scripts/setup.sh status shows device bound to vfio-pci or uio_pci_generic

Verification
  [ ] sudo ./build/examples/nvme/hello_world runs without error
```

---

## Summary

Setting up SPDK requires attention to several layers of the system that most userspace library setups do not touch:

- **Hugepages** provide the DMA-capable, pinned memory that SPDK's polled drivers require
- **IOMMU + VFIO** give userspace code safe, isolated access to PCI hardware
- **setup.sh** orchestrates device binding and hugepage allocation in a single step
- **configure flags** let you trade compile time and binary size against debug capability and optional features
- **bear + compile_commands.json** are the key to getting accurate IDE support in a complex C codebase like SPDK

With a working environment, Module 11 begins writing SPDK applications directly.

**Next**: [11-S2-Hello-World.md](./11-S2-Hello-World.md)

---

*End of Module 10*

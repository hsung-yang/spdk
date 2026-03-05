#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024-2026 CPCS Implementation Team. All rights reserved.

# CPCS Vagrant Test Runner
# This script automates running CPCS tests in a Vagrant VM environment

set -e

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
SPDK_ROOT=$(readlink -f "$SCRIPT_DIR/../..")
VAGRANT_DIR="$SPDK_ROOT/scripts/vagrant"
TEST_VM_DIR="${TEST_VM_DIR:-$HOME/spdk-cpcs-test-vm}"

# Default configuration
DISTRO="${SPDK_VAGRANT_DISTRO:-ubuntu2204}"
VMCPU="${SPDK_VAGRANT_VMCPU:-4}"
VMRAM="${SPDK_VAGRANT_VMRAM:-8192}"
PROVIDER="${SPDK_VAGRANT_PROVIDER:-virtualbox}"
SKIP_UBPF="${SKIP_UBPF:-1}"
RUN_EBPF_UT="${RUN_EBPF_UT:-0}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

function info() {
    echo -e "${GREEN}[INFO]${NC} $*"
}

function warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

function error() {
    echo -e "${RED}[ERROR]${NC} $*"
}

function check_prerequisites() {
    info "Checking prerequisites..."

    # Check for vagrant
    if ! command -v vagrant &> /dev/null; then
        error "Vagrant is not installed!"
        echo ""
        echo "Please install Vagrant:"
        echo "  macOS:   brew install vagrant"
        echo "  Linux:   https://www.vagrantup.com/downloads"
        echo "  Windows: https://www.vagrantup.com/downloads"
        exit 1
    fi

    info "Vagrant version: $(vagrant --version)"

    # Check for VirtualBox or libvirt
    if [[ "$PROVIDER" == "virtualbox" ]]; then
        if ! command -v vboxmanage &> /dev/null; then
            error "VirtualBox is not installed!"
            echo ""
            echo "Please install VirtualBox:"
            echo "  macOS:  brew install --cask virtualbox"
            echo "  Linux:  https://www.virtualbox.org/wiki/Linux_Downloads"
            exit 1
        fi
        info "VirtualBox version: $(vboxmanage --version)"
    elif [[ "$PROVIDER" == "libvirt" ]]; then
        if ! command -v virsh &> /dev/null; then
            error "libvirt is not installed!"
            exit 1
        fi
        info "libvirt available"
    fi

    info "Prerequisites check passed!"
}

function create_vm() {
    info "Creating test VM in $TEST_VM_DIR..."

    mkdir -p "$TEST_VM_DIR"
    cd "$TEST_VM_DIR"

    # Copy vagrant files
    cp "$VAGRANT_DIR/Vagrantfile" .

    # Set environment variables for VM creation
    export SPDK_VAGRANT_DISTRO="$DISTRO"
    export SPDK_VAGRANT_VMCPU="$VMCPU"
    export SPDK_VAGRANT_VMRAM="$VMRAM"
    export SPDK_VAGRANT_PROVIDER="$PROVIDER"
    export COPY_SPDK_DIR=1
    export SPDK_DIR="$SPDK_ROOT"
    # Optional: keep VM bootstrap minimal and do explicit setup in setup_vm()
    export DEPLOY_TEST_VM="${DEPLOY_TEST_VM:-0}"

    info "VM Configuration:"
    info "  Distro:   $DISTRO"
    info "  CPUs:     $VMCPU"
    info "  RAM:      $VMRAM MB"
    info "  Provider: $PROVIDER"

    # Create and provision VM
    if vagrant up --provider="$PROVIDER"; then
        info "VM created successfully!"
    else
        error "Failed to create VM"
        exit 1
    fi
}

function setup_vm() {
    info "Setting up SPDK in VM..."

    cd "$TEST_VM_DIR"

    # Install base build dependencies first to avoid distro-specific bootstrap failures.
    vagrant ssh -c "if command -v apt-get >/dev/null 2>&1; then \
        sudo sh -c 'printf \"nameserver 1.1.1.1\\nnameserver 8.8.8.8\\n\" > /etc/resolv.conf' && \
        sudo apt-get update && sudo apt-get install -y \
        build-essential meson ninja-build pkg-config \
        python3 python3-pip python3-pyelftools \
        libaio-dev libssl-dev liburing-dev \
        libnuma-dev uuid-dev libcunit1-dev \
        nasm libpciaccess-dev git \
        autoconf automake libtool libncurses-dev; \
    elif command -v dnf >/dev/null 2>&1; then \
        sudo dnf install -y \
        gcc gcc-c++ make meson ninja-build pkgconf-pkg-config \
        python3 python3-pip python3-pyelftools \
        libaio-devel openssl-devel liburing-devel \
        numactl-devel libuuid-devel CUnit-devel \
        nasm libpciaccess-devel git \
        autoconf automake libtool ncurses-devel; \
    elif command -v yum >/dev/null 2>&1; then \
        sudo yum install -y \
        gcc gcc-c++ make meson ninja-build pkgconfig \
        python3 python3-pip \
        libaio-devel openssl-devel liburing-devel \
        numactl-devel libuuid-devel CUnit-devel \
        nasm libpciaccess-devel git \
        autoconf automake libtool ncurses-devel; \
    else \
        echo 'No supported package manager found' >&2; \
        exit 1; \
    fi" || {
        error "Failed to install base build dependencies"
        exit 1
    }

    # Optional uBPF install (required only for eBPF runtime unit tests).
    if [[ "$SKIP_UBPF" != "1" ]]; then
        vagrant ssh -c "if [ ! -f /usr/local/include/ubpf.h ]; then \
            if command -v apt-get >/dev/null 2>&1; then \
                sudo apt-get update && sudo apt-get install -y build-essential git cmake; \
            elif command -v dnf >/dev/null 2>&1; then \
                sudo dnf install -y make gcc gcc-c++ git cmake; \
            elif command -v yum >/dev/null 2>&1; then \
                sudo yum install -y make gcc gcc-c++ git cmake; \
            else \
                echo 'No supported package manager found for uBPF install' >&2; \
                exit 1; \
            fi && \
            rm -rf /tmp/ubpf && \
            GIT_SSL_NO_VERIFY=true git clone https://github.com/iovisor/ubpf.git /tmp/ubpf && \
            cd /tmp/ubpf && \
            cmake -S . -B build -DUBPF_ENABLE_INSTALL=ON -DUBPF_SKIP_EXTERNAL=ON && \
            cmake --build build && \
            sudo cmake --install build; \
        fi" || {
            error "Failed to install uBPF"
            exit 1
        }
    else
        info "Skipping uBPF install (SKIP_UBPF=1)"
    fi

    # Build SPDK
    vagrant ssh -c 'cd /home/vagrant/spdk_repo/spdk && ./configure --without-nvme-cuse && JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN || echo 1) && make -j${JOBS}' || {
        error "Failed to build SPDK"
        exit 1
    }

    info "SPDK built successfully!"
}

function run_tests() {
    info "Running CPCS tests in VM..."

    cd "$TEST_VM_DIR"

    # Upload frequently edited test scripts without syncing the full tree.
    # Full rsync can delete guest build artifacts when host build dirs are absent.
    vagrant upload "$SPDK_ROOT/test/cpcs/cpcs.sh" "/home/vagrant/spdk_repo/spdk/test/cpcs/cpcs.sh" || {
        error "Failed to upload CPCS test script into VM"
        return 1
    }

    # Ensure SPDK target exists (it may be missing after source refresh operations).
    vagrant ssh -c "test -x /home/vagrant/spdk_repo/spdk/build/bin/spdk_tgt" || {
        warn "spdk_tgt not found in VM; rebuilding SPDK"
        vagrant ssh -c 'cd /home/vagrant/spdk_repo/spdk && ./configure --without-nvme-cuse && JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN || echo 1) && make -j${JOBS}' || {
            error "Failed to rebuild SPDK in VM"
            return 1
        }
    }

    # SPDK target requires hugepages in the guest. Ensure they are configured
    # for this test run and cleaned up afterwards.
    vagrant ssh -c "cd /home/vagrant/spdk_repo/spdk && sudo scripts/setup.sh" || {
        error "Failed to configure hugepages in VM"
        return 1
    }

    cleanup_hugepages() {
        vagrant ssh -c "cd /home/vagrant/spdk_repo/spdk && sudo scripts/setup.sh reset" >/dev/null 2>&1 || true
    }
    trap cleanup_hugepages EXIT

    # Run CPCS integration tests
    vagrant ssh -c "cd /home/vagrant/spdk_repo/spdk && sudo ./test/cpcs/cpcs.sh" || {
        error "CPCS integration tests failed"
        return 1
    }

    info "CPCS integration tests passed!"

    # Run reachability tests
    vagrant ssh -c "cd /home/vagrant/spdk_repo/spdk/test/cpcs/reachability_test && make && sudo ./reachability_test" || {
        error "Reachability tests failed"
        return 1
    }

    info "Reachability tests passed!"

    # Run SLM basic tests
    vagrant ssh -c "cd /home/vagrant/spdk_repo/spdk/test/cpcs/slm_basic_test && sudo ./slm_basic_test.sh" || {
        error "SLM basic tests failed"
        return 1
    }

    info "SLM basic tests passed!"

    # Run builtin smoke tests
    vagrant ssh -c "cd /home/vagrant/spdk_repo/spdk/test/cpcs/cpcs_builtin_smoke && make && sudo ./cpcs_builtin_smoke" || {
        error "Builtin smoke tests failed"
        return 1
    }

    info "Builtin smoke tests passed!"

    if [[ "$RUN_EBPF_UT" == "1" ]]; then
        # Run eBPF runtime unit tests
        vagrant ssh -c "cd /home/vagrant/spdk_repo/spdk/test/unit/lib/nvmf/cpcs_ebpf.c && make && ./cpcs_ebpf_ut" || {
            error "eBPF runtime unit tests failed"
            return 1
        }

        info "eBPF runtime unit tests passed!"
    else
        info "Skipping eBPF runtime unit tests (RUN_EBPF_UT=0)"
    fi

    info "All CPCS tests passed successfully!"

    # The run succeeded; remove trap and cleanup explicitly for deterministic flow.
    trap - EXIT
    cleanup_hugepages
}

function cleanup_vm() {
    info "Cleaning up VM..."
    cd "$TEST_VM_DIR"
    vagrant destroy -f
    info "VM destroyed"
}

function show_usage() {
    cat << EOF
Usage: $0 [OPTIONS] [COMMAND]

CPCS Vagrant Test Runner - Automated testing in VM environment

OPTIONS:
    -d, --distro <distro>       VM distribution (default: ubuntu2204)
                                Options: ubuntu2004, ubuntu2204, fedora37, fedora38
    -c, --cpus <num>           Number of CPUs (default: 4)
    -m, --mem <mb>             RAM in MB (default: 8192)
    -p, --provider <provider>   vagrant provider (default: virtualbox)
                                Options: virtualbox, libvirt
    --vm-dir <path>            VM directory (default: ~/spdk-cpcs-test-vm)
    -h, --help                 Show this help

COMMANDS:
    create      Create and provision VM
    setup       Build SPDK in VM
    test        Run CPCS tests
    all         Create VM, build SPDK, and run tests (default)
    cleanup     Destroy VM
    shell       Open SSH shell to VM

EXAMPLES:
    # Run all tests (create VM, build, test)
    $0

    # Create VM with custom configuration
    $0 -d fedora38 -c 8 -m 16384 create

    # Just run tests (VM must already exist)
    $0 test

    # Open shell to VM
    $0 shell

    # Cleanup after testing
    $0 cleanup

ENVIRONMENT VARIABLES:
    SPDK_VAGRANT_DISTRO    Override default distribution
    SPDK_VAGRANT_VMCPU     Override default CPU count
    SPDK_VAGRANT_VMRAM     Override default RAM size
    SPDK_VAGRANT_PROVIDER  Override default provider
    TEST_VM_DIR            Override default VM directory

EOF
}

# Parse command line arguments
COMMAND="all"
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--distro)
            DISTRO="$2"
            shift 2
            ;;
        -c|--cpus)
            VMCPU="$2"
            shift 2
            ;;
        -m|--mem)
            VMRAM="$2"
            shift 2
            ;;
        -p|--provider)
            PROVIDER="$2"
            shift 2
            ;;
        --vm-dir)
            TEST_VM_DIR="$2"
            shift 2
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        create|setup|test|all|cleanup|shell)
            COMMAND="$1"
            shift
            ;;
        *)
            error "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
done

# Main execution
info "CPCS Vagrant Test Runner"
info "======================="

check_prerequisites

case "$COMMAND" in
    create)
        create_vm
        ;;
    setup)
        setup_vm
        ;;
    test)
        run_tests
        ;;
    all)
        create_vm
        setup_vm
        run_tests
        ;;
    cleanup)
        cleanup_vm
        ;;
    shell)
        cd "$TEST_VM_DIR"
        vagrant ssh
        ;;
    *)
        error "Unknown command: $COMMAND"
        show_usage
        exit 1
        ;;
esac

info "Done!"

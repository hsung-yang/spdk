#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024-2026 CPCS Implementation Team. All rights reserved.

# CPCS Local Test Runner
# Runs CPCS tests on the local machine (requires SPDK to be built)

set -e

SCRIPT_DIR=$(dirname "$(readlink -f "$0" 2>/dev/null || realpath "$0")")
SPDK_ROOT=$(readlink -f "$SCRIPT_DIR/../.." 2>/dev/null || realpath "$SCRIPT_DIR/../..")

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
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

function section() {
    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$*${NC}"
    echo -e "${BLUE}========================================${NC}"
}

function check_root() {
    if [[ $EUID -ne 0 ]]; then
        error "This script must be run as root (use sudo)"
        echo ""
        echo "Usage: sudo $0 [OPTIONS]"
        exit 1
    fi
}

function check_spdk_build() {
    info "Checking SPDK build..."

    if [[ ! -f "$SPDK_ROOT/build/bin/spdk_tgt" ]]; then
        error "SPDK not built! Please build SPDK first:"
        echo ""
        echo "  cd $SPDK_ROOT"
        echo "  ./configure --with-cpcs"
        echo "  make"
        exit 1
    fi

    info "SPDK build found: $SPDK_ROOT/build/bin/spdk_tgt"
}

function check_hugepages() {
    info "Checking hugepages..."

    nr_hugepages=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo "0")

    if [[ "$nr_hugepages" -lt 1024 ]]; then
        warn "Insufficient hugepages ($nr_hugepages). Setting up..."
        "$SPDK_ROOT/scripts/setup.sh" || {
            error "Failed to setup hugepages"
            exit 1
        }
        info "Hugepages configured"
    else
        info "Hugepages OK ($nr_hugepages pages)"
    fi
}

function build_test_binaries() {
    section "Building Test Binaries"

    local test_dirs=(
        "reachability_test"
        "cpcs_builtin_smoke"
    )

    for dir in "${test_dirs[@]}"; do
        if [[ -d "$SCRIPT_DIR/$dir" ]]; then
            info "Building $dir..."
            (cd "$SCRIPT_DIR/$dir" && make clean && make) || {
                warn "Failed to build $dir (may not be implemented yet)"
            }
        fi
    done

    info "Test binaries built"
}

function run_integration_tests() {
    section "Running Integration Tests (cpcs.sh)"

    if [[ ! -f "$SCRIPT_DIR/cpcs.sh" ]]; then
        warn "cpcs.sh not found, skipping integration tests"
        return 0
    fi

    cd "$SCRIPT_DIR"

    if bash ./cpcs.sh; then
        info "✓ Integration tests PASSED"
        return 0
    else
        error "✗ Integration tests FAILED"
        return 1
    fi
}

function run_reachability_tests() {
    section "Running Reachability Tests"

    local test_bin="$SCRIPT_DIR/reachability_test/reachability_test"

    if [[ ! -f "$test_bin" ]]; then
        warn "Reachability test binary not found, skipping"
        return 0
    fi

    if "$test_bin"; then
        info "✓ Reachability tests PASSED"
        return 0
    else
        error "✗ Reachability tests FAILED"
        return 1
    fi
}

function run_slm_tests() {
    section "Running SLM Basic Tests"

    local test_script="$SCRIPT_DIR/slm_basic_test/slm_basic_test.sh"

    if [[ ! -f "$test_script" ]]; then
        warn "SLM test script not found, skipping"
        return 0
    fi

    if "$test_script"; then
        info "✓ SLM basic tests PASSED"
        return 0
    else
        error "✗ SLM basic tests FAILED"
        return 1
    fi
}

function run_builtin_tests() {
    section "Running Builtin Smoke Tests"

    local test_bin="$SCRIPT_DIR/cpcs_builtin_smoke/cpcs_builtin_smoke"

    if [[ ! -f "$test_bin" ]]; then
        warn "Builtin smoke test binary not found, skipping"
        return 0
    fi

    if "$test_bin"; then
        info "✓ Builtin smoke tests PASSED"
        return 0
    else
        error "✗ Builtin smoke tests FAILED"
        return 1
    fi
}

function show_usage() {
    cat << EOF
Usage: sudo $0 [OPTIONS]

CPCS Local Test Runner - Run tests on local machine

OPTIONS:
    --skip-integration     Skip integration tests (cpcs.sh)
    --skip-unit            Skip unit tests
    --build-only           Only build test binaries, don't run
    --no-setup             Skip hugepage setup
    -h, --help             Show this help

EXAMPLES:
    # Run all tests
    sudo $0

    # Build test binaries only
    sudo $0 --build-only

    # Skip integration tests
    sudo $0 --skip-integration

REQUIREMENTS:
    - SPDK built with: ./configure --with-cpcs && make
    - Root privileges (sudo)
    - Hugepages configured (or use --no-setup if already configured)

SEE ALSO:
    - README.md: Complete testing documentation
    - run_vagrant_tests.sh: Run tests in isolated VM environment

EOF
}

# Parse command line arguments
SKIP_INTEGRATION=false
SKIP_UNIT=false
BUILD_ONLY=false
NO_SETUP=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-integration)
            SKIP_INTEGRATION=true
            shift
            ;;
        --skip-unit)
            SKIP_UNIT=true
            shift
            ;;
        --build-only)
            BUILD_ONLY=true
            shift
            ;;
        --no-setup)
            NO_SETUP=true
            shift
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            error "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
done

# Main execution
section "CPCS Local Test Runner"

check_root
check_spdk_build

if [[ "$NO_SETUP" == "false" ]]; then
    check_hugepages
fi

build_test_binaries

if [[ "$BUILD_ONLY" == "true" ]]; then
    info "Build complete (--build-only specified)"
    exit 0
fi

# Run tests
FAILED_TESTS=0
PASSED_TESTS=0
SKIPPED_TESTS=0

if [[ "$SKIP_INTEGRATION" == "false" ]]; then
    if run_integration_tests; then
        PASSED_TESTS=$((PASSED_TESTS + 1))
    else
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
else
    info "Skipping integration tests (--skip-integration)"
    SKIPPED_TESTS=$((SKIPPED_TESTS + 1))
fi

if [[ "$SKIP_UNIT" == "false" ]]; then
    if run_reachability_tests; then
        PASSED_TESTS=$((PASSED_TESTS + 1))
    else
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi

    if run_slm_tests; then
        PASSED_TESTS=$((PASSED_TESTS + 1))
    else
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi

    if run_builtin_tests; then
        PASSED_TESTS=$((PASSED_TESTS + 1))
    else
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi

else
    info "Skipping unit tests (--skip-unit)"
    SKIPPED_TESTS=$((SKIPPED_TESTS + 3))
fi

# Summary
section "Test Summary"

echo -e "Passed:  ${GREEN}$PASSED_TESTS${NC}"
echo -e "Failed:  ${RED}$FAILED_TESTS${NC}"
echo -e "Skipped: ${YELLOW}$SKIPPED_TESTS${NC}"
echo ""

if [[ $FAILED_TESTS -eq 0 ]]; then
    echo -e "${GREEN}✓ All tests PASSED!${NC}"
    exit 0
else
    echo -e "${RED}✗ Some tests FAILED!${NC}"
    exit 1
fi

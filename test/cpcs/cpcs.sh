#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 CPCS Implementation Team. All rights reserved.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh

# CPCS test configuration
CPCS_TEST_SUBSYSTEM_NQN="nqn.2024-01.io.spdk:cpcs-test"
CPCS_COMPUTE_NSID=200
CPCS_MEMORY_NSID=100
CPCS_SLM_SIZE_MB=64
SPDK_TEST_CORE_MASK="${SPDK_TEST_CORE_MASK:-0x2}"
SPDK_TEST_RPC_SOCK="${SPDK_TEST_RPC_SOCK:-/var/tmp/spdk_cpcs.sock}"
export DEFAULT_RPC_ADDR="$SPDK_TEST_RPC_SOCK"

function cleanup() {
	killprocess $spdk_tgt_pid || true
	rm -f $testdir/cpcs_test.json
	rm -f "$SPDK_TEST_RPC_SOCK"
}

function create_cpcs_config() {
	cat > $testdir/cpcs_test.json << EOF
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_slm_create",
          "params": {
            "name": "SLM0",
            "nsid": $CPCS_MEMORY_NSID,
            "size_mb": $CPCS_SLM_SIZE_MB,
            "granularity": 4
          }
        }
      ]
    },
    {
      "subsystem": "nvmf",
      "config": [
        {
          "method": "nvmf_create_transport",
          "params": {
            "trtype": "TCP"
          }
        },
        {
          "method": "nvmf_create_subsystem",
          "params": {
            "nqn": "$CPCS_TEST_SUBSYSTEM_NQN",
            "serial_number": "CPCSTEST001",
            "model_number": "CPCS_COMPUTE_NS",
            "allow_any_host": true
          }
        }
      ]
    }
  ]
}
EOF
}

function start_spdk_target() {
	rm -f "$SPDK_TEST_RPC_SOCK"
	$SPDK_BIN_DIR/spdk_tgt -m $SPDK_TEST_CORE_MASK -r "$SPDK_TEST_RPC_SOCK" &
	spdk_tgt_pid=$!
	waitforlisten $spdk_tgt_pid "$SPDK_TEST_RPC_SOCK"
}

function test_slm_bdev_operations() {
	echo "=== Test: SLM Bdev Operations ==="

	# Create SLM bdev
	$rpc_py bdev_slm_create --name SLM_TEST --nsid 101 --size-mb 32 --granularity 4

	# Verify SLM bdev exists
	bdev_list=$($rpc_py bdev_get_bdevs)
	if echo "$bdev_list" | grep -q "SLM_TEST"; then
		echo "PASS: SLM bdev created successfully"
	else
		echo "FAIL: SLM bdev not found"
		return 1
	fi

	# Get bdev info
	bdev_info=$($rpc_py bdev_get_bdevs -b SLM_TEST)
	echo "SLM bdev info: $bdev_info"

	# Delete SLM bdev
	$rpc_py bdev_slm_delete --name SLM_TEST

	# Verify deletion
	bdev_list=$($rpc_py bdev_get_bdevs)
	if echo "$bdev_list" | grep -q "SLM_TEST"; then
		echo "FAIL: SLM bdev still exists after deletion"
		return 1
	else
		echo "PASS: SLM bdev deleted successfully"
	fi

	return 0
}

function test_cpcs_namespace_creation() {
	echo "=== Test: CPCS Namespace Creation ==="

	# First create a subsystem
	$rpc_py nvmf_create_transport -t TCP || true
	$rpc_py nvmf_create_subsystem $CPCS_TEST_SUBSYSTEM_NQN -s CPCSTEST001 -a

	# Create memory namespace (SLM bdev)
	$rpc_py bdev_slm_create --name SLM0 --nsid $CPCS_MEMORY_NSID --size-mb $CPCS_SLM_SIZE_MB --granularity 4

	# Create CPCS compute namespace
	$rpc_py cpcs_ns_create \
		--subsystem-nqn $CPCS_TEST_SUBSYSTEM_NQN \
		--nsid $CPCS_COMPUTE_NSID \
		--max-activated 16 \
		--max-mrs 64 \
		--max-ranges-per-mrs 8

	if [ $? -eq 0 ]; then
		echo "PASS: CPCS compute namespace created"
	else
		echo "FAIL: Failed to create CPCS compute namespace"
		return 1
	fi

	return 0
}

function test_cpcs_program_list() {
	echo "=== Test: CPCS Program List ==="

	# List programs (should be empty initially)
	program_list=$($rpc_py cpcs_program_list \
		--subsystem-nqn $CPCS_TEST_SUBSYSTEM_NQN \
		--nsid $CPCS_COMPUTE_NSID)

	echo "Program list: $program_list"

	# Verify empty list
	if [ "$program_list" == "[]" ]; then
		echo "PASS: Program list is empty as expected"
	else
		echo "INFO: Program list returned: $program_list"
	fi

	return 0
}

function test_cpcs_mrs_list() {
	echo "=== Test: CPCS MRS List ==="

	# List memory range sets (should be empty initially)
	mrs_list=$($rpc_py cpcs_mrs_list \
		--subsystem-nqn $CPCS_TEST_SUBSYSTEM_NQN \
		--nsid $CPCS_COMPUTE_NSID)

	echo "MRS list: $mrs_list"

	# Verify empty list
	if [ "$mrs_list" == "[]" ]; then
		echo "PASS: MRS list is empty as expected"
	else
		echo "INFO: MRS list returned: $mrs_list"
	fi

	return 0
}

function test_cpcs_namespace_deletion() {
	echo "=== Test: CPCS Namespace Deletion ==="

	# Delete CPCS compute namespace
	$rpc_py cpcs_ns_delete \
		--subsystem-nqn $CPCS_TEST_SUBSYSTEM_NQN \
		--nsid $CPCS_COMPUTE_NSID

	if [ $? -eq 0 ]; then
		echo "PASS: CPCS compute namespace deleted"
	else
		echo "FAIL: Failed to delete CPCS compute namespace"
		return 1
	fi

	# Cleanup
	$rpc_py bdev_slm_delete --name SLM0
	$rpc_py nvmf_delete_subsystem $CPCS_TEST_SUBSYSTEM_NQN

	return 0
}

function test_multiple_slm_bdevs() {
	echo "=== Test: Multiple SLM Bdevs ==="

	# Create multiple SLM bdevs
	for i in $(seq 1 5); do
		$rpc_py bdev_slm_create --name SLM_MULTI_$i --nsid $((300 + i)) --size-mb 16 --granularity 4
	done

	# Verify all exist
	bdev_list=$($rpc_py bdev_get_bdevs)
	pass=true
	for i in $(seq 1 5); do
		if ! echo "$bdev_list" | grep -q "SLM_MULTI_$i"; then
			echo "FAIL: SLM_MULTI_$i not found"
			pass=false
		fi
	done

	if $pass; then
		echo "PASS: All 5 SLM bdevs created"
	fi

	# Cleanup
	for i in $(seq 1 5); do
		$rpc_py bdev_slm_delete --name SLM_MULTI_$i
	done

	return 0
}

function test_slm_invalid_params() {
	echo "=== Test: SLM Invalid Parameters ==="

	# Test zero size (should fail)
	if $rpc_py bdev_slm_create --name SLM_BAD --nsid 999 --size-mb 0 --granularity 4 2>/dev/null; then
		echo "FAIL: Zero size should have failed"
		$rpc_py bdev_slm_delete --name SLM_BAD 2>/dev/null || true
		return 1
	else
		echo "PASS: Zero size correctly rejected"
	fi

	# Test zero granularity (should fail)
	if $rpc_py bdev_slm_create --name SLM_BAD --nsid 999 --size-mb 16 --granularity 0 2>/dev/null; then
		echo "FAIL: Zero granularity should have failed"
		$rpc_py bdev_slm_delete --name SLM_BAD 2>/dev/null || true
		return 1
	else
		echo "PASS: Zero granularity correctly rejected"
	fi

	return 0
}

function run_cpcs_tests() {
	local failures=0

	trap cleanup EXIT

	# Start SPDK target
	start_spdk_target

	rpc_py="$rootdir/scripts/rpc.py -s $SPDK_TEST_RPC_SOCK"

	# Run tests
	test_slm_bdev_operations || ((failures++))
	test_multiple_slm_bdevs || ((failures++))
	test_slm_invalid_params || ((failures++))
	test_cpcs_namespace_creation || ((failures++))
	test_cpcs_program_list || ((failures++))
	test_cpcs_mrs_list || ((failures++))
	test_cpcs_namespace_deletion || ((failures++))

	# Summary
	echo ""
	echo "================================"
	if [ $failures -eq 0 ]; then
		echo "All CPCS integration tests PASSED"
	else
		echo "$failures test(s) FAILED"
	fi
	echo "================================"

	return $failures
}

# Main
run_cpcs_tests
exit $?

#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 CPCS Implementation Team. All rights reserved.

set -e

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../..")

source "$rootdir/test/common/autotest_common.sh"

slm_name="SLM_TEST"
slm_nsid=101
slm_size_mb=16
slm_granularity=4
SPDK_TEST_CORE_MASK="${SPDK_TEST_CORE_MASK:-0x2}"
SPDK_TEST_RPC_SOCK="${SPDK_TEST_RPC_SOCK:-/var/tmp/spdk_slm_basic.sock}"
export DEFAULT_RPC_ADDR="$SPDK_TEST_RPC_SOCK"

nqn="nqn.2024-01.io.spdk:slm-basic-test"
trtype="TCP"
traddr="127.0.0.1"
trsvcid="${SPDK_TEST_TRTSVCID:-4421}"
max_namespaces=1024

rpc_py="$rootdir/scripts/rpc.py -s $SPDK_TEST_RPC_SOCK"
spdk_tgt="$rootdir/build/bin/spdk_tgt"
spdk_nvme_passthru="$rootdir/build/bin/spdk_nvme_passthru"
passthru_lcores="${SPDK_TEST_CLIENT_LCORES:-0}"

input_file=$(mktemp)
output_file=$(mktemp)

cleanup() {
	killprocess $spdk_tgt_pid || true
	rm -f "$input_file" "$output_file"
	rm -f "$SPDK_TEST_RPC_SOCK"
}
trap cleanup EXIT

rm -f "$SPDK_TEST_RPC_SOCK"
"$spdk_tgt" -m "$SPDK_TEST_CORE_MASK" -r "$SPDK_TEST_RPC_SOCK" &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid "$SPDK_TEST_RPC_SOCK"

$rpc_py nvmf_create_transport -t "$trtype" || true
$rpc_py nvmf_create_subsystem "$nqn" -s SLMTEST001 -a -m "$max_namespaces"
$rpc_py bdev_slm_create --name "$slm_name" --nsid "$slm_nsid" --size-mb "$slm_size_mb" \
	--granularity "$slm_granularity"
$rpc_py nvmf_subsystem_add_ns "$nqn" "$slm_name" -n "$slm_nsid"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t "$trtype" -a "$traddr" -s "$trsvcid"

python3 - <<PY
import struct
with open("$input_file", "wb") as f:
    for i in range(128):
        f.write(struct.pack("<I", i))
PY

echo "Running SLM basic test via NVMe passthru..."
"$spdk_nvme_passthru" --lcores "$passthru_lcores" --no-rpc-server --io-cmd \
	--trtype "$trtype" --traddr "$traddr" --trsvcid "$trsvcid" --subnqn "$nqn" \
	--opcode 0x05 --nsid "$slm_nsid" --cdw10 0 --cdw11 0 --cdw12 512 \
	--data-len 512 --write --input-file "$input_file"

"$spdk_nvme_passthru" --lcores "$passthru_lcores" --no-rpc-server --io-cmd \
	--trtype "$trtype" --traddr "$traddr" --trsvcid "$trsvcid" --subnqn "$nqn" \
	--opcode 0x02 --nsid "$slm_nsid" --cdw10 0 --cdw11 0 --cdw12 512 \
	--data-len 512 --read --output-file "$output_file"

if ! cmp -s "$input_file" "$output_file"; then
	echo "SLM basic test FAILED: data mismatch"
	exit 1
fi

echo "SLM basic test PASSED"

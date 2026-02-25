#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.
#

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py="$rootdir/scripts/rpc.py"
wait_for_rpc="--wait-for-rpc"

function cleanup() {
	if [[ -n "${spdk_tgt_pid:-}" ]]; then
		killprocess "$spdk_tgt_pid"
	fi
}
trap cleanup EXIT

"$SPDK_BIN_DIR/spdk_tgt" "$env_ctx" "$wait_for_rpc" &
spdk_tgt_pid=$!
waitforlisten "$spdk_tgt_pid"

"$rpc_py" framework_start_init
"$rpc_py" bdev_malloc_create -b Malloc0 64 512
"$rpc_py" bdev_vslm_create --name vslm0 --base-bdev-name Malloc0 --sram-size-mb 8 \
	--nsid 1 --semantics-mode lease --writeback-policy hybrid --admission-disabled

"$rpc_py" bdev_vslm_get_policy --name vslm0 | python3 -c '
import json, sys
policy = json.load(sys.stdin)
if policy.get("name") != "vslm0":
    raise SystemExit(1)
if policy.get("semantics_mode") != "lease":
    raise SystemExit(1)
if policy.get("writeback_policy") != "hybrid":
    raise SystemExit(1)
if policy.get("admission_enabled") not in (False, 0):
    raise SystemExit(1)
'

"$rpc_py" bdev_vslm_set_policy --name vslm0 --semantics-mode lease \
	--writeback-policy at_boundary --admission-enabled \
	--admission-faults-per-sec-threshold 10

"$rpc_py" bdev_vslm_get_policy --name vslm0 | python3 -c '
import json, sys
policy = json.load(sys.stdin)
if policy.get("name") != "vslm0":
    raise SystemExit(1)
if policy.get("semantics_mode") != "lease":
    raise SystemExit(1)
if policy.get("writeback_policy") != "at_boundary":
    raise SystemExit(1)
if policy.get("admission_enabled") not in (True, 1):
    raise SystemExit(1)
if policy.get("admission_faults_per_sec_threshold") != 10:
    raise SystemExit(1)
'

"$rpc_py" bdev_vslm_lease_acquire --lease-id 1 --nsid 1 --offset 0 --length 4096
NOT "$rpc_py" bdev_vslm_lease_acquire --lease-id 2 --nsid 1 --offset 0 --length 4096
"$rpc_py" bdev_vslm_lease_release --lease-id 1
"$rpc_py" bdev_vslm_lease_acquire --lease-id 2 --nsid 1 --offset 0 --length 4096
"$rpc_py" bdev_vslm_lease_release --lease-id 2

"$rpc_py" bdev_get_bdevs -b vslm0 | python3 -c '
import json, sys
data = json.load(sys.stdin)
if not data or data[0].get("name") != "vslm0":
    raise SystemExit(1)
'

"$rpc_py" bdev_vslm_get_stats --name vslm0 | python3 -c '
import json, sys
stats = json.load(sys.stdin)
required = {
    "name",
    "backing_write_ops",
    "backing_write_bytes",
    "backing_write_tagged_bytes",
    "backing_write_untagged_bytes",
    "page_faults",
    "page_fault_bytes",
    "page_evictions",
    "page_eviction_bytes",
    "page_writebacks",
    "page_writeback_bytes",
    "dirty_resident_pages",
    "dirty_writeback_bytes",
    "lease_conflicts",
    "lease_blocked_ns",
    "admission_rejects",
}
if not isinstance(stats, dict):
    raise SystemExit(1)
if required - set(stats.keys()):
    raise SystemExit(1)
if stats.get("name") != "vslm0":
    raise SystemExit(1)
'

"$rpc_py" bdev_vslm_reset_stats --name vslm0

"$rpc_py" bdev_vslm_set_fdp_mode --name vslm0 --disable --dspec 7
NOT "$rpc_py" bdev_vslm_set_fdp_mode --name vslm0 --enable --dspec 7

"$rpc_py" bdev_vslm_delete --name vslm0
"$rpc_py" bdev_malloc_delete Malloc0

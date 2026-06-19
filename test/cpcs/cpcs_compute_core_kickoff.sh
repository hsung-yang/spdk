#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SPDK_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)

function usage() {
	cat << EOF
Usage: $0 [check|prompt|validate|files]

Guide another agent through the CPCS compute-core-mask change.

Commands:
  check      Print the handoff summary and verify implementation markers.
  prompt     Print a concise prompt for another coding agent.
  validate   Run lightweight local validation after the change is applied.
  files      Print the files expected to change.
EOF
}

function files() {
	cat << EOF
app/spdk_tgt/Makefile
app/spdk_tgt/spdk_tgt.c
lib/nvmf/cpcs/builtin_runtime.c
lib/nvmf/cpcs/builtin_runtime.h
module/event/subsystems/nvmf/Makefile
module/event/subsystems/nvmf/nvmf_tgt.c
test/cpcs/0_test_platform.md
test/cpcs/README.md
test/cpcs/CPCS_COMPUTE_CORE_GUIDE.md
test/cpcs/CPCS_COMPUTE_CORE_MERGE_GUIDE.md
test/cpcs/cpcs_compute_core_kickoff.sh
test/cpcs/vslm_pslm_perf_compare.py
test/cpcs/cpcs_data_movement_compare.py
test/cpcs/cpcs_slm_capacity_gap_compare.py
test/cpcs/cpcs_transport_bottleneck_compare.py
test/cpcs/cpcs_vector_eval_compare.py
test/cpcs/experiment_platform.py
test/cpcs/examples/inventory_loopback.yaml
test/cpcs/examples/inventory_loopback_minimal.yaml
test/cpcs/examples/inventory_loopback_real_nvme_11_00_0.yaml
test/cpcs/examples/inventory_split.yaml
test/cpcs/examples/inventory_split_minimal.yaml
test/cpcs/examples/inventory_split_vagrant.yaml
EOF
}

function prompt() {
	cat << EOF
You are applying the CPCS compute-core-mask change to this SPDK fork.

Goal:
- Add spdk_tgt option: --cpcs-compute-core-mask <mask>
- Require the compute mask to be non-empty and a subset of the app core mask (-m).
- Start one SPDK lightweight compute thread per compute core.
- Route long-running builtin CPCS work to those compute threads.
- Preserve completion affinity by sending completion back to the submit SPDK thread.
- Exclude compute cores from NVMf poll-group creation.
- Do not use pthread_create or pthread_detach in lib/nvmf/cpcs/builtin_runtime.c.

Read first:
- test/cpcs/CPCS_COMPUTE_CORE_MERGE_GUIDE.md
- test/cpcs/CPCS_COMPUTE_CORE_GUIDE.md

Apply order:
1. Runtime API: lib/nvmf/cpcs/builtin_runtime.h
2. Runtime implementation: lib/nvmf/cpcs/builtin_runtime.c
3. spdk_tgt CLI and lifecycle: app/spdk_tgt/spdk_tgt.c
4. NVMf poll-group exclusion: module/event/subsystems/nvmf/nvmf_tgt.c
5. Makefile include paths for spdk_tgt and event_nvmf
6. Scenario script forwarding and inventory key target_compute_core_mask
7. Docs and example inventories

After applying, run:
./test/cpcs/cpcs_compute_core_kickoff.sh check
./test/cpcs/cpcs_compute_core_kickoff.sh validate
EOF
}

function handoff_summary() {
	cat << EOF
CPCS compute-core-mask handoff

Operator CLI:
  build/bin/spdk_tgt -m 0xF --cpcs-compute-core-mask 0xC --wait-for-rpc

Inventory:
  runtime:
    target_core_mask: "0xF"
    target_compute_core_mask: "0xC"

Core behavior:
  - compute mask is a subset of the app core mask
  - NVMf poll groups skip compute cores
  - builtin long-running work runs on SPDK compute threads
  - completion is sent back to the submit SPDK thread
  - builtin_runtime.c does not create pthread workers

Changed-file checklist:
EOF
	files | sed 's/^/  - /'
	echo
}

function marker() {
	local file=$1
	local pattern=$2
	local label=$3

	if [[ ! -f "$SPDK_ROOT/$file" ]]; then
		printf 'MISSING file: %s (%s)\n' "$file" "$label"
		return 1
	fi

	if grep -q -- "$pattern" "$SPDK_ROOT/$file"; then
		printf 'OK      %s\n' "$label"
		return 0
	fi

	printf 'MISSING %s in %s\n' "$label" "$file"
	return 1
}

function check_markers() {
	local rc=0

	echo "Marker checks:"
	marker "app/spdk_tgt/spdk_tgt.c" "--cpcs-compute-core-mask" "spdk_tgt CLI option" || rc=1
	marker "app/spdk_tgt/spdk_tgt.c" "spdk_tgt_validate_cpcs_compute_cores" "compute mask subset validation" || rc=1
	marker "lib/nvmf/cpcs/builtin_runtime.h" "cpcs_builtin_runtime_start_compute_threads" "runtime compute-thread API" || rc=1
	marker "lib/nvmf/cpcs/builtin_runtime.c" "cpcs_builtin_runtime_start_compute_threads" "runtime compute-thread implementation" || rc=1
	marker "lib/nvmf/cpcs/builtin_runtime.c" "_cpcs_builtin_extended_msg" "SPDK message execution path" || rc=1
	marker "lib/nvmf/cpcs/builtin_runtime.c" "sizeof(struct cpcs_builtin_kv_req_header) == 44" "KV header size assert" || rc=1
	marker "module/event/subsystems/nvmf/nvmf_tgt.c" "cpcs_builtin_runtime_is_compute_core" "NVMf compute-core exclusion" || rc=1
	marker "test/cpcs/vslm_pslm_perf_compare.py" "--cpcs-compute-core-mask" "scenario spdk_tgt forwarding" || rc=1
	marker "test/cpcs/experiment_platform.py" "target_compute_core_mask" "inventory runtime key" || rc=1

	if grep -Eq "pthread_create|pthread_detach" "$SPDK_ROOT/lib/nvmf/cpcs/builtin_runtime.c"; then
		printf 'MISSING pthread worker removal in lib/nvmf/cpcs/builtin_runtime.c\n'
		rc=1
	else
		printf 'OK      no pthread_create/pthread_detach in builtin_runtime.c\n'
	fi

	return "$rc"
}

function validate() {
	local cc_bin=${CC:-cc}

	echo "Running lightweight validation from $SPDK_ROOT"
	cd "$SPDK_ROOT"

	python3 -m py_compile \
		test/cpcs/vslm_pslm_perf_compare.py \
		test/cpcs/cpcs_data_movement_compare.py \
		test/cpcs/cpcs_slm_capacity_gap_compare.py \
		test/cpcs/cpcs_transport_bottleneck_compare.py \
		test/cpcs/cpcs_vector_eval_compare.py \
		test/cpcs/experiment_platform.py

	"$cc_bin" -fsyntax-only -Iinclude -Ilib -Ilib/nvmf -Ilib/nvmf/cpcs \
		-Ibuild/include -include include/spdk/config.h \
		app/spdk_tgt/spdk_tgt.c
	"$cc_bin" -fsyntax-only -Iinclude -Ilib -Ilib/nvmf -Ilib/nvmf/cpcs \
		-Ibuild/include -include include/spdk/config.h \
		module/event/subsystems/nvmf/nvmf_tgt.c
	"$cc_bin" -fsyntax-only -Iinclude -Ilib -Ilib/nvmf -Ilib/nvmf/cpcs \
		-Ibuild/include -include include/spdk/config.h \
		lib/nvmf/cpcs/builtin_runtime.c

	echo "Lightweight validation completed."
	echo "Next build checks:"
	echo "  make -C module/event/subsystems/nvmf DEFAULT_CC=\"\${CC:-cc}\" V=1"
	echo "  make -C app/spdk_tgt DEFAULT_CC=\"\${CC:-cc}\" V=1"
}

command=${1:-check}

case "$command" in
	check)
		handoff_summary
		check_markers
		;;
	prompt)
		prompt
		;;
	validate)
		validate
		;;
	files)
		files
		;;
	-h|--help|help)
		usage
		;;
	*)
		usage
		exit 1
		;;
esac

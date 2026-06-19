#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SPDK_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)

RPC="${RPC:-$SPDK_ROOT/scripts/rpc.py}"
SPDK_TGT="${SPDK_TGT:-$SPDK_ROOT/build/bin/spdk_tgt}"
SPDK_NVME_PASSTHRU="${SPDK_NVME_PASSTHRU:-$SPDK_ROOT/build/bin/spdk_nvme_passthru}"
SPDK_NVME_PASSTHRU_LCORES="${SPDK_NVME_PASSTHRU_LCORES:-1}"
RPC_SOCK="${RPC_SOCK:-/var/tmp/spdk.sock}"
NVME="${NVME:-nvme}"

NQN="${NQN:-nqn.2024-01.io.spdk:cpcs-demo}"
SERIAL="${SERIAL:-CPCSDEMO001}"
TRTYPE="${TRTYPE:-TCP}"
TRADDR="${TRADDR:-127.0.0.1}"
TRSVCID="${TRSVCID:-4420}"
NVME_TRTYPE="${NVME_TRTYPE:-${TRTYPE,,}}"
MAX_NAMESPACES="${MAX_NAMESPACES:-1024}"

if [[ -t 1 && -z "${NO_COLOR:-}" && "${TERM:-dumb}" != "dumb" ]]; then
	COLOR_RESET=$'\033[0m'
	COLOR_TITLE=$'\033[1;36m'
	COLOR_DESC=$'\033[2m'
	COLOR_CMD=$'\033[33m'
	COLOR_LOG=$'\033[90m'
	COLOR_RESULT=$'\033[32m'
else
	COLOR_RESET=""
	COLOR_TITLE=""
	COLOR_DESC=""
	COLOR_CMD=""
	COLOR_LOG=""
	COLOR_RESULT=""
fi

SLM_BDEV="${SLM_BDEV:-SLM0}"
SLM_NSID="${SLM_NSID:-100}"
SLM_MB="${SLM_MB:-64}"
SLM_GRANULARITY="${SLM_GRANULARITY:-512}"
CTRL_DEV="${CTRL_DEV:-/dev/nvme0}"
SLM_DEV="${SLM_DEV:-/dev/nvme0n${SLM_NSID}}"

CPCS_NSID="${CPCS_NSID:-200}"
MAX_ACTIVATED="${MAX_ACTIVATED:-16}"
MAX_MRS="${MAX_MRS:-64}"
MAX_RANGES_PER_MRS="${MAX_RANGES_PER_MRS:-8}"
LOAD_PROGRAM_GRAN="${LOAD_PROGRAM_GRAN:-3}"
EBPF_PIND="${EBPF_PIND:-10}"
EBPF_PTYPE="${EBPF_PTYPE:-0xC0}"
EBPF_PIT="${EBPF_PIT:-0x01}"
EBPF_PUID="${EBPF_PUID:-0xEBF00001}"
EBPF_SRC="$SPDK_ROOT/test/cpcs/ebpf/ebpf_mul64.c"

STATE_DIR="${STATE_DIR:-/tmp/cpcs_demo}"
PID_FILE="$STATE_DIR/spdk_tgt.pid"
LOG_FILE="$STATE_DIR/spdk_tgt.log"
DATA_DIR="$STATE_DIR/data"
INPUT_BIN="$DATA_DIR/input.bin"
MRS_BIN="$DATA_DIR/mrs.bin"
SUM64_BIN="$DATA_DIR/sum64.bin"
MAX64_BIN="$DATA_DIR/max64.bin"
MIN64_BIN="$DATA_DIR/min64.bin"
EBPF_OBJ="$DATA_DIR/ebpf_mul64.o"
EBPF_BIN="$DATA_DIR/ebpf_mul64.bin"
EBPF_OUT="$DATA_DIR/ebpf_out.bin"

AUTO=0
if [[ "${1:-}" == "--auto" ]]; then
	AUTO=1
	shift
fi

if [[ "${1:-}" == "--help" ]]; then
	cat <<EOF
Usage: $0 [--auto]

Run a step-by-step CPCS PoC demo inside the VM.
Set AUTO=1 or pass --auto to skip prompts.
EOF
	exit 0
fi

if [[ $EUID -ne 0 ]]; then
	echo "Please run as root (sudo) inside the VM." >&2
	exit 1
fi

if [[ ! -x "$SPDK_TGT" ]]; then
	echo "spdk_tgt not found at $SPDK_TGT. Build SPDK first." >&2
	exit 1
fi
if [[ ! -x "$SPDK_NVME_PASSTHRU" ]]; then
	echo "spdk_nvme_passthru not found at $SPDK_NVME_PASSTHRU. Build SPDK first." >&2
	exit 1
fi

if ! command -v "$NVME" >/dev/null 2>&1; then
	echo "nvme CLI not found. Install nvme-cli or set NVME=/path/to/nvme." >&2
	exit 1
fi

if [[ -x "$RPC" ]]; then
	RPC_CMD=("$RPC")
else
	RPC_CMD=("python3" "$RPC")
fi

step_no=0
step() {
	step_no=$((step_no + 1))
	echo ""
	printf "%s== Step %s: %s ==%s\n" "$COLOR_TITLE" "$step_no" "$1" "$COLOR_RESET"
	if [[ -n "${2:-}" ]]; then
		printf "%s%s%s\n" "$COLOR_DESC" "$2" "$COLOR_RESET"
	fi
}

pause() {
	if [[ $AUTO -eq 1 ]]; then
		return 0
	fi
	read -r -p "Press Enter to continue..."
}

run() {
	printf "%s+" "$COLOR_CMD"
	for arg in "$@"; do
		printf " %q" "$arg"
	done
	printf "%s\n" "$COLOR_RESET"
	"$@"
}

run_capture() {
	printf "%s+" "$COLOR_CMD" >&2
	for arg in "$@"; do
		printf " %q" "$arg" >&2
	done
	printf "%s\n" "$COLOR_RESET" >&2
	"$@"
}

rpc() {
	run "${RPC_CMD[@]}" -s "$RPC_SOCK" "$@"
}

rpc_quiet() {
	"${RPC_CMD[@]}" -s "$RPC_SOCK" "$@" >/dev/null 2>&1 || true
}

find_tool() {
	local name
	for name in "$@"; do
		if command -v "$name" >/dev/null 2>&1; then
			command -v "$name"
			return 0
		fi
	done
	return 1
}

build_ebpf_program() {
	local clang objcopy

	if [[ ! -f "$EBPF_SRC" ]]; then
		echo "eBPF source not found at $EBPF_SRC" >&2
		exit 1
	fi

	mkdir -p "$DATA_DIR"

	clang=$(find_tool clang clang-14 clang-15 clang-16) || true
	objcopy=$(find_tool llvm-objcopy llvm-objcopy-14 llvm-objcopy-15 objcopy) || true

	if [[ -z "$clang" || -z "$objcopy" ]]; then
		echo "Missing clang/llvm-objcopy. Install llvm + clang to build eBPF program." >&2
		exit 1
	fi

	run "$clang" -O2 -target bpf -c "$EBPF_SRC" -o "$EBPF_OBJ"
	run "$objcopy" -O binary --only-section=cpcs "$EBPF_OBJ" "$EBPF_BIN"
	if [[ ! -s "$EBPF_BIN" ]]; then
		run "$objcopy" -O binary --only-section=.text "$EBPF_OBJ" "$EBPF_BIN"
	fi
	if [[ ! -s "$EBPF_BIN" ]]; then
		echo "eBPF bytecode section is empty. Check compiler/section names." >&2
		exit 1
	fi
}

transport_exists() {
	local trtype="$1"

	"${RPC_CMD[@]}" -s "$RPC_SOCK" nvmf_get_transports 2>/dev/null | \
		python3 -c 'import json, sys
trtype = sys.argv[1].lower()
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(1)
for item in data:
    if str(item.get("trtype", "")).lower() == trtype:
        sys.exit(0)
sys.exit(1)
' "$trtype"
}

subsystem_exists() {
	local nqn="$1"

	"${RPC_CMD[@]}" -s "$RPC_SOCK" nvmf_get_subsystems "$nqn" 2>/dev/null | \
		python3 -c 'import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(1)
sys.exit(0 if data else 1)
'
}

bdev_exists() {
	local name="$1"

	"${RPC_CMD[@]}" -s "$RPC_SOCK" bdev_get_bdevs -b "$name" 2>/dev/null | \
		python3 -c 'import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(1)
sys.exit(0 if data else 1)
'
}

wait_for_rpc() {
	local i
	for i in $(seq 1 50); do
		if "${RPC_CMD[@]}" -s "$RPC_SOCK" bdev_get_bdevs >/dev/null 2>&1; then
			return 0
		fi
		sleep 0.2
	done
	echo "Timed out waiting for RPC socket: $RPC_SOCK" >&2
	exit 1
}

start_target() {
	mkdir -p "$STATE_DIR"
	if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
		echo "SPDK target already running (pid $(cat "$PID_FILE"))."
		return 0
	fi

	run "$SPDK_TGT" -m 0x1 >"$LOG_FILE" 2>&1 &
	echo $! >"$PID_FILE"
	wait_for_rpc
}

stop_target() {
	if [[ -f "$PID_FILE" ]]; then
		local pid
		pid=$(cat "$PID_FILE")
		if kill -0 "$pid" 2>/dev/null; then
			run kill "$pid"
			wait "$pid" || true
		fi
		rm -f "$PID_FILE"
	fi
}

cleanup_rpc() {
	local has_subsystem=0

	if subsystem_exists "$NQN"; then
		has_subsystem=1
	fi
	if [[ $has_subsystem -eq 1 ]]; then
		rpc_quiet cpcs_ns_delete --subsystem-nqn "$NQN" --nsid "$CPCS_NSID"
	fi
	if bdev_exists "$SLM_BDEV"; then
		rpc_quiet bdev_slm_delete --name "$SLM_BDEV"
	fi
	if [[ $has_subsystem -eq 1 ]]; then
		rpc_quiet nvmf_delete_subsystem "$NQN"
	fi
}

disconnect_nvme() {
	if "$NVME" list-subsys 2>/dev/null | grep -q "$NQN"; then
		run "$NVME" disconnect -n "$NQN" || true
	fi
}

step "Start SPDK target" \
	"Launch spdk_tgt and wait for the RPC socket to become available."
start_target
pause

step "Ensure clean demo state" \
	"Remove any leftover CPCS/SLM objects from previous demo runs."
cleanup_rpc
pause

step "Create NVMf transport + subsystem" \
	"Ensure a TCP transport exists and create the CPCS demo subsystem."
if transport_exists "$TRTYPE"; then
	echo "Transport $TRTYPE already exists; skipping create."
else
	rpc nvmf_create_transport -t "$TRTYPE"
fi
rpc nvmf_create_subsystem "$NQN" -s "$SERIAL" -m "$MAX_NAMESPACES" -a
pause

step "Create SLM bdev and add as memory namespace" \
	"Create a memory-backed bdev (SLM) and attach it as a namespace."
rpc bdev_slm_create --name "$SLM_BDEV" --nsid "$SLM_NSID" --size-mb "$SLM_MB" \
	--granularity "$SLM_GRANULARITY"
rpc nvmf_subsystem_add_ns "$NQN" "$SLM_BDEV" -n "$SLM_NSID"
rpc nvmf_subsystem_add_listener "$NQN" -t "$TRTYPE" -a "$TRADDR" -s "$TRSVCID"
rpc bdev_get_bdevs -b "$SLM_BDEV"
pause

step "Create CPCS compute namespace" \
	$'Create the compute namespace that will run CPCS programs.\nNote: LOAD_PROGRAM_GRAN=2^'"$LOAD_PROGRAM_GRAN"$' bytes to allow small eBPF binaries.'
rpc cpcs_ns_create --subsystem-nqn "$NQN" --nsid "$CPCS_NSID" \
	--max-activated "$MAX_ACTIVATED" --max-mrs "$MAX_MRS" \
	--max-ranges-per-mrs "$MAX_RANGES_PER_MRS" \
	--load-program-gran "$LOAD_PROGRAM_GRAN"
pause

step "Install builtins and list programs" \
	"Register built-in programs (e.g., sum64) and list them."
rpc cpcs_program_install_builtins --subsystem-nqn "$NQN" --nsid "$CPCS_NSID"
rpc cpcs_program_list --subsystem-nqn "$NQN" --nsid "$CPCS_NSID"
pause

step "Connect host and discover devices" \
	"Connect via nvme-cli and show controller/namespace metadata."
if [[ "$NVME_TRTYPE" == "tcp" ]]; then
	modprobe nvme-tcp >/dev/null 2>&1 || true
fi
run "$NVME" disconnect -n "$NQN" || true
run "$NVME" connect -t "$NVME_TRTYPE" -n "$NQN" -a "$TRADDR" -s "$TRSVCID"
run "$NVME" list
run "$NVME" list-subsys || true

CTRL_NAME=$("$NVME" list-subsys 2>/dev/null | awk -v nqn="$NQN" '
	$0 ~ "NQN="nqn {found=1; next}
	found && $1 == "+-" {print $2; exit}
')
if [[ -n "${CTRL_NAME:-}" ]]; then
	CTRL_DEV="/dev/$CTRL_NAME"
fi

if [[ -z "${SLM_DEV:-}" || ! -b "$SLM_DEV" ]]; then
	SLM_DEV_DETECT=$( NVME_JSON="$( "$NVME" list -o json 2>/dev/null || true )" python3 - <<PY
import json, os, sys
try:
    data = json.loads(os.environ.get("NVME_JSON") or "{}")
except Exception:
    sys.exit(0)
serial = "$SERIAL"
nsid = int("$SLM_NSID")
for dev in data.get("Devices", []):
    if dev.get("SerialNumber") == serial and dev.get("Namespace") == nsid:
        print(dev.get("DevicePath", ""))
        break
PY
)
	if [[ -n "${SLM_DEV_DETECT:-}" ]]; then
		SLM_DEV="$SLM_DEV_DETECT"
	fi
fi

if [[ -z "${SLM_DEV:-}" && -n "${CTRL_NAME:-}" ]]; then
	SLM_DEV="/dev/${CTRL_NAME}n${SLM_NSID}"
fi

if [[ ! -b "$SLM_DEV" && -n "${CTRL_NAME:-}" ]]; then
	SLM_DEV="/dev/${CTRL_NAME}n${SLM_NSID}"
fi

if [[ ! -b "$SLM_DEV" && -n "${CTRL_NAME:-}" ]]; then
	SLM_DEV_FALLBACK="/dev/${CTRL_NAME}n1"
	if [[ -b "$SLM_DEV_FALLBACK" ]]; then
		SLM_DEV="$SLM_DEV_FALLBACK"
	fi
fi

if [[ ! -e "$CTRL_DEV" && -b "$SLM_DEV" ]]; then
	CTRL_FROM_SLM="${SLM_DEV%n*}"
	if [[ -e "$CTRL_FROM_SLM" ]]; then
		CTRL_DEV="$CTRL_FROM_SLM"
	else
		CTRL_DEV="$SLM_DEV"
	fi
fi

if [[ ! -e "$CTRL_DEV" ]]; then
	for _ in $(seq 1 20); do
		if [[ -e "$CTRL_DEV" ]]; then
			break
		fi
		sleep 0.2
	done
fi

if [[ ! -e "$CTRL_DEV" ]]; then
	echo "CTRL_DEV not found at $CTRL_DEV. Set CTRL_DEV and re-run." >&2
	exit 1
fi
run "$NVME" id-ctrl "$CTRL_DEV"
run "$NVME" list-ns "$CTRL_DEV" || true
run "$NVME" id-ns "$CTRL_DEV" -n "$SLM_NSID" || true
run "$NVME" id-ns "$CTRL_DEV" -n "$CPCS_NSID" || true
pause

step "Write demo data into SLM namespace (SLM Memory Write opcode 0x05)" \
	$'Write 64 little-endian uint64 values [1..64] using byte addressing (dword aligned).\nFields:\n  opcode=0x05        SLM Memory Write\n  nsid=100           SLM namespace\n  cdw10=0            Starting Byte (SB) low\n  cdw11=0            Starting Byte (SB) high\n  cdw12=512          Write Length (bytes)\n  data-len=512       payload length\n  input-file=input.bin 64x uint64 LE values'
mkdir -p "$DATA_DIR"
python3 - <<PY
import struct
with open("$INPUT_BIN", "wb") as f:
    for v in range(1, 65):
        f.write(struct.pack("<Q", v))
PY
run "$SPDK_NVME_PASSTHRU" \
	--lcores "$SPDK_NVME_PASSTHRU_LCORES" \
	--no-rpc-server \
	--io-cmd --trtype "$TRTYPE" --traddr "$TRADDR" --trsvcid "$TRSVCID" --subnqn "$NQN" \
	--opcode 0x05 --nsid "$SLM_NSID" --cdw10 0 --cdw11 0 --cdw12 512 \
	--data-len 512 --write --input-file "$INPUT_BIN"
pause

step "Create Memory Range Set (admin opcode 0x89)" \
	$'Create a Memory Range Set that points to the SLM data.\nFields:\n  opcode=0x89        CPCS MRS create/manage\n  namespace-id=200   CPCS namespace (compute)\n  cdw10=0            MACT=0 (create)\n  cdw11=1            NUMR=1 range descriptor\n  data-len=32        one 32-byte range descriptor\n  input-file=mrs.bin layout: MNID(4) LEN(4) SB(8) ATTR(16)\n  result cdw0        RSID (range set ID) returned by controller'
python3 - <<PY
import struct
mnsid = int("$SLM_NSID")
length = 65536
start = 0
with open("$MRS_BIN", "wb") as f:
    f.write(struct.pack("<IIQ16s", mnsid, length, start, b"\\x00" * 16))
PY
MRS_OUT=$(run_capture "$NVME" admin-passthru "$CTRL_DEV" --opcode=0x89 \
	--namespace-id="$CPCS_NSID" --cdw10=0 --cdw11=1 \
	--data-len 32 --write --input-file "$MRS_BIN")
printf "%s" "$COLOR_LOG"
printf "%s" "$MRS_OUT"
printf "%s" "$COLOR_RESET"
RSID_HEX=$(echo "$MRS_OUT" | awk '/result/ {print $NF}' | sed 's/0x//')
RSID=$((16#${RSID_HEX:-1}))
printf "%sMRS RSID=%s%s\n" "$COLOR_RESULT" "$RSID" "$COLOR_RESET"
pause

step "Execute builtin sum64 (I/O opcode 0x01)" \
	$'Execute sum64 over the MRS; expected result is 2080.\nFields:\n  opcode=0x01        CPCS Execute\n  nsid=200           CPCS namespace (compute)\n  cdw2               (RSID << 16) | PIND, PIND=2 for sum64\n  cdw3=0             reserved\n  cdw4=24            execute descriptor length (bytes)\n  data-len=24        execute descriptor payload\n  input-file=sum64.bin layout: mr_id(8) off(8) len(8)'
python3 - <<PY
import struct
with open("$SUM64_BIN", "wb") as f:
    f.write(struct.pack("<QQQ", 1, 0, 512))
PY
CDW2=$(( (RSID << 16) | 2 ))
EXEC_OUT=$(run_capture "$SPDK_NVME_PASSTHRU" \
	--lcores "$SPDK_NVME_PASSTHRU_LCORES" \
	--no-rpc-server \
	--io-cmd --trtype "$TRTYPE" --traddr "$TRADDR" --trsvcid "$TRSVCID" --subnqn "$NQN" \
	--opcode 0x01 --nsid "$CPCS_NSID" --cdw2 "$CDW2" --cdw3 0 --cdw4 24 \
	--data-len 24 --write --input-file "$SUM64_BIN")
printf "%s" "$COLOR_LOG"
printf "%s" "$EXEC_OUT"
printf "%s" "$COLOR_RESET"
EXEC_HEX=$(echo "$EXEC_OUT" | sed -n 's/.*result=0x//p')
if [[ -n "$EXEC_HEX" ]]; then
	EXEC_VAL=$((16#$EXEC_HEX))
	printf "%sExecute return value: %s (expected 2080)%s\n" \
	"$COLOR_RESULT" "$EXEC_VAL" "$COLOR_RESET"
fi
pause

step "Execute builtin max64 (I/O opcode 0x01)" \
	$'Execute max64 over the MRS; expected result is 64.\nFields:\n  opcode=0x01        CPCS Execute\n  nsid=200           CPCS namespace (compute)\n  cdw2               (RSID << 16) | PIND, PIND=3 for max64\n  cdw3=0             reserved\n  cdw4=24            execute descriptor length (bytes)\n  data-len=24        execute descriptor payload\n  input-file=max64.bin layout: mr_id(8) off(8) len(8)'
python3 - <<PY
import struct
with open("$MAX64_BIN", "wb") as f:
    f.write(struct.pack("<QQQ", 1, 0, 512))
PY
CDW2=$(( (RSID << 16) | 3 ))
EXEC_OUT=$(run_capture "$SPDK_NVME_PASSTHRU" \
	--lcores "$SPDK_NVME_PASSTHRU_LCORES" \
	--no-rpc-server \
	--io-cmd --trtype "$TRTYPE" --traddr "$TRADDR" --trsvcid "$TRSVCID" --subnqn "$NQN" \
	--opcode 0x01 --nsid "$CPCS_NSID" --cdw2 "$CDW2" --cdw3 0 --cdw4 24 \
	--data-len 24 --write --input-file "$MAX64_BIN")
printf "%s" "$COLOR_LOG"
printf "%s" "$EXEC_OUT"
printf "%s" "$COLOR_RESET"
EXEC_HEX=$(echo "$EXEC_OUT" | sed -n 's/.*result=0x//p')
if [[ -n "$EXEC_HEX" ]]; then
	EXEC_VAL=$((16#$EXEC_HEX))
	printf "%sExecute return value: %s (expected 64)%s\n" \
		"$COLOR_RESULT" "$EXEC_VAL" "$COLOR_RESET"
fi
pause

step "Execute builtin min64 (I/O opcode 0x01)" \
	$'Execute min64 over the MRS; expected result is 1.\nFields:\n  opcode=0x01        CPCS Execute\n  nsid=200           CPCS namespace (compute)\n  cdw2               (RSID << 16) | PIND, PIND=4 for min64\n  cdw3=0             reserved\n  cdw4=24            execute descriptor length (bytes)\n  data-len=24        execute descriptor payload\n  input-file=min64.bin layout: mr_id(8) off(8) len(8)'
python3 - <<PY
import struct
with open("$MIN64_BIN", "wb") as f:
    f.write(struct.pack("<QQQ", 1, 0, 512))
PY
CDW2=$(( (RSID << 16) | 4 ))
EXEC_OUT=$(run_capture "$SPDK_NVME_PASSTHRU" \
	--lcores "$SPDK_NVME_PASSTHRU_LCORES" \
	--no-rpc-server \
	--io-cmd --trtype "$TRTYPE" --traddr "$TRADDR" --trsvcid "$TRSVCID" --subnqn "$NQN" \
	--opcode 0x01 --nsid "$CPCS_NSID" --cdw2 "$CDW2" --cdw3 0 --cdw4 24 \
	--data-len 24 --write --input-file "$MIN64_BIN")
printf "%s" "$COLOR_LOG"
printf "%s" "$EXEC_OUT"
printf "%s" "$COLOR_RESET"
EXEC_HEX=$(echo "$EXEC_OUT" | sed -n 's/.*result=0x//p')
if [[ -n "$EXEC_HEX" ]]; then
	EXEC_VAL=$((16#$EXEC_HEX))
	printf "%sExecute return value: %s (expected 1)%s\n" \
		"$COLOR_RESULT" "$EXEC_VAL" "$COLOR_RESET"
fi
pause

step "Build eBPF program (mul64)" \
	$'Compile eBPF bytecode for a user-defined program that multiplies two uint64 values.\nIt reads 16 bytes from MRS #1 offset 0, writes the product to offset 16, and returns it.'
build_ebpf_program
pause

step "Load eBPF program (admin opcode 0x85)" \
	$'Download the eBPF program into CPCS.\nFields:\n  opcode=0x85        CPCS Load Program\n  namespace-id=200   CPCS namespace (compute)\n  cdw10              PIND + PTYPE + SEL + PIT\n  cdw11              PSIZE (bytes)\n  cdw12/cdw13        PUID (program identifier)\n  cdw14              NUMB (bytes in this transfer)\n  cdw15              LOFF (load offset, 0 for first chunk)'
EBPF_SIZE=$(stat -c %s "$EBPF_BIN")
EBPF_PTYPE_VAL=$((EBPF_PTYPE))
EBPF_PIT_VAL=$((EBPF_PIT))
CDW10=$(( (EBPF_PIND & 0xFFFF) | ((EBPF_PTYPE_VAL & 0xFF) << 16) | ((0 & 0x1) << 24) | ((EBPF_PIT_VAL & 0x7) << 25) ))
PUID_LO=$((EBPF_PUID & 0xFFFFFFFF))
PUID_HI=$(( (EBPF_PUID >> 32) & 0xFFFFFFFF ))
run "$NVME" admin-passthru "$CTRL_DEV" --opcode=0x85 \
	--namespace-id="$CPCS_NSID" --cdw10="$CDW10" --cdw11="$EBPF_SIZE" \
	--cdw12="$PUID_LO" --cdw13="$PUID_HI" --cdw14="$EBPF_SIZE" --cdw15=0 \
	--data-len "$EBPF_SIZE" --write --input-file "$EBPF_BIN"
pause

step "Activate eBPF program (admin opcode 0x88)" \
	$'Activate the downloaded program so it can execute.\nFields:\n  opcode=0x88        CPCS Program Activation\n  namespace-id=200   CPCS namespace (compute)\n  cdw10              PIND='"$EBPF_PIND"$', SEL=1 (activate)'
CDW10=$(( (EBPF_PIND & 0xFFFF) | (1 << 16) ))
run "$NVME" admin-passthru "$CTRL_DEV" --opcode=0x88 --namespace-id="$CPCS_NSID" --cdw10="$CDW10"
pause

step "Execute eBPF mul64 (I/O opcode 0x01)" \
	$'Run the eBPF program over the MRS.\nIt multiplies the first two uint64 values in SLM and returns the product.\nFields:\n  opcode=0x01        CPCS Execute\n  nsid=200           CPCS namespace (compute)\n  cdw2               (RSID << 16) | PIND\n  cdw3=0             reserved\n  cdw4=0             no execute descriptor payload'
CDW2=$(( (RSID << 16) | EBPF_PIND ))
EXEC_OUT=$(run_capture "$SPDK_NVME_PASSTHRU" \
	--lcores "$SPDK_NVME_PASSTHRU_LCORES" \
	--no-rpc-server \
	--io-cmd --trtype "$TRTYPE" --traddr "$TRADDR" --trsvcid "$TRSVCID" --subnqn "$NQN" \
	--opcode 0x01 --nsid "$CPCS_NSID" --cdw2 "$CDW2" --cdw3 0 --cdw4 0)
printf "%s" "$COLOR_LOG"
printf "%s" "$EXEC_OUT"
printf "%s" "$COLOR_RESET"
EXEC_HEX=$(echo "$EXEC_OUT" | sed -n 's/.*result=0x//p')
if [[ -n "$EXEC_HEX" ]]; then
	EXEC_VAL=$((16#$EXEC_HEX))
	printf "%sExecute return value: %s (expected 2)%s\n" \
		"$COLOR_RESULT" "$EXEC_VAL" "$COLOR_RESET"
fi
pause

step "Validate eBPF output in SLM (SLM Memory Read opcode 0x02)" \
	$'Read back the product written by eBPF from SLM offset 16.\nFields:\n  opcode=0x02        SLM Memory Read\n  nsid=100           SLM namespace\n  cdw10=16           Starting Byte (SB) low\n  cdw11=0            Starting Byte (SB) high\n  cdw12=8            Read Length (bytes)\n  output-file=ebpf_out.bin'
run "$SPDK_NVME_PASSTHRU" \
	--lcores "$SPDK_NVME_PASSTHRU_LCORES" \
	--no-rpc-server \
	--io-cmd --trtype "$TRTYPE" --traddr "$TRADDR" --trsvcid "$TRSVCID" --subnqn "$NQN" \
	--opcode 0x02 --nsid "$SLM_NSID" --cdw10 16 --cdw11 0 --cdw12 8 \
	--data-len 8 --read --output-file "$EBPF_OUT"
EBPF_VAL=$(python3 - <<PY
import struct
with open("$EBPF_OUT", "rb") as f:
    val = struct.unpack("<Q", f.read(8))[0]
print(val)
PY
)
printf "%sSLM result at offset 16: %s (expected 2)%s\n" \
	"$COLOR_RESULT" "$EBPF_VAL" "$COLOR_RESET"
pause

step "Cleanup CPCS objects and stop target" \
	"Disconnect and remove CPCS/SLM objects, then stop the target."
disconnect_nvme || true
cleanup_rpc
stop_target
pause

echo ""
echo "Demo complete."

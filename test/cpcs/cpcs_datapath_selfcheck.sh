#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# CPCS/NVMe-oF data-path self-check helper.
# Goal: identify probable failure point without sharing full logs.

set -u -o pipefail

TARGET_SSH="root@10.1.10.37"            # control-path SSH endpoint
SSH_PORT=22
INIT_IF="enp9s0f1np1"                   # initiator data NIC
INIT_IP="10.10.10.72"                   # initiator data IP
TARGET_IF="enp3s0f0s0"                  # target data NIC
TARGET_IP="10.10.100.1"                 # target data IP used for NVMe/TCP
TRSVCID="4420"

NQN="nqn.2026-03.io.spdk:cpcs-exp"
HOSTNQN="nqn.2026-03.io.spdk:cpcs-exp-host"

SPDK_NVME_PASSTHRU="/home/kyuho/cpcs_paper/spdk/build/bin/spdk_nvme_passthru"
TARGET_RPC_PY="/home/ubuntu/cpcs_paper/spdk/scripts/rpc.py"
TARGET_RPC_SOCK="/var/tmp/vslm_pslm_bench.sock"

TIMEOUT_SSH=5
TIMEOUT_TCP=3

usage() {
	cat <<EOF
Usage: $0 [options]

Options:
  --target-ssh <user@host>         default: ${TARGET_SSH}
  --ssh-port <port>                default: ${SSH_PORT}
  --init-if <ifname>               default: ${INIT_IF}
  --init-ip <ip>                   default: ${INIT_IP}
  --target-if <ifname>             default: ${TARGET_IF}
  --target-ip <ip>                 default: ${TARGET_IP}
  --trsvcid <port>                 default: ${TRSVCID}
  --nqn <nqn>                      default: ${NQN}
  --hostnqn <hostnqn>              default: ${HOSTNQN}
  --spdk-nvme-passthru <path>      default: ${SPDK_NVME_PASSTHRU}
  --target-rpc-py <path>           default: ${TARGET_RPC_PY}
  --target-rpc-sock <path>         default: ${TARGET_RPC_SOCK}
  -h, --help

Example:
  $0 --target-ssh root@10.1.10.37 \\
     --init-if enp9s0f1np1 --init-ip 10.10.10.72 \\
     --target-if enp3s0f0s0 --target-ip 10.10.100.1
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--target-ssh) TARGET_SSH="$2"; shift 2 ;;
		--ssh-port) SSH_PORT="$2"; shift 2 ;;
		--init-if) INIT_IF="$2"; shift 2 ;;
		--init-ip) INIT_IP="$2"; shift 2 ;;
		--target-if) TARGET_IF="$2"; shift 2 ;;
		--target-ip) TARGET_IP="$2"; shift 2 ;;
		--trsvcid) TRSVCID="$2"; shift 2 ;;
		--nqn) NQN="$2"; shift 2 ;;
		--hostnqn) HOSTNQN="$2"; shift 2 ;;
		--spdk-nvme-passthru) SPDK_NVME_PASSTHRU="$2"; shift 2 ;;
		--target-rpc-py) TARGET_RPC_PY="$2"; shift 2 ;;
		--target-rpc-sock) TARGET_RPC_SOCK="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown option: $1"; usage; exit 1 ;;
	esac
done

SSH_OPTS=(
	-p "${SSH_PORT}"
	-o BatchMode=yes
	-o ConnectTimeout="${TIMEOUT_SSH}"
	-o StrictHostKeyChecking=no
	-o UserKnownHostsFile=/dev/null
)

run_remote() {
	# shellcheck disable=SC2029 # command in "$@" is intended to expand client-side
	ssh "${SSH_OPTS[@]}" "${TARGET_SSH}" "$@"
}

pass() { echo "[PASS] $*"; }
warn() { echo "[WARN] $*"; }
fail() { echo "[FAIL] $*"; }

echo "=== CPCS Data-path Self Check ==="
echo "initiator: if=${INIT_IF} ip=${INIT_IP}"
echo "target:    ssh=${TARGET_SSH} if=${TARGET_IF} ip=${TARGET_IP} port=${TRSVCID}"
echo "nqn=${NQN}"
echo

need_cmds=(ip nc python3 ssh)
for c in "${need_cmds[@]}"; do
	if ! command -v "${c}" >/dev/null 2>&1; then
		echo "Missing required command: ${c}"
		exit 1
	fi
done

route_ok=1
ssh_ok=1
listener_ok=1
rpc_listener_ok=1
arp_match=1
nc_ok=1
passthru_ok=1
firewall_suspect=0

echo "--- 1) Initiator route/source check ---"
route_line="$(ip route get "${TARGET_IP}" 2>&1 || true)"
echo "${route_line}"
route_dev="$(awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}' <<<"${route_line}")"
route_src="$(awk '{for(i=1;i<=NF;i++) if($i=="src"){print $(i+1); exit}}' <<<"${route_line}")"
if [[ "${route_dev}" != "${INIT_IF}" ]]; then
	fail "Route device mismatch: expected ${INIT_IF}, got ${route_dev:-<none>}"
	route_ok=0
else
	pass "Route device is ${INIT_IF}"
fi
if [[ "${route_src}" != "${INIT_IP}" ]]; then
	fail "Route source mismatch: expected ${INIT_IP}, got ${route_src:-<none>}"
	route_ok=0
else
	pass "Route source is ${INIT_IP}"
fi

echo
echo "--- 2) SSH/control reachability ---"
if ! run_remote "echo remote_ok" >/tmp/cpcs_selfcheck_ssh.out 2>/tmp/cpcs_selfcheck_ssh.err; then
	fail "SSH to target failed"
	echo "stderr:"
	sed -n '1,40p' /tmp/cpcs_selfcheck_ssh.err
	ssh_ok=0
else
	pass "SSH to target works"
fi
rm -f /tmp/cpcs_selfcheck_ssh.out /tmp/cpcs_selfcheck_ssh.err

echo
echo "--- 3) ARP/MAC ownership check ---"
target_mac="$(run_remote "cat /sys/class/net/${TARGET_IF}/address 2>/dev/null || true" | tr -d '\r' | tr '[:upper:]' '[:lower:]')"
if [[ -n "${target_mac}" ]]; then
	# Populate neighbor table via one quick probe.
	ping -I "${INIT_IF}" -c 1 -W 1 "${TARGET_IP}" >/dev/null 2>&1 || true
	neigh_line="$(ip neigh show "${TARGET_IP}" 2>/dev/null | head -n1)"
	echo "neigh: ${neigh_line:-<none>}"
	neigh_mac="$(awk '{for(i=1;i<=NF;i++) if($i=="lladdr"){print $(i+1); exit}}' <<<"${neigh_line}" | tr '[:upper:]' '[:lower:]')"
	echo "target_if(${TARGET_IF}) mac: ${target_mac}"
	if [[ -n "${neigh_mac}" && "${neigh_mac}" != "${target_mac}" ]]; then
		fail "ARP MAC mismatch: destination IP may point to a different host (duplicate IP/wrong L2 path)"
		arp_match=0
	else
		pass "ARP MAC matches target interface (or neighbor unresolved)"
	fi
else
	warn "Could not read target NIC MAC (check TARGET_IF)"
fi

echo
echo "--- 4) Target listener/process checks ---"
spdk_proc="$(run_remote "pgrep -af spdk_tgt || true")"
if [[ -n "${spdk_proc}" ]]; then
	pass "spdk_tgt process exists"
	echo "${spdk_proc}"
else
	fail "spdk_tgt process not found"
	listener_ok=0
fi

ss_line="$(run_remote "ss -lntp | grep -E '${TARGET_IP//./\\.}:${TRSVCID}[[:space:]]' || true")"
if [[ -n "${ss_line}" ]]; then
	pass "Target is listening on ${TARGET_IP}:${TRSVCID}"
	echo "${ss_line}"
else
	fail "No listener on ${TARGET_IP}:${TRSVCID} from target ss output"
	listener_ok=0
fi

echo
echo "--- 5) RPC listener config check ---"
listeners_json="$(run_remote "python3 '${TARGET_RPC_PY}' -s '${TARGET_RPC_SOCK}' nvmf_subsystem_get_listeners '${NQN}' 2>/dev/null || true")"
if [[ -z "${listeners_json}" ]]; then
	warn "Could not query listeners via RPC (socket/path/permissions?)"
	rpc_listener_ok=0
else
	if LISTENERS_JSON="${listeners_json}" python3 - "${TARGET_IP}" "${TRSVCID}" <<'PY'
import json, os, sys
target_ip = sys.argv[1]
target_port = str(sys.argv[2])
raw = os.environ.get("LISTENERS_JSON", "")
try:
    data = json.loads(raw)
except Exception:
    sys.exit(2)
ok = False
for item in data:
    traddr = str(item.get("traddr", ""))
    trsvcid = str(item.get("trsvcid", ""))
    if traddr == target_ip and trsvcid == target_port:
        ok = True
        break
sys.exit(0 if ok else 1)
PY
	then
		pass "RPC listener config includes ${TARGET_IP}:${TRSVCID}"
	else
		fail "RPC listener config does NOT include ${TARGET_IP}:${TRSVCID}"
		rpc_listener_ok=0
	fi
fi

echo
echo "--- 6) TCP connectivity check (initiator -> target) ---"
nc_out="$(nc -vz -w "${TIMEOUT_TCP}" -s "${INIT_IP}" "${TARGET_IP}" "${TRSVCID}" 2>&1 || true)"
echo "${nc_out}"
if grep -qi "succeeded\|open" <<<"${nc_out}"; then
	pass "TCP connectivity to ${TARGET_IP}:${TRSVCID} is OK"
else
	fail "TCP connectivity failed to ${TARGET_IP}:${TRSVCID}"
	nc_ok=0
fi

echo
echo "--- 7) Optional SPDK passthru identify probe ---"
if [[ -x "${SPDK_NVME_PASSTHRU}" ]]; then
	pt_out="$("${SPDK_NVME_PASSTHRU}" \
		--lcores 1 \
		--disable-cpumask-locks \
		--no-rpc-server \
		--admin-cmd \
		--trtype TCP \
		--traddr "${TARGET_IP}" \
		--trsvcid "${TRSVCID}" \
		--subnqn "${NQN}" \
		--hostnqn "${HOSTNQN}" \
		--opcode 0x06 \
		--nsid 0 \
		--data-len 4096 \
		--read 2>&1 || true)"
	echo "${pt_out}" | sed -n '1,40p'
	if grep -qi "Command completed" <<<"${pt_out}"; then
		pass "spdk_nvme_passthru identify probe succeeded"
	else
		fail "spdk_nvme_passthru identify probe failed"
		passthru_ok=0
	fi
else
	warn "spdk_nvme_passthru not found/executable at ${SPDK_NVME_PASSTHRU}"
fi

echo
echo "--- 8) Firewall quick hint (target) ---"
fw_lines="$(run_remote "sudo -n sh -c 'nft list ruleset 2>/dev/null; iptables -S 2>/dev/null'" | grep -Ei '4420|reject|drop' || true)"
if [[ -n "${fw_lines}" ]]; then
	warn "Potential firewall rules detected (showing up to 20 lines):"
	echo "${fw_lines}" | sed -n '1,20p'
	firewall_suspect=1
else
	pass "No obvious reject/drop rule snippet detected for 4420"
fi

echo
echo "=== Diagnosis ==="
if [[ ${ssh_ok} -eq 0 ]]; then
	echo "Problem point: CONTROL PATH SSH reachability/authentication."
	exit 1
fi

if [[ ${route_ok} -eq 0 ]]; then
	echo "Problem point: INITIATOR ROUTE/SOURCE selection is wrong for data path."
	exit 2
fi

if [[ ${listener_ok} -eq 0 || ${rpc_listener_ok} -eq 0 ]]; then
	echo "Problem point: TARGET LISTENER setup mismatch (spdk_tgt listener not active or wrong traddr/trsvcid)."
	exit 3
fi

if [[ ${arp_match} -eq 0 ]]; then
	echo "Problem point: L2 ownership mismatch (duplicate IP / wrong neighbor mapping)."
	exit 4
fi

if [[ ${nc_ok} -eq 0 ]]; then
	if [[ ${firewall_suspect} -eq 1 ]]; then
		echo "Problem point: likely TARGET FIREWALL/ACL rejecting port ${TRSVCID}."
	else
		echo "Problem point: data-path TCP not established despite listener; investigate routing/netns/firewall."
	fi
	exit 5
fi

if [[ ${passthru_ok} -eq 0 ]]; then
	echo "Problem point: NVMe-oF level (NQN/hostnqn/subsystem permissions), not raw TCP."
	exit 6
fi

echo "No obvious data-path issue detected by this checker."
echo "If experiment still fails, capture app-specific logs (cpcs/spdk_tgt) around the failing command."
exit 0

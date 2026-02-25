#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


SCRIPT_DIR = Path(__file__).resolve().parent
SPDK_ROOT = SCRIPT_DIR.parent.parent.parent


def env_default(name: str, default: str) -> str:
	return os.environ.get(name, default)


DEFAULT_RPC = env_default("RPC", str(SPDK_ROOT / "scripts" / "rpc.py"))
DEFAULT_SPDK_TGT = env_default("SPDK_TGT", str(SPDK_ROOT / "build" / "bin" / "spdk_tgt"))
DEFAULT_BDEVPERF = env_default("BDEVPERF", str(SPDK_ROOT / "build" / "examples" / "bdevperf"))
DEFAULT_RPC_SOCK = env_default("RPC_SOCK", "/var/tmp/vslm_demo.sock")


def quote_cmd(cmd: List[str]) -> str:
	return " ".join(shlex.quote(x) for x in cmd)


def run(
	cmd: List[str],
	*,
	check: bool = True,
	capture_output: bool = False,
) -> subprocess.CompletedProcess:
	print(f"+ {quote_cmd(cmd)}")
	return subprocess.run(
		cmd,
		check=check,
		text=True,
		capture_output=capture_output,
	)


def require_executable(path: str, name: str) -> None:
	if not (os.path.isfile(path) and os.access(path, os.X_OK)):
		raise FileNotFoundError(f"{name} not found or not executable: {path}")


def parse_json_output(cp: subprocess.CompletedProcess) -> object:
	if cp.stdout is None:
		raise RuntimeError("No stdout captured for JSON parsing")
	try:
		return json.loads(cp.stdout)
	except json.JSONDecodeError as ex:
		raise RuntimeError(f"Failed to parse JSON output: {ex}\nOutput:\n{cp.stdout}") from ex


def parse_bdevperf_total(output: str) -> Tuple[float, float]:
	text = output.replace("\r", "\n")
	for line in text.splitlines():
		if not line.strip().startswith("Total"):
			continue
		values = re.findall(r"[-+]?\d+(?:\.\d+)?", line)
		if len(values) >= 2:
			return float(values[0]), float(values[1])
	raise RuntimeError("Could not parse bdevperf summary line containing 'Total'")


def parse_latency_percentiles(output: str) -> Dict[str, Optional[float]]:
	values: Dict[str, Optional[float]] = {"p50": None, "p95": None, "p99": None}

	for key in values.keys():
		match = re.search(rf"(?i)\b{key}\b[^0-9]*([0-9]+(?:\.[0-9]+)?)", output)
		if match is not None:
			values[key] = float(match.group(1))

	return values


@dataclass
class RpcClient:
	rpc_script: str
	rpc_sock: str

	def _rpc_cmd(self, method: str, *args: str) -> List[str]:
		return ["python3", self.rpc_script, "-s", self.rpc_sock, method, *args]

	def call(self, method: str, *args: str, check: bool = True) -> subprocess.CompletedProcess:
		return run(self._rpc_cmd(method, *args), check=check, capture_output=True)

	def call_json(self, method: str, *args: str) -> object:
		cp = self.call(method, *args, check=True)
		return parse_json_output(cp)

	def quiet(self, method: str, *args: str) -> None:
		subprocess.run(
			self._rpc_cmd(method, *args),
			check=False,
			stdout=subprocess.DEVNULL,
			stderr=subprocess.DEVNULL,
			text=True,
		)


class SpdkTarget:
	def __init__(self, spdk_tgt: str, rpc_sock: str, log_path: str):
		self._spdk_tgt = spdk_tgt
		self._rpc_sock = rpc_sock
		self._log_path = log_path
		self._proc: Optional[subprocess.Popen] = None
		self._log_file = None

	def start(self) -> None:
		if os.path.exists(self._rpc_sock):
			os.remove(self._rpc_sock)

		self._log_file = open(self._log_path, "w", encoding="utf-8")
		cmd = [self._spdk_tgt, "-r", self._rpc_sock, "--wait-for-rpc"]
		print(f"+ {quote_cmd(cmd)}")
		self._proc = subprocess.Popen(
			cmd,
			stdout=self._log_file,
			stderr=subprocess.STDOUT,
			text=True,
		)

	def wait_for_rpc(self, rpc: RpcClient, timeout_sec: float = 15.0) -> None:
		deadline = time.time() + timeout_sec
		while time.time() < deadline:
			if self._proc and self._proc.poll() is not None:
				raise RuntimeError(f"spdk_tgt exited early. Check log: {self._log_path}")
			cp = rpc.call("rpc_get_methods", check=False)
			if cp.returncode == 0:
				return
			time.sleep(0.2)
		raise TimeoutError(f"Timed out waiting for RPC socket: {self._rpc_sock}")

	def stop(self) -> None:
		if self._proc is not None:
			self._proc.terminate()
			try:
				self._proc.wait(timeout=5)
			except subprocess.TimeoutExpired:
				self._proc.kill()
				self._proc.wait(timeout=5)
			self._proc = None

		if self._log_file is not None:
			self._log_file.close()
			self._log_file = None

		if os.path.exists(self._rpc_sock):
			os.remove(self._rpc_sock)


def run_functional(args: argparse.Namespace) -> Dict[str, object]:
	require_executable(args.spdk_tgt, "spdk_tgt")
	if not os.path.isfile(args.rpc_script):
		raise FileNotFoundError(f"rpc.py not found: {args.rpc_script}")

	log_path = args.target_log
	target = SpdkTarget(args.spdk_tgt, args.rpc_sock, log_path)
	rpc = RpcClient(args.rpc_script, args.rpc_sock)

	result: Dict[str, object] = {
		"fdp_enable_attempted": False,
		"fdp_enable_succeeded": False,
		"stats_before": {},
	}

	target.start()
	try:
		target.wait_for_rpc(rpc)
		rpc.call("framework_start_init")

		rpc.call(
			"bdev_malloc_create",
			"-b",
			args.base_bdev_name,
			str(args.base_size_mb),
			str(args.block_size),
		)

		create_args = [
			"--name", args.vslm_name,
			"--base-bdev-name", args.base_bdev_name,
			"--sram-size-mb", str(args.sram_size_mb),
		]
		if args.nsid is not None:
			create_args.extend(["--nsid", str(args.nsid)])
		if args.fdp_mode_enabled:
			create_args.append("--fdp-mode-enabled")
		if args.fdp_dspec is not None:
			create_args.extend(["--fdp-dspec", str(args.fdp_dspec)])
		if args.semantics_mode is not None:
			create_args.extend(["--semantics-mode", args.semantics_mode])
		if args.writeback_policy is not None:
			create_args.extend(["--writeback-policy", args.writeback_policy])
		if args.admission_enabled:
			create_args.append("--admission-enabled")
			create_args.extend([
				"--admission-faults-per-sec-threshold",
				str(args.admission_faults_per_sec_threshold),
			])
		else:
			create_args.append("--admission-disabled")
		rpc.call("bdev_vslm_create", *create_args)

		bdev_info = rpc.call_json("bdev_get_bdevs", "-b", args.vslm_name)
		if not isinstance(bdev_info, list) or not bdev_info:
			raise RuntimeError("vSLM bdev was not returned by bdev_get_bdevs")
		bdev_entry = bdev_info[0]
		bdev_capacity = int(bdev_entry.get("num_blocks", 0)) * int(bdev_entry.get("block_size", 0))
		if bdev_capacity <= args.sram_size_mb * 1024 * 1024:
			raise RuntimeError("vSLM capacity is not larger than SRAM cache size")

		stats_before = rpc.call_json("bdev_vslm_get_stats", "--name", args.vslm_name)
		if not isinstance(stats_before, dict):
			raise RuntimeError("Unexpected stats response format")

		required_stat_keys = {
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
		missing = required_stat_keys.difference(stats_before.keys())
		if missing:
			raise RuntimeError(f"Missing stats fields: {sorted(missing)}")

		policy_before = rpc.call_json("bdev_vslm_get_policy", "--name", args.vslm_name)
		if not isinstance(policy_before, dict):
			raise RuntimeError("Unexpected policy response format")
		if policy_before.get("name") != args.vslm_name:
			raise RuntimeError("Policy lookup returned mismatched bdev name")

		set_policy_args = [
			"--name", args.vslm_name,
			"--semantics-mode", args.semantics_mode,
			"--writeback-policy", args.writeback_policy,
		]
		if args.admission_enabled:
			set_policy_args.append("--admission-enabled")
			set_policy_args.extend([
				"--admission-faults-per-sec-threshold",
				str(args.admission_faults_per_sec_threshold),
			])
		else:
			set_policy_args.append("--admission-disabled")
		rpc.call("bdev_vslm_set_policy", *set_policy_args)

		policy_after = rpc.call_json("bdev_vslm_get_policy", "--name", args.vslm_name)
		if policy_after.get("semantics_mode") != args.semantics_mode:
			raise RuntimeError("semantics_mode mismatch after set_policy")
		if policy_after.get("writeback_policy") != args.writeback_policy:
			raise RuntimeError("writeback_policy mismatch after set_policy")
		if bool(policy_after.get("admission_enabled")) != bool(args.admission_enabled):
			raise RuntimeError("admission_enabled mismatch after set_policy")

		rpc.call("bdev_vslm_reset_stats", "--name", args.vslm_name)
		stats_after_reset = rpc.call_json("bdev_vslm_get_stats", "--name", args.vslm_name)
		if not isinstance(stats_after_reset, dict):
			raise RuntimeError("Unexpected stats response format after reset")

		if args.nsid is not None:
			lease1 = 1001
			lease2 = 1002
			rpc.call(
				"bdev_vslm_lease_acquire",
				"--lease-id", str(lease1),
				"--nsid", str(args.nsid),
				"--offset", "0",
				"--length", "4096",
			)
			conflict = rpc.call(
				"bdev_vslm_lease_acquire",
				"--lease-id", str(lease2),
				"--nsid", str(args.nsid),
				"--offset", "0",
				"--length", "4096",
				check=False,
			)
			if conflict.returncode == 0:
				raise RuntimeError("Lease conflict test failed: overlapping lease unexpectedly succeeded")
			rpc.call("bdev_vslm_lease_release", "--lease-id", str(lease1))
			rpc.call(
				"bdev_vslm_lease_acquire",
				"--lease-id", str(lease2),
				"--nsid", str(args.nsid),
				"--offset", "0",
				"--length", "4096",
			)
			rpc.call("bdev_vslm_lease_release", "--lease-id", str(lease2))

		rpc.call(
			"bdev_vslm_set_fdp_mode",
			"--name", args.vslm_name,
			"--disable",
			"--dspec", str(args.fdp_dspec or 0),
		)

		if args.try_fdp_enable:
			result["fdp_enable_attempted"] = True
			cp = rpc.call(
				"bdev_vslm_set_fdp_mode",
				"--name", args.vslm_name,
				"--enable",
				"--dspec", str(args.fdp_dspec or 1),
				check=False,
			)
			if cp.returncode == 0:
				result["fdp_enable_succeeded"] = True
				if args.expect_fdp_enable_failure:
					raise RuntimeError("FDP enable succeeded unexpectedly on this backend")
			else:
				if not args.expect_fdp_enable_failure:
					err = (cp.stderr or "").strip()
					raise RuntimeError(f"FDP enable failed unexpectedly: {err}")

		result["stats_before"] = stats_before
		result["policy_before"] = policy_before
		result["policy_after"] = policy_after
		result["stats_after_reset"] = stats_after_reset
		return result
	finally:
		rpc.quiet("bdev_vslm_delete", "--name", args.vslm_name)
		rpc.quiet("bdev_malloc_delete", args.base_bdev_name)
		target.stop()


def build_bdevperf_config(
	path: Path,
	*,
	base_bdev_name: str,
	base_size_mb: int,
	block_size: int,
	include_vslm: bool,
	vslm_name: str,
	sram_size_mb: int,
	fdp_mode_enabled: bool,
	fdp_dspec: int,
) -> None:
	config = {
		"subsystems": [
			{
				"subsystem": "bdev",
				"config": [
					{
						"method": "bdev_malloc_create",
						"params": {
							"name": base_bdev_name,
							"num_blocks": (base_size_mb * 1024 * 1024) // block_size,
							"block_size": block_size,
						},
					},
				],
			},
		],
	}

	if include_vslm:
		config["subsystems"][0]["config"].append(
			{
				"method": "bdev_vslm_create",
				"params": {
					"name": vslm_name,
					"base_bdev_name": base_bdev_name,
					"sram_size_mb": sram_size_mb,
				},
			}
		)
		config["subsystems"][0]["config"].append(
			{
				"method": "bdev_vslm_set_fdp_mode",
				"params": {
					"name": vslm_name,
					"enabled": fdp_mode_enabled,
					"dspec": fdp_dspec,
				},
			}
		)

	with open(path, "w", encoding="utf-8") as f:
		json.dump(config, f, indent=2)


def run_single_bdevperf(
	*,
	bdevperf: str,
	config_path: Path,
	target_bdev: str,
	workload: str,
	runtime_sec: int,
	queue_depth: int,
	io_size: int,
	extra_args: List[str],
) -> Tuple[float, float, str, Dict[str, Optional[float]]]:
	cmd = [
		bdevperf,
		"--json", str(config_path),
		"-T", target_bdev,
		"-q", str(queue_depth),
		"-o", str(io_size),
		"-w", workload,
		"-t", str(runtime_sec),
	]
	cmd.extend(extra_args)
	cp = run(cmd, capture_output=True)
	output = (cp.stdout or "") + (cp.stderr or "")
	iops, mib_per_sec = parse_bdevperf_total(output)
	lat = parse_latency_percentiles(output)
	return iops, mib_per_sec, output, lat


def run_performance(args: argparse.Namespace) -> Dict[str, object]:
	require_executable(args.bdevperf, "bdevperf")

	workloads = [w.strip().lower() for w in args.workloads.split(",") if w.strip()]
	if not workloads:
		raise ValueError("At least one workload must be specified")

	extra_args = list(args.bdevperf_extra_arg or [])
	if args.no_huge_data and "-H" not in extra_args:
		extra_args.append("-H")

	rows = []
	full_outputs: Dict[str, str] = {}

	with tempfile.TemporaryDirectory(prefix="vslm_perf_") as tmpdir:
		tmp = Path(tmpdir)
		base_config = tmp / "base.json"
		vslm_config = tmp / "vslm.json"

		build_bdevperf_config(
			base_config,
			base_bdev_name=args.base_bdev_name,
			base_size_mb=args.base_size_mb,
			block_size=args.block_size,
			include_vslm=False,
			vslm_name=args.vslm_name,
			sram_size_mb=args.sram_size_mb,
			fdp_mode_enabled=False,
			fdp_dspec=args.fdp_dspec or 0,
		)
		build_bdevperf_config(
			vslm_config,
			base_bdev_name=args.base_bdev_name,
			base_size_mb=args.base_size_mb,
			block_size=args.block_size,
			include_vslm=True,
			vslm_name=args.vslm_name,
			sram_size_mb=args.sram_size_mb,
			fdp_mode_enabled=args.fdp_mode_enabled,
			fdp_dspec=args.fdp_dspec or 0,
		)

		for workload in workloads:
			base_iops, base_mib, base_out, base_lat = run_single_bdevperf(
				bdevperf=args.bdevperf,
				config_path=base_config,
				target_bdev=args.base_bdev_name,
				workload=workload,
				runtime_sec=args.runtime_sec,
				queue_depth=args.queue_depth,
				io_size=args.io_size,
				extra_args=extra_args,
			)

			vslm_iops, vslm_mib, vslm_out, vslm_lat = run_single_bdevperf(
				bdevperf=args.bdevperf,
				config_path=vslm_config,
				target_bdev=args.vslm_name,
				workload=workload,
				runtime_sec=args.runtime_sec,
				queue_depth=args.queue_depth,
				io_size=args.io_size,
				extra_args=extra_args,
			)

			iops_delta_pct = ((vslm_iops - base_iops) / base_iops * 100.0) if base_iops else 0.0
			mib_delta_pct = ((vslm_mib - base_mib) / base_mib * 100.0) if base_mib else 0.0
			rows.append(
				{
					"workload": workload,
					"baseline_iops": base_iops,
					"vslm_iops": vslm_iops,
					"iops_delta_pct": iops_delta_pct,
					"baseline_mib_per_sec": base_mib,
					"vslm_mib_per_sec": vslm_mib,
					"mib_delta_pct": mib_delta_pct,
					"baseline_p50_us": base_lat["p50"],
					"vslm_p50_us": vslm_lat["p50"],
					"baseline_p95_us": base_lat["p95"],
					"vslm_p95_us": vslm_lat["p95"],
					"baseline_p99_us": base_lat["p99"],
					"vslm_p99_us": vslm_lat["p99"],
				}
			)
			full_outputs[f"{workload}_baseline"] = base_out
			full_outputs[f"{workload}_vslm"] = vslm_out

	print("\nPerformance Summary")
	print(
		"workload       baseline_iops  vslm_iops  delta(%)  "
		"baseline_mib/s  vslm_mib/s  delta(%)"
	)
	for row in rows:
		print(
			f"{row['workload']:<13}"
			f"{row['baseline_iops']:>13.2f}"
			f"{row['vslm_iops']:>11.2f}"
			f"{row['iops_delta_pct']:>10.2f}"
			f"{row['baseline_mib_per_sec']:>16.2f}"
			f"{row['vslm_mib_per_sec']:>12.2f}"
			f"{row['mib_delta_pct']:>10.2f}"
		)

	print("\nLatency Percentiles (us)")
	print("workload       baseline_p50  vslm_p50  baseline_p95  vslm_p95  baseline_p99  vslm_p99")
	for row in rows:
		def fmt(v: Optional[float]) -> str:
			return "n/a" if v is None else f"{v:.2f}"

		print(
			f"{row['workload']:<13}"
			f"{fmt(row['baseline_p50_us']):>13}"
			f"{fmt(row['vslm_p50_us']):>10}"
			f"{fmt(row['baseline_p95_us']):>14}"
			f"{fmt(row['vslm_p95_us']):>10}"
			f"{fmt(row['baseline_p99_us']):>14}"
			f"{fmt(row['vslm_p99_us']):>10}"
		)

	return {"rows": rows, "raw_outputs": full_outputs}


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(
		description="vSLM PoC test script: functional RPC checks + bdevperf performance runs",
	)
	parser.add_argument(
		"--mode",
		choices=["all", "functional", "performance"],
		default="all",
		help="Which phase to run",
	)
	parser.add_argument("--rpc-script", default=DEFAULT_RPC, help="Path to scripts/rpc.py")
	parser.add_argument("--spdk-tgt", default=DEFAULT_SPDK_TGT, help="Path to spdk_tgt binary")
	parser.add_argument("--bdevperf", default=DEFAULT_BDEVPERF, help="Path to bdevperf binary")
	parser.add_argument("--rpc-sock", default=DEFAULT_RPC_SOCK, help="RPC unix socket path")
	parser.add_argument("--target-log", default="/tmp/vslm_demo_spdk_tgt.log", help="spdk_tgt log file path")

	parser.add_argument("--base-bdev-name", default="MallocBase0", help="Base bdev name")
	parser.add_argument("--vslm-name", default="vslm0", help="vSLM bdev name")
	parser.add_argument("--base-size-mb", type=int, default=1024, help="Malloc base size in MiB")
	parser.add_argument("--sram-size-mb", type=int, default=64, help="vSLM SRAM size in MiB")
	parser.add_argument("--block-size", type=int, default=4096, help="Block size in bytes")
	parser.add_argument("--nsid", type=int, default=1, help="NSID for vSLM")

	parser.add_argument("--fdp-mode-enabled", action="store_true", help="Enable FDP tagging in performance config")
	parser.add_argument("--fdp-dspec", type=int, default=1, help="FDP dspec value")
	parser.add_argument("--semantics-mode", choices=["lease", "double_buffer"], default="lease",
			    help="vSLM semantics mode for create/policy checks")
	parser.add_argument("--writeback-policy", choices=["on_evict", "at_boundary", "hybrid"], default="on_evict",
			    help="vSLM writeback policy for create/policy checks")
	parser.add_argument("--admission-enabled", action="store_true", default=False,
			    help="Enable admission control for create/policy checks")
	parser.add_argument("--admission-faults-per-sec-threshold", type=int, default=100,
			    help="Admission faults/sec threshold when admission is enabled")
	parser.add_argument("--try-fdp-enable", action="store_true", default=True,
			    help="Attempt runtime FDP enable in functional phase")
	parser.add_argument("--no-try-fdp-enable", dest="try_fdp_enable", action="store_false",
			    help="Skip runtime FDP enable attempt in functional phase")
	parser.add_argument("--expect-fdp-enable-failure", action="store_true", default=True,
			    help="Expect runtime FDP enable to fail on non-FDP backends")
	parser.add_argument("--expect-fdp-enable-success", dest="expect_fdp_enable_failure", action="store_false",
			    help="Require runtime FDP enable success")

	parser.add_argument("--workloads", default="randread,randwrite", help="Comma-separated bdevperf workloads")
	parser.add_argument("--runtime-sec", type=int, default=5, help="bdevperf runtime per workload in seconds")
	parser.add_argument("--queue-depth", type=int, default=64, help="bdevperf queue depth")
	parser.add_argument("--io-size", type=int, default=4096, help="bdevperf I/O size bytes")
	parser.add_argument(
		"--bdevperf-extra-arg",
		action="append",
		help="Extra arg forwarded to bdevperf (repeatable)",
	)
	parser.add_argument(
		"--no-huge-data",
		action="store_true",
		default=True,
		help="Append -H so bdevperf data buffers use non-huge allocations",
	)
	parser.add_argument(
		"--with-huge-data",
		dest="no_huge_data",
		action="store_false",
		help="Do not add -H to bdevperf",
	)
	parser.add_argument(
		"--output-json",
		default=None,
		help="Optional output file for structured result JSON",
	)
	return parser.parse_args()


def main() -> int:
	args = parse_args()
	results: Dict[str, object] = {}

	try:
		if args.mode in ("all", "functional"):
			print("\n=== Functional phase ===")
			functional_result = run_functional(args)
			results["functional"] = functional_result
			print("Functional checks: PASS")

		if args.mode in ("all", "performance"):
			print("\n=== Performance phase ===")
			perf_result = run_performance(args)
			results["performance"] = perf_result

		if args.output_json:
			with open(args.output_json, "w", encoding="utf-8") as f:
				json.dump(results, f, indent=2)
			print(f"\nWrote structured results to: {args.output_json}")

		return 0
	except Exception as ex:
		print(f"\nERROR: {ex}", file=sys.stderr)
		return 1


if __name__ == "__main__":
	sys.exit(main())

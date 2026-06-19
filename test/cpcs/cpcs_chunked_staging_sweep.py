#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

import argparse
import json
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_COMPARE_SCRIPT = str(SCRIPT_DIR / "vslm_pslm_perf_compare.py")


def quote_cmd(cmd: List[str]) -> str:
    return " ".join(shlex.quote(token) for token in cmd)


def parse_iteration_counts(text: str) -> List[int]:
    values: List[int] = []
    for raw in text.split(","):
        value = int(raw.strip())
        if value <= 0:
            raise ValueError("iteration counts must be > 0")
        values.append(value)
    if not values:
        raise ValueError("at least one iteration count is required")
    return values


def run_compare(cmd: List[str], json_path: Path) -> Dict[str, Any]:
    print(f"+ {quote_cmd(cmd)}")
    cp = subprocess.run(cmd, text=True, capture_output=True)
    if cp.returncode != 0:
        raise RuntimeError(
            f"Chunked staging point failed (rc={cp.returncode}).\n"
            f"command: {quote_cmd(cmd)}\n"
            f"stdout:\n{cp.stdout}\n"
            f"stderr:\n{cp.stderr}"
        )
    with open(json_path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def print_summary(points: List[Dict[str, Any]]) -> None:
    print("\nChunked Staging Sweep Summary")
    print("iters  chunk_mb  pslm_cmds  vslm_cmds  pslm_e2e_s  vslm_e2e_s  pslm_penalty_x  vslm_speedup_x")
    for point in points:
        print(
            f"{point['iteration_count']:>5}  "
            f"{point['chunk_size_mb']:>8}  "
            f"{point['pslm_copy_cmds_mean']:>9.1f}  "
            f"{point['vslm_copy_cmds_mean']:>9.1f}  "
            f"{point['pslm_end_to_end_seconds_mean']:>10.3f}  "
            f"{point['vslm_end_to_end_seconds_mean']:>10.3f}  "
            f"{point['pslm_penalty_vs_single_chunk_x']:>14.3f}  "
            f"{point['vslm_speedup_vs_pslm_x']:>16.3f}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Sweep pSLM chunk counts and measure chunked staging penalty vs vSLM",
    )
    parser.add_argument("--compare-script", default=DEFAULT_COMPARE_SCRIPT, help="Path to vslm_pslm_perf_compare.py")
    parser.add_argument("--pcie-bdf", required=True, help="PCIe BDF for bdev_nvme_attach_controller")
    parser.add_argument("--dataset-bdev", required=True, help="Dataset source bdev")
    parser.add_argument("--backing-bdev", required=True, help="vSLM backing bdev")
    parser.add_argument("--dataset-size-gb", type=int, required=True, help="Dataset size in GiB")
    parser.add_argument(
        "--iteration-counts",
        default="1,2,4,8,16",
        help="Comma-separated logical iteration counts for the hard-cap baseline",
    )
    parser.add_argument(
        "--builtin-program",
        choices=("sum64", "max64", "min64"),
        default="sum64",
        help="Builtin CPCS program used during each point",
    )
    parser.add_argument("--runs", type=int, default=3, help="Repetitions per point")

    parser.add_argument("--rpc-script", default=None, help="Optional override for scripts/rpc.py")
    parser.add_argument("--spdk-tgt", default=None, help="Optional override for spdk_tgt")
    parser.add_argument("--spdk-nvme-passthru", default=None, help="Optional override for spdk_nvme_passthru")
    parser.add_argument("--controller-name", default="Nvme0", help="SPDK NVMe controller alias")
    parser.add_argument("--sram-mb", type=int, default=32, help="vSLM SRAM size in MiB")
    parser.add_argument("--pslm-size-mb", type=int, default=32, help="pSLM size in MiB")
    parser.add_argument("--vslm-backing-min-gb", type=int, default=64, help="Minimum required backing size in GiB")
    parser.add_argument("--dataset-nsid", type=int, default=1, help="Dataset namespace NSID")
    parser.add_argument("--slm-nsid", type=int, default=100, help="SLM namespace NSID")
    parser.add_argument("--cpcs-nsid", type=int, default=200, help="CPCS compute namespace NSID")
    parser.add_argument("--pslm-granularity", type=int, default=4, help="pSLM granularity in bytes")

    parser.add_argument("--nqn", default="nqn.2026-03.io.spdk:vslm-staging-sweep", help="NVMf subsystem NQN")
    parser.add_argument("--serial", default="VSLMSTAGSWEEP01", help="NVMf subsystem serial")
    parser.add_argument("--max-namespaces", type=int, default=1024, help="Max namespaces for subsystem")
    parser.add_argument("--trtype", default="TCP", help="NVMf transport type")
    parser.add_argument("--traddr", default="127.0.0.1", help="NVMf target address")
    parser.add_argument("--trsvcid", default="4420", help="NVMf target service ID")
    parser.add_argument("--src-addr", default=None, help="Source address for initiator-side fabrics connection")
    parser.add_argument("--src-svcid", default=None, help="Source service id (port) for initiator-side fabrics connection")
    parser.add_argument("--passthru-lcores", default="1", help="Lcores argument for spdk_nvme_passthru")
    parser.add_argument("--core-mask", default="0x1", help="CPU core mask for spdk_tgt")
    parser.add_argument("--output-json", default=None, help="Write structured output JSON")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        iteration_counts = parse_iteration_counts(args.iteration_counts)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    dataset_mb = args.dataset_size_gb * 1024
    for count in iteration_counts:
        if dataset_mb % count != 0:
            print(
                f"ERROR: dataset-size-gb={args.dataset_size_gb} does not divide cleanly into {count} iterations",
                file=sys.stderr,
            )
            return 1

    points: List[Dict[str, Any]] = []

    try:
        with tempfile.TemporaryDirectory(prefix="cpcs_chunk_sweep_") as tmp:
            tmp_dir = Path(tmp)
            for count in iteration_counts:
                chunk_size_mb = dataset_mb // count
                point_json = tmp_dir / f"point_{count}.json"

                cmd = [
                    "python3",
                    args.compare_script,
                    "--pcie-bdf", args.pcie_bdf,
                    "--dataset-bdev", args.dataset_bdev,
                    "--backing-bdev", args.backing_bdev,
                    "--dataset-size-gb", str(args.dataset_size_gb),
                    "--chunk-size-mb", str(chunk_size_mb),
                    "--builtin-program", args.builtin_program,
                    "--runs", str(args.runs),
                    "--controller-name", args.controller_name,
                    "--sram-mb", str(args.sram_mb),
                    "--pslm-size-mb", str(args.pslm_size_mb),
                    "--vslm-backing-min-gb", str(args.vslm_backing_min_gb),
                    "--dataset-nsid", str(args.dataset_nsid),
                    "--slm-nsid", str(args.slm_nsid),
                    "--cpcs-nsid", str(args.cpcs_nsid),
                    "--pslm-granularity", str(args.pslm_granularity),
                    "--nqn", args.nqn,
                    "--serial", args.serial,
                    "--max-namespaces", str(args.max_namespaces),
                    "--trtype", args.trtype,
                    "--traddr", args.traddr,
                    "--trsvcid", args.trsvcid,
                    "--passthru-lcores", args.passthru_lcores,
                    "--core-mask", args.core_mask,
                    "--output-json", str(point_json),
                ]
                if args.src_addr:
                    cmd.extend(["--src-addr", args.src_addr])
                if args.src_svcid:
                    cmd.extend(["--src-svcid", args.src_svcid])

                if args.rpc_script:
                    cmd.extend(["--rpc-script", args.rpc_script])
                if args.spdk_tgt:
                    cmd.extend(["--spdk-tgt", args.spdk_tgt])
                if args.spdk_nvme_passthru:
                    cmd.extend(["--spdk-nvme-passthru", args.spdk_nvme_passthru])

                payload = run_compare(cmd, point_json)
                agg = payload["aggregate"]
                pslm_e2e = float(agg["pslm"]["end_to_end_seconds"]["mean"])
                vslm_e2e = float(agg["vslm"]["end_to_end_seconds"]["mean"])
                point = {
                    "iteration_count": count,
                    "chunk_size_mb": chunk_size_mb,
                    "pslm_copy_cmds_mean": float(agg["pslm"]["copy_cmd_count"]["mean"]),
                    "vslm_copy_cmds_mean": float(agg["vslm"]["copy_cmd_count"]["mean"]),
                    "pslm_execute_cmds_mean": float(agg["pslm"]["execute_cmd_count"]["mean"]),
                    "vslm_execute_cmds_mean": float(agg["vslm"]["execute_cmd_count"]["mean"]),
                    "pslm_end_to_end_seconds_mean": pslm_e2e,
                    "vslm_end_to_end_seconds_mean": vslm_e2e,
                    "vslm_speedup_vs_pslm_x": (pslm_e2e / vslm_e2e) if vslm_e2e > 0 else 0.0,
                    "raw": payload,
                }
                points.append(point)

        single_chunk = next((point for point in points if point["iteration_count"] == 1), None)
        if single_chunk is None:
            single_chunk = points[0]
        base_pslm = single_chunk["pslm_end_to_end_seconds_mean"]
        for point in points:
            point["pslm_penalty_vs_single_chunk_x"] = (
                point["pslm_end_to_end_seconds_mean"] / base_pslm if base_pslm > 0 else 0.0
            )

        print_summary(points)

        if args.output_json:
            payload = {
                "config": {
                    "dataset_size_gb": args.dataset_size_gb,
                    "dataset_bdev": args.dataset_bdev,
                    "backing_bdev": args.backing_bdev,
                    "builtin_program": args.builtin_program,
                    "iteration_counts": iteration_counts,
                    "runs": args.runs,
                },
                "points": points,
            }
            with open(args.output_json, "w", encoding="utf-8") as fh:
                json.dump(payload, fh, indent=2)
            print(f"\nWrote JSON results: {args.output_json}")
        return 0
    except Exception as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

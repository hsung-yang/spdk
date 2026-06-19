#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

import argparse
import json
import sys
from typing import Any, Dict, Optional, Sequence

import vslm_pslm_perf_compare as perf_compare


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run capacity-gap experiments with small pSLM/vSLM and an optional "
            "full-fit pSLM baseline (dataset fits at once)."
        ),
    )
    parser.add_argument("--pcie-bdf", required=True, help="PCIe BDF for bdev_nvme_attach_controller")
    parser.add_argument("--dataset-bdev", required=True, help="Dataset source bdev")
    parser.add_argument("--backing-bdev", required=True, help="vSLM backing bdev")
    parser.add_argument("--skip-nvme-attach", action="store_true",
                        help="Skip bdev_nvme_attach_controller and use pre-existing dataset/backing bdevs")
    parser.add_argument("--dataset-malloc-mb", type=int, default=0,
                        help="Create dataset-bdev as malloc with this size in MiB (0 disables)")
    parser.add_argument("--backing-malloc-mb", type=int, default=0,
                        help="Create backing-bdev as malloc with this size in MiB (0 disables)")
    parser.add_argument("--dataset-aio-file", default=None,
                        help="Create dataset-bdev from this aio file path")
    parser.add_argument("--backing-aio-file", default=None,
                        help="Create backing-bdev from this aio file path")
    parser.add_argument("--dataset-aio-size-mb", type=int, default=0,
                        help="Truncate/create dataset aio file to this size in MiB")
    parser.add_argument("--backing-aio-size-mb", type=int, default=0,
                        help="Truncate/create backing aio file to this size in MiB")

    parser.add_argument("--dataset-size-gb", type=int, default=4, help="Dataset size in GiB")
    parser.add_argument(
        "--dataset-size-mb",
        type=int,
        default=None,
        help="Optional dataset size in MiB (overrides --dataset-size-gb)",
    )
    parser.add_argument(
        "--physical-slm-mb",
        type=int,
        default=4,
        help="Small-DRAM SLM window size in MiB (used as both pSLM size and chunk size)",
    )
    parser.add_argument("--sram-mb", type=int, default=4, help="vSLM SRAM size in MiB for the small-DRAM scenario")
    parser.add_argument(
        "--vslm-max-copy-mb",
        type=int,
        default=None,
        help="vSLM max bytes per copy command in MiB (default: dataset-size-gb * 1024)",
    )

    fullfit_group = parser.add_mutually_exclusive_group()
    fullfit_group.add_argument(
        "--fullfit-pslm-enabled",
        dest="fullfit_pslm_enabled",
        action="store_true",
        help="Run extra full-fit pSLM baseline (default enabled)",
    )
    fullfit_group.add_argument(
        "--fullfit-pslm-disabled",
        dest="fullfit_pslm_enabled",
        action="store_false",
        help="Disable extra full-fit pSLM baseline",
    )
    parser.set_defaults(fullfit_pslm_enabled=True)
    parser.add_argument(
        "--fullfit-pslm-mb",
        type=int,
        default=None,
        help="pSLM size in MiB for full-fit baseline (default: dataset size in MiB)",
    )
    parser.add_argument(
        "--fullfit-chunk-mb",
        type=int,
        default=256,
        help="Chunk size in MiB for full-fit pSLM baseline copies",
    )

    parser.add_argument(
        "--builtin-program",
        choices=("sum64", "max64", "min64"),
        default="sum64",
        help="Builtin CPCS program",
    )
    parser.add_argument(
        "--builtin-exec-max-mb",
        type=int,
        default=256,
        help="Max bytes per builtin Execute command",
    )
    parser.add_argument("--vslm-execute-repeats", type=int, default=1, help="Execute repeats for vSLM")
    parser.add_argument("--runs", type=int, default=3, help="Repetitions per mode")
    parser.add_argument("--skip-execute", action="store_true", help="Run copy-only comparison")

    parser.add_argument("--rpc-script", default=None, help="Optional override for scripts/rpc.py")
    parser.add_argument("--spdk-tgt", default=None, help="Optional override for spdk_tgt")
    parser.add_argument("--spdk-nvme-passthru", default=None, help="Optional override for spdk_nvme_passthru")
    parser.add_argument("--controller-name", default="Nvme0", help="SPDK NVMe controller alias")
    parser.add_argument("--vslm-backing-min-gb", type=int, default=4, help="Minimum required backing size in GiB")
    parser.add_argument("--dataset-nsid", type=int, default=1, help="Dataset namespace NSID")
    parser.add_argument("--slm-nsid", type=int, default=100, help="SLM namespace NSID")
    parser.add_argument("--cpcs-nsid", type=int, default=200, help="CPCS compute namespace NSID")
    parser.add_argument("--pslm-granularity", type=int, default=4, help="pSLM granularity in bytes")
    parser.add_argument("--nqn", default="nqn.2026-03.io.spdk:vslm-capacity-gap", help="NVMf subsystem NQN")
    parser.add_argument("--serial", default="VSLMCAPGAP01", help="NVMf subsystem serial")
    parser.add_argument("--max-namespaces", type=int, default=1024, help="Max namespaces for subsystem")
    parser.add_argument("--trtype", default="TCP", help="NVMf transport type")
    parser.add_argument("--traddr", default="127.0.0.1", help="NVMf target address")
    parser.add_argument("--trsvcid", default="4420", help="NVMf target service ID")
    parser.add_argument("--src-addr", default=None, help="Source address for initiator-side fabrics connection")
    parser.add_argument("--src-svcid", default=None, help="Source service id (port) for initiator-side fabrics connection")
    parser.add_argument("--hostnqn", default="nqn.2026-03.io.spdk:vslm-capacity-gap-host", help="Host NQN")
    parser.add_argument("--passthru-lcores", default="1", help="Lcores argument for spdk_nvme_passthru")
    parser.add_argument("--core-mask", default="0xFF", help="CPU core mask for spdk_tgt")
    parser.add_argument(
        "--compute-core-mask",
        default=None,
        help="CPU core mask dedicated to CPCS builtin compute work inside spdk_tgt",
    )
    parser.add_argument("--target-log", default="/var/log/spdk.log", help="spdk_tgt log path")
    parser.add_argument("--target-ssh-host", default=None, help="Remote target SSH host (enables split mode)")
    parser.add_argument("--target-ssh-user", default=None, help="Remote target SSH user")
    parser.add_argument("--target-ssh-port", type=int, default=22, help="Remote target SSH port")
    parser.add_argument(
        "--target-ssh-option",
        action="append",
        default=[],
        help="Additional -o options for SSH (repeatable)",
    )
    parser.add_argument("--target-use-sudo", action="store_true", help="Use sudo -n for remote target commands")
    parser.add_argument(
        "--target-repo-path",
        default=None,
        help="Remote SPDK repo path used to derive remote rpc.py/spdk_tgt paths",
    )
    parser.add_argument("--target-rpc-script", default=None, help="Remote rpc.py path override")
    parser.add_argument("--target-spdk-tgt", default=None, help="Remote spdk_tgt path override")
    parser.add_argument("--output-json", default=None, help="Write scenario summary JSON")
    return parser.parse_args(argv)


def format_ratio(numer: float, denom: float) -> str:
    if denom <= 0:
        return "n/a"
    return f"{(numer / denom):.2f}x"


def extract_mode_means(payload: Dict[str, Any], mode: str) -> Dict[str, float]:
    agg = payload["aggregate"][mode]
    copy_cmd = float(agg["copy_cmd_count"]["mean"])
    exec_cmd = float(agg["execute_cmd_count"]["mean"])
    end_to_end = float(agg["end_to_end_seconds"]["mean"])
    copy_sec = float(agg["copy_seconds"]["mean"])
    exec_total_sec = float(agg["execute_total_seconds"]["mean"])
    return {
        "copy_cmd_count": copy_cmd,
        "execute_cmd_count": exec_cmd,
        "total_control_cmd_count": copy_cmd + exec_cmd,
        "end_to_end_seconds": end_to_end,
        "copy_seconds": copy_sec,
        "execute_total_seconds": exec_total_sec,
    }


def run_capacity_gap(args: argparse.Namespace) -> Dict[str, Any]:

    dataset_mb = int(args.dataset_size_mb) if args.dataset_size_mb is not None else int(args.dataset_size_gb) * 1024
    if dataset_mb <= 0 or args.physical_slm_mb <= 0:
        raise ValueError("dataset size and physical-slm-mb must be > 0")
    if dataset_mb % args.physical_slm_mb != 0:
        raise ValueError(
            f"dataset-size-mb={dataset_mb} does not divide by physical-slm-mb={args.physical_slm_mb}"
        )

    small_chunk_count = dataset_mb // args.physical_slm_mb
    vslm_max_copy_mb = args.vslm_max_copy_mb
    if vslm_max_copy_mb is None:
        vslm_max_copy_mb = dataset_mb

    fullfit_pslm_mb = args.fullfit_pslm_mb if args.fullfit_pslm_mb is not None else dataset_mb
    if args.fullfit_pslm_enabled and fullfit_pslm_mb < dataset_mb:
        raise ValueError(
            f"fullfit-pslm-mb ({fullfit_pslm_mb}) must be >= dataset size in MiB ({dataset_mb})"
        )
    if args.fullfit_pslm_enabled:
        if args.fullfit_chunk_mb <= 0:
            raise ValueError("--fullfit-chunk-mb must be > 0")
        if args.fullfit_chunk_mb > dataset_mb:
            raise ValueError("--fullfit-chunk-mb cannot exceed dataset size in MiB")
        if dataset_mb % args.fullfit_chunk_mb != 0:
            raise ValueError("--fullfit-chunk-mb must evenly divide dataset size")

    print("\n=== Scenario: small-dram pSLM vs vSLM ===")
    small_payload = perf_compare.run_benchmark_from_parent_args(
        args,
        overrides={
            "chunk_size_mb": args.physical_slm_mb,
            "pslm_size_mb": args.physical_slm_mb,
            "sram_mb": args.sram_mb,
            "vslm_max_copy_mb": vslm_max_copy_mb,
            "nqn": f"{args.nqn}.small",
            "hostnqn": f"{args.hostnqn}.small",
            "serial": f"{args.serial}SM",
            "output_json": None,
        },
    )

    fullfit_payload: Optional[Dict[str, Any]] = None
    if args.fullfit_pslm_enabled:
        print("\n=== Scenario: full-fit pSLM baseline ===")
        fullfit_payload = perf_compare.run_benchmark_from_parent_args(
            args,
            overrides={
                "chunk_size_mb": args.fullfit_chunk_mb,
                "pslm_size_mb": fullfit_pslm_mb,
                "sram_mb": args.sram_mb,
                "vslm_max_copy_mb": vslm_max_copy_mb,
                "nqn": f"{args.nqn}.fullfit",
                "hostnqn": f"{args.hostnqn}.fullfit",
                "serial": f"{args.serial}FF",
                "output_json": None,
            },
        )

    small_pslm = extract_mode_means(small_payload, "pslm")
    small_vslm = extract_mode_means(small_payload, "vslm")

    fullfit_pslm: Optional[Dict[str, float]] = None
    if fullfit_payload is not None:
        fullfit_pslm = extract_mode_means(fullfit_payload, "pslm")

    summary: Dict[str, Any] = {
        "config": {
            "dataset_size_gb": float(dataset_mb) / 1024.0,
            "dataset_size_mb": dataset_mb,
            "small_physical_slm_mb": args.physical_slm_mb,
            "small_chunk_count": small_chunk_count,
            "sram_mb": args.sram_mb,
            "vslm_max_copy_mb": vslm_max_copy_mb,
            "fullfit_pslm_enabled": args.fullfit_pslm_enabled,
            "fullfit_pslm_mb": fullfit_pslm_mb if args.fullfit_pslm_enabled else None,
            "builtin_program": args.builtin_program,
            "builtin_exec_max_mb": args.builtin_exec_max_mb,
            "runs": args.runs,
        },
        "means": {
            "small_pslm": small_pslm,
            "small_vslm": small_vslm,
            "fullfit_pslm": fullfit_pslm,
        },
        "ratios": {
            "small_pslm_vs_small_vslm_total_cmd_reduction_x": (
                small_pslm["total_control_cmd_count"] / small_vslm["total_control_cmd_count"]
                if small_vslm["total_control_cmd_count"] > 0 else None
            ),
            "small_pslm_vs_small_vslm_end_to_end_penalty_x": (
                small_pslm["end_to_end_seconds"] / small_vslm["end_to_end_seconds"]
                if small_vslm["end_to_end_seconds"] > 0 else None
            ),
            "small_pslm_vs_fullfit_pslm_end_to_end_penalty_x": (
                (small_pslm["end_to_end_seconds"] / fullfit_pslm["end_to_end_seconds"])
                if (fullfit_pslm is not None and fullfit_pslm["end_to_end_seconds"] > 0) else None
            ),
            "small_vslm_vs_fullfit_pslm_end_to_end_ratio_x": (
                (small_vslm["end_to_end_seconds"] / fullfit_pslm["end_to_end_seconds"])
                if (fullfit_pslm is not None and fullfit_pslm["end_to_end_seconds"] > 0) else None
            ),
        },
        "raw": {
            "small": small_payload,
            "fullfit": fullfit_payload,
        },
    }

    print("\nCapacity-Gap Scenario Summary")
    print(f"  dataset: {dataset_mb} MiB ({float(dataset_mb) / 1024.0:.4f} GiB)")
    print(f"  small pSLM window: {args.physical_slm_mb} MiB")
    print(f"  expected small-pSLM loop cycles/run: {small_chunk_count}")
    print(
        f"  expected small-pSLM workflow: {small_chunk_count} x "
        "(COPY -> EXECUTE -> RETRIEVE(result))"
    )
    print("\n  [A] small pSLM")
    print(f"    COPY cmds/run: {small_pslm['copy_cmd_count']:.2f}")
    print(f"    EXEC cmds/run: {small_pslm['execute_cmd_count']:.2f}")
    print(f"    Total control cmds/run: {small_pslm['total_control_cmd_count']:.2f}")
    print(f"    End-to-end seconds: {small_pslm['end_to_end_seconds']:.3f}")

    print("\n  [B] vSLM (small SRAM)")
    print(f"    COPY cmds/run: {small_vslm['copy_cmd_count']:.2f}")
    print(f"    EXEC cmds/run: {small_vslm['execute_cmd_count']:.2f}")
    print(f"    Total control cmds/run: {small_vslm['total_control_cmd_count']:.2f}")
    print(f"    End-to-end seconds: {small_vslm['end_to_end_seconds']:.3f}")
    print(
        "    small pSLM vs vSLM (total control cmds): "
        f"{format_ratio(small_pslm['total_control_cmd_count'], small_vslm['total_control_cmd_count'])}"
    )
    print(
        "    small pSLM vs vSLM (end-to-end penalty): "
        f"{format_ratio(small_pslm['end_to_end_seconds'], small_vslm['end_to_end_seconds'])}"
    )

    if fullfit_pslm is not None:
        print(f"\n  [C] full-fit pSLM upper bound ({fullfit_pslm_mb} MiB)")
        print(f"    COPY cmds/run: {fullfit_pslm['copy_cmd_count']:.2f}")
        print(f"    EXEC cmds/run: {fullfit_pslm['execute_cmd_count']:.2f}")
        print(f"    Total control cmds/run: {fullfit_pslm['total_control_cmd_count']:.2f}")
        print(f"    End-to-end seconds: {fullfit_pslm['end_to_end_seconds']:.3f}")
        print(
            "    small pSLM vs full-fit pSLM (end-to-end penalty): "
            f"{format_ratio(small_pslm['end_to_end_seconds'], fullfit_pslm['end_to_end_seconds'])}"
        )
        print(
            "    vSLM (small SRAM) vs full-fit pSLM (end-to-end ratio): "
            f"{format_ratio(small_vslm['end_to_end_seconds'], fullfit_pslm['end_to_end_seconds'])}"
        )

    print(
        "\n  note: RETRIEVE is counted as execute-result completion parsing in this builtin workflow "
        "(no separate data-read command)."
    )

    if args.output_json:
        with open(args.output_json, "w", encoding="utf-8") as fh:
            json.dump(summary, fh, indent=2)
        print(f"\nWrote JSON summary: {args.output_json}")
    return summary


def run_capacity_gap_from_argv(argv: Sequence[str]) -> Dict[str, Any]:
    """Programmatic scenario entrypoint used by the unified orchestrator."""
    return run_capacity_gap(parse_args(list(argv)))


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    try:
        run_capacity_gap(args)
        return 0
    except Exception as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

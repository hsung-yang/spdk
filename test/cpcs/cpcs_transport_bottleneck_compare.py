#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

import argparse
import json
import sys
from typing import Any, Dict, List, Optional, Sequence

import cpcs_data_movement_compare as data_move


def parse_bandwidths(text: str) -> List[float]:
    values: List[float] = []
    for raw in text.split(","):
        value = float(raw.strip())
        if value <= 0:
            raise ValueError("transport bandwidth values must be > 0")
        values.append(value)
    if not values:
        raise ValueError("at least one transport bandwidth value is required")
    return values


def gbps_to_bytes_per_sec(gbps: float) -> float:
    return (gbps * 1_000_000_000.0) / 8.0


def format_ratio(numer: float, denom: float) -> str:
    if denom <= 0:
        return "n/a"
    return f"{(numer / denom):.2f}x"


def print_summary(payload: Dict[str, Any], bandwidths_gbps: List[float]) -> None:
    host = payload["host"]
    cpcs = payload["cpcs"]
    byte_reduction_pct = float(payload["byte_reduction_pct"])
    config = payload.get("config", {}) if isinstance(payload.get("config"), dict) else {}

    print("\nTransport-Bottleneck Summary")
    print(f"  backend: {cpcs['backend']}")
    print(f"  builtin: {config.get('builtin_program')}")
    print(f"  dataset_size_mb: {config.get('dataset_size_mb')}")
    print(f"  host_target_bytes (host path): {host['host_target_bytes']}")
    print(f"  host_target_bytes (CPCS path): {cpcs['host_target_bytes']}")
    print(f"  byte_reduction_pct: {byte_reduction_pct:.2f}%")

    print("\nBandwidth Projections")
    print("gbps  host_transport_s  cpcs_transport_s  transport_speedup_x")
    for gbps in bandwidths_gbps:
        bandwidth_bytes = gbps_to_bytes_per_sec(gbps)
        host_transport = host["host_target_bytes"] / bandwidth_bytes
        cpcs_transport = cpcs["host_target_bytes"] / bandwidth_bytes
        print(
            f"{gbps:>4.0f}  "
            f"{host_transport:>16.6f}  "
            f"{cpcs_transport:>16.6f}  "
            f"{format_ratio(host_transport, cpcs_transport):>19}"
        )


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Project the benefit of CPCS under transport bottlenecks by combining "
            "measured host-target byte counts with bandwidth-based transport models."
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

    parser.add_argument("--dataset-size-gb", type=int, default=1, help="Dataset size in GiB")
    parser.add_argument(
        "--dataset-size-mb",
        type=int,
        default=None,
        help="Optional dataset size in MiB for bounded smoke runs (overrides --dataset-size-gb when set)",
    )
    parser.add_argument(
        "--backend",
        choices=("pslm", "vslm"),
        default="pslm",
        help="CPCS memory backend used for the near-storage path",
    )
    parser.add_argument(
        "--builtin-program",
        choices=("sum64", "max64", "min64"),
        default="sum64",
        help="Builtin CPCS workload used as a stand-in for transport-reducing compute",
    )
    parser.add_argument(
        "--transport-bandwidth-gbps",
        default="1,10,25,100",
        help="Comma-separated transport bandwidth values in Gbps",
    )
    parser.add_argument(
        "--host-read-chunk-mb",
        type=float,
        default=32.0,
        help="Host baseline read chunk size in MiB (fractional values allowed)",
    )
    parser.add_argument("--builtin-exec-max-mb", type=int, default=256, help="Max bytes per builtin Execute command")

    parser.add_argument(
        "--pslm-size-mb",
        type=int,
        default=None,
        help="pSLM size in MiB for the near-storage path (default: dataset size in MiB)",
    )
    parser.add_argument("--pslm-granularity", type=int, default=4, help="pSLM granularity in bytes")
    parser.add_argument("--sram-mb", type=int, default=32, help="vSLM SRAM size in MiB")
    parser.add_argument("--vslm-backing-min-gb", type=int, default=4, help="Minimum required backing size in GiB")

    parser.add_argument("--rpc-script", default=None, help="Optional override for scripts/rpc.py")
    parser.add_argument("--spdk-tgt", default=None, help="Optional override for spdk_tgt")
    parser.add_argument("--spdk-nvme-passthru", default=None, help="Optional override for spdk_nvme_passthru")
    parser.add_argument("--controller-name", default="Nvme0", help="SPDK NVMe controller alias")
    parser.add_argument("--dataset-nsid", type=int, default=1, help="Dataset namespace NSID")
    parser.add_argument("--slm-nsid", type=int, default=100, help="SLM namespace NSID")
    parser.add_argument("--cpcs-nsid", type=int, default=200, help="CPCS compute namespace NSID")
    parser.add_argument("--nqn", default="nqn.2026-03.io.spdk:cpcs-transport-study", help="NVMf subsystem NQN")
    parser.add_argument("--serial", default="CPCSTRANSPRT01", help="NVMf subsystem serial")
    parser.add_argument("--max-namespaces", type=int, default=1024, help="Max namespaces for subsystem")
    parser.add_argument("--trtype", default="TCP", help="NVMf transport type")
    parser.add_argument("--traddr", default="127.0.0.1", help="NVMf target address")
    parser.add_argument("--trsvcid", default="4420", help="NVMf target service ID")
    parser.add_argument("--src-addr", default=None, help="Source address for initiator-side fabrics connection")
    parser.add_argument("--src-svcid", default=None, help="Source service id (port) for initiator-side fabrics connection")
    parser.add_argument("--hostnqn", default="nqn.2026-03.io.spdk:cpcs-transport-study-host", help="Host NQN")
    parser.add_argument("--passthru-lcores", default="1", help="Lcores argument for spdk_nvme_passthru")
    parser.add_argument("--core-mask", default="0x1", help="CPU core mask for spdk_tgt")
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
    parser.add_argument("--output-json", default=None, help="Write structured output JSON")
    return parser.parse_args(argv)


def run_transport_study(args: argparse.Namespace) -> Dict[str, Any]:
    bandwidths_gbps = parse_bandwidths(args.transport_bandwidth_gbps)

    dataset_mb = int(args.dataset_size_mb) if args.dataset_size_mb is not None else int(args.dataset_size_gb) * 1024
    if dataset_mb <= 0:
        raise ValueError("dataset size must be > 0 MiB")
    pslm_size_mb = args.pslm_size_mb if args.pslm_size_mb is not None else dataset_mb

    compare_payload = data_move.run_comparison_from_parent_args(
        args,
        overrides={
            "pslm_size_mb": pslm_size_mb,
            "output_json": None,
        },
    )

    host = compare_payload["host"]
    cpcs = compare_payload["cpcs"]
    projections: List[Dict[str, Any]] = []
    for gbps in bandwidths_gbps:
        bandwidth_bytes = gbps_to_bytes_per_sec(gbps)
        host_transport = host["host_target_bytes"] / bandwidth_bytes
        cpcs_transport = cpcs["host_target_bytes"] / bandwidth_bytes
        projections.append({
            "transport_bandwidth_gbps": gbps,
            "host_transport_seconds": host_transport,
            "cpcs_transport_seconds": cpcs_transport,
            "transport_speedup_x": (
                host_transport / cpcs_transport if cpcs_transport > 0 else None
            ),
        })

    payload = {
        "config": {
            "backend": args.backend,
            "builtin_program": args.builtin_program,
            "dataset_size_mb": dataset_mb,
            "dataset_size_gb": (float(dataset_mb) / 1024.0),
            "transport_bandwidth_gbps": bandwidths_gbps,
            "pslm_size_mb": pslm_size_mb,
            "sram_mb": args.sram_mb,
        },
        "compare": compare_payload,
        "projections": projections,
    }

    print_summary(compare_payload, bandwidths_gbps)
    if args.output_json:
        with open(args.output_json, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2)
        print(f"\nWrote JSON results: {args.output_json}")
    return payload


def run_transport_study_from_argv(argv: Sequence[str]) -> Dict[str, Any]:
    """Programmatic scenario entrypoint used by the unified orchestrator."""
    return run_transport_study(parse_args(list(argv)))


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    try:
        run_transport_study(args)
        return 0
    except Exception as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

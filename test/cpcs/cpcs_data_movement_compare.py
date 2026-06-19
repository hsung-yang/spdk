#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

import argparse
import json
import math
import struct
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

from vslm_pslm_perf_compare import (
    BUILTIN_PROGRAM_PIND,
    MAX_MRS_RANGE_LEN,
    MAX_BLOCKS_PER_DESC,
    RpcClient,
    SpdkTarget,
    add_ns,
    bdev_capacity_bytes,
    build_mrs_ranges,
    build_vslm_copy_groups,
    cleanup_common,
    create_mrs,
    get_bdev_info,
    issue_slm_copy,
    now,
    passthru_cmd_prefix,
    parse_result_hex,
    require_executable,
    require_file,
    resolve_target_runtime,
    run_builtin_execute,
    run_cmd,
    setup_common,
)


DEFAULT_RPC = str((Path(__file__).resolve().parents[2] / "scripts" / "rpc.py"))
DEFAULT_SPDK_TGT = str((Path(__file__).resolve().parents[2] / "build" / "bin" / "spdk_tgt"))
DEFAULT_SPDK_NVME_PASSTHRU = str(
    (Path(__file__).resolve().parents[2] / "build" / "bin" / "spdk_nvme_passthru")
)


def resolve_dataset_bytes(args: argparse.Namespace) -> int:
    if getattr(args, "dataset_size_mb", None) is not None:
        dataset_size_mb = int(getattr(args, "dataset_size_mb"))
        if dataset_size_mb <= 0:
            raise ValueError("--dataset-size-mb must be > 0")
        return dataset_size_mb * (1 << 20)
    return int(args.dataset_size_gb) * (1 << 30)


def reduce_host_values(program: str, aggregate: Optional[int], chunk_values: Sequence[int]) -> int:
    if not chunk_values:
        raise ValueError("host baseline reduction requires at least one value")

    if program == "sum64":
        value = int(sum(chunk_values))
        return value if aggregate is None else aggregate + value
    if program == "max64":
        value = int(max(chunk_values))
        return value if aggregate is None else max(aggregate, value)
    if program == "min64":
        value = int(min(chunk_values))
        return value if aggregate is None else min(aggregate, value)
    raise ValueError(f"unsupported builtin program: {program}")


def reduce_file_u64(path: Path, program: str) -> int:
    data = path.read_bytes()
    if len(data) % 8 != 0:
        raise ValueError(f"host baseline chunk length is not uint64-aligned: {len(data)}")

    values = [value[0] for value in struct.iter_unpack("<Q", data)]
    if not values:
        return 0
    if program == "sum64":
        return int(sum(values))
    if program == "max64":
        return int(max(values))
    if program == "min64":
        return int(min(values))
    raise ValueError(f"unsupported builtin program: {program}")


def issue_dataset_read(
        args: argparse.Namespace,
        nsid: int,
        slba: int,
        block_count: int,
        data_len: int,
        output_file: Path,
) -> None:
    cmd = passthru_cmd_prefix(args, admin=False) + [
        "--opcode", "0x02",
        "--nsid", str(nsid),
        "--cdw10", str(slba & 0xFFFFFFFF),
        "--cdw11", str((slba >> 32) & 0xFFFFFFFF),
        "--cdw12", str(block_count - 1),
        "--data-len", str(data_len),
        "--read",
        "--output-file", str(output_file),
    ]
    run_cmd(cmd, check=True, capture_output=True, log_cmd=False)


def run_host_baseline(
        args: argparse.Namespace,
        tmp_dir: Path,
        dataset_block_size: int,
        dataset_bytes: int,
) -> Dict[str, Any]:
    if dataset_bytes % 8 != 0:
        raise ValueError("dataset-size must be uint64-aligned for builtin reductions")

    host_chunk_bytes = int(float(args.host_read_chunk_mb) * float(1 << 20))
    if host_chunk_bytes <= 0:
        raise ValueError("--host-read-chunk-mb must be > 0")
    if host_chunk_bytes % dataset_block_size != 0:
        raise ValueError("host-read-chunk-mb must align to the dataset block size")

    host_chunk_blocks = host_chunk_bytes // dataset_block_size
    if host_chunk_blocks <= 0:
        raise ValueError("host-read chunk resolves to zero blocks")
    host_chunk_blocks = min(host_chunk_blocks, MAX_BLOCKS_PER_DESC)
    host_chunk_bytes = host_chunk_blocks * dataset_block_size

    total_blocks = dataset_bytes // dataset_block_size
    read_blocks = 0
    command_count = 0
    result: Optional[int] = None
    out_file = tmp_dir / "host_read.bin"

    start = now()
    while read_blocks < total_blocks:
        block_count = min(total_blocks - read_blocks, host_chunk_blocks)
        data_len = block_count * dataset_block_size
        issue_dataset_read(
            args=args,
            nsid=args.dataset_nsid,
            slba=read_blocks,
            block_count=block_count,
            data_len=data_len,
            output_file=out_file,
        )
        chunk_value = reduce_file_u64(out_file, args.builtin_program)
        result = reduce_host_values(args.builtin_program, result, [chunk_value])
        read_blocks += block_count
        command_count += 1

    elapsed = now() - start
    if result is None:
        raise RuntimeError("host baseline produced no result")

    return {
        "command_count": command_count,
        "host_target_bytes": dataset_bytes,
        "internal_stage_bytes": 0,
        "elapsed_seconds": elapsed,
        "throughput_gib_s": (dataset_bytes / float(1 << 30)) / elapsed if elapsed > 0 else 0.0,
        "result": result,
    }


def stage_dataset(
        args: argparse.Namespace,
        tmp_dir: Path,
        slm_nsid: int,
        dataset_block_size: int,
        copy_groups: Sequence[Dict[str, Any]],
        run_tag: str,
) -> Dict[str, Any]:
    descriptor_payload_bytes = 0
    stage_internal_bytes = 0
    command_count = 0
    stage_seconds = 0.0

    for group_index, group in enumerate(copy_groups):
        start = now()
        issue_slm_copy(
            args=args,
            tmp_dir=tmp_dir,
            slm_nsid=slm_nsid,
            dest_offset=int(group["dest_offset"]),
            source_nsid=args.dataset_nsid,
            ranges=group["ranges"],
            run_tag=run_tag,
            desc_tag=f"group{group_index}",
            dataset_block_size=dataset_block_size,
        )
        stage_seconds += now() - start
        command_count += 1
        descriptor_payload_bytes += len(group["ranges"]) * 32
        stage_internal_bytes += int(group["total_bytes"])

    return {
        "stage_seconds": stage_seconds,
        "copy_command_count": command_count,
        "copy_descriptor_bytes": descriptor_payload_bytes,
        "internal_stage_bytes": stage_internal_bytes,
    }


def run_cpcs_path(
        args: argparse.Namespace,
        rpc: RpcClient,
        tmp_dir: Path,
        dataset_block_size: int,
        dataset_bytes: int,
) -> Dict[str, Any]:
    dataset_blocks = dataset_bytes // dataset_block_size
    copy_groups = build_vslm_copy_groups(dataset_blocks, dataset_block_size, dataset_bytes)
    mrs_ranges = build_mrs_ranges(dataset_bytes, align_bytes=8)
    host_target_bytes = 0
    run_tag = args.backend

    if args.backend == "pslm":
        pslm_bytes = args.pslm_size_mb * (1 << 20)
        if dataset_bytes > pslm_bytes:
            raise ValueError(
                f"dataset-size ({dataset_bytes}) exceeds pSLM size ({pslm_bytes}); "
                "use a larger --pslm-size-mb or switch to --backend vslm"
            )
        bdev_name = "PSLM_DATA_MOVE"
        rpc.call(
            "bdev_slm_create",
            "--name", bdev_name,
            "--nsid", str(args.slm_nsid),
            "--size-mb", str(args.pslm_size_mb),
            "--granularity", str(args.pslm_granularity),
        )
    else:
        bdev_name = "VSLM_DATA_MOVE"
        # Replace in-place reset with fresh lifecycle for benchmarking stability.
        rpc.quiet("nvmf_subsystem_remove_ns", args.nqn, str(args.slm_nsid))
        rpc.quiet("bdev_vslm_delete", "--name", bdev_name)
        rpc.call(
            "bdev_vslm_create",
            "--name", bdev_name,
            "--base-bdev-name", args.backing_bdev,
            "--sram-size-mb", str(args.sram_mb),
            "--nsid", str(args.slm_nsid),
        )

    try:
        add_ns(args, rpc, args.nqn, bdev_name, args.slm_nsid)

        stage = stage_dataset(
            args=args,
            tmp_dir=tmp_dir,
            slm_nsid=args.slm_nsid,
            dataset_block_size=dataset_block_size,
            copy_groups=copy_groups,
            run_tag=run_tag,
        )
        host_target_bytes += int(stage["copy_descriptor_bytes"])

        rsid = create_mrs(
            args=args,
            tmp_dir=tmp_dir,
            slm_nsid=args.slm_nsid,
            ranges=mrs_ranges,
            run_tag=run_tag,
        )
        mrs_bytes = len(mrs_ranges) * 32
        host_target_bytes += mrs_bytes

        exec_info = run_builtin_execute(
            args=args,
            tmp_dir=tmp_dir,
            rsid=rsid,
            ranges=mrs_ranges,
            max_exec_bytes=args.builtin_exec_max_mb * (1 << 20),
            run_tag=run_tag,
        )
        execute_desc_bytes = int(exec_info["cmd_count"]) * 24
        result_bytes = int(exec_info["cmd_count"]) * 8
        host_target_bytes += execute_desc_bytes + result_bytes

        payload: Dict[str, Any] = {
            "backend": args.backend,
            "copy_command_count": int(stage["copy_command_count"]),
            "execute_command_count": int(exec_info["cmd_count"]),
            "command_count": int(stage["copy_command_count"]) + 1 + int(exec_info["cmd_count"]),
            "copy_descriptor_bytes": int(stage["copy_descriptor_bytes"]),
            "mrs_descriptor_bytes": mrs_bytes,
            "execute_descriptor_bytes": execute_desc_bytes,
            "execute_result_bytes": result_bytes,
            "host_target_bytes": host_target_bytes,
            "internal_stage_bytes": int(stage["internal_stage_bytes"]),
            "stage_seconds": float(stage["stage_seconds"]),
            "execute_seconds": float(exec_info["seconds"]),
            "elapsed_seconds": float(stage["stage_seconds"]) + float(exec_info["seconds"]),
            "result": int(exec_info["result"]),
        }

        if args.backend == "vslm":
            payload["vslm_stats"] = rpc.call_json(
                "bdev_vslm_get_stats",
                "--name", bdev_name,
                log_cmd=False,
            )
        return payload
    finally:
        if args.backend == "pslm":
            rpc.quiet("nvmf_subsystem_remove_ns", args.nqn, str(args.slm_nsid))
            rpc.quiet("bdev_slm_delete", "--name", bdev_name)
        else:
            rpc.quiet("nvmf_subsystem_remove_ns", args.nqn, str(args.slm_nsid))
            rpc.quiet("bdev_vslm_delete", "--name", bdev_name)


def print_summary(host: Dict[str, Any], cpcs: Dict[str, Any]) -> None:
    byte_reduction = 0.0
    if host["host_target_bytes"] > 0:
        byte_reduction = (
            (host["host_target_bytes"] - cpcs["host_target_bytes"]) /
            host["host_target_bytes"]
        ) * 100.0

    print("\nData Movement Summary")
    print("path    commands  host_target_bytes  internal_stage_bytes  elapsed_s  result")
    print(
        f"host    {host['command_count']:>8}  {host['host_target_bytes']:>17}  "
        f"{host['internal_stage_bytes']:>20}  {host['elapsed_seconds']:>9.3f}  {host['result']}"
    )
    print(
        f"{cpcs['backend']:<7}"
        f"{cpcs['command_count']:>8}  {cpcs['host_target_bytes']:>17}  "
        f"{cpcs['internal_stage_bytes']:>20}  {cpcs['elapsed_seconds']:>9.3f}  {cpcs['result']}"
    )
    print(f"\nHost-target byte reduction: {byte_reduction:.2f}%")


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare host-executed vs CPCS near-storage data movement for builtin workloads",
    )

    parser.add_argument("--rpc-script", default=DEFAULT_RPC, help="Path to scripts/rpc.py")
    parser.add_argument("--spdk-tgt", default=DEFAULT_SPDK_TGT, help="Path to spdk_tgt")
    parser.add_argument(
        "--spdk-nvme-passthru",
        default=DEFAULT_SPDK_NVME_PASSTHRU,
        help="Path to spdk_nvme_passthru",
    )
    parser.add_argument("--rpc-sock", default="/var/tmp/cpcs_data_movement.sock", help="RPC socket path")
    parser.add_argument("--target-log", default="/var/log/spdk.log", help="spdk_tgt log path")
    parser.add_argument("--core-mask", default="0x1", help="CPU core mask for spdk_tgt")
    parser.add_argument(
        "--compute-core-mask",
        default=None,
        help="CPU core mask dedicated to CPCS builtin compute work inside spdk_tgt",
    )
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

    parser.add_argument("--controller-name", default="Nvme0", help="SPDK NVMe controller alias")
    parser.add_argument("--pcie-bdf", required=True, help="PCIe BDF for bdev_nvme_attach_controller")
    parser.add_argument("--dataset-bdev", required=True, help="Dataset source bdev (for example Nvme0n1)")
    parser.add_argument("--backing-bdev", required=True, help="vSLM backing bdev (for example Nvme0n2)")
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
        help="CPCS memory backend to compare against host execution",
    )
    parser.add_argument(
        "--builtin-program",
        choices=sorted(BUILTIN_PROGRAM_PIND.keys()),
        default="sum64",
        help="Builtin CPCS program to execute for the comparison",
    )
    parser.add_argument(
        "--builtin-exec-max-mb",
        type=int,
        default=256,
        help="Max bytes per builtin Execute command",
    )
    parser.add_argument(
        "--host-read-chunk-mb",
        type=float,
        default=32.0,
        help="Host baseline read chunk size in MiB (fractional values allowed)",
    )

    parser.add_argument("--sram-mb", type=int, default=32, help="vSLM SRAM size in MiB")
    parser.add_argument("--pslm-size-mb", type=int, default=1024, help="pSLM size in MiB")
    parser.add_argument("--pslm-granularity", type=int, default=4, help="pSLM granularity in bytes")
    parser.add_argument("--vslm-backing-min-gb", type=int, default=4, help="Minimum required backing size in GiB")

    parser.add_argument("--dataset-nsid", type=int, default=1, help="Dataset namespace NSID")
    parser.add_argument("--slm-nsid", type=int, default=100, help="SLM namespace NSID")
    parser.add_argument("--cpcs-nsid", type=int, default=200, help="CPCS compute namespace NSID")

    parser.add_argument("--nqn", default="nqn.2026-03.io.spdk:cpcs-data-movement", help="NVMf subsystem NQN")
    parser.add_argument("--serial", default="CPCSDATAMOVE01", help="NVMf subsystem serial")
    parser.add_argument("--max-namespaces", type=int, default=1024, help="Max namespaces for subsystem")
    parser.add_argument("--trtype", default="TCP", help="NVMf transport type")
    parser.add_argument("--traddr", default="127.0.0.1", help="NVMf target address")
    parser.add_argument("--trsvcid", default="4420", help="NVMf target service ID")
    parser.add_argument("--src-addr", default=None, help="Source address for initiator-side fabrics connection")
    parser.add_argument("--src-svcid", default=None, help="Source service id (port) for initiator-side fabrics connection")
    parser.add_argument("--hostnqn", default="nqn.2026-03.io.spdk:cpcs-data-movement-host", help="Host NQN")
    parser.add_argument("--passthru-lcores", default="1", help="Lcores argument for spdk_nvme_passthru")

    parser.add_argument("--cpcs-max-activated", type=int, default=16, help="CPCS max activated programs")
    parser.add_argument("--cpcs-max-mrs", type=int, default=256, help="CPCS max MRS entries")
    parser.add_argument("--output-json", default=None, help="Write structured output JSON")
    return parser.parse_args(argv)


def _build_comparison_namespace_from_source_args(
        source_args: argparse.Namespace,
        *,
        overrides: Optional[Dict[str, Any]] = None,
) -> argparse.Namespace:
    """
    Build a complete comparison Namespace from a parent scenario Namespace.

    This enables non-CLI scenario composition without nested parse_args chains.
    """
    for required in ("pcie_bdf", "dataset_bdev", "backing_bdev"):
        value = getattr(source_args, required, None)
        if value is None or str(value) == "":
            raise ValueError(f"Missing required comparison field on source args: {required}")

    # Start from canonical defaults produced by the comparison parser.
    ns = parse_args(
        [
            "--pcie-bdf",
            str(getattr(source_args, "pcie_bdf")),
            "--dataset-bdev",
            str(getattr(source_args, "dataset_bdev")),
            "--backing-bdev",
            str(getattr(source_args, "backing_bdev")),
        ]
    )

    for key, value in vars(source_args).items():
        # Preserve this parser's defaults when a parent arg omits an optional value.
        if value is None:
            continue
        if hasattr(ns, key):
            setattr(ns, key, value)

    if overrides:
        for key, value in overrides.items():
            if hasattr(ns, key):
                setattr(ns, key, value)

    return ns


def run_comparison_from_parent_args(
        source_args: argparse.Namespace,
        *,
        overrides: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """
    Scenario composition helper for callers that already parsed their own args.
    """
    return run_comparison(
        _build_comparison_namespace_from_source_args(
            source_args,
            overrides=overrides,
        )
    )


def run_comparison(args: argparse.Namespace) -> Dict[str, Any]:
    args.execute_template = None
    args.skip_execute = False

    target_ssh, rpc_script, spdk_tgt = resolve_target_runtime(args)
    if target_ssh is None:
        require_file(rpc_script, "rpc.py")
        require_executable(spdk_tgt, "spdk_tgt")
    require_executable(args.spdk_nvme_passthru, "spdk_nvme_passthru")

    target = SpdkTarget(
        spdk_tgt,
        args.rpc_sock,
        args.target_log,
        args.core_mask,
        compute_core_mask=args.compute_core_mask,
        target_ssh=target_ssh,
    )
    rpc = RpcClient(rpc_script, args.rpc_sock, target_ssh=target_ssh)
    dataset_bytes = resolve_dataset_bytes(args)
    dataset_size_mib = int(dataset_bytes // (1 << 20))
    dataset_size_gib = float(dataset_bytes) / float(1 << 30)
    backing_min_bytes = args.vslm_backing_min_gb * (1 << 30)

    try:
        target.start()
        target.wait_for_rpc(rpc)

        max_mrs_range_len = MAX_MRS_RANGE_LEN - (MAX_MRS_RANGE_LEN % 8)
        required_ranges = int(math.ceil(dataset_bytes / float(max_mrs_range_len)))
        max_ranges_per_mrs = max(32, required_ranges + 4)
        if max_ranges_per_mrs > 255:
            raise ValueError(
                f"Required max_ranges_per_mrs exceeds uint8 limit: {max_ranges_per_mrs}. "
                "Reduce dataset-size."
            )

        setup_common(args, rpc, max_ranges_per_mrs)

        dataset_info = get_bdev_info(rpc, args.dataset_bdev)
        backing_info = get_bdev_info(rpc, args.backing_bdev)
        dataset_block_size = int(dataset_info["block_size"])
        dataset_capacity = bdev_capacity_bytes(dataset_info)
        backing_capacity = bdev_capacity_bytes(backing_info)

        if args.dataset_bdev == args.backing_bdev:
            raise ValueError("dataset-bdev and backing-bdev must be different bdevs")
        if dataset_bytes > dataset_capacity:
            raise ValueError(
                f"dataset-size ({dataset_bytes}) exceeds dataset-bdev capacity ({dataset_capacity})"
            )
        if dataset_bytes > backing_capacity:
            raise ValueError(
                f"dataset-size ({dataset_bytes}) exceeds backing-bdev capacity ({backing_capacity})"
            )
        if backing_min_bytes > backing_capacity:
            raise ValueError(
                f"backing-bdev capacity ({backing_capacity}) is less than vslm-backing-min-gb ({backing_min_bytes})"
            )
        if dataset_bytes % dataset_block_size != 0:
            raise ValueError("dataset-size must align to the dataset block size")
        if dataset_bytes % 8 != 0:
            raise ValueError("dataset-size must be uint64-aligned for builtin reductions")

        with tempfile.TemporaryDirectory(prefix="cpcs_data_move_") as tmp:
            tmp_dir = Path(tmp)
            host = run_host_baseline(
                args=args,
                tmp_dir=tmp_dir,
                dataset_block_size=dataset_block_size,
                dataset_bytes=dataset_bytes,
            )
            cpcs = run_cpcs_path(
                args=args,
                rpc=rpc,
                tmp_dir=tmp_dir,
                dataset_block_size=dataset_block_size,
                dataset_bytes=dataset_bytes,
            )

        if host["result"] != cpcs["result"]:
            raise RuntimeError(
                f"Host and CPCS results differ: host={host['result']} cpcs={cpcs['result']}"
            )

        payload = {
            "config": {
                "backend": args.backend,
                "builtin_program": args.builtin_program,
                "dataset_size_bytes": dataset_bytes,
                "dataset_size_mb": dataset_size_mib,
                "dataset_size_gb": dataset_size_gib,
                "dataset_bdev": args.dataset_bdev,
                "backing_bdev": args.backing_bdev,
            },
            "host": host,
            "cpcs": cpcs,
            "byte_reduction_pct": (
                ((host["host_target_bytes"] - cpcs["host_target_bytes"]) /
                 host["host_target_bytes"]) * 100.0
                if host["host_target_bytes"] > 0 else 0.0
            ),
        }

        print_summary(host, cpcs)
        if args.output_json:
            with open(args.output_json, "w", encoding="utf-8") as fh:
                json.dump(payload, fh, indent=2)
            print(f"\nWrote JSON results: {args.output_json}")
        return payload
    finally:
        try:
            cleanup_common(args, rpc)
        except Exception:
            pass
        target.stop()


def run_comparison_from_argv(argv: Sequence[str]) -> Dict[str, Any]:
    """Programmatic scenario entrypoint used by the unified orchestrator."""
    return run_comparison(parse_args(list(argv)))


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    try:
        run_comparison(args)
        return 0
    except Exception as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

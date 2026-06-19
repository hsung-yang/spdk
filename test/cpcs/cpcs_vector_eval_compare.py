#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import ipaddress
import json
import os
import re
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Sequence, Optional, Tuple

from vslm_pslm_perf_compare import RpcClient, SpdkTarget, resolve_target_runtime


SLM_COPY_DESC_FMT_2H = 0x2
SLM_COPY_DESC_FMT_SHIFT = 8
SLM_COPY_CDW12_ONE_DESC = (SLM_COPY_DESC_FMT_2H << SLM_COPY_DESC_FMT_SHIFT)

RESULT_HDR_STRUCT = struct.Struct("<IHHIIII")
RESULT_STATS_STRUCT = struct.Struct("<10Q")
FILTER_AGG_TAIL_STRUCT = struct.Struct("<BBHIQd")
NVME_MAX_BLOCKS_PER_IO = 1 << 16
AUTO_PREFILL_CHUNK_BYTES = 64 * 1024 * 1024
TCP_DEFAULT_MAX_IO_SIZE = 128 * 1024


def run_cmd(
    cmd: Sequence[str],
    check: bool = True,
    capture_output: bool = True,
    timeout_sec: Optional[float] = None,
) -> str:
    try:
        cp = subprocess.run(list(cmd), text=True, capture_output=capture_output, timeout=timeout_sec)
    except subprocess.TimeoutExpired as exc:
        timeout_label = f"{timeout_sec:.1f}s" if timeout_sec is not None else "unknown"
        raise RuntimeError(f"Command timed out after {timeout_label}: {' '.join(cmd)}") from exc
    if check and cp.returncode != 0:
        stdout = cp.stdout if cp.stdout is not None else "<not captured>"
        stderr = cp.stderr if cp.stderr is not None else "<not captured>"
        raise RuntimeError(
            f"Command failed rc={cp.returncode}\ncmd={' '.join(cmd)}\nstdout={stdout}\nstderr={stderr}"
        )
    return (cp.stdout or "") + (cp.stderr or "")


def parse_result_hex(text: str) -> int:
    m = re.search(r"result[:=]\s*0x([0-9a-fA-F]+)", text)
    if not m:
        return 0
    return int(m.group(1), 16)


def _aligned_floor(value: int, align: int) -> int:
    return (value // align) * align


def _aligned_ceil(value: int, align: int) -> int:
    if value <= 0:
        return 0
    return ((value + align - 1) // align) * align


def _resolve_prefill_chunk(
    requested_bytes: int,
    lba_size: int,
    max_io_size_bytes: int = 0,
) -> int:
    if lba_size <= 0:
        raise ValueError(f"invalid LBA size: {lba_size}")
    max_bytes = NVME_MAX_BLOCKS_PER_IO * lba_size
    if max_io_size_bytes > 0:
        max_bytes = min(max_bytes, max_io_size_bytes)
    target = requested_bytes if requested_bytes > 0 else min(AUTO_PREFILL_CHUNK_BYTES, max_bytes)
    target = min(target, max_bytes)
    target = _aligned_floor(target, lba_size)
    if target <= 0:
        target = lba_size
    return target


def _to_pos_int(value: Any) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return 0
    return parsed if parsed > 0 else 0


def detect_dataset_write_max_io_size(args: argparse.Namespace, rpc: RpcClient) -> int:
    override = _to_pos_int(getattr(args, "dataset_write_max_io_size", 0))
    if override > 0:
        print(f"[prefill] dataset write max I/O override: {override} bytes", flush=True)
        return override

    trtype = str(getattr(args, "trtype", "")).upper()
    try:
        cp = rpc.call(
            "nvmf_get_transports",
            "--trtype",
            args.trtype,
            capture_output=True,
            log_cmd=False,
        )
        transports = json.loads(cp.stdout or "[]")
        if isinstance(transports, list):
            for entry in transports:
                if str(entry.get("trtype", "")).upper() != trtype:
                    continue
                max_io_size = _to_pos_int(entry.get("max_io_size"))
                io_unit_size = _to_pos_int(entry.get("io_unit_size"))
                shared_buffers = _to_pos_int(entry.get("num_shared_buffers"))
                pooled_cap = io_unit_size * shared_buffers if io_unit_size and shared_buffers else 0

                effective = max_io_size
                if pooled_cap > 0:
                    effective = min(effective, pooled_cap) if effective > 0 else pooled_cap

                if effective > 0:
                    print(
                        "[prefill] dataset write max I/O auto-detected: "
                        f"{effective} bytes (max_io_size={max_io_size}, "
                        f"io_unit_size={io_unit_size}, num_shared_buffers={shared_buffers})",
                        flush=True,
                    )
                    return effective
    except Exception as exc:
        print(f"[prefill] warning: failed to query nvmf transport limits: {exc}", flush=True)

    if trtype == "TCP":
        print(
            f"[prefill] dataset write max I/O fallback for TCP: {TCP_DEFAULT_MAX_IO_SIZE} bytes",
            flush=True,
        )
        return TCP_DEFAULT_MAX_IO_SIZE
    return 0


def transport_exists(rpc: RpcClient, trtype: str) -> bool:
    try:
        transports = rpc.call_json("nvmf_get_transports", log_cmd=False)
    except Exception:
        return False
    for entry in transports:
        if str(entry.get("trtype", "")).upper() == trtype.upper():
            return True
    return False


def ensure_transport_ready(args: argparse.Namespace, rpc: RpcClient) -> None:
    if transport_exists(rpc, args.trtype):
        return
    cp = rpc.call("nvmf_create_transport", "-t", args.trtype, check=False, capture_output=True)
    if cp.returncode != 0:
        raise RuntimeError(
            "Failed to create NVMf transport "
            f"{args.trtype!r}. stdout:\n{cp.stdout or ''}\nstderr:\n{cp.stderr or ''}\n"
            "If using RDMA, verify SPDK was built with RDMA and rdma-core/devices are available on target."
        )
    if not transport_exists(rpc, args.trtype):
        raise RuntimeError(
            f"NVMf transport {args.trtype!r} is not available after create attempt. "
            "Check target transport support and driver setup."
        )


def infer_listener_adrfam(traddr: str) -> Optional[str]:
    addr = str(traddr or "").strip()
    if not addr:
        return None
    # IPv6 link-local values may include "%<ifname>" zone suffix.
    if "%" in addr:
        addr = addr.split("%", 1)[0]
    try:
        ip = ipaddress.ip_address(addr)
    except ValueError:
        return None
    return "ipv6" if ip.version == 6 else "ipv4"


def _progress_stride(total: int, base_chunk: int) -> int:
    if total <= 0:
        return base_chunk
    slices = 20
    return max(base_chunk, ((total // slices) // base_chunk) * base_chunk or base_chunk)


def _parse_lcore_tokens(spec: str) -> List[str]:
    text = spec.strip()
    if not text:
        raise ValueError("--passthru-lcores cannot be empty")
    if text.startswith("[") and text.endswith("]"):
        text = text[1:-1].strip()
    if not text:
        raise ValueError("--passthru-lcores cannot be empty")

    tokens: List[str] = []
    for raw in text.split(","):
        part = raw.strip()
        if not part:
            continue
        if "-" in part:
            lo_s, hi_s = [x.strip() for x in part.split("-", 1)]
            if lo_s.isdigit() and hi_s.isdigit():
                lo = int(lo_s)
                hi = int(hi_s)
                if lo > hi:
                    raise ValueError(f"invalid lcore range: {part}")
                tokens.extend([str(v) for v in range(lo, hi + 1)])
                continue
        if part.isdigit():
            tokens.append(part)
            continue
        return [spec.strip()]

    if not tokens:
        raise ValueError("--passthru-lcores cannot be empty")
    return tokens


def _resolve_prefill_worker_lcores(args: argparse.Namespace) -> List[str]:
    lcore_tokens = _parse_lcore_tokens(args.passthru_lcores)
    req = int(args.prefill_workers)
    if req < 0:
        raise ValueError("--prefill-workers must be >= 0")
    workers = req if req > 0 else len(lcore_tokens)
    workers = max(1, workers)
    return [lcore_tokens[i % len(lcore_tokens)] for i in range(workers)]


def _split_aligned_ranges(total: int, align: int, parts: int) -> List[Tuple[int, int]]:
    if total <= 0:
        return []
    parts = max(1, parts)
    part_size = _aligned_ceil((total + parts - 1) // parts, align)
    ranges: List[Tuple[int, int]] = []
    start = 0
    for _ in range(parts):
        if start >= total:
            break
        end = min(total, start + part_size)
        ranges.append((start, end))
        start = end
    return ranges


def _make_progress_reporter(label: str, total: int, base_chunk: int) -> Callable[[int], None]:
    lock = threading.Lock()
    progress_stride = _progress_stride(total, base_chunk)
    next_report = progress_stride
    completed = 0

    def _update(delta: int) -> None:
        nonlocal completed, next_report
        if delta <= 0:
            return
        with lock:
            completed += delta
            if completed >= next_report or completed == total:
                pct = (100.0 * completed) / total if total > 0 else 100.0
                print(f"[prefill] {label}: {completed}/{total} bytes ({pct:.1f}%)", flush=True)
                while next_report <= completed:
                    next_report += progress_stride

    return _update


def passthru_prefix(args: argparse.Namespace, admin: bool, lcores: Optional[str] = None) -> List[str]:
    mode = "--admin-cmd" if admin else "--io-cmd"
    selected_lcores = lcores if lcores is not None else args.passthru_lcores
    cmd = [
        args.spdk_nvme_passthru,
        "--lcores", selected_lcores,
        "--disable-cpumask-locks",
        "--no-rpc-server",
        mode,
        "--trtype", args.trtype,
        "--traddr", args.traddr,
        "--trsvcid", args.trsvcid,
        "--subnqn", args.nqn,
        "--hostnqn", args.hostnqn,
    ]
    if args.src_addr:
        cmd.extend(["--src-addr", args.src_addr])
    if args.src_svcid:
        cmd.extend(["--src-svcid", args.src_svcid])
    return cmd


def setup_subsystem(args: argparse.Namespace, rpc: RpcClient) -> None:
    ensure_transport_ready(args, rpc)
    rpc.call("nvmf_create_subsystem", args.nqn, "-s", args.serial, "-a", "-m", str(args.max_namespaces))

    if getattr(args, "pcie_bdf", None) and not getattr(args, "skip_nvme_attach", False):
        rpc.call("bdev_nvme_attach_controller", "-b", getattr(args, "controller_name", "Nvme0"), "-t", "pcie", "-a", args.pcie_bdf)

    if getattr(args, "dataset_size_mb", 0) > 0:
        rpc.call("bdev_malloc_create", "-b", args.dataset_bdev, str(args.dataset_size_mb), str(args.dataset_block_size))

    backend = getattr(args, "backend", "pslm")
    if backend == "vslm":
        if not getattr(args, "backing_bdev", None):
            raise ValueError("--backing-bdev must be specified when using vslm backend")
        cmd = [
            "bdev_vslm_create",
            "--name", args.slm_name,
            "--base-bdev-name", args.backing_bdev,
            "--sram-size-mb", str(args.slm_size_mb),
            "--nsid", str(args.slm_nsid),
        ]
        rpc.call(*cmd)
    else:
        rpc.call("bdev_slm_create", "--name", args.slm_name, "--nsid", str(args.slm_nsid),
                 "--size-mb", str(args.slm_size_mb), "--granularity", "4")

    rpc.call("nvmf_subsystem_add_ns", args.nqn, args.dataset_bdev, "-n", str(args.dataset_nsid))
    rpc.call("nvmf_subsystem_add_ns", args.nqn, args.slm_name, "-n", str(args.slm_nsid))
    rpc.call("nvmf_subsystem_add_host", args.nqn, args.hostnqn)
    listener_cmd = [
        "nvmf_subsystem_add_listener",
        args.nqn,
        "-t",
        args.trtype,
        "-a",
        args.traddr,
        "-s",
        args.trsvcid,
    ]
    adrfam = infer_listener_adrfam(args.traddr)
    if adrfam:
        listener_cmd.extend(["-f", adrfam])
    rpc.call(*listener_cmd)
    rpc.call(
        "cpcs_ns_create",
        "--subsystem-nqn", args.nqn,
        "--nsid", str(args.cpcs_nsid),
        "--max-activated", str(args.cpcs_max_activated),
        "--max-mrs", str(args.cpcs_max_mrs),
        "--max-ranges-per-mrs", str(args.cpcs_max_ranges_per_mrs),
    )
    print("[setup] installing CPCS built-in programs", flush=True)
    rpc.call("cpcs_program_install_builtins", "--subsystem-nqn", args.nqn, "--nsid", str(args.cpcs_nsid))
    print("[setup] CPCS built-in programs installed", flush=True)


def generate_dataset(args: argparse.Namespace, out_dir: Path) -> Dict[str, Any]:
    dataset_dir = out_dir / "dataset"
    run_cmd(
        [
            "python3",
            "test/cpcs/cpcs_vector_dataset_gen.py",
            "--profile",
            args.profile,
            "--tier",
            args.tier,
            "--count",
            str(args.count),
            "--dim",
            str(args.dim),
            "--query-count",
            str(args.query_count),
            "--seed",
            str(args.seed),
            "--output-dir",
            str(dataset_dir),
        ],
        capture_output=False,
    )
    with (dataset_dir / "manifest.json").open("r", encoding="utf-8") as fh:
        return json.load(fh)


def write_dataset_namespace(
    args: argparse.Namespace,
    nsid: int,
    src_path: Path,
    offset: int,
    lba_size: int,
    chunk_bytes: int,
) -> Dict[str, Any]:
    raw_size = src_path.stat().st_size
    padded = ((raw_size + lba_size - 1) // lba_size) * lba_size
    if offset % lba_size != 0:
        raise ValueError(f"offset {offset} must align to LBA size {lba_size}")
    chunk_goal = _resolve_prefill_chunk(
        chunk_bytes,
        lba_size,
        _to_pos_int(getattr(args, "dataset_write_effective_max_io_size", 0)),
    )
    worker_lcores = _resolve_prefill_worker_lcores(args)
    ranges = _split_aligned_ranges(padded, lba_size, len(worker_lcores))
    if not ranges:
        return {"raw_bytes": raw_size, "staged_bytes": padded, "commands": 0, "workers": 0}
    progress_update = _make_progress_reporter("dataset writes", padded, chunk_goal)
    print(
        f"[prefill] dataset writers: {len(ranges)} workers, lcores={','.join(worker_lcores[:len(ranges)])}",
        flush=True,
    )

    def _write_range(start: int, end: int, lcore: str) -> int:
        prefix = passthru_prefix(args, admin=False, lcores=lcore)
        cmd_count_local = 0
        cur = start
        current_chunk = chunk_goal
        with src_path.open("rb") as src_fh:
            with tempfile.NamedTemporaryFile(prefix="vec_ds_write_", suffix=".bin", delete=False) as tf:
                tmp = tf.name
            try:
                while cur < end:
                    length = min(current_chunk, end - cur)
                    while True:
                        src_fh.seek(cur)
                        available = max(0, raw_size - cur)
                        read_len = min(length, available)
                        chunk = src_fh.read(read_len) if read_len > 0 else b""
                        if len(chunk) < length:
                            chunk += b"\x00" * (length - len(chunk))
                        Path(tmp).write_bytes(chunk)

                        slba = (offset + cur) // lba_size
                        nlb = (length // lba_size) - 1
                        try:
                            run_cmd(
                                prefix
                                + [
                                    "--opcode",
                                    "0x01",
                                    "--nsid",
                                    str(nsid),
                                    "--cdw10",
                                    str(slba & 0xFFFFFFFF),
                                    "--cdw11",
                                    str((slba >> 32) & 0xFFFFFFFF),
                                    "--cdw12",
                                    str(nlb),
                                    "--data-len",
                                    str(length),
                                    "--write",
                                    "--input-file",
                                    tmp,
                                ],
                                timeout_sec=(
                                    args.prefill_cmd_timeout_sec
                                    if args.prefill_cmd_timeout_sec > 0
                                    else None
                                ),
                            )
                            break
                        except Exception:
                            if length <= lba_size:
                                raise
                            next_len = _aligned_floor(max(lba_size, length // 2), lba_size)
                            if next_len <= 0:
                                next_len = lba_size
                            print(
                                f"[prefill] write fallback: offset={cur} length={length} -> {next_len}",
                                flush=True,
                            )
                            length = next_len
                            current_chunk = next_len

                    cur += length
                    cmd_count_local += 1
                    progress_update(length)
            finally:
                Path(tmp).unlink(missing_ok=True)
        return cmd_count_local

    cmd_count = 0
    if len(ranges) == 1:
        start, end = ranges[0]
        cmd_count = _write_range(start, end, worker_lcores[0])
    else:
        with ThreadPoolExecutor(max_workers=len(ranges)) as pool:
            futures = [
                pool.submit(_write_range, start, end, worker_lcores[idx])
                for idx, (start, end) in enumerate(ranges)
            ]
            for fut in futures:
                cmd_count += fut.result()

    return {
        "raw_bytes": raw_size,
        "staged_bytes": padded,
        "commands": cmd_count,
        "workers": len(ranges),
    }


def issue_slm_copy(
    args: argparse.Namespace,
    source_nsid: int,
    source_slba: int,
    nlb: int,
    dest_offset: int,
    byte_len: int,
    lcores: Optional[str] = None,
) -> None:
    desc = struct.pack("<IIQHHIIHH", source_nsid, 0, source_slba, nlb, 0, 0, 0, 0, 0)
    with tempfile.NamedTemporaryFile(prefix="vec_slm_copy_", suffix=".bin", delete=False) as tf:
        tf.write(desc)
        tf.flush()
        path = tf.name
    try:
        run_cmd(
            passthru_prefix(args, admin=False, lcores=lcores)
            + [
                "--opcode",
                "0x01",
                "--nsid",
                str(args.slm_nsid),
                "--cdw2",
                str(byte_len & 0xFFFFFFFF),
                "--cdw3",
                str((byte_len >> 32) & 0xFFFFFFFF),
                "--cdw10",
                str(dest_offset & 0xFFFFFFFF),
                "--cdw11",
                str((dest_offset >> 32) & 0xFFFFFFFF),
                "--cdw12",
                hex(SLM_COPY_CDW12_ONE_DESC),
                "--data-len",
                "32",
                "--write",
                "--input-file",
                path,
            ],
            timeout_sec=(
                args.prefill_cmd_timeout_sec if args.prefill_cmd_timeout_sec > 0 else None
            ),
        )
    finally:
        Path(path).unlink(missing_ok=True)


def stage_into_slm(
    args: argparse.Namespace,
    meta_stage: Dict[str, Any],
    vec_stage: Dict[str, Any],
) -> None:
    ds_lba = args.dataset_block_size
    chunk_goal = _resolve_prefill_chunk(args.copy_chunk_bytes, ds_lba)
    worker_lcores = _resolve_prefill_worker_lcores(args)

    def _copy_range(label: str, total: int, src_base_slba: int, dst_offset: int) -> None:
        ranges = _split_aligned_ranges(total, ds_lba, len(worker_lcores))
        if not ranges:
            return
        progress_update = _make_progress_reporter(f"SLM copy ({label})", total, chunk_goal)
        print(
            f"[prefill] SLM copy workers ({label}): {len(ranges)} workers, lcores={','.join(worker_lcores[:len(ranges)])}",
            flush=True,
        )

        def _copy_worker(start: int, end: int, lcore: str) -> None:
            copied = start
            chunk = chunk_goal
            while copied < end:
                length = min(chunk, end - copied)
                while True:
                    src_slba = src_base_slba + (copied // ds_lba)
                    nlb = (length // ds_lba) - 1
                    try:
                        issue_slm_copy(
                            args,
                            args.dataset_nsid,
                            src_slba,
                            nlb,
                            dst_offset + copied,
                            length,
                            lcores=lcore,
                        )
                        break
                    except Exception:
                        if length <= ds_lba:
                            raise
                        next_len = _aligned_floor(max(ds_lba, length // 2), ds_lba)
                        if next_len <= 0:
                            next_len = ds_lba
                        print(
                            f"[prefill] copy fallback: offset={copied} length={length} -> {next_len}",
                            flush=True,
                        )
                        length = next_len
                        chunk = next_len
                copied += length
                progress_update(length)

        if len(ranges) == 1:
            start, end = ranges[0]
            _copy_worker(start, end, worker_lcores[0])
            return

        with ThreadPoolExecutor(max_workers=len(ranges)) as pool:
            futures = [
                pool.submit(_copy_worker, start, end, worker_lcores[idx])
                for idx, (start, end) in enumerate(ranges)
            ]
            for fut in futures:
                fut.result()

    # Meta copy
    meta_total = int(meta_stage["staged_bytes"])
    meta_src_base = args.metadata_offset // ds_lba
    _copy_range("meta", meta_total, meta_src_base, args.metadata_offset)

    # Vector copy
    vec_total = int(vec_stage["staged_bytes"])
    vec_src_base = args.vector_offset // ds_lba
    _copy_range("vector", vec_total, vec_src_base, args.vector_offset)


def create_mrs(args: argparse.Namespace, out_dir: Path) -> int:
    mrs_len = args.output_offset + args.output_length
    mrs_path = out_dir / "mrs.bin"
    mrs_path.write_bytes(struct.pack("<IIQ16s", args.slm_nsid, mrs_len, 0, b"\x00" * 16))
    out = run_cmd(
        passthru_prefix(args, admin=True)
        + [
            "--opcode",
            "0x89",
            "--nsid",
            str(args.cpcs_nsid),
            "--cdw10",
            "0",
            "--cdw11",
            "1",
            "--data-len",
            "32",
            "--write",
            "--input-file",
            str(mrs_path),
        ]
    )
    (out_dir / "mrs_create.out").write_text(out, encoding="utf-8")
    rsid = parse_result_hex(out)
    if rsid <= 0:
        raise RuntimeError(f"failed to parse RSID from MRS output: {out}")
    return rsid


def run_host_baselines(args: argparse.Namespace, manifest_path: Path, out_dir: Path) -> Dict[str, Path]:
    k1 = out_dir / "k1_h0.json"
    k2 = out_dir / "k2_h0.json"
    run_cmd(
        [
            "python3",
            "test/cpcs/cpcs_vector_host_baseline.py",
            "--manifest",
            str(manifest_path),
            "--kernel",
            "K1",
            "--query-limit",
            str(args.query_limit),
            "--output-json",
            str(k1),
        ]
    )
    run_cmd(
        [
            "python3",
            "test/cpcs/cpcs_vector_host_baseline.py",
            "--manifest",
            str(manifest_path),
            "--kernel",
            "K2",
            "--query-limit",
            str(args.query_limit),
            "--output-json",
            str(k2),
        ]
    )
    return {"k1_h0": k1, "k2_h0": k2}


def run_device_k2(args: argparse.Namespace, manifest_path: Path, rsid: int, out_dir: Path) -> Path:
    k2 = out_dir / "k2_c0.json"
    cmd = [
        "python3",
        "test/cpcs/cpcs_vector_device_eval.py",
        "--manifest",
        str(manifest_path),
        "--query-limit",
        str(args.query_limit),
        "--spdk-nvme-passthru",
        args.spdk_nvme_passthru,
        "--trtype",
        args.trtype,
        "--traddr",
        args.traddr,
        "--trsvcid",
        args.trsvcid,
        "--subnqn",
        args.nqn,
        "--hostnqn",
        args.hostnqn,
        "--passthru-lcores",
        args.passthru_lcores,
        "--cpcs-nsid",
        str(args.cpcs_nsid),
        "--slm-nsid",
        str(args.slm_nsid),
        "--rsid",
        str(rsid),
        "--metadata-offset",
        str(args.metadata_offset),
        "--vector-offset",
        str(args.vector_offset),
        "--output-offset",
        str(args.output_offset),
        "--output-length",
        str(args.output_length),
        "--lba-size",
        str(args.slm_lba_size),
        "--output-json",
        str(k2),
    ]
    if args.src_addr:
        cmd.extend(["--src-addr", args.src_addr])
    if args.src_svcid:
        cmd.extend(["--src-svcid", args.src_svcid])
    run_cmd(cmd)
    return k2


def read_slm_bytes(args: argparse.Namespace, offset: int, length: int) -> bytes:
    lba = args.slm_lba_size
    start = (offset // lba) * lba
    head = offset - start
    total = head + length
    padded = ((total + lba - 1) // lba) * lba
    with tempfile.NamedTemporaryFile(prefix="vec_read_", suffix=".bin", delete=False) as tf:
        out_path = tf.name
    run_cmd(
        passthru_prefix(args, admin=False)
        + [
            "--opcode",
            "0x02",
            "--nsid",
            str(args.slm_nsid),
            "--cdw10",
            str(start & 0xFFFFFFFF),
            "--cdw11",
            str((start >> 32) & 0xFFFFFFFF),
            "--cdw12",
            str(padded),
            "--data-len",
            str(padded),
            "--read",
            "--output-file",
            out_path,
        ]
    )
    raw = Path(out_path).read_bytes()
    Path(out_path).unlink(missing_ok=True)
    return raw[head: head + length]


def run_device_k1(args: argparse.Namespace, manifest_path: Path, rsid: int, out_dir: Path) -> Path:
    import sys

    sys.path.insert(0, str((Path(args.spdk_root) / "test/cpcs").resolve()))
    from cpcs_vector_common import AGG_COUNT, FIELD_CATEGORY_ID, FilterClause, encode_filter_agg_request, summarize_latency_us

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    query_manifest = json.loads(Path(manifest["paths"]["queries_json"]).read_text(encoding="utf-8"))
    queries = list(query_manifest["queries"])[: args.query_limit]

    rows: List[Dict[str, Any]] = []
    latencies_us: List[float] = []
    for q in queries:
        clauses = [
            FilterClause(
                op=int(c["op"]),
                field_id=int(c["field_id"]),
                args=tuple(int(x) for x in c.get("args", [])),
                in_count=int(c.get("in_count", 0)),
            )
            for c in q["filters"]
        ]
        req = encode_filter_agg_request(
            output_mr_id=1,
            output_offset=args.output_offset,
            output_length=128,
            metadata_mr_id=1,
            metadata_offset=args.metadata_offset,
            record_count=int(manifest["record_count"]),
            metadata_stride=int(manifest["metadata_stride"]),
            agg_op=AGG_COUNT,
            agg_field_id=FIELD_CATEGORY_ID,
            clauses=clauses,
        )

        with tempfile.NamedTemporaryFile(prefix="vec_k1_req_", suffix=".bin", delete=False) as tf:
            tf.write(req)
            tf.flush()
            req_path = tf.name

        t0 = time.perf_counter()
        out = run_cmd(
            passthru_prefix(args, admin=False)
            + [
                "--opcode",
                "0x01",
                "--nsid",
                str(args.cpcs_nsid),
                "--cdw2",
                str((rsid << 16) | 5),
                "--cdw3",
                "0",
                "--cdw4",
                str(len(req)),
                "--data-len",
                str(len(req)),
                "--write",
                "--input-file",
                req_path,
            ]
        )
        elapsed_us = (time.perf_counter() - t0) * 1_000_000.0
        Path(req_path).unlink(missing_ok=True)

        raw = read_slm_bytes(args, args.output_offset, 128)
        version, opcode, status, returned_count, matched_count, scored_count, _ = RESULT_HDR_STRUCT.unpack_from(raw, 0)
        stats = RESULT_STATS_STRUCT.unpack_from(raw, RESULT_HDR_STRUCT.size)
        agg_op, agg_field, _r0, _r1, agg_u64, agg_f64 = FILTER_AGG_TAIL_STRUCT.unpack_from(
            raw, RESULT_HDR_STRUCT.size + RESULT_STATS_STRUCT.size
        )
        rows.append(
            {
                "query_id": int(q["query_id"]),
                "outer_request_id": int(q.get("outer_request_id", q["query_id"])),
                "round": int(q.get("round", 0)),
                "returned_dw0": parse_result_hex(out),
                "elapsed_us": elapsed_us,
                "version": version,
                "opcode": opcode,
                "status": status,
                "returned_count": int(returned_count),
                "matched_count": int(matched_count),
                "scored_count": int(scored_count),
                "aggregate_count": int(agg_u64),
                "aggregate_f64": float(agg_f64),
                "aggregate_op": int(agg_op),
                "aggregate_field_id": int(agg_field),
                "stats": {
                    "host_req_bytes": stats[0],
                    "host_resp_bytes": stats[1],
                    "media_read_bytes": stats[2],
                    "media_write_bytes": stats[3],
                    "vslm_fault_read_bytes": stats[4],
                    "vslm_fault_write_bytes": stats[5],
                    "metadata_records_scanned": stats[6],
                    "vectors_scored": stats[7],
                    "command_count": stats[8],
                    "latency_us": stats[9],
                },
            }
        )
        latencies_us.append(elapsed_us)

    summary = {
        "query_count": len(rows),
        "latency_us": summarize_latency_us(latencies_us),
        "avg_host_req_bytes": sum(r["stats"]["host_req_bytes"] for r in rows) / max(1, len(rows)),
        "avg_host_resp_bytes": sum(r["stats"]["host_resp_bytes"] for r in rows) / max(1, len(rows)),
        "avg_host_total_bytes": sum((r["stats"]["host_req_bytes"] + r["stats"]["host_resp_bytes"]) for r in rows)
        / max(1, len(rows)),
        "avg_media_read_bytes": sum(r["stats"]["media_read_bytes"] for r in rows) / max(1, len(rows)),
        "avg_media_write_bytes": sum(r["stats"]["media_write_bytes"] for r in rows) / max(1, len(rows)),
        "avg_vslm_fault_read_bytes": sum(r["stats"]["vslm_fault_read_bytes"] for r in rows) / max(1, len(rows)),
        "avg_vslm_fault_write_bytes": sum(r["stats"]["vslm_fault_write_bytes"] for r in rows) / max(1, len(rows)),
        "avg_records_matched": sum(r["matched_count"] for r in rows) / max(1, len(rows)),
        "avg_vectors_scored": sum(r["scored_count"] for r in rows) / max(1, len(rows)),
        "avg_returned_count": sum(r["returned_count"] for r in rows) / max(1, len(rows)),
        "avg_aggregate_count": sum(r["aggregate_count"] for r in rows) / max(1, len(rows)),
    }
    payload = {"summary": summary, "per_query": rows}
    out_path = out_dir / "k1_c0.json"
    out_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return out_path


def _normalize_topk_records(records: Sequence[Dict[str, Any]]) -> Dict[int, float]:
    out: Dict[int, float] = {}
    for rec in records:
        out[int(rec["doc_id"])] = float(rec["score"])
    return out


def compare_exactness(
    k1_h0: Path,
    k1_c0: Path,
    k2_h0: Path,
    k2_c0: Path,
    out_dir: Path,
    *,
    score_tol: float = 1e-4,
    ignore_order: bool = True,
) -> Path:
    h1 = json.loads(k1_h0.read_text(encoding="utf-8"))
    c1 = json.loads(k1_c0.read_text(encoding="utf-8"))
    h2 = json.loads(k2_h0.read_text(encoding="utf-8"))
    c2 = json.loads(k2_c0.read_text(encoding="utf-8"))

    by_q_h1 = {int(r["query_id"]): r for r in h1["per_query"]}
    by_q_c1 = {int(r["query_id"]): r for r in c1["per_query"]}
    by_q_h2 = {int(r["query_id"]): r for r in h2["per_query"]}
    by_q_c2 = {int(r["query_id"]): r for r in c2["per_query"]}

    k1_mismatches: List[Dict[str, Any]] = []
    for qid, hrow in sorted(by_q_h1.items()):
        crow = by_q_c1.get(qid)
        if crow is None:
            k1_mismatches.append({"query_id": qid, "reason": "missing_device_row"})
            continue
        host_count = int(hrow["result"]["aggregate_count"])
        dev_count = int(crow["aggregate_count"])
        if host_count != dev_count:
            k1_mismatches.append({"query_id": qid, "host_count": host_count, "device_count": dev_count})

    k2_mismatches: List[Dict[str, Any]] = []
    for qid, hrow in sorted(by_q_h2.items()):
        crow = by_q_c2.get(qid)
        if crow is None:
            k2_mismatches.append({"query_id": qid, "reason": "missing_device_row"})
            continue
        hrec = list(hrow["result"]["topk"])
        crec = list(crow.get("records", []))
        if len(hrec) != len(crec):
            k2_mismatches.append({"query_id": qid, "reason": "record_count", "host": len(hrec), "device": len(crec)})
            continue
        if ignore_order:
            hmap = _normalize_topk_records(hrec)
            cmap = _normalize_topk_records(crec)
            if set(hmap.keys()) != set(cmap.keys()):
                host_only = sorted(set(hmap.keys()) - set(cmap.keys()))
                dev_only = sorted(set(cmap.keys()) - set(hmap.keys()))
                k2_mismatches.append(
                    {
                        "query_id": qid,
                        "reason": "doc_id_set",
                        "host_only_doc_ids": host_only[:8],
                        "device_only_doc_ids": dev_only[:8],
                        "host_only_count": len(host_only),
                        "device_only_count": len(dev_only),
                    }
                )
                continue
            score_bad = None
            for doc_id in sorted(hmap.keys()):
                hs = hmap[doc_id]
                cs = cmap[doc_id]
                if abs(hs - cs) > score_tol:
                    score_bad = (doc_id, hs, cs)
                    break
            if score_bad is not None:
                k2_mismatches.append(
                    {
                        "query_id": qid,
                        "reason": "score",
                        "doc_id": int(score_bad[0]),
                        "host_score": float(score_bad[1]),
                        "device_score": float(score_bad[2]),
                    }
                )
            continue

        for idx, (ha, ca) in enumerate(zip(hrec, crec)):
            if int(ha["doc_id"]) != int(ca["doc_id"]):
                k2_mismatches.append(
                    {
                        "query_id": qid,
                        "reason": "doc_id",
                        "index": idx,
                        "host_doc_id": int(ha["doc_id"]),
                        "device_doc_id": int(ca["doc_id"]),
                    }
                )
                break
            if abs(float(ha["score"]) - float(ca["score"])) > score_tol:
                k2_mismatches.append(
                    {
                        "query_id": qid,
                        "reason": "score",
                        "index": idx,
                        "host_score": float(ha["score"]),
                        "device_score": float(ca["score"]),
                    }
                )
                break

    payload = {
        "k1_query_count": len(by_q_h1),
        "k2_query_count": len(by_q_h2),
        "score_tolerance": score_tol,
        "ignore_order": ignore_order,
        "k1_mismatch_count": len(k1_mismatches),
        "k2_mismatch_count": len(k2_mismatches),
        "k1_mismatches": k1_mismatches,
        "k2_mismatches": k2_mismatches,
    }
    out_path = out_dir / "exactness.json"
    out_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    if k1_mismatches or k2_mismatches:
        raise RuntimeError(f"exactness mismatch detected: {out_path}")
    return out_path


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    default_root = str(Path(__file__).resolve().parents[2])
    default_passthru = str(Path(default_root) / "build/bin/spdk_nvme_passthru")
    p = argparse.ArgumentParser(description="Run tiny A1/A2 H0 vs C0 CPCS vector validation in VM")
    p.add_argument("--spdk-root", default=default_root)
    p.add_argument("--spdk-nvme-passthru", default=default_passthru, help="Path to spdk_nvme_passthru")
    p.add_argument("--rpc-sock", default="/var/tmp/spdk_vec.sock")
    p.add_argument("--rpc-timeout-sec", type=float, default=120.0, help="Timeout for each rpc.py invocation in seconds")
    p.add_argument("--target-core-mask", default="0x3")
    p.add_argument("--core-mask", default="0x3")
    p.add_argument(
        "--target-compute-core-mask",
        default=None,
        help="CPU core mask dedicated to CPCS builtin compute work inside target spdk_tgt",
    )
    p.add_argument("--target-log", default="/var/log/spdk.log")
    p.add_argument("--serial", default="VECTINY001")
    p.add_argument("--max-namespaces", type=int, default=1024)
    p.add_argument("--nqn", default="nqn.2026-04.io.spdk:cpcs-vec-tiny")
    p.add_argument("--hostnqn", default="nqn.2026-04.io.spdk:cpcs-vec-host")
    p.add_argument("--trtype", default="TCP")
    p.add_argument("--traddr", default="127.0.0.1")
    p.add_argument("--trsvcid", default="4420")
    p.add_argument("--src-addr", default=None, help="Source address for initiator-side fabrics connection")
    p.add_argument("--src-svcid", default=None, help="Source service id for initiator-side fabrics connection")
    p.add_argument("--passthru-lcores", default="1", help="Lcores argument for spdk_nvme_passthru")
    p.add_argument(
        "--prefill-workers",
        type=int,
        default=0,
        help="Parallel workers for prefill write/copy (0=auto from passthru-lcores entries)",
    )
    p.add_argument("--target-ssh-host", default=None, help="Remote target SSH host (enables split mode)")
    p.add_argument("--target-ssh-user", default=None, help="Remote target SSH user")
    p.add_argument("--target-ssh-port", type=int, default=22, help="Remote target SSH port")
    p.add_argument("--target-ssh-option", action="append", default=[], help="Additional -o options for SSH")
    p.add_argument("--target-use-sudo", action="store_true", help="Use sudo -n for remote target commands")
    p.add_argument("--target-repo-path", default=None, help="Remote SPDK repo path")
    p.add_argument("--target-rpc-script", default=None, help="Remote rpc.py path override")
    p.add_argument("--target-spdk-tgt", default=None, help="Remote spdk_tgt path override")
    p.add_argument("--rpc-script", default=str(Path(default_root) / "scripts/rpc.py"), help="Path to scripts/rpc.py")
    p.add_argument("--spdk-tgt", default=str(Path(default_root) / "build/bin/spdk_tgt"), help="Path to spdk_tgt")
    p.add_argument("--pcie-bdf", default=None, help="PCIe BDF for NVMe device (e.g. 0000:06:00.0)")
    p.add_argument("--controller-name", default="Nvme0", help="SPDK NVMe controller alias")
    p.add_argument("--skip-nvme-attach", action="store_true", help="Skip bdev_nvme_attach_controller")
    p.add_argument("--dataset-bdev", default="DATASET0")
    p.add_argument("--dataset-size-mb", type=int, default=64, help="Size of dataset-bdev if using malloc (0 disables)")
    p.add_argument("--backing-bdev", default=None, help="vSLM backing bdev (e.g., Nvme0n2)")
    p.add_argument("--backend", choices=["pslm", "vslm"], default="pslm", help="SLM backend type")
    p.add_argument("--dataset-block-size", type=int, default=4096)
    p.add_argument("--dataset-nsid", type=int, default=100)
    p.add_argument("--slm-name", default="SLM_VEC0")
    p.add_argument("--slm-size-mb", type=int, default=64)
    p.add_argument("--slm-nsid", type=int, default=101)
    p.add_argument("--slm-lba-size", type=int, default=4)
    p.add_argument("--cpcs-nsid", type=int, default=200)
    p.add_argument("--cpcs-max-activated", type=int, default=16)
    p.add_argument("--cpcs-max-mrs", type=int, default=64)
    p.add_argument("--cpcs-max-ranges-per-mrs", type=int, default=8)
    p.add_argument("--output-dir", default="/tmp/cpcs_vec_a1a2_tiny")
    p.add_argument("--profile", default="mongo_like")
    p.add_argument("--tier", default="T0")
    p.add_argument("--count", type=int, default=2000)
    p.add_argument("--dim", type=int, default=64)
    p.add_argument("--query-count", type=int, default=20)
    p.add_argument("--seed", type=int, default=20260409)
    p.add_argument("--query-limit", type=int, default=10)
    p.add_argument(
        "--exactness-score-tol",
        type=float,
        default=1e-4,
        help="Allowed absolute score difference for exactness check",
    )
    p.add_argument(
        "--exactness-strict-order",
        action="store_true",
        help="Require identical top-k ordering (default compares unordered doc_id set + score tolerance)",
    )
    p.add_argument("--metadata-offset", type=int, default=0)
    p.add_argument("--vector-offset", type=int, default=131072)
    p.add_argument("--output-offset", type=int, default=1048576)
    p.add_argument("--output-length", type=int, default=65536)
    p.add_argument(
        "--dataset-write-chunk-bytes",
        type=int,
        default=0,
        help="Chunk size for dataset namespace prefill writes in bytes (0=auto bulk)",
    )
    p.add_argument(
        "--dataset-write-max-io-size",
        type=int,
        default=0,
        help="Max bytes per dataset prefill write command (0=auto from transport)",
    )
    p.add_argument(
        "--copy-chunk-bytes",
        type=int,
        default=0,
        help="Chunk size for SLM copy prefill in bytes (0=auto bulk)",
    )
    p.add_argument(
        "--prefill-cmd-timeout-sec",
        type=float,
        default=300.0,
        help="Timeout for each passthru command during prefill (<=0 disables)",
    )
    return p.parse_args(argv)


def run_vector_eval(args: argparse.Namespace) -> Dict[str, Any]:
    _parse_lcore_tokens(args.passthru_lcores)
    if args.prefill_workers < 0:
        raise ValueError("--prefill-workers must be >= 0")
    os.chdir(args.spdk_root)
    out_dir = Path(args.output_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    target_ssh, rpc_script, spdk_tgt = resolve_target_runtime(args)
    target = SpdkTarget(
        spdk_tgt,
        args.rpc_sock,
        args.target_log,
        args.target_core_mask,
        compute_core_mask=args.target_compute_core_mask,
        target_ssh=target_ssh,
    )
    rpc = RpcClient(
        rpc_script,
        args.rpc_sock,
        target_ssh=target_ssh,
        rpc_timeout_sec=args.rpc_timeout_sec,
    )

    try:
        target.start()
        target.wait_for_rpc(rpc)
        rpc.call("framework_start_init")
        setup_subsystem(args, rpc)
        args.dataset_write_effective_max_io_size = detect_dataset_write_max_io_size(args, rpc)

        print("[dataset] generating vector dataset", flush=True)
        manifest = generate_dataset(args, out_dir)
        print("[dataset] vector dataset generated", flush=True)
        manifest_path = Path(manifest["paths"]["manifest"]) if "manifest" in manifest else out_dir / "dataset" / "manifest.json"
        if not manifest_path.exists():
            manifest_path = out_dir / "dataset" / "manifest.json"

        meta_stage = write_dataset_namespace(
            args,
            nsid=args.dataset_nsid,
            src_path=Path(manifest["paths"]["meta_bin"]),
            offset=args.metadata_offset,
            lba_size=args.dataset_block_size,
            chunk_bytes=args.dataset_write_chunk_bytes,
        )
        vec_stage = write_dataset_namespace(
            args,
            nsid=args.dataset_nsid,
            src_path=Path(manifest["paths"]["vec_bin"]),
            offset=args.vector_offset,
            lba_size=args.dataset_block_size,
            chunk_bytes=args.dataset_write_chunk_bytes,
        )
        stage_into_slm(args, meta_stage, vec_stage)
        rsid = create_mrs(args, out_dir)

        host_files = run_host_baselines(args, manifest_path, out_dir)
        k2_c0 = run_device_k2(args, manifest_path, rsid, out_dir)
        k1_c0 = run_device_k1(args, manifest_path, rsid, out_dir)
        exact = compare_exactness(
            host_files["k1_h0"],
            k1_c0,
            host_files["k2_h0"],
            k2_c0,
            out_dir,
            score_tol=float(args.exactness_score_tol),
            ignore_order=(not args.exactness_strict_order),
        )

        run_config = {
            "manifest": str(manifest_path),
            "queries": str(manifest["paths"]["queries_json"]),
            "spdk_nvme_passthru": args.spdk_nvme_passthru,
            "nqn": args.nqn,
            "hostnqn": args.hostnqn,
            "trtype": args.trtype,
            "traddr": args.traddr,
            "trsvcid": args.trsvcid,
            "src_addr": args.src_addr,
            "src_svcid": args.src_svcid,
            "passthru_lcores": args.passthru_lcores,
            "prefill_workers": args.prefill_workers,
            "dataset_nsid": args.dataset_nsid,
            "slm_nsid": args.slm_nsid,
            "dataset_write_max_io_size": _to_pos_int(getattr(args, "dataset_write_effective_max_io_size", 0)),
            "prefill_cmd_timeout_sec": args.prefill_cmd_timeout_sec,
            "cpcs_nsid": args.cpcs_nsid,
            "rsid": rsid,
            "metadata_offset": args.metadata_offset,
            "vector_offset": args.vector_offset,
            "output_offset": args.output_offset,
            "output_length": args.output_length,
            "slm_lba_size": args.slm_lba_size,
            "exactness_score_tol": args.exactness_score_tol,
            "exactness_strict_order": args.exactness_strict_order,
            "artifacts": {
                "k1_h0": str(host_files["k1_h0"]),
                "k1_c0": str(k1_c0),
                "k2_h0": str(host_files["k2_h0"]),
                "k2_c0": str(k2_c0),
                "exactness": str(exact),
            },
        }
        (out_dir / "run_config.json").write_text(json.dumps(run_config, indent=2), encoding="utf-8")
        print(json.dumps(run_config, indent=2))
        return run_config
    finally:
        target.stop()


def run_vector_eval_from_argv(argv: Sequence[str]) -> Dict[str, Any]:
    """Programmatic scenario entrypoint used by the unified orchestrator."""
    return run_vector_eval(parse_args(list(argv)))


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    try:
        run_vector_eval(args)
        return 0
    except Exception as exc:
        print(f"\\nERROR: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

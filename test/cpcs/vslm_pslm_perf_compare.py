#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

import argparse
import ipaddress
import json
import math
import os
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from statistics import mean
from typing import Any, Dict, List, Optional, Sequence, Tuple


SCRIPT_DIR = Path(__file__).resolve().parent
SPDK_ROOT = SCRIPT_DIR.parent.parent


def env_default(name: str, default: str) -> str:
    return os.environ.get(name, default)


DEFAULT_RPC = env_default("RPC", str(SPDK_ROOT / "scripts" / "rpc.py"))
DEFAULT_SPDK_TGT = env_default("SPDK_TGT", str(SPDK_ROOT / "build" / "bin" / "spdk_tgt"))
DEFAULT_SPDK_NVME_PASSTHRU = env_default(
    "SPDK_NVME_PASSTHRU",
    str(SPDK_ROOT / "build" / "bin" / "spdk_nvme_passthru"),
)

SLM_COPY_DESC_FMT_2H = 0x2
SLM_COPY_DESC_FMT_SHIFT = 8
MAX_DESC_PER_COPY_CMD = 256
MAX_BLOCKS_PER_DESC = 0x10000  # nlb is uint16, nlb + 1 blocks per descriptor.
MAX_MRS_RANGE_LEN = 0xFFFFFFFF  # uint32 max from CPCS memory range descriptor.
BUILTIN_PROGRAM_PIND = {
    "memcpy": 0,
    "memfill": 1,
    "sum64": 2,
    "max64": 3,
    "min64": 4,
    "filter_agg": 5,
}
# Programs whose execution dirties the SLM image (results must be copied OUT for
# pSLM to make them host-visible; vSLM publishes them for free). Used by the
# --copy-out-mode fairness control.
BUILTIN_WRITE_PROGRAMS = {"memcpy", "memfill"}
BUILTIN_EXEC_DESC_LEN = 24
NS_UPDATE_QUIESCE_TIMEOUT_SEC = 20.0
VSLM_MAX_READAHEAD_PAGES = 64
DEFAULT_BUILTIN_EXEC_MAX_MB = 256
DEFAULT_VSLM_EXECUTE_REPEATS = 2


def quote_cmd(cmd: Sequence[str]) -> str:
    return " ".join(shlex.quote(str(token)) for token in cmd)


def run_cmd(
        cmd: Sequence[str],
        *,
        check: bool = True,
        capture_output: bool = True,
        log_cmd: bool = True,
        timeout_sec: Optional[float] = None,
) -> subprocess.CompletedProcess:
    if log_cmd:
        print(f"+ {quote_cmd(cmd)}")

    try:
        cp = subprocess.run(
            list(cmd),
            text=True,
            capture_output=capture_output,
            timeout=timeout_sec,
        )
    except subprocess.TimeoutExpired as exc:
        timeout_label = f"{timeout_sec:.1f}s" if timeout_sec is not None else "unknown"
        raise RuntimeError(
            f"Command timed out after {timeout_label}: {quote_cmd(cmd)}\n"
            "Increase --rpc-timeout-sec or inspect target log for a blocked RPC path."
        ) from exc
    if check and cp.returncode != 0:
        stdout = (cp.stdout or "").strip()
        stderr = (cp.stderr or "").strip()
        raise RuntimeError(
            f"Command failed (rc={cp.returncode}): {quote_cmd(cmd)}\n"
            f"stdout:\n{stdout}\n"
            f"stderr:\n{stderr}"
        )
    return cp


@dataclass
class TargetSshConfig:
    host: str
    user: Optional[str]
    port: int
    options: Sequence[str]
    use_sudo: bool


def _ssh_prefix(cfg: TargetSshConfig) -> List[str]:
    dest = f"{cfg.user}@{cfg.host}" if cfg.user else cfg.host
    cmd = ["ssh"]
    if cfg.port > 0:
        cmd.extend(["-p", str(cfg.port)])
    for opt in cfg.options:
        cmd.extend(["-o", opt])
    cmd.append(dest)
    return cmd


def run_ssh_shell(
        cfg: TargetSshConfig,
        command: str,
        *,
        check: bool = True,
        capture_output: bool = True,
        log_cmd: bool = True,
) -> subprocess.CompletedProcess:
    remote_cmd = command
    if cfg.use_sudo:
        remote_cmd = f"sudo -n sh -c {shlex.quote(command)}"
    cmd = _ssh_prefix(cfg) + [remote_cmd]
    return run_cmd(
        cmd,
        check=check,
        capture_output=capture_output,
        log_cmd=log_cmd,
    )


def require_file(path: str, what: str) -> None:
    if not os.path.isfile(path):
        raise FileNotFoundError(f"{what} not found: {path}")


def require_executable(path: str, what: str) -> None:
    if not (os.path.isfile(path) and os.access(path, os.X_OK)):
        raise FileNotFoundError(f"{what} not found or not executable: {path}")


def parse_json_output(cp: subprocess.CompletedProcess) -> Any:
    try:
        return json.loads(cp.stdout or "")
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Failed to parse JSON output:\n{cp.stdout}") from exc


def parse_result_hex(output: str) -> str:
    match = re.search(r"result[:=]\s*0x([0-9a-fA-F]+)", output)
    if not match:
        return ""
    return match.group(1)


def now() -> float:
    return time.monotonic()


def gib_per_sec(num_bytes: int, seconds: float) -> float:
    if seconds <= 0:
        return 0.0
    return (num_bytes / float(1 << 30)) / seconds


class RpcClient:
    def __init__(
            self,
            rpc_script: str,
            rpc_sock: str,
            target_ssh: Optional[TargetSshConfig] = None,
            rpc_timeout_sec: Optional[float] = None,
    ):
        self.rpc_script = rpc_script
        self.rpc_sock = rpc_sock
        self.target_ssh = target_ssh
        self.rpc_timeout_sec = rpc_timeout_sec

    def _cmd(self, method: str, *args: str) -> List[str]:
        base = ["python3", self.rpc_script, "-s", self.rpc_sock, method, *args]
        if self.target_ssh is None:
            return base

        remote_cmd = quote_cmd(base)
        if self.target_ssh.use_sudo:
            remote_cmd = f"sudo -n {remote_cmd}"
        return _ssh_prefix(self.target_ssh) + [remote_cmd]

    def call(
            self,
            method: str,
            *args: str,
            check: bool = True,
            capture_output: bool = True,
            log_cmd: bool = True,
    ) -> subprocess.CompletedProcess:
        return run_cmd(
            self._cmd(method, *args),
            check=check,
            capture_output=capture_output,
            log_cmd=log_cmd,
            timeout_sec=self.rpc_timeout_sec,
        )

    def call_json(self, method: str, *args: str, log_cmd: bool = True) -> Any:
        cp = self.call(method, *args, check=True, capture_output=True, log_cmd=log_cmd)
        return parse_json_output(cp)

    def quiet(self, method: str, *args: str) -> None:
        run_cmd(self._cmd(method, *args), check=False, capture_output=True, log_cmd=False)


class SpdkTarget:
    def __init__(
            self,
            spdk_tgt: str,
            rpc_sock: str,
            target_log: str,
            core_mask: str,
            compute_core_mask: Optional[str] = None,
            target_ssh: Optional[TargetSshConfig] = None,
    ):
        self.spdk_tgt = spdk_tgt
        self.rpc_sock = rpc_sock
        self.target_log = target_log
        self.core_mask = core_mask
        self.compute_core_mask = compute_core_mask
        self.target_ssh = target_ssh
        self.proc: Optional[subprocess.Popen] = None
        self.log_file = None
        self.remote_pid: Optional[int] = None

    def start(self) -> None:
        if self.target_ssh is None:
            Path(self.rpc_sock).unlink(missing_ok=True)
            self.log_file = open(self.target_log, "w", encoding="utf-8")
            cmd = [self.spdk_tgt, "-m", self.core_mask, "-r", self.rpc_sock, "--wait-for-rpc"]
            if self.compute_core_mask:
                cmd.extend(["--cpcs-compute-core-mask", self.compute_core_mask])
            print(f"+ {quote_cmd(cmd)}")
            self.proc = subprocess.Popen(
                cmd,
                stdout=self.log_file,
                stderr=subprocess.STDOUT,
                text=True,
            )
            return

        cmd = [
            shlex.quote(self.spdk_tgt),
            "-m", shlex.quote(self.core_mask),
            "-r", shlex.quote(self.rpc_sock),
            "--wait-for-rpc",
        ]
        if self.compute_core_mask:
            cmd.extend(["--cpcs-compute-core-mask", shlex.quote(self.compute_core_mask)])
        launch = (
            f"rm -f {shlex.quote(self.rpc_sock)}; "
            f"nohup {' '.join(cmd)} > {shlex.quote(self.target_log)} 2>&1 < /dev/null & echo $!"
        )
        cp = run_ssh_shell(
            self.target_ssh,
            launch,
            check=True,
            capture_output=True,
            log_cmd=True,
        )
        try:
            self.remote_pid = int((cp.stdout or "").strip().splitlines()[-1])
        except Exception as exc:
            raise RuntimeError(f"Failed to parse remote spdk_tgt PID from output: {cp.stdout}") from exc

    def wait_for_rpc(self, rpc: RpcClient, timeout_sec: float = 20.0) -> None:
        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                raise RuntimeError(f"spdk_tgt exited early. Check log: {self.target_log}")
            cp = rpc.call("rpc_get_methods", check=False, capture_output=True, log_cmd=False)
            if cp.returncode == 0:
                return
            time.sleep(0.2)
        raise TimeoutError(f"Timed out waiting for RPC socket: {self.rpc_sock}")

    def stop(self) -> None:
        if self.target_ssh is not None:
            if self.remote_pid is not None:
                kill_cmd = (
                    f"kill {self.remote_pid} >/dev/null 2>&1 || true; "
                    f"sleep 0.2; "
                    f"kill -9 {self.remote_pid} >/dev/null 2>&1 || true"
                )
                run_ssh_shell(self.target_ssh, kill_cmd, check=False, capture_output=True, log_cmd=False)
                self.remote_pid = None
            run_ssh_shell(
                self.target_ssh,
                f"rm -f {shlex.quote(self.rpc_sock)}",
                check=False,
                capture_output=True,
                log_cmd=False,
            )
            return

        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
            self.proc = None

        if self.log_file is not None:
            self.log_file.close()
            self.log_file = None

        Path(self.rpc_sock).unlink(missing_ok=True)


def transport_exists(rpc: RpcClient, trtype: str) -> bool:
    try:
        transports = rpc.call_json("nvmf_get_transports", log_cmd=False)
    except Exception:
        return False
    for entry in transports:
        if str(entry.get("trtype", "")).lower() == trtype.lower():
            return True
    return False


def infer_listener_adrfam(traddr: str) -> Optional[str]:
    addr = str(traddr or "").strip()
    if not addr:
        return None
    if "%" in addr:
        addr = addr.split("%", 1)[0]
    try:
        ip = ipaddress.ip_address(addr)
    except ValueError:
        return None
    return "ipv6" if ip.version == 6 else "ipv4"


def get_bdev_info(rpc: RpcClient, name: str) -> Dict[str, Any]:
    info = rpc.call_json("bdev_get_bdevs", "-b", name, log_cmd=False)
    if not isinstance(info, list) or not info:
        raise RuntimeError(f"bdev_get_bdevs returned no entry for {name}")
    return info[0]


def bdev_capacity_bytes(bdev: Dict[str, Any]) -> int:
    return int(bdev.get("num_blocks", 0)) * int(bdev.get("block_size", 0))


def pack_slm_copy_desc_2h(snsid: int, slba: int, nlb: int) -> bytes:
    return struct.pack("<IIQHHIIHH", snsid, 0, slba, nlb, 0, 0, 0, 0, 0)


def pack_mrs_desc(mnsid: int, length: int, starting_byte: int) -> bytes:
    return struct.pack("<IIQ16s", mnsid, length, starting_byte, b"\x00" * 16)


def passthru_cmd_prefix(args: argparse.Namespace, admin: bool) -> List[str]:
    mode = "--admin-cmd" if admin else "--io-cmd"
    src_addr = getattr(args, "src_addr", None)
    src_svcid = getattr(args, "src_svcid", None)
    cmd = [
        args.spdk_nvme_passthru,
        "--lcores", args.passthru_lcores,
        "--disable-cpumask-locks",
        "--no-rpc-server",
        mode,
        "--trtype", args.trtype,
        "--traddr", args.traddr,
        "--trsvcid", args.trsvcid,
        "--subnqn", args.nqn,
        "--hostnqn", args.hostnqn,
    ]
    if src_addr:
        cmd.extend(["--src-addr", src_addr])
    if src_svcid:
        cmd.extend(["--src-svcid", src_svcid])
    return cmd


def build_vslm_copy_groups(total_blocks: int, block_size: int, max_group_bytes: int) -> List[Dict[str, Any]]:
    if block_size <= 0:
        raise ValueError(f"Invalid block size: {block_size}")
    if max_group_bytes < block_size:
        raise ValueError(
            f"vSLM max copy bytes ({max_group_bytes}) must be >= block size ({block_size})"
        )

    max_group_blocks = max_group_bytes // block_size
    if max_group_blocks <= 0:
        raise ValueError(
            f"vSLM max copy bytes ({max_group_bytes}) maps to 0 blocks with block size {block_size}"
        )

    max_blocks_per_desc = min(MAX_BLOCKS_PER_DESC, max_group_blocks)

    segments: List[Tuple[int, int]] = []
    slba = 0
    remaining = total_blocks
    while remaining > 0:
        blocks = min(remaining, max_blocks_per_desc)
        segments.append((slba, blocks))
        slba += blocks
        remaining -= blocks

    groups: List[Dict[str, Any]] = []
    dest_offset = 0
    group_ranges: List[Dict[str, int]] = []
    group_bytes = 0

    for seg_slba, seg_blocks in segments:
        seg_bytes = seg_blocks * block_size
        limit_hit = (
            len(group_ranges) >= MAX_DESC_PER_COPY_CMD or
            (group_ranges and (group_bytes + seg_bytes > max_group_bytes))
        )
        if limit_hit:
            groups.append({
                "dest_offset": dest_offset,
                "ranges": group_ranges,
                "total_bytes": group_bytes,
            })
            dest_offset += group_bytes
            group_ranges = []
            group_bytes = 0

        group_ranges.append({"slba": seg_slba, "nlb": seg_blocks - 1})
        group_bytes += seg_bytes

    if group_ranges:
        groups.append({
            "dest_offset": dest_offset,
            "ranges": group_ranges,
            "total_bytes": group_bytes,
        })

    return groups


def build_mrs_ranges(total_bytes: int, align_bytes: int = 4) -> List[Tuple[int, int]]:
    if align_bytes <= 0:
        raise ValueError(f"Invalid MRS range alignment: {align_bytes}")
    if (align_bytes & (align_bytes - 1)) != 0:
        raise ValueError(f"MRS range alignment must be a power of two: {align_bytes}")
    if total_bytes % align_bytes != 0:
        raise ValueError(
            f"Dataset bytes must be {align_bytes}-byte aligned for selected execute mode. "
            f"bytes={total_bytes}"
        )

    max_range_len = MAX_MRS_RANGE_LEN - (MAX_MRS_RANGE_LEN % align_bytes)
    if max_range_len == 0:
        raise ValueError(f"Invalid MRS max range for alignment {align_bytes}")

    ranges: List[Tuple[int, int]] = []
    offset = 0
    remaining = total_bytes
    while remaining > 0:
        length = min(remaining, max_range_len)
        ranges.append((offset, length))
        offset += length
        remaining -= length
    return ranges


def load_mrs_spec(path: str) -> List[Tuple[int, int]]:
    """Load an access-pattern MRS spec (from experiments/vslm_eval/workload/mrs_gen.py).
    Returns the [(offset, length), ...] range list that defines which bytes the
    program touches -- i.e. the access pattern (sequential/strided/random/sparse)."""
    with open(path) as fh:
        spec = json.load(fh)
    return [(int(o), int(l)) for o, l in spec["ranges"]]


def effective_exec_ranges(args: argparse.Namespace, total_bytes: int,
                          align_bytes: int) -> List[Tuple[int, int]]:
    """Execute-range list. When --mrs-spec is given the access pattern comes from
    the spec (the new vSLM-eval suite); otherwise the legacy contiguous tiling.
    Single-SSD / existing-suite behavior is preserved (no --mrs-spec => unchanged)."""
    spec_path = getattr(args, "mrs_spec", None)
    if spec_path:
        ranges = load_mrs_spec(spec_path)
        for off, length in ranges:
            if off % align_bytes or length % align_bytes:
                raise ValueError(
                    f"--mrs-spec range not {align_bytes}-byte aligned: ({off}, {length})")
        return ranges
    return build_mrs_ranges(total_bytes, align_bytes=align_bytes)


def create_mrs(
        args: argparse.Namespace,
        tmp_dir: Path,
        slm_nsid: int,
        ranges: Sequence[Tuple[int, int]],
        run_tag: str,
) -> int:
    mrs_file = tmp_dir / f"mrs_{run_tag}.bin"
    with mrs_file.open("wb") as fh:
        for starting_byte, length in ranges:
            fh.write(pack_mrs_desc(slm_nsid, length, starting_byte))

    cmd = passthru_cmd_prefix(args, admin=True) + [
        "--opcode", "0x89",
        "--nsid", str(args.cpcs_nsid),
        "--cdw10", "0",
        "--cdw11", str(len(ranges)),
        "--data-len", str(len(ranges) * 32),
        "--write",
        "--input-file", str(mrs_file),
    ]
    cp = run_cmd(cmd, check=True, capture_output=True, log_cmd=False)
    out = (cp.stdout or "") + (cp.stderr or "")
    rsid_hex = parse_result_hex(out)
    if not rsid_hex:
        raise RuntimeError(f"Failed to parse RSID from MRS create output:\n{out}")
    return int(rsid_hex, 16)


def issue_slm_copy(
        args: argparse.Namespace,
        tmp_dir: Path,
        slm_nsid: int,
        dest_offset: int,
        source_nsid: int,
        ranges: Sequence[Dict[str, int]],
        run_tag: str,
        desc_tag: str,
        dataset_block_size: int,
) -> Tuple[int, int]:
    expanded_ranges: List[Dict[str, int]] = []
    for idx, entry in enumerate(ranges):
        slba = int(entry["slba"])
        nlb = int(entry["nlb"])

        if slba < 0 or nlb < 0:
            raise ValueError(f"Descriptor fields must be non-negative at index {idx}: slba={slba}, nlb={nlb}")

        remaining_blocks = nlb + 1
        cur_slba = slba
        while remaining_blocks > 0:
            blocks = min(remaining_blocks, MAX_BLOCKS_PER_DESC)
            expanded_ranges.append({"slba": cur_slba, "nlb": blocks - 1})
            cur_slba += blocks
            remaining_blocks -= blocks

    if not expanded_ranges:
        return 0, 0

    total_bytes = 0
    total_cmds = 0
    current_dest_offset = dest_offset
    range_offset = 0

    while range_offset < len(expanded_ranges):
        chunk_ranges = expanded_ranges[range_offset:range_offset + MAX_DESC_PER_COPY_CMD]
        range_offset += len(chunk_ranges)

        desc_file = tmp_dir / f"copy_desc_{run_tag}_{desc_tag}_{total_cmds}.bin"
        chunk_bytes = 0
        with desc_file.open("wb") as fh:
            for entry in chunk_ranges:
                slba = int(entry["slba"])
                nlb = int(entry["nlb"])
                fh.write(pack_slm_copy_desc_2h(source_nsid, slba, nlb))
                chunk_bytes += (nlb + 1) * dataset_block_size

        cdw12 = (SLM_COPY_DESC_FMT_2H << SLM_COPY_DESC_FMT_SHIFT) | (len(chunk_ranges) - 1)
        cdw2 = chunk_bytes & 0xFFFFFFFF
        cdw3 = (chunk_bytes >> 32) & 0xFFFFFFFF
        cdw10 = current_dest_offset & 0xFFFFFFFF
        cdw11 = (current_dest_offset >> 32) & 0xFFFFFFFF

        cmd = passthru_cmd_prefix(args, admin=False) + [
            "--opcode", "0x01",
            "--nsid", str(slm_nsid),
            "--cdw2", str(cdw2),
            "--cdw3", str(cdw3),
            "--cdw10", str(cdw10),
            "--cdw11", str(cdw11),
            "--cdw12", hex(cdw12),
            "--data-len", str(len(chunk_ranges) * 32),
            "--write",
            "--input-file", str(desc_file),
        ]
        run_cmd(cmd, check=True, capture_output=True, log_cmd=False)

        total_bytes += chunk_bytes
        current_dest_offset += chunk_bytes
        total_cmds += 1

    return total_bytes, total_cmds


def render_execute_cmd(template: str, values: Dict[str, Any]) -> str:
    try:
        return template.format(**values)
    except KeyError as exc:
        raise ValueError(f"execute template placeholder missing key: {exc}") from exc


def run_execute_template(template: str, values: Dict[str, Any]) -> float:
    rendered = render_execute_cmd(template, values)
    start = now()
    cp = subprocess.run(rendered, shell=True, text=True, capture_output=True)
    elapsed = now() - start
    if cp.returncode != 0:
        raise RuntimeError(
            f"Execute template failed (rc={cp.returncode}).\n"
            f"command: {rendered}\n"
            f"stdout:\n{cp.stdout}\n"
            f"stderr:\n{cp.stderr}"
        )
    return elapsed


def reduce_builtin_results(program: str, values: Sequence[int]) -> int:
    if not values:
        raise ValueError("builtin execution produced no values")
    if program in ("sum64", "filter_agg"):
        return int(sum(values))
    if program == "max64":
        return int(max(values))
    if program == "min64":
        return int(min(values))
    if program in ("memcpy", "memfill"):
        # write kernels: device returns per-segment bytes-written (or status); the
        # absolute output is verified by a readback-sum at the driver layer.
        return int(sum(values))
    raise ValueError(f"unsupported builtin program: {program}")


def get_subsystem_controllers(rpc: RpcClient, nqn: str) -> List[Dict[str, Any]]:
    try:
        ctrlrs = rpc.call_json("nvmf_subsystem_get_controllers", nqn, log_cmd=False)
    except Exception:
        return []
    if isinstance(ctrlrs, list):
        return ctrlrs
    return []


def wait_for_no_subsystem_controllers(
        rpc: RpcClient,
        nqn: str,
        timeout_sec: float = 5.0,
        poll_sec: float = 0.1,
) -> bool:
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        if not get_subsystem_controllers(rpc, nqn):
            return True
        time.sleep(poll_sec)
    return not get_subsystem_controllers(rpc, nqn)


def run_builtin_execute(
        args: argparse.Namespace,
        tmp_dir: Path,
        rsid: int,
        ranges: Sequence[Tuple[int, int]],
        max_exec_bytes: int,
        run_tag: str,
) -> Dict[str, Any]:
    program = str(args.builtin_program)
    pind = BUILTIN_PROGRAM_PIND.get(program)
    if pind is None:
        raise ValueError(f"unsupported builtin program: {program}")
    if max_exec_bytes <= 0:
        raise ValueError(f"invalid builtin execute max bytes: {max_exec_bytes}")
    if (max_exec_bytes % 8) != 0:
        raise ValueError(f"builtin execute max bytes must be 8-byte aligned: {max_exec_bytes}")

    values: List[int] = []
    total_seconds = 0.0
    total_cmds = 0
    latency_samples_ns: List[float] = []

    for mr_id, (_, length) in enumerate(ranges, start=1):
        mr_length = int(length)
        mr_offset = 0
        segment_id = 0

        while mr_offset < mr_length:
            seg_len = min(mr_length - mr_offset, max_exec_bytes)
            desc_file = tmp_dir / f"exec_{run_tag}_{program}_mr{mr_id}_seg{segment_id}.bin"
            with desc_file.open("wb") as fh:
                fh.write(struct.pack("<QQQ", mr_id, mr_offset, int(seg_len)))

            cmd = passthru_cmd_prefix(args, admin=False) + [
                "--opcode", "0x01",
                "--nsid", str(args.cpcs_nsid),
                "--cdw2", str((rsid << 16) | pind),
                "--cdw3", "0",
                "--cdw4", str(BUILTIN_EXEC_DESC_LEN),
                "--data-len", str(BUILTIN_EXEC_DESC_LEN),
                "--write",
                "--input-file", str(desc_file),
            ]

            start = now()
            cp = run_cmd(cmd, check=True, capture_output=True, log_cmd=False)
            seg_seconds = now() - start
            total_seconds += seg_seconds
            total_cmds += 1
            if getattr(args, "per_op_latency", "off") != "off":
                latency_samples_ns.append(seg_seconds * 1e9)

            out = (cp.stdout or "") + (cp.stderr or "")
            result_hex = parse_result_hex(out)
            if not result_hex:
                raise RuntimeError(f"Failed to parse builtin execute return value:\n{out}")
            values.append(int(result_hex, 16))

            mr_offset += seg_len
            segment_id += 1

    return {
        "cmd_count": total_cmds,
        "seconds": total_seconds,
        "result": reduce_builtin_results(program, values),
        "range_results": values,
        "latency_samples_ns": latency_samples_ns,
    }


def quiesce_host_connections(args: argparse.Namespace, rpc: RpcClient) -> None:
    if not wait_for_no_subsystem_controllers(rpc, args.nqn, timeout_sec=NS_UPDATE_QUIESCE_TIMEOUT_SEC):
        ctrlrs = get_subsystem_controllers(rpc, args.nqn)
        raise TimeoutError(
            f"Timed out waiting for controllers to drain before NS update: "
            f"subsystem={args.nqn} controllers={len(ctrlrs)}"
        )


def add_ns(args: argparse.Namespace, rpc: RpcClient, nqn: str, bdev_name: str, nsid: int) -> None:
    quiesce_host_connections(args, rpc)
    rpc.call("nvmf_subsystem_add_ns", nqn, bdev_name, "-n", str(nsid))


def remove_ns_quiet(args: argparse.Namespace, rpc: RpcClient, nqn: str, nsid: int) -> None:
    quiesce_host_connections(args, rpc)
    rpc.quiet("nvmf_subsystem_remove_ns", nqn, str(nsid))


def enable_spdk_debug_logging(args: argparse.Namespace, rpc: RpcClient) -> None:
    if not args.spdk_debug:
        return

    print("INFO: Enabling SPDK debug logging")
    for method in ("log_set_level", "log_set_print_level"):
        cp = rpc.call(method, "DEBUG", check=False, capture_output=True, log_cmd=False)
        if cp.returncode != 0:
            stderr = (cp.stderr or "").strip()
            stdout = (cp.stdout or "").strip()
            print(f"WARN: {method} DEBUG failed: {stderr or stdout or 'unknown error'}")

    flags = [f.strip() for f in args.spdk_debug_flags.split(",") if f.strip()]
    for flag in flags:
        cp = rpc.call("log_set_flag", flag, check=False, capture_output=True, log_cmd=False)
        if cp.returncode != 0:
            stderr = (cp.stderr or "").strip()
            stdout = (cp.stdout or "").strip()
            print(f"WARN: log_set_flag {flag} failed: {stderr or stdout or 'unknown error'}")


def parse_bdf_csv(value: Optional[str]) -> List[str]:
    if not value:
        return []
    return [tok.strip() for tok in str(value).split(",") if tok.strip()]


def multissd_backing_requested(args: argparse.Namespace) -> bool:
    return bool(parse_bdf_csv(getattr(args, "backing_bdfs", None)))


def multissd_dataset_requested(args: argparse.Namespace) -> bool:
    return bool(getattr(args, "dataset_bdf", None)) or bool(
        parse_bdf_csv(getattr(args, "dataset_bdfs", None))
    )


@dataclass
class MultiSsdState:
    # Controller aliases attached by the multi-SSD path, in attach order; torn
    # down in reverse on cleanup. The single-SSD --controller-name is NOT listed
    # here (it stays owned by setup_common/cleanup_common unchanged).
    backing_controllers: List[str]
    dataset_controllers: List[str]
    backing_raid_bdev: Optional[str]
    dataset_raid_bdev: Optional[str]
    # True when the single-SSD --controller-name attach must be skipped because
    # the multi-SSD path now owns every device the benchmark needs (so the
    # default --pcie-bdf controller would double-attach a BDF -> -EALREADY).
    skip_single_ssd_attach: bool


def _attach_devices(
        rpc: RpcClient,
        bdfs: Sequence[str],
        prefix: str,
        nsid: int,
) -> Tuple[List[str], List[str]]:
    """Attach one NVMe controller per BDF as <prefix><i> and return
    (controller_aliases, base_bdev_names) where each base bdev is the
    <prefix><i>n<nsid> namespace (mirrors how the single-SSD path names
    Nvme0n<nsid>). Attach order is preserved for reverse-order teardown.
    """
    controllers: List[str] = []
    base_bdevs: List[str] = []
    for index, bdf in enumerate(bdfs):
        alias = f"{prefix}{index}"
        rpc.call(
            "bdev_nvme_attach_controller",
            "-b", alias,
            "-t", "pcie",
            "-a", bdf,
        )
        controllers.append(alias)
        base_bdevs.append(f"{alias}n{nsid}")
    return controllers, base_bdevs


def _create_raid0(
        rpc: RpcClient,
        raid_name: str,
        base_bdevs: Sequence[str],
        strip_size_kb: int,
) -> None:
    rpc.call(
        "bdev_raid_create",
        "-n", raid_name,
        "-z", str(strip_size_kb),
        "-r", "raid0",
        "-b", " ".join(base_bdevs),
    )


def setup_multissd(args: argparse.Namespace, rpc: RpcClient) -> None:
    """Attach the multi-SSD backing/dataset devices and build RAID0 stripes,
    then rewrite args.backing_bdev / args.dataset_bdev to point at the resulting
    bdevs so the rest of the flow (vSLM create, ns add, capacity checks) is
    unchanged. Purely additive: a no-op unless --backing-bdfs / --dataset-bdf(s)
    are supplied.

    When --backing-bdfs is set but no dataset flag is, the dataset stays on the
    FIRST backing device (its --dataset-device-nsid namespace) exactly like the
    single-SSD case keeps dataset (n1) and backing (n2) on the same drive; the
    single-SSD --controller-name attach is then skipped so the first backing
    BDF is not double-attached (-EALREADY). --dataset-bdf/--dataset-bdfs isolate
    the dataset onto separate device(s).
    """
    state = MultiSsdState(
        backing_controllers=[],
        dataset_controllers=[],
        backing_raid_bdev=None,
        dataset_raid_bdev=None,
        skip_single_ssd_attach=False,
    )
    setattr(args, "_multissd_state", state)

    backing_first_base: Optional[str] = None
    if multissd_backing_requested(args):
        backing_bdfs = parse_bdf_csv(args.backing_bdfs)
        controllers, base_bdevs = _attach_devices(
            rpc,
            backing_bdfs,
            prefix="VslmBack",
            nsid=args.backing_device_nsid,
        )
        state.backing_controllers = controllers
        if len(base_bdevs) > 1:
            raid_name = "VSLM_BACKING_RAID0"
            _create_raid0(rpc, raid_name, base_bdevs, args.raid_strip_size_kb)
            state.backing_raid_bdev = raid_name
            args.backing_bdev = raid_name
        else:
            # Single device: use the namespace bdev directly (raid-of-1, no raid).
            args.backing_bdev = base_bdevs[0]
        # First backing controller alias, used to host the dataset namespace
        # when the dataset is not isolated onto its own device(s).
        backing_first_base = f"{controllers[0]}n{args.dataset_device_nsid}"
        # The multi-SSD path owns the backing (and, by default, the dataset on
        # the same first device): skip the single-SSD --controller-name attach.
        state.skip_single_ssd_attach = True

    if multissd_dataset_requested(args):
        # Dataset isolated onto its own separate device(s).
        if args.dataset_bdfs:
            dataset_bdfs = parse_bdf_csv(args.dataset_bdfs)
        else:
            dataset_bdfs = [str(args.dataset_bdf).strip()]
        controllers, base_bdevs = _attach_devices(
            rpc,
            dataset_bdfs,
            prefix="VslmData",
            nsid=args.dataset_device_nsid,
        )
        state.dataset_controllers = controllers
        if len(base_bdevs) > 1:
            raid_name = "VSLM_DATASET_RAID0"
            _create_raid0(rpc, raid_name, base_bdevs, args.raid_strip_size_kb)
            state.dataset_raid_bdev = raid_name
            args.dataset_bdev = raid_name
        else:
            args.dataset_bdev = base_bdevs[0]
    elif backing_first_base is not None:
        # No dataset isolation requested: keep the dataset on the first backing
        # device (its --dataset-device-nsid namespace), mirroring the single-SSD
        # layout where dataset (n1) and backing (n2) share one drive.
        args.dataset_bdev = backing_first_base


def cleanup_multissd(args: argparse.Namespace, rpc: RpcClient) -> None:
    """Tear the multi-SSD stripes and controllers down: delete the dataset and
    backing RAID0 bdevs first, then detach every controller this path attached
    (reverse of attach order). No-op when the single-SSD path was used.
    """
    state = getattr(args, "_multissd_state", None)
    if state is None:
        return
    if state.dataset_raid_bdev:
        rpc.quiet("bdev_raid_delete", state.dataset_raid_bdev)
    if state.backing_raid_bdev:
        rpc.quiet("bdev_raid_delete", state.backing_raid_bdev)
    for alias in reversed(state.dataset_controllers):
        rpc.quiet("bdev_nvme_detach_controller", alias)
    for alias in reversed(state.backing_controllers):
        rpc.quiet("bdev_nvme_detach_controller", alias)


def setup_common(args: argparse.Namespace, rpc: RpcClient, max_ranges_per_mrs: int) -> Dict[str, Any]:
    def _prepare_aio_file(path: str, size_mb: int) -> Path:
        if size_mb <= 0:
            raise ValueError(f"aio file size must be > 0 MiB for {path}")
        file_path = Path(path).expanduser().resolve()
        file_path.parent.mkdir(parents=True, exist_ok=True)
        with file_path.open("wb") as fh:
            fh.truncate(size_mb * (1 << 20))
        return file_path

    rpc.call("framework_start_init")
    # The single-SSD --controller-name attach is skipped only when --backing-bdfs
    # is set: the backing then comes from the VslmBack* controllers, and the
    # dataset rides the first backing device (or its own --dataset-bdf(s)), so
    # the --pcie-bdf controller is unused and re-attaching its BDF would -EALREADY.
    # When only dataset flags are set (backing stays single-SSD), --pcie-bdf is
    # still needed for the backing namespace, so the attach is kept.
    skip_single_ssd_attach = multissd_backing_requested(args)
    if not args.skip_nvme_attach and not skip_single_ssd_attach:
        rpc.call(
            "bdev_nvme_attach_controller",
            "-b", args.controller_name,
            "-t", "pcie",
            "-a", args.pcie_bdf,
        )

    # Attach multi-SSD backing/dataset devices and (if >1) build RAID0 stripes,
    # rewriting args.backing_bdev / args.dataset_bdev before bdev resolution below.
    setup_multissd(args, rpc)

    if args.dataset_malloc_mb > 0:
        rpc.call("bdev_malloc_create", "-b", args.dataset_bdev, str(args.dataset_malloc_mb), "4096")
    if args.backing_malloc_mb > 0:
        rpc.call("bdev_malloc_create", "-b", args.backing_bdev, str(args.backing_malloc_mb), "4096")
    if args.dataset_aio_file:
        dataset_aio = _prepare_aio_file(args.dataset_aio_file, args.dataset_aio_size_mb)
        rpc.call("bdev_aio_create", str(dataset_aio), args.dataset_bdev, "4096")
    if args.backing_aio_file:
        backing_aio = _prepare_aio_file(args.backing_aio_file, args.backing_aio_size_mb)
        rpc.call("bdev_aio_create", str(backing_aio), args.backing_bdev, "4096")

    dataset_bdev = get_bdev_info(rpc, args.dataset_bdev)
    backing_bdev = get_bdev_info(rpc, args.backing_bdev)

    if not transport_exists(rpc, args.trtype):
        rpc.call("nvmf_create_transport", "-t", args.trtype)

    rpc.call("nvmf_create_subsystem", args.nqn, "-s", args.serial, "-m", str(args.max_namespaces), "-a")
    rpc.call("nvmf_subsystem_add_host", args.nqn, args.hostnqn)
    add_ns(args, rpc, args.nqn, args.dataset_bdev, args.dataset_nsid)
    listener_cmd = ["nvmf_subsystem_add_listener", args.nqn, "-t", args.trtype, "-a", args.traddr, "-s", args.trsvcid]
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
        "--max-ranges-per-mrs", str(max_ranges_per_mrs),
    )
    rpc.call("cpcs_program_install_builtins", "--subsystem-nqn", args.nqn, "--nsid", str(args.cpcs_nsid))

    return {
        "dataset_bdev": dataset_bdev,
        "backing_bdev": backing_bdev,
    }


def cleanup_common(args: argparse.Namespace, rpc: RpcClient) -> None:
    remove_ns_quiet(args, rpc, args.nqn, args.slm_nsid)
    remove_ns_quiet(args, rpc, args.nqn, args.dataset_nsid)
    rpc.quiet("cpcs_ns_delete", "--subsystem-nqn", args.nqn, "--nsid", str(args.cpcs_nsid))
    rpc.quiet("nvmf_delete_subsystem", args.nqn)
    if args.dataset_aio_file:
        rpc.quiet("bdev_aio_delete", args.dataset_bdev)
        Path(args.dataset_aio_file).expanduser().resolve().unlink(missing_ok=True)
    if args.backing_aio_file:
        rpc.quiet("bdev_aio_delete", args.backing_bdev)
        Path(args.backing_aio_file).expanduser().resolve().unlink(missing_ok=True)
    # Tear down multi-SSD RAID0 stripes + detach the controllers this path owns,
    # before detaching the single-SSD controller. No-op for the single-SSD path.
    cleanup_multissd(args, rpc)
    # Mirror the setup_common attach condition: the single-SSD controller was
    # only attached when --backing-bdfs was NOT set.
    skip_single_ssd_attach = multissd_backing_requested(args)
    if not args.skip_nvme_attach and not skip_single_ssd_attach:
        rpc.quiet("bdev_nvme_detach_controller", args.controller_name)


def _apply_vslm_debug(rpc: RpcClient, bdev_name: str) -> None:
    """Apply optional vSLM debug/ablation overrides from the environment, right
    after create while the MMU is idle (used by the E-E5 mechanism ablation).

    Env knobs (unset = leave unchanged): VSLM_DEBUG_NUM_SHARDS (int),
    VSLM_DEBUG_ASYNC, VSLM_DEBUG_FAULT_BATCH, VSLM_DEBUG_PREFETCH_BATCH,
    VSLM_DEBUG_BACKGROUND_CLEANER, VSLM_DEBUG_STREAMING,
    VSLM_DEBUG_FORCE_DMA_FALLBACK, VSLM_DEBUG_DISABLE_COW_BYPASS
    (enable=1 / disable=0).
    """
    import os

    cmd = ["bdev_vslm_set_debug", "--name", bdev_name]
    num_shards = os.environ.get("VSLM_DEBUG_NUM_SHARDS")
    if num_shards:
        cmd += ["--num-shards", str(int(num_shards))]
    for env_key, knob in (
        ("VSLM_DEBUG_ASYNC", "async-exec"),
        ("VSLM_DEBUG_FAULT_BATCH", "fault-batch"),
        ("VSLM_DEBUG_PREFETCH_BATCH", "prefetch-batch"),
        ("VSLM_DEBUG_BACKGROUND_CLEANER", "background-cleaner"),
        ("VSLM_DEBUG_STREAMING", "streaming-mode"),
        ("VSLM_DEBUG_FORCE_DMA_FALLBACK", "force-dma-fallback"),
        ("VSLM_DEBUG_DISABLE_COW_BYPASS", "disable-cow-bypass"),
    ):
        val = os.environ.get(env_key)
        if not val:
            continue
        off = val.lower() in ("0", "false", "off", "no")
        cmd.append("--%s-disabled" % knob if off else "--%s-enabled" % knob)
    if len(cmd) > 3:
        rpc.call(*cmd)


def create_vslm_bench_bdev(args: argparse.Namespace, rpc: RpcClient, bdev_name: str) -> None:
    cmd = [
        "bdev_vslm_create",
        "--name", bdev_name,
        "--base-bdev-name", args.backing_bdev,
        "--sram-size-mb", str(args.sram_mb),
        "--nsid", str(args.slm_nsid),
    ]
    if args.vslm_readahead_enabled:
        cmd.extend([
            "--readahead-enabled",
            "--readahead-pages", str(args.vslm_readahead_pages),
        ])
    else:
        cmd.append("--readahead-disabled")

    rpc.call(*cmd)
    _apply_vslm_debug(rpc, bdev_name)
    add_ns(args, rpc, args.nqn, bdev_name, args.slm_nsid)


def delete_vslm_bench_bdev(args: argparse.Namespace, rpc: RpcClient, bdev_name: str) -> None:
    remove_ns_quiet(args, rpc, args.nqn, args.slm_nsid)
    rpc.quiet("bdev_vslm_delete", "--name", bdev_name)


def run_pslm_once(
        args: argparse.Namespace,
        rpc: RpcClient,
        tmp_dir: Path,
        run_index: int,
        dataset_block_size: int,
        dataset_bytes: int,
        chunk_bytes: int,
        chunk_count: int,
        chunk_blocks: int,
        execute_bytes: int,
        builtin_exec_max_bytes: int,
) -> Dict[str, Any]:
    bdev_name = f"PSLM_BENCH_{run_index}"
    run_tag = f"pslm_run{run_index}"
    progress_stride = max(1, chunk_count // 8)
    execute_enabled = (not args.skip_execute) and execute_bytes > 0
    mrs_align_bytes = 8 if args.builtin_program else 4

    copy_seconds = 0.0
    execute_seconds = 0.0
    total_copy_cmds = 0
    total_execute_cmds = 0
    execute_values: List[int] = []
    copy_out_seconds = 0.0
    copy_out_cmds = 0
    copy_out_bytes = 0
    copy_out_mode = getattr(args, "copy_out_mode", "none")
    is_write_program = bool(args.builtin_program) and args.builtin_program in BUILTIN_WRITE_PROGRAMS

    try:
        rpc.call(
            "bdev_slm_create",
            "--name", bdev_name,
            "--nsid", str(args.slm_nsid),
            "--size-mb", str(args.pslm_size_mb),
            "--granularity", str(args.pslm_granularity),
        )
        add_ns(args, rpc, args.nqn, bdev_name, args.slm_nsid)

        chunk_mrs_ranges = build_mrs_ranges(chunk_bytes, align_bytes=mrs_align_bytes)
        rsid = create_mrs(
            args,
            tmp_dir,
            args.slm_nsid,
            chunk_mrs_ranges,
            run_tag,
        )

        start_e2e = now()
        for chunk_index in range(chunk_count):
            chunk_start = chunk_index * chunk_bytes
            chunk_execute_bytes = 0
            if execute_enabled and chunk_start < execute_bytes:
                chunk_execute_bytes = min(chunk_bytes, execute_bytes - chunk_start)

            desc = [{"slba": chunk_index * chunk_blocks, "nlb": chunk_blocks - 1}]
            t0 = now()
            _, issued_cmds = issue_slm_copy(
                args=args,
                tmp_dir=tmp_dir,
                slm_nsid=args.slm_nsid,
                dest_offset=0,
                source_nsid=args.dataset_nsid,
                ranges=desc,
                run_tag=run_tag,
                desc_tag=f"chunk{chunk_index}",
                dataset_block_size=dataset_block_size,
            )
            copy_seconds += now() - t0
            total_copy_cmds += issued_cmds

            if chunk_execute_bytes > 0:
                if args.builtin_program:
                    chunk_exec_ranges = build_mrs_ranges(
                        chunk_execute_bytes,
                        align_bytes=mrs_align_bytes,
                    )
                    exec_info = run_builtin_execute(
                        args=args,
                        tmp_dir=tmp_dir,
                        rsid=rsid,
                        ranges=chunk_exec_ranges,
                        max_exec_bytes=builtin_exec_max_bytes,
                        run_tag=f"{run_tag}_chunk{chunk_index}",
                    )
                    execute_seconds += float(exec_info["seconds"])
                    total_execute_cmds += int(exec_info["cmd_count"])
                    execute_values.append(int(exec_info["result"]))
                else:
                    execute_seconds += run_execute_template(
                        args.execute_template,
                        {
                            "mode": "pslm",
                            "run_index": run_index,
                                    "chunk_index": chunk_index,
                                    "chunk_count": chunk_count,
                                    "offset_bytes": chunk_index * chunk_bytes,
                                    "length_bytes": chunk_execute_bytes,
                                    "rsid": rsid,
                                    "dataset_nsid": args.dataset_nsid,
                                    "slm_nsid": args.slm_nsid,
                                    "cpcs_nsid": args.cpcs_nsid,
                                    "nqn": args.nqn,
                                    "trtype": args.trtype,
                                    "traddr": args.traddr,
                                    "trsvcid": args.trsvcid,
                        },
                    )
                    total_execute_cmds += 1

            # --copy-out-mode explicit: a write kernel dirties the staged SLM image,
            # so pSLM must copy those bytes back out for the host to see them -- a
            # symmetric fabric transfer of the dirtied chunk. vSLM publishes for free
            # and pays nothing here. Modeled as a same-volume SLM Copy of the chunk
            # (proxy for the egress cost); counted as copy-out (also inside e2e).
            if copy_out_mode == "explicit" and is_write_program and chunk_execute_bytes > 0:
                t_out = now()
                _, out_cmds = issue_slm_copy(
                    args=args, tmp_dir=tmp_dir, slm_nsid=args.slm_nsid,
                    dest_offset=0, source_nsid=args.dataset_nsid, ranges=desc,
                    run_tag=run_tag, desc_tag=f"chunkout{chunk_index}",
                    dataset_block_size=dataset_block_size,
                )
                copy_out_seconds += now() - t_out
                copy_out_cmds += out_cmds
                copy_out_bytes += chunk_execute_bytes

            if ((chunk_index + 1) % progress_stride == 0) or (chunk_index + 1 == chunk_count):
                print(
                    f"[run {run_index}] pSLM progress: "
                    f"{chunk_index + 1}/{chunk_count} chunks "
                    f"({((chunk_index + 1) * 100.0 / chunk_count):.1f}%)"
                )

        end_to_end_seconds = now() - start_e2e
        return {
            "copy_cmd_count": total_copy_cmds,
            "copy_seconds": copy_seconds,
            "copy_throughput_gib_s": gib_per_sec(dataset_bytes, copy_seconds),
            "copy_out_cmd_count": copy_out_cmds,
            "copy_out_seconds": copy_out_seconds,
            "copy_out_bytes": copy_out_bytes,
            "execute_cmd_count": total_execute_cmds,
            "execute_total_cmd_count": total_execute_cmds,
            "execute_repeats": 1 if execute_enabled else 0,
            "execute_bytes": execute_bytes if execute_enabled else 0,
            "execute_seconds": execute_seconds,
            "execute_total_seconds": execute_seconds,
            "execute_warm_seconds": 0.0,
            "execute_warm_avg_seconds": 0.0,
            "execute_result": (
                reduce_builtin_results(args.builtin_program, execute_values)
                if execute_values else None
            ),
            "end_to_end_seconds": end_to_end_seconds,
            "rsid": rsid,
        }
    finally:
        remove_ns_quiet(args, rpc, args.nqn, args.slm_nsid)
        rpc.quiet("bdev_slm_delete", "--name", bdev_name)


def run_vslm_once(
        args: argparse.Namespace,
        rpc: RpcClient,
        tmp_dir: Path,
        run_index: int,
        bdev_name: str,
        dataset_block_size: int,
        dataset_bytes: int,
        chunk_count: int,
        mrs_align_bytes: int,
        vslm_copy_groups: Sequence[Dict[str, Any]],
        execute_bytes: int,
        builtin_exec_max_bytes: int,
) -> Dict[str, Any]:
    run_tag = f"vslm_run{run_index}"
    progress_stride = max(1, len(vslm_copy_groups) // 8)
    execute_enabled = (not args.skip_execute) and execute_bytes > 0

    copy_seconds = 0.0
    execute_seconds = 0.0
    execute_total_seconds = 0.0
    execute_warm_seconds = 0.0
    execute_warm_avg_seconds = 0.0
    execute_repeats = 0
    total_copy_cmds = 0
    total_execute_cmds = 0
    execute_total_cmds = 0

    rpc.call("bdev_vslm_reset_stats", "--name", bdev_name, log_cmd=False)

    start_e2e = now()
    for group_index, group in enumerate(vslm_copy_groups):
        t0 = now()
        _, issued_cmds = issue_slm_copy(
            args=args,
            tmp_dir=tmp_dir,
            slm_nsid=args.slm_nsid,
            dest_offset=int(group["dest_offset"]),
            source_nsid=args.dataset_nsid,
            ranges=group["ranges"],
            run_tag=run_tag,
            desc_tag=f"group{group_index}",
            dataset_block_size=dataset_block_size,
        )
        copy_seconds += now() - t0
        total_copy_cmds += issued_cmds

        if ((group_index + 1) % progress_stride == 0) or (group_index + 1 == len(vslm_copy_groups)):
            print(
                f"[run {run_index}] vSLM copy progress: "
                f"{group_index + 1}/{len(vslm_copy_groups)} groups "
                f"({((group_index + 1) * 100.0 / len(vslm_copy_groups)):.1f}%)"
            )

    mrs_ranges: List[Tuple[int, int]] = []
    rsid = 0
    if execute_enabled:
        mrs_ranges = effective_exec_ranges(args, execute_bytes, mrs_align_bytes)
        rsid = create_mrs(
            args,
            tmp_dir,
            args.slm_nsid,
            mrs_ranges,
            run_tag,
        )

    execute_result = None
    execute_values: List[int] = []
    execute_pass_seconds: List[float] = []
    execute_pass_cmds: List[int] = []
    execute_latency_samples: List[float] = []

    if execute_enabled:
        pass_count = max(1, int(args.vslm_execute_repeats))
        for pass_index in range(pass_count):
            if args.builtin_program:
                exec_info = run_builtin_execute(
                    args=args,
                    tmp_dir=tmp_dir,
                    rsid=rsid,
                    ranges=mrs_ranges,
                    max_exec_bytes=builtin_exec_max_bytes,
                    run_tag=f"{run_tag}_pass{pass_index}",
                )
                execute_pass_seconds.append(float(exec_info["seconds"]))
                execute_pass_cmds.append(int(exec_info["cmd_count"]))
                execute_values.append(int(exec_info["result"]))
                execute_latency_samples.extend(exec_info.get("latency_samples_ns", []))
            else:
                exec_sec = run_execute_template(
                    args.execute_template,
                    {
                        "mode": "vslm",
                        "run_index": run_index,
                                "execute_pass_index": pass_index,
                                "execute_pass_count": pass_count,
                                "chunk_index": -1,
                                "chunk_count": chunk_count,
                                "offset_bytes": 0,
                                "length_bytes": execute_bytes,
                                "rsid": rsid,
                                "dataset_nsid": args.dataset_nsid,
                                "slm_nsid": args.slm_nsid,
                                "cpcs_nsid": args.cpcs_nsid,
                                "nqn": args.nqn,
                                "trtype": args.trtype,
                                "traddr": args.traddr,
                                "trsvcid": args.trsvcid,
                    },
                )
                execute_pass_seconds.append(exec_sec)
                execute_pass_cmds.append(1)

        execute_repeats = len(execute_pass_seconds)
        execute_seconds = execute_pass_seconds[0]
        execute_warm_seconds = sum(execute_pass_seconds[1:])
        execute_total_seconds = sum(execute_pass_seconds)
        execute_warm_avg_seconds = (
            execute_warm_seconds / float(execute_repeats - 1)
            if execute_repeats > 1 else 0.0
        )
        total_execute_cmds = execute_pass_cmds[0]
        execute_total_cmds = sum(execute_pass_cmds)
        if execute_values:
            if any(v != execute_values[0] for v in execute_values[1:]):
                raise RuntimeError(f"Inconsistent vSLM builtin execute results across passes: {execute_values}")
            execute_result = int(execute_values[0])

    end_to_end_seconds = now() - start_e2e
    stats = rpc.call_json("bdev_vslm_get_stats", "--name", bdev_name, log_cmd=False)

    return {
        "copy_cmd_count": total_copy_cmds,
        "copy_seconds": copy_seconds,
        "copy_throughput_gib_s": gib_per_sec(dataset_bytes, copy_seconds),
        "execute_cmd_count": total_execute_cmds,
        "execute_total_cmd_count": execute_total_cmds,
        "execute_repeats": execute_repeats,
        "execute_bytes": execute_bytes if execute_enabled else 0,
        "execute_seconds": execute_seconds,
        "execute_total_seconds": execute_total_seconds,
        "execute_warm_seconds": execute_warm_seconds,
        "execute_warm_avg_seconds": execute_warm_avg_seconds,
        "execute_result": execute_result,
        "end_to_end_seconds": end_to_end_seconds,
        "rsid": rsid,
        "vslm_stats": stats,
        "execute_latency_samples_ns": execute_latency_samples,
    }


def stats_triplet(values: Sequence[float]) -> Dict[str, float]:
    if not values:
        return {"mean": 0.0, "min": 0.0, "max": 0.0}
    return {
        "mean": float(mean(values)),
        "min": float(min(values)),
        "max": float(max(values)),
    }


def aggregate(results: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    metrics = [
        "copy_cmd_count",
        "copy_seconds",
        "copy_throughput_gib_s",
        "execute_cmd_count",
        "execute_total_cmd_count",
        "execute_repeats",
        "execute_seconds",
        "execute_warm_avg_seconds",
        "execute_total_seconds",
        "end_to_end_seconds",
    ]
    agg: Dict[str, Any] = {"pslm": {}, "vslm": {}, "delta_pct": {}, "vslm_stats": {}}

    for metric in metrics:
        pslm_values = [float(run["pslm"][metric]) for run in results]
        vslm_values = [float(run["vslm"][metric]) for run in results]
        agg["pslm"][metric] = stats_triplet(pslm_values)
        agg["vslm"][metric] = stats_triplet(vslm_values)

        pslm_mean = agg["pslm"][metric]["mean"]
        vslm_mean = agg["vslm"][metric]["mean"]
        if pslm_mean == 0:
            agg["delta_pct"][metric] = None
        else:
            agg["delta_pct"][metric] = ((vslm_mean - pslm_mean) / pslm_mean) * 100.0

    vslm_stat_keys = [
        "page_faults",
        "page_evictions",
        "page_writebacks",
        "backing_write_bytes",
        "dirty_writeback_bytes",
    ]
    for key in vslm_stat_keys:
        vals = [float(run["vslm"]["vslm_stats"].get(key, 0)) for run in results]
        agg["vslm_stats"][key] = stats_triplet(vals)

    return agg


def fmt_triplet(entry: Dict[str, float], precision: int = 3) -> str:
    return f"{entry['mean']:.{precision}f}/{entry['min']:.{precision}f}/{entry['max']:.{precision}f}"


def print_summary(results: Sequence[Dict[str, Any]], agg: Dict[str, Any]) -> None:
    print("\nPer-run Results")
    print("run  mode  copy_cmds  exec_cmds(c/t)  copy_s  copy_gib_s  exec_cold_s  exec_warm_avg_s  exec_total_s  end_to_end_s")
    for run in results:
        for mode in ("pslm", "vslm"):
            row = run[mode]
            print(
                f"{run['run_index']:>3}  {mode:<4}  "
                f"{int(row['copy_cmd_count']):>9}  "
                f"{int(row['execute_cmd_count']):>4}/{int(row['execute_total_cmd_count']):<4}      "
                f"{row['copy_seconds']:>6.3f}  "
                f"{row['copy_throughput_gib_s']:>10.3f}  "
                f"{row['execute_seconds']:>11.3f}  "
                f"{row['execute_warm_avg_seconds']:>15.3f}  "
                f"{row['execute_total_seconds']:>12.3f}  "
                f"{row['end_to_end_seconds']:>12.3f}"
            )

    print("\nAggregate (mean/min/max)")
    print("metric                 pSLM                  vSLM                  delta%")
    for metric in (
            "copy_cmd_count",
            "copy_seconds",
            "copy_throughput_gib_s",
            "execute_cmd_count",
            "execute_total_cmd_count",
            "execute_repeats",
            "execute_seconds",
            "execute_warm_avg_seconds",
            "execute_total_seconds",
            "end_to_end_seconds",
    ):
        delta = agg["delta_pct"][metric]
        delta_str = "n/a" if delta is None else f"{delta:.2f}"
        print(
            f"{metric:<21}"
            f"{fmt_triplet(agg['pslm'][metric], 3):>22}"
            f"{fmt_triplet(agg['vslm'][metric], 3):>22}"
            f"{delta_str:>10}"
        )

    print("\nvSLM Stats (mean/min/max)")
    print("stat                   values")
    for key in (
            "page_faults",
            "page_evictions",
            "page_writebacks",
            "backing_write_bytes",
            "dirty_writeback_bytes",
    ):
        print(f"{key:<21}{fmt_triplet(agg['vslm_stats'][key], 3):>22}")


def validate_args_and_derive(args: argparse.Namespace) -> Dict[str, Any]:
    if args.skip_execute:
        if args.execute_template or args.builtin_program:
            print("INFO: --skip-execute is set, execute settings will be ignored.")
    else:
        if args.execute_template and args.builtin_program:
            raise ValueError("Choose only one of --execute-template or --builtin-program")
        if not args.execute_template and not args.builtin_program:
            raise ValueError(
                "--execute-template or --builtin-program is required unless --skip-execute is set"
            )

    if args.dataset_bdev == args.backing_bdev:
        raise ValueError("dataset-bdev and backing-bdev must be different bdevs")

    nsids = {args.dataset_nsid, args.slm_nsid, args.cpcs_nsid}
    if len(nsids) != 3:
        raise ValueError("dataset_nsid, slm_nsid, and cpcs_nsid must be distinct")

    if getattr(args, "dataset_size_mb", None) is not None:
        dataset_size_mb = int(getattr(args, "dataset_size_mb"))
        if dataset_size_mb <= 0:
            raise ValueError("--dataset-size-mb must be > 0")
        dataset_bytes = dataset_size_mb * (1 << 20)
    else:
        dataset_size_gb = int(args.dataset_size_gb)
        if dataset_size_gb <= 0:
            raise ValueError("--dataset-size-gb must be > 0")
        dataset_bytes = dataset_size_gb * (1 << 30)
    chunk_bytes = args.chunk_size_mb * (1 << 20)
    pslm_bytes = args.pslm_size_mb * (1 << 20)
    sram_bytes = args.sram_mb * (1 << 20)
    backing_min_bytes = args.vslm_backing_min_gb * (1 << 30)

    if chunk_bytes <= 0 or dataset_bytes <= 0:
        raise ValueError("dataset size and chunk-size-mb must be > 0")
    if chunk_bytes > pslm_bytes:
        raise ValueError(f"chunk-size ({chunk_bytes}) must be <= pslm-size ({pslm_bytes})")
    if dataset_bytes % chunk_bytes != 0:
        raise ValueError("dataset-size must be divisible by chunk-size")
    if args.runs <= 0:
        raise ValueError("--runs must be > 0")
    if args.passthru_lcores.strip() == "":
        raise ValueError("--passthru-lcores cannot be empty")
    if args.vslm_max_copy_mb <= 0:
        raise ValueError("--vslm-max-copy-mb must be > 0")
    if args.builtin_exec_max_mb <= 0:
        raise ValueError("--builtin-exec-max-mb must be > 0")
    if args.vslm_execute_repeats <= 0:
        raise ValueError("--vslm-execute-repeats must be > 0")
    if args.vslm_readahead_pages <= 0 or args.vslm_readahead_pages > VSLM_MAX_READAHEAD_PAGES:
        raise ValueError(
            f"--vslm-readahead-pages must be in [1, {VSLM_MAX_READAHEAD_PAGES}]"
        )

    mrs_align_bytes = 8 if args.builtin_program else 4

    builtin_exec_max_bytes = args.builtin_exec_max_mb * (1 << 20)
    if builtin_exec_max_bytes < 8:
        raise ValueError("builtin execute max bytes must be >= 8")
    builtin_exec_max_bytes -= builtin_exec_max_bytes % 8
    if builtin_exec_max_bytes == 0:
        raise ValueError("builtin execute max bytes must be 8-byte aligned")

    execute_bytes = 0
    if not args.skip_execute:
        execute_bytes = dataset_bytes
        if args.exec_hotset_mb is not None:
            execute_bytes = args.exec_hotset_mb * (1 << 20)

        if execute_bytes <= 0:
            raise ValueError("execute bytes must be > 0")
        if execute_bytes > dataset_bytes:
            raise ValueError(
                f"execute bytes ({execute_bytes}) cannot exceed dataset bytes ({dataset_bytes})"
            )
        if execute_bytes % mrs_align_bytes != 0:
            raise ValueError(
                f"execute bytes ({execute_bytes}) must be aligned to {mrs_align_bytes} bytes"
            )

        if args.exec_hotset_mb is not None:
            if execute_bytes <= pslm_bytes:
                raise ValueError(
                    f"hotset ({execute_bytes}) must be larger than pSLM size ({pslm_bytes}) "
                    "for oversubscription experiments"
                )
            if execute_bytes > sram_bytes:
                raise ValueError(
                    f"hotset ({execute_bytes}) must be <= vSLM SRAM size ({sram_bytes}) "
                    "to fit the vSLM warm working set"
                )

    return {
        "dataset_bytes": dataset_bytes,
        "dataset_size_mb": int(dataset_bytes // (1 << 20)),
        "dataset_size_gb": float(dataset_bytes) / float(1 << 30),
        "chunk_bytes": chunk_bytes,
        "pslm_bytes": pslm_bytes,
        "sram_bytes": sram_bytes,
        "backing_min_bytes": backing_min_bytes,
        "vslm_max_copy_bytes": args.vslm_max_copy_mb * (1 << 20),
        "mrs_align_bytes": mrs_align_bytes,
        "execute_bytes": execute_bytes,
        "builtin_exec_max_bytes": builtin_exec_max_bytes,
    }


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Benchmark vSLM vs pSLM under DRAM-oversubscription CPCS workloads",
    )

    parser.add_argument("--rpc-script", default=DEFAULT_RPC, help="Path to scripts/rpc.py")
    parser.add_argument("--spdk-tgt", default=DEFAULT_SPDK_TGT, help="Path to spdk_tgt")
    parser.add_argument("--spdk-nvme-passthru", default=DEFAULT_SPDK_NVME_PASSTHRU,
                        help="Path to spdk_nvme_passthru")
    parser.add_argument("--rpc-sock", default="/var/tmp/vslm_pslm_bench.sock", help="RPC socket path")
    parser.add_argument(
        "--rpc-timeout-sec",
        type=float,
        default=120.0,
        help="Timeout for each rpc.py invocation in seconds",
    )
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
    parser.add_argument("--spdk-debug", action="store_true",
                        help="Enable SPDK DEBUG level and selected log flags at runtime")
    parser.add_argument("--spdk-debug-flags", default="nvmf,nvmf_tcp,nvmf_cpcs,bdev,nvme",
                        help="Comma-separated SPDK log flags to set when --spdk-debug is enabled")

    parser.add_argument("--controller-name", default="Nvme0", help="SPDK NVMe controller alias")
    parser.add_argument("--pcie-bdf", required=True, help="PCIe BDF for bdev_nvme_attach_controller")
    parser.add_argument("--dataset-bdev", required=True, help="Dataset source bdev (e.g., Nvme0n1)")
    parser.add_argument("--backing-bdev", required=True, help="vSLM backing bdev (e.g., Nvme0n2)")
    parser.add_argument("--skip-nvme-attach", action="store_true",
                        help="Skip bdev_nvme_attach_controller and use pre-existing dataset/backing bdevs")
    parser.add_argument(
        "--backing-bdfs",
        default=None,
        help="CSV of PCIe BDFs whose BACKING namespace is RAID0-striped into one vSLM backing bdev. "
             "A single BDF behaves exactly like the single-SSD --pcie-bdf path (raid-of-1, no raid). "
             "When unset, the single-SSD --pcie-bdf/--backing-bdev path is used unchanged.")
    parser.add_argument(
        "--backing-device-nsid",
        type=int,
        default=2,
        help="NSID of the BACKING namespace on each --backing-bdfs device (Nvme<i>n<NSID>); default 2")
    parser.add_argument(
        "--dataset-bdf",
        default=None,
        help="Optional PCIe BDF that places the dataset namespace on a SEPARATE physical device. "
             "When unset, the dataset stays on the first backing/--pcie-bdf device (today's behavior).")
    parser.add_argument(
        "--dataset-bdfs",
        default=None,
        help="Optional CSV of PCIe BDFs whose DATASET namespace is RAID0-striped into one dataset bdev. "
             "When unset, the dataset stays on the first backing/--pcie-bdf device (today's behavior).")
    parser.add_argument(
        "--dataset-device-nsid",
        type=int,
        default=1,
        help="NSID of the DATASET namespace on each --dataset-bdf/--dataset-bdfs device "
             "(Nvme<i>n<NSID>); default 1")
    parser.add_argument(
        "--raid-strip-size-kb",
        type=int,
        default=64,
        help="RAID0 strip size in KiB used when striping multiple backing/dataset devices; default 64")
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

    parser.add_argument("--sram-mb", type=int, default=32, help="vSLM SRAM size in MiB")
    parser.add_argument("--pslm-size-mb", type=int, default=32, help="pSLM size in MiB")
    parser.add_argument("--dataset-size-gb", type=int, default=32, help="Dataset size to stage in GiB")
    parser.add_argument(
        "--dataset-size-mb",
        type=int,
        default=None,
        help="Optional dataset size to stage in MiB (overrides --dataset-size-gb)",
    )
    parser.add_argument("--chunk-size-mb", type=int, default=32, help="pSLM chunk size in MiB")
    parser.add_argument("--vslm-max-copy-mb", type=int, default=1024,
                        help="Maximum bytes per single vSLM COPY command in MiB")
    parser.add_argument("--vslm-backing-min-gb", type=int, default=64,
                        help="Minimum required backing bdev size in GiB for validation")
    readahead_group = parser.add_mutually_exclusive_group()
    readahead_group.add_argument("--vslm-readahead-enabled", dest="vslm_readahead_enabled",
                                 action="store_true", help="Enable vSLM execute-view readahead")
    readahead_group.add_argument("--vslm-readahead-disabled", dest="vslm_readahead_enabled",
                                 action="store_false", help="Disable vSLM execute-view readahead")
    parser.add_argument("--vslm-readahead-pages", type=int, default=VSLM_MAX_READAHEAD_PAGES,
                        help=f"vSLM execute-view readahead window in pages (1-{VSLM_MAX_READAHEAD_PAGES})")
    parser.set_defaults(vslm_readahead_enabled=True)

    parser.add_argument("--dataset-nsid", type=int, default=1, help="Dataset namespace NSID")
    parser.add_argument("--slm-nsid", type=int, default=100, help="SLM namespace NSID")
    parser.add_argument("--cpcs-nsid", type=int, default=200, help="CPCS compute namespace NSID")
    parser.add_argument("--pslm-granularity", type=int, default=4, help="pSLM bdev granularity in bytes")

    parser.add_argument("--execute-template", default=None, help="Execute command template")
    parser.add_argument(
        "--builtin-program",
        choices=sorted(BUILTIN_PROGRAM_PIND.keys()),
        default=None,
        help="Run one builtin CPCS program directly instead of an external execute template",
    )
    parser.add_argument(
        "--builtin-exec-max-mb",
        type=int,
        default=DEFAULT_BUILTIN_EXEC_MAX_MB,
        help=f"Max bytes per builtin Execute command (default: {DEFAULT_BUILTIN_EXEC_MAX_MB})",
    )
    parser.add_argument(
        "--exec-hotset-mb",
        type=int,
        default=None,
        help="Execute hotset size in MiB (must be > pSLM and <= vSLM SRAM when provided)",
    )
    parser.add_argument(
        "--vslm-execute-repeats",
        type=int,
        default=DEFAULT_VSLM_EXECUTE_REPEATS,
        help=f"Number of repeated execute passes on the same vSLM image (default: {DEFAULT_VSLM_EXECUTE_REPEATS})",
    )
    parser.add_argument("--skip-execute", action="store_true", help="Skip execute phase; copy-only benchmark")

    # --- vslm_eval suite extensions (additive; defaults preserve legacy behavior) ---
    parser.add_argument(
        "--mrs-spec", default=None,
        help="Path to an access-pattern MRS spec JSON (experiments/vslm_eval/workload/mrs_gen.py). "
             "When set, the vSLM execute ranges come from the spec (random/strided/sparse/selective) "
             "instead of the legacy contiguous tiling. Unset => unchanged behavior.")
    parser.add_argument(
        "--copy-out-mode", choices=["none", "inline", "explicit"], default="none",
        help="pSLM write-result handling. 'explicit' charges pSLM a symmetric copy-OUT of the dirtied "
             "bytes (write-kernel fairness vs vSLM publish). Default none (unchanged).")
    parser.add_argument(
        "--per-op-latency-sample", dest="per_op_latency",
        choices=["off", "exec", "all"], default="off",
        help="Collect per-execute-op latency samples for p50/p95/p99 (execute-descriptor level).")

    parser.add_argument("--runs", type=int, default=3, help="Number of repetitions per mode")
    parser.add_argument("--output-json", default=None, help="Write structured output JSON")

    parser.add_argument("--nqn", default="nqn.2026-03.io.spdk:vslm-pslm-bench", help="NVMf subsystem NQN")
    parser.add_argument("--serial", default="VSLMPSLMBENCH01", help="NVMf subsystem serial")
    parser.add_argument("--max-namespaces", type=int, default=1024, help="Max namespaces for subsystem")
    parser.add_argument("--trtype", default="TCP", help="NVMf transport type")
    parser.add_argument("--traddr", default="127.0.0.1", help="NVMf target address")
    parser.add_argument("--trsvcid", default="4420", help="NVMf target service ID")
    parser.add_argument("--src-addr", default=None, help="Source address for initiator-side fabrics connection")
    parser.add_argument("--src-svcid", default=None, help="Source service id (port) for initiator-side fabrics connection")
    parser.add_argument("--hostnqn", default="nqn.2026-03.io.spdk:vslm-pslm-bench-host",
                        help="Host NQN used by spdk_nvme_passthru")
    parser.add_argument("--passthru-lcores", default="1", help="Lcores argument for spdk_nvme_passthru")

    parser.add_argument("--cpcs-max-activated", type=int, default=16, help="CPCS max activated programs")
    parser.add_argument("--cpcs-max-mrs", type=int, default=256, help="CPCS max MRS entries")
    return parser.parse_args(argv)


def resolve_target_runtime(args: argparse.Namespace) -> Tuple[Optional[TargetSshConfig], str, str]:
    if args.target_ssh_host is None:
        return None, args.rpc_script, args.spdk_tgt

    target_ssh = TargetSshConfig(
        host=args.target_ssh_host,
        user=args.target_ssh_user,
        port=args.target_ssh_port,
        options=args.target_ssh_option,
        use_sudo=args.target_use_sudo,
    )
    target_rpc_script = args.target_rpc_script
    target_spdk_tgt = args.target_spdk_tgt
    if args.target_repo_path:
        if target_rpc_script is None:
            target_rpc_script = str(Path(args.target_repo_path) / "scripts" / "rpc.py")
        if target_spdk_tgt is None:
            target_spdk_tgt = str(Path(args.target_repo_path) / "build" / "bin" / "spdk_tgt")

    if target_rpc_script is None or target_spdk_tgt is None:
        raise ValueError(
            "split mode requires either --target-repo-path or both "
            "--target-rpc-script and --target-spdk-tgt"
        )

    return target_ssh, target_rpc_script, target_spdk_tgt


def _build_benchmark_namespace_from_source_args(
        source_args: argparse.Namespace,
        *,
        overrides: Optional[Dict[str, Any]] = None,
) -> argparse.Namespace:
    """
    Build a complete benchmark Namespace from a parent scenario Namespace.

    This enables non-CLI scenario composition without nested parse_args chains.
    """
    for required in ("pcie_bdf", "dataset_bdev", "backing_bdev"):
        value = getattr(source_args, required, None)
        if value is None or str(value) == "":
            raise ValueError(f"Missing required benchmark field on source args: {required}")

    # Start from canonical defaults produced by the benchmark parser.
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
        if hasattr(ns, key):
            setattr(ns, key, value)

    if overrides:
        for key, value in overrides.items():
            if hasattr(ns, key):
                setattr(ns, key, value)

    return ns


def run_benchmark_from_parent_args(
        source_args: argparse.Namespace,
        *,
        overrides: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """
    Scenario composition helper for callers that already parsed their own args.
    """
    return run_benchmark(
        _build_benchmark_namespace_from_source_args(
            source_args,
            overrides=overrides,
        )
    )


def run_benchmark(args: argparse.Namespace) -> Dict[str, Any]:
    derived = validate_args_and_derive(args)
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
    rpc = RpcClient(
        rpc_script,
        args.rpc_sock,
        target_ssh=target_ssh,
        rpc_timeout_sec=args.rpc_timeout_sec,
    )

    results: List[Dict[str, Any]] = []
    config_output: Dict[str, Any] = {
        "pcie_bdf": args.pcie_bdf,
        "dataset_bdev": args.dataset_bdev,
        "backing_bdev": args.backing_bdev,
        "backing_bdfs": args.backing_bdfs,
        "backing_device_nsid": args.backing_device_nsid,
        "dataset_bdf": args.dataset_bdf,
        "dataset_bdfs": args.dataset_bdfs,
        "dataset_device_nsid": args.dataset_device_nsid,
        "raid_strip_size_kb": args.raid_strip_size_kb,
        "dataset_size_gb": derived["dataset_size_gb"],
        "dataset_size_mb": derived["dataset_size_mb"],
        "chunk_size_mb": args.chunk_size_mb,
        "vslm_max_copy_mb": args.vslm_max_copy_mb,
        "vslm_readahead_enabled": args.vslm_readahead_enabled,
        "vslm_readahead_pages": args.vslm_readahead_pages,
        "vslm_execute_repeats": args.vslm_execute_repeats,
        "runs": args.runs,
        "skip_execute": args.skip_execute,
        "builtin_program": args.builtin_program,
        "builtin_exec_max_mb": args.builtin_exec_max_mb,
        "exec_hotset_mb": args.exec_hotset_mb,
        "spdk_debug": args.spdk_debug,
        "spdk_debug_flags": args.spdk_debug_flags,
        "src_addr": args.src_addr,
        "src_svcid": args.src_svcid,
        "split_mode": target_ssh is not None,
    }

    try:
        target.start()
        target.wait_for_rpc(rpc)
        enable_spdk_debug_logging(args, rpc)

        max_mrs_range_len = MAX_MRS_RANGE_LEN - (MAX_MRS_RANGE_LEN % derived["mrs_align_bytes"])
        required_ranges = int(math.ceil(derived["dataset_bytes"] / float(max_mrs_range_len)))
        max_ranges_per_mrs = max(32, required_ranges + 4)
        if max_ranges_per_mrs > 255:
            raise ValueError(
                f"Required max_ranges_per_mrs exceeds uint8 limit: {max_ranges_per_mrs}. "
                "Reduce dataset-size."
            )

        setup = setup_common(args, rpc, max_ranges_per_mrs)
        dataset_info = setup["dataset_bdev"]
        backing_info = setup["backing_bdev"]
        dataset_block_size = int(dataset_info["block_size"])
        dataset_capacity = bdev_capacity_bytes(dataset_info)
        backing_capacity = bdev_capacity_bytes(backing_info)

        if derived["dataset_bytes"] > dataset_capacity:
            raise ValueError(
                f"dataset-size ({derived['dataset_bytes']}) exceeds dataset-bdev capacity ({dataset_capacity})"
            )
        if derived["backing_min_bytes"] > backing_capacity:
            raise ValueError(
                f"backing-bdev capacity ({backing_capacity}) is less than "
                f"vslm-backing-min-gb ({derived['backing_min_bytes']})"
            )
        if derived["dataset_bytes"] > backing_capacity:
            raise ValueError(
                f"dataset-size ({derived['dataset_bytes']}) must fit in backing-bdev ({backing_capacity})"
            )
        if derived["sram_bytes"] > backing_capacity:
            raise ValueError(
                f"sram-size ({derived['sram_bytes']}) cannot exceed backing-bdev capacity ({backing_capacity})"
            )
        if derived["dataset_bytes"] % dataset_block_size != 0:
            raise ValueError(
                f"dataset-size bytes ({derived['dataset_bytes']}) not aligned to dataset block size "
                f"({dataset_block_size})"
            )
        if derived["chunk_bytes"] % dataset_block_size != 0:
            raise ValueError(
                f"chunk-size bytes ({derived['chunk_bytes']}) not aligned to dataset block size "
                f"({dataset_block_size})"
            )

        chunk_blocks = derived["chunk_bytes"] // dataset_block_size
        if chunk_blocks <= 0:
            raise ValueError(f"chunk-size maps to invalid block count: {chunk_blocks}")

        dataset_blocks = derived["dataset_bytes"] // dataset_block_size
        chunk_count = derived["dataset_bytes"] // derived["chunk_bytes"]
        vslm_groups = build_vslm_copy_groups(
            dataset_blocks,
            dataset_block_size,
            derived["vslm_max_copy_bytes"],
        )

        print(
            "Benchmark configuration:\n"
            f"  dataset_bytes={derived['dataset_bytes']} "
            f"({derived['dataset_size_mb']} MiB / {derived['dataset_size_gb']:.4f} GiB)\n"
            f"  chunk_bytes={derived['chunk_bytes']} ({args.chunk_size_mb} MiB)\n"
            f"  execute_bytes={derived['execute_bytes']} ({(derived['execute_bytes'] // (1 << 20))} MiB)\n"
            f"  vslm_execute_repeats={args.vslm_execute_repeats}\n"
            f"  vslm_readahead={args.vslm_readahead_enabled} pages={args.vslm_readahead_pages}\n"
            f"  builtin_exec_max_bytes={derived['builtin_exec_max_bytes']} ({args.builtin_exec_max_mb} MiB)\n"
            f"  vslm_max_copy_bytes={derived['vslm_max_copy_bytes']} ({args.vslm_max_copy_mb} MiB)\n"
            f"  chunk_count={chunk_count}\n"
            f"  pSLM expected copy commands/run={chunk_count}\n"
            f"  vSLM planned copy commands/run={len(vslm_groups)}\n"
            f"  dataset_block_size={dataset_block_size}\n"
            f"  mrs_align_bytes={derived['mrs_align_bytes']}\n"
            f"  max_ranges_per_mrs={max_ranges_per_mrs}\n"
            f"  split_mode={target_ssh is not None}"
        )

        with tempfile.TemporaryDirectory(prefix="vslm_pslm_bench_") as tmp:
            tmp_dir = Path(tmp)
            pslm_results: List[Dict[str, Any]] = []
            vslm_results: List[Dict[str, Any]] = []

            for run_index in range(args.runs):
                print(f"\n=== Run {run_index + 1}/{args.runs}: pSLM ===")
                try:
                    pslm = run_pslm_once(
                        args=args,
                        rpc=rpc,
                        tmp_dir=tmp_dir,
                        run_index=run_index,
                        dataset_block_size=dataset_block_size,
                        dataset_bytes=derived["dataset_bytes"],
                        chunk_bytes=derived["chunk_bytes"],
                        chunk_count=chunk_count,
                        chunk_blocks=chunk_blocks,
                        execute_bytes=derived["execute_bytes"],
                        builtin_exec_max_bytes=derived["builtin_exec_max_bytes"],
                    )
                except Exception as exc:
                    raise RuntimeError(
                        f"pSLM run {run_index + 1}/{args.runs} failed: {exc}"
                    ) from exc
                pslm_results.append(pslm)

            vslm_bdev_name = "VSLM_BENCH_SHARED"
            vslm_created = False
            try:
                create_vslm_bench_bdev(args, rpc, vslm_bdev_name)
                vslm_created = True

                for run_index in range(args.runs):
                    print(f"\n=== Run {run_index + 1}/{args.runs}: vSLM ===")
                    try:
                        vslm = run_vslm_once(
                            args=args,
                            rpc=rpc,
                            tmp_dir=tmp_dir,
                            run_index=run_index,
                            bdev_name=vslm_bdev_name,
                            dataset_block_size=dataset_block_size,
                            dataset_bytes=derived["dataset_bytes"],
                            chunk_count=chunk_count,
                            mrs_align_bytes=derived["mrs_align_bytes"],
                            vslm_copy_groups=vslm_groups,
                            execute_bytes=derived["execute_bytes"],
                            builtin_exec_max_bytes=derived["builtin_exec_max_bytes"],
                        )
                    except Exception as exc:
                        raise RuntimeError(
                            f"vSLM run {run_index + 1}/{args.runs} failed: {exc}"
                        ) from exc
                    vslm_results.append(vslm)
            finally:
                if vslm_created:
                    delete_vslm_bench_bdev(args, rpc, vslm_bdev_name)

            for run_index in range(args.runs):
                results.append({
                    "run_index": run_index,
                    "pslm": pslm_results[run_index],
                    "vslm": vslm_results[run_index],
                })

        agg = aggregate(results)
        print_summary(results, agg)
        payload = {
            "config": config_output,
            "runs": results,
            "aggregate": agg,
        }
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


def run_benchmark_from_argv(argv: Sequence[str]) -> Dict[str, Any]:
    """Programmatic scenario entrypoint used by the unified orchestrator."""
    return run_benchmark(parse_args(list(argv)))


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    try:
        run_benchmark(args)
        return 0
    except Exception as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

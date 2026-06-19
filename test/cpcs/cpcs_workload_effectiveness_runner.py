#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Set, Tuple


METADATA_STRIDE_BYTES = 32
DEFAULT_OUTPUT_LENGTH = 65536
DEFAULT_LBA_ALIGN = 4096


@dataclass(frozen=True)
class WorkloadSpec:
    name: str
    profile: str
    dim: int
    canonical_count: int
    k: int


@dataclass(frozen=True)
class SshThrottleTarget:
    host: str
    user: Optional[str]
    port: int
    options: Tuple[str, ...]
    use_sudo: bool


WORKLOAD_SPECS: Mapping[str, WorkloadSpec] = {
    "mongo": WorkloadSpec(
        name="mongo",
        profile="mongo_like",
        dim=2048,
        canonical_count=15_300_000,
        k=100,
    ),
    "pinecone_yfcc": WorkloadSpec(
        name="pinecone_yfcc",
        profile="pinecone_yfcc_like",
        dim=192,
        canonical_count=10_000_000,
        k=10,
    ),
    "pinecone_customer": WorkloadSpec(
        name="pinecone_customer",
        profile="pinecone_customer_like",
        dim=768,
        canonical_count=35_000_000,
        k=100,
    ),
    "elastic_agentic": WorkloadSpec(
        name="elastic_agentic",
        profile="elastic_agentic_like",
        dim=128,
        canonical_count=20_000_000,
        k=100,
    ),
}

TIER_COUNTS: Mapping[str, int] = {
    "T0": 100_000,
    "T1": 1_000_000,
    "T2": 5_000_000,
}


def quote_cmd(cmd: Sequence[str]) -> str:
    return " ".join(shlex.quote(str(token)) for token in cmd)


def run_cmd(
    cmd: Sequence[str],
    *,
    cwd: Optional[Path] = None,
    capture_output: bool = False,
    check: bool = True,
) -> subprocess.CompletedProcess:
    print(f"+ {quote_cmd(cmd)}", flush=True)
    cp = subprocess.run(
        list(cmd),
        cwd=str(cwd) if cwd else None,
        text=True,
        capture_output=capture_output,
    )
    if cp.returncode != 0 and check:
        raise RuntimeError(
            f"Command failed (rc={cp.returncode}): {quote_cmd(cmd)}\n"
            f"stdout:\n{cp.stdout or ''}\n"
            f"stderr:\n{cp.stderr or ''}"
        )
    return cp


def load_inventory_yaml(path: Path) -> Dict[str, Any]:
    try:
        import yaml  # type: ignore
    except Exception as exc:
        raise RuntimeError("PyYAML is required for inventory parsing") from exc
    with path.open("r", encoding="utf-8") as fh:
        data = yaml.safe_load(fh)
    if not isinstance(data, dict):
        raise ValueError(f"invalid inventory YAML: {path}")
    return data


def _ssh_prefix(cfg: SshThrottleTarget) -> List[str]:
    dest = f"{cfg.user}@{cfg.host}" if cfg.user else cfg.host
    cmd = ["ssh", "-p", str(cfg.port)]
    for opt in cfg.options:
        cmd.extend(["-o", opt])
    cmd.append(dest)
    return cmd


def run_ssh_shell(
    cfg: SshThrottleTarget,
    command: str,
    *,
    check: bool = True,
    capture_output: bool = True,
) -> subprocess.CompletedProcess:
    remote_cmd = command
    if cfg.use_sudo:
        remote_cmd = f"sudo -n sh -lc {shlex.quote(command)}"
    return run_cmd(_ssh_prefix(cfg) + [remote_cmd], capture_output=capture_output, check=check)


def infer_local_route_info(dest_ip: str) -> Tuple[str, Optional[str]]:
    cp = run_cmd(["bash", "-lc", f"ip -o route get {shlex.quote(dest_ip)}"], capture_output=True)
    line = (cp.stdout or "").strip()
    m = re.search(r"\bdev\s+(\S+)", line)
    if not m:
        raise RuntimeError(f"failed to infer route device for destination {dest_ip}: {line}")
    dev = m.group(1)
    m_src = re.search(r"\bsrc\s+(\S+)", line)
    src_ip = m_src.group(1) if m_src else None
    return dev, src_ip


def infer_remote_route_dev(cfg: SshThrottleTarget, dest_ip: str) -> str:
    # Route lookup does not require root; avoid sudo dependency for auto-detection.
    no_sudo_cfg = SshThrottleTarget(
        host=cfg.host,
        user=cfg.user,
        port=cfg.port,
        options=cfg.options,
        use_sudo=False,
    )
    cp = run_ssh_shell(no_sudo_cfg, f"ip -o route get {shlex.quote(dest_ip)}", capture_output=True, check=True)
    line = (cp.stdout or "").strip()
    m = re.search(r"\bdev\s+(\S+)", line)
    if not m:
        raise RuntimeError(
            f"failed to infer remote route device for destination {dest_ip} via {cfg.host}: {line}"
        )
    return m.group(1)


def resolve_target_ssh_for_throttle(args: argparse.Namespace, inventory: Dict[str, Any]) -> Optional[SshThrottleTarget]:
    if args.throttle_target_ssh_host:
        return SshThrottleTarget(
            host=args.throttle_target_ssh_host,
            user=args.throttle_target_ssh_user,
            port=int(args.throttle_target_ssh_port),
            options=tuple(args.throttle_target_ssh_option),
            use_sudo=(not args.throttle_target_no_sudo),
        )

    roles = inventory.get("roles", {}) if isinstance(inventory.get("roles"), dict) else {}
    hosts = inventory.get("hosts", {}) if isinstance(inventory.get("hosts"), dict) else {}
    target_role = roles.get("target")
    if not target_role or target_role not in hosts:
        return None
    target = hosts[target_role]
    if not isinstance(target, dict):
        return None
    if str(target.get("mode", "local")) != "ssh":
        return None
    host = target.get("ssh_host")
    if not host:
        return None
    return SshThrottleTarget(
        host=str(host),
        user=(str(target["ssh_user"]) if target.get("ssh_user") else None),
        port=int(target.get("ssh_port", 22)),
        options=tuple(str(x) for x in target.get("ssh_options", [])),
        use_sudo=bool(target.get("use_sudo", True)),
    )


def _tc_set_cmd(dev: str, gbps: float, burst_kb: int, latency_ms: int) -> str:
    rate_mbit = gbps * 1000.0
    return (
        f"tc qdisc replace dev {shlex.quote(dev)} root tbf "
        f"rate {rate_mbit:.3f}mbit burst {int(burst_kb)}kb latency {int(latency_ms)}ms"
    )


def _tc_clear_cmd(dev: str) -> str:
    return f"tc qdisc del dev {shlex.quote(dev)} root"


def _tc_stats_cmd(dev: str) -> str:
    return f"tc -s qdisc show dev {shlex.quote(dev)}"


def _parse_tc_sent_bytes(text: str) -> int:
    # tc -s includes lines like: "Sent 12345 bytes 67 pkt ..."
    m = re.search(r"\bSent\s+(\d+)\s+bytes\b", text)
    if not m:
        return 0
    return int(m.group(1))


def read_local_tc_sent_bytes(dev: str, *, local_use_sudo: bool) -> int:
    cmd = _tc_stats_cmd(dev)
    if local_use_sudo:
        cmd = f"sudo -n sh -lc {shlex.quote(cmd)}"
    cp = run_cmd(["bash", "-lc", cmd], capture_output=True, check=False)
    if cp.returncode != 0:
        return 0
    return _parse_tc_sent_bytes((cp.stdout or "") + "\n" + (cp.stderr or ""))


def read_remote_tc_sent_bytes(cfg: SshThrottleTarget, dev: str) -> int:
    cp = run_ssh_shell(cfg, _tc_stats_cmd(dev), check=False, capture_output=True)
    if cp.returncode != 0:
        return 0
    return _parse_tc_sent_bytes((cp.stdout or "") + "\n" + (cp.stderr or ""))


def collect_throttle_stats(
    *,
    initiator_dev: Optional[str],
    target_dev: Optional[str],
    target_ssh: Optional[SshThrottleTarget],
    local_use_sudo: bool,
    elapsed_sec: float,
) -> Dict[str, Any]:
    initiator_sent = 0
    target_sent = 0
    if initiator_dev:
        initiator_sent = read_local_tc_sent_bytes(initiator_dev, local_use_sudo=local_use_sudo)
    if target_dev and target_ssh is not None:
        target_sent = read_remote_tc_sent_bytes(target_ssh, target_dev)
    total_sent = initiator_sent + target_sent
    observed_gbps = 0.0
    if elapsed_sec > 0:
        observed_gbps = (total_sent * 8.0) / elapsed_sec / 1_000_000_000.0
    return {
        "elapsed_sec": elapsed_sec,
        "initiator_egress_bytes": initiator_sent,
        "target_egress_bytes": target_sent,
        "total_shaped_egress_bytes": total_sent,
        "observed_shaped_egress_gbps": observed_gbps,
    }


def apply_bandwidth_throttle(
    *,
    gbps: float,
    initiator_dev: str,
    target_dev: Optional[str],
    target_ssh: Optional[SshThrottleTarget],
    burst_kb: int,
    latency_ms: int,
    local_use_sudo: bool,
) -> None:
    local_cmd = _tc_set_cmd(initiator_dev, gbps, burst_kb, latency_ms)
    if local_use_sudo:
        local_cmd = f"sudo -n sh -lc {shlex.quote(local_cmd)}"
    run_cmd(["bash", "-lc", local_cmd], capture_output=True)

    if target_dev:
        if target_ssh is None:
            raise ValueError("--throttle-target-dev requires target SSH configuration")
        run_ssh_shell(target_ssh, _tc_set_cmd(target_dev, gbps, burst_kb, latency_ms), check=True)


def clear_bandwidth_throttle(
    *,
    initiator_dev: Optional[str],
    target_dev: Optional[str],
    target_ssh: Optional[SshThrottleTarget],
    local_use_sudo: bool,
) -> None:
    if initiator_dev:
        local_cmd = _tc_clear_cmd(initiator_dev)
        if local_use_sudo:
            local_cmd = f"sudo -n sh -lc {shlex.quote(local_cmd)}"
        run_cmd(["bash", "-lc", local_cmd], capture_output=True, check=False)
    if target_dev and target_ssh is not None:
        run_ssh_shell(target_ssh, _tc_clear_cmd(target_dev), check=False)


def load_json(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def parse_bandwidths(text: str) -> List[float]:
    out: List[float] = []
    for raw in text.split(","):
        token = raw.strip()
        if not token:
            continue
        value = float(token)
        if value <= 0:
            raise ValueError("transport bandwidth values must be > 0")
        out.append(value)
    if not out:
        raise ValueError("at least one transport bandwidth value is required")
    return out


def align_up(value: int, align: int = DEFAULT_LBA_ALIGN) -> int:
    if align <= 0:
        raise ValueError("align must be > 0")
    return ((value + align - 1) // align) * align


def safe_div(num: float, den: float) -> float:
    if den == 0:
        return 0.0
    return num / den


def percentile(values: Sequence[float], q: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return float(values[0])
    ordered = sorted(float(v) for v in values)
    rank = (len(ordered) - 1) * max(0.0, min(1.0, q))
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return ordered[lo]
    frac = rank - lo
    return ordered[lo] + (ordered[hi] - ordered[lo]) * frac


def summarize_lat_us(values: Sequence[float]) -> Dict[str, float]:
    if not values:
        return {"p50": 0.0, "p95": 0.0, "p99": 0.0, "avg": 0.0}
    vals = [float(v) for v in values]
    return {
        "p50": percentile(vals, 0.50),
        "p95": percentile(vals, 0.95),
        "p99": percentile(vals, 0.99),
        "avg": sum(vals) / len(vals),
    }


def gib_to_bytes(gib: float) -> int:
    if gib <= 0:
        raise ValueError("GiB value must be > 0")
    return int(gib * float(1 << 30))


def gbps_to_bytes_per_sec(gbps: float) -> float:
    return (gbps * 1_000_000_000.0) / 8.0


def layout_for_count(
    count: int,
    dim: int,
    *,
    output_length: int,
    align: int = DEFAULT_LBA_ALIGN,
) -> Dict[str, int]:
    if count <= 0:
        raise ValueError("count must be > 0")
    if dim <= 0:
        raise ValueError("dim must be > 0")
    if output_length <= 0:
        raise ValueError("output_length must be > 0")

    metadata_offset = 0
    metadata_bytes = count * METADATA_STRIDE_BYTES
    vector_offset = align_up(metadata_offset + metadata_bytes, align)
    vector_bytes = count * dim * 4
    output_offset = align_up(vector_offset + vector_bytes, align)
    required_bytes = output_offset + output_length

    return {
        "metadata_offset": metadata_offset,
        "metadata_bytes": metadata_bytes,
        "vector_offset": vector_offset,
        "vector_bytes": vector_bytes,
        "output_offset": output_offset,
        "output_length": output_length,
        "required_bytes": required_bytes,
    }


def max_count_fit_slm(slm_bytes: int, dim: int, output_length: int) -> int:
    if slm_bytes <= 0:
        raise ValueError("slm_bytes must be > 0")
    lo = 1
    hi = max(1, slm_bytes // max(1, METADATA_STRIDE_BYTES + dim * 4))
    best = 0
    while lo <= hi:
        mid = (lo + hi) // 2
        required = layout_for_count(mid, dim, output_length=output_length)["required_bytes"]
        if required <= slm_bytes:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def desired_count_for_tier(spec: WorkloadSpec, tier: str) -> int:
    if tier == "T3":
        return spec.canonical_count
    if tier in TIER_COUNTS:
        return int(TIER_COUNTS[tier])
    raise ValueError(f"unsupported tier: {tier}")


def choose_effective_count(
    spec: WorkloadSpec,
    *,
    slm_size_mb: int,
    dataset_target_gib: float,
    output_length: int,
    tier: str,
    count_override: Optional[int],
) -> Tuple[int, int, int]:
    slm_bytes = slm_size_mb * (1 << 20)
    max_fit = max_count_fit_slm(slm_bytes, spec.dim, output_length)
    if max_fit <= 0:
        raise ValueError(f"{spec.name}: cannot fit even one record into SLM {slm_size_mb} MiB")

    if count_override is not None and count_override > 0:
        desired = count_override
    else:
        by_tier = desired_count_for_tier(spec, tier)
        by_target = gib_to_bytes(dataset_target_gib) // (METADATA_STRIDE_BYTES + spec.dim * 4)
        desired = max(1, min(by_tier, int(by_target)))

    effective = min(desired, max_fit)
    if effective <= 0:
        raise ValueError(f"{spec.name}: effective count is 0")
    return desired, max_fit, effective


def extract_query_bytes(row: Dict[str, Any], *, device: bool) -> float:
    if device:
        stats = row.get("stats", {})
        return float(stats.get("host_req_bytes", 0.0)) + float(stats.get("host_resp_bytes", 0.0))
    return float(row.get("host_req_bytes", 0.0)) + float(row.get("host_resp_bytes", 0.0))


def summarize_outer_requests(per_query: Sequence[Dict[str, Any]], *, device: bool) -> Dict[str, Any]:
    by_outer: Dict[int, Dict[str, float]] = {}
    max_round = 0
    for row in per_query:
        outer_id = int(row.get("outer_request_id", row.get("query_id", 0)))
        round_id = int(row.get("round", 0))
        max_round = max(max_round, round_id)

        bucket = by_outer.setdefault(outer_id, {"lat_us": 0.0, "bytes": 0.0, "count": 0.0})
        bucket["lat_us"] += float(row.get("elapsed_us", 0.0))
        bucket["bytes"] += extract_query_bytes(row, device=device)
        bucket["count"] += 1.0

    expected_rounds = max_round + 1
    latencies: List[float] = []
    bytes_list: List[float] = []
    for agg in by_outer.values():
        if int(agg["count"]) < expected_rounds:
            continue
        latencies.append(float(agg["lat_us"]))
        bytes_list.append(float(agg["bytes"]))

    return {
        "expected_rounds": expected_rounds,
        "outer_request_count": len(latencies),
        "latency_us": summarize_lat_us(latencies),
        "avg_total_bytes": (sum(bytes_list) / len(bytes_list)) if bytes_list else 0.0,
    }


def build_transport_projection(
    host_bytes: float,
    cpcs_bytes: float,
    host_latency_p50_us: float,
    cpcs_latency_p50_us: float,
    bandwidths_gbps: Sequence[float],
) -> List[Dict[str, float]]:
    rows: List[Dict[str, float]] = []
    for link in bandwidths_gbps:
        bw_bps = gbps_to_bytes_per_sec(link)
        host_transport_us = safe_div(host_bytes, bw_bps) * 1_000_000.0
        cpcs_transport_us = safe_div(cpcs_bytes, bw_bps) * 1_000_000.0
        host_projected = host_latency_p50_us + host_transport_us
        cpcs_projected = cpcs_latency_p50_us + cpcs_transport_us
        rows.append(
            {
                "bandwidth_gbps": float(link),
                "host_transport_us": host_transport_us,
                "cpcs_transport_us": cpcs_transport_us,
                "transport_speedup_x": safe_div(host_transport_us, cpcs_transport_us),
                "host_projected_p50_us": host_projected,
                "cpcs_projected_p50_us": cpcs_projected,
                "projected_speedup_x": safe_div(host_projected, cpcs_projected),
            }
        )
    return rows


def write_csv(path: Path, rows: Sequence[Dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames = sorted({key for row in rows for key in row.keys()})
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def resolve_workloads(text: str) -> List[WorkloadSpec]:
    token_map = dict(WORKLOAD_SPECS)
    token_map.update({spec.profile: spec for spec in WORKLOAD_SPECS.values()})
    out: List[WorkloadSpec] = []
    for raw in text.split(","):
        token = raw.strip()
        if not token:
            continue
        if token == "all":
            return [WORKLOAD_SPECS[k] for k in ("mongo", "pinecone_yfcc", "pinecone_customer", "elastic_agentic")]
        if token not in token_map:
            raise ValueError(
                f"unknown workload token: {token}. "
                f"Use one of {', '.join(sorted(WORKLOAD_SPECS.keys()))}, profile names, or all."
            )
        out.append(token_map[token])
    if not out:
        raise ValueError("no workloads selected")

    uniq: List[WorkloadSpec] = []
    seen: Set[str] = set()
    for spec in out:
        if spec.name in seen:
            continue
        seen.add(spec.name)
        uniq.append(spec)
    return uniq


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    default_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(
        description=(
            "Run CPCS effectiveness experiments for workload profiles in 04_WORKLOAD_SPEC.md "
            "on split initiator/target topology, and export byte-reduction + bottleneck-latency reports."
        )
    )
    parser.add_argument("--inventory", required=True, help="Inventory YAML (for example test/cpcs/examples/inventory_split.yaml)")
    parser.add_argument("--spdk-root", default=str(default_root), help="SPDK repo root")
    parser.add_argument(
        "--cpcs-experiments",
        default="test/cpcs/cpcs_experiments.py",
        help="Path to cpcs_experiments.py (absolute or relative to --spdk-root)",
    )
    parser.add_argument("--output-dir", default="/tmp/cpcs_workload_effectiveness", help="Output directory")
    parser.add_argument("--workloads", default="all", help="Comma-separated workload names or profiles, or all")
    parser.add_argument("--tier", choices=["T0", "T1", "T2", "T3"], default="T1", help="Target dataset tier")
    parser.add_argument(
        "--dataset-target-gib",
        type=float,
        default=3.8,
        help="Approximate per-workload dataset footprint target in GiB (auto-capped by SLM size)",
    )
    parser.add_argument("--count-override", type=int, default=None, help="Override record count for all workloads")
    parser.add_argument("--slm-size-mb", type=int, default=4096, help="SLM size in MiB (default: 4096)")
    parser.add_argument("--backend", choices=["pslm", "vslm"], default="pslm", help="Near-storage backend")
    parser.add_argument("--dataset-size-mb", type=int, default=0, help="dataset-bdev malloc size in MiB (0 disables)")
    parser.add_argument("--output-length", type=int, default=DEFAULT_OUTPUT_LENGTH, help="Result buffer length in bytes")
    parser.add_argument("--query-limit", type=int, default=200, help="Queries to evaluate per workload")
    parser.add_argument("--query-count", type=int, default=300, help="Queries to generate per workload")
    parser.add_argument("--seed", type=int, default=20260409, help="Dataset/query seed")
    parser.add_argument("--transport-bandwidth-gbps", default="1,5,10,25,100", help="Bandwidths for bottleneck projection")
    parser.add_argument(
        "--measure-real-bandwidth",
        action="store_true",
        help="Apply real Linux tc bandwidth shaping and measure latency at each bandwidth point",
    )
    parser.add_argument(
        "--throttle-initiator-dev",
        default=None,
        help="Initiator netdev to shape (default: auto infer from inventory nvmeof.traddr route)",
    )
    parser.add_argument(
        "--throttle-target-dev",
        default=None,
        help="Optional target netdev to shape over SSH (recommended for throttling target->initiator data path)",
    )
    parser.add_argument("--throttle-target-ssh-host", default=None, help="Optional target SSH host override for tc shaping")
    parser.add_argument("--throttle-target-ssh-user", default=None, help="Optional target SSH user override for tc shaping")
    parser.add_argument("--throttle-target-ssh-port", type=int, default=22, help="Optional target SSH port override for tc shaping")
    parser.add_argument(
        "--throttle-target-ssh-option",
        action="append",
        default=[],
        help="Additional -o option for target SSH shaping path (repeatable)",
    )
    parser.add_argument("--throttle-local-no-sudo", action="store_true", help="Run local tc without sudo -n")
    parser.add_argument("--throttle-target-no-sudo", action="store_true", help="Run remote tc without sudo -n")
    parser.add_argument("--throttle-burst-kb", type=int, default=256, help="tc tbf burst in KiB")
    parser.add_argument("--throttle-latency-ms", type=int, default=100, help="tc tbf latency in ms")
    parser.add_argument("--target-log", default="/var/log/spdk.log", help="spdk_tgt log path")
    parser.add_argument("--prefill-workers", type=int, default=None, help="Override prefill worker count")
    parser.add_argument("--passthru-lcores", default=None, help="Override passthru lcores")
    parser.add_argument("--target-core-mask", default=None, help="Override target core mask")
    parser.add_argument(
        "--exactness-score-tol",
        type=float,
        default=1e-4,
        help="Exactness score tolerance passed to vector-eval",
    )
    parser.add_argument(
        "--exactness-strict-order",
        action="store_true",
        help="Require identical top-k ordering during vector-eval exactness checks",
    )
    parser.add_argument(
        "--prefill-cmd-timeout-sec",
        type=float,
        default=300.0,
        help="Timeout for each passthru prefill command",
    )
    parser.add_argument(
        "--extra-vector-arg",
        action="append",
        default=[],
        help="Extra arg forwarded to vector-eval (repeatable)",
    )
    parser.add_argument(
        "--continue-on-failure",
        action="store_true",
        help="Continue remaining workloads and write partial outputs if one workload fails",
    )
    return parser.parse_args(argv)


def run_one_workload(
    args: argparse.Namespace,
    spec: WorkloadSpec,
    *,
    spdk_root: Path,
    inventory_path: Path,
    cpcs_experiments_path: Path,
    bandwidths_gbps: Sequence[float],
    output_dir: Path,
    run_suffix: Optional[str] = None,
    bandwidth_gbps: Optional[float] = None,
) -> Dict[str, Any]:
    desired_count, max_fit_count, effective_count = choose_effective_count(
        spec,
        slm_size_mb=int(args.slm_size_mb),
        dataset_target_gib=float(args.dataset_target_gib),
        output_length=int(args.output_length),
        tier=str(args.tier),
        count_override=args.count_override,
    )
    layout = layout_for_count(effective_count, spec.dim, output_length=int(args.output_length))

    run_dir = output_dir / spec.name
    if run_suffix:
        run_dir = run_dir / run_suffix
    run_dir.mkdir(parents=True, exist_ok=True)
    run_log = run_dir / "run.log"
    artifacts_root = run_dir / "artifacts"

    print(
        f"\n=== workload={spec.name} profile={spec.profile} count={effective_count} dim={spec.dim} "
        f"(desired={desired_count}, fit_max={max_fit_count}) ===",
        flush=True,
    )

    query_count = max(int(args.query_count), int(args.query_limit))
    scenario_args: List[str] = [
        "--profile", spec.profile,
        "--tier", args.tier,
        "--count", str(effective_count),
        "--dim", str(spec.dim),
        "--query-count", str(query_count),
        "--query-limit", str(args.query_limit),
        "--seed", str(args.seed),
        "--backend", args.backend,
        "--slm-size-mb", str(args.slm_size_mb),
        "--dataset-size-mb", str(args.dataset_size_mb),
        "--metadata-offset", str(layout["metadata_offset"]),
        "--vector-offset", str(layout["vector_offset"]),
        "--output-offset", str(layout["output_offset"]),
        "--output-length", str(layout["output_length"]),
        "--exactness-score-tol", str(args.exactness_score_tol),
        "--prefill-cmd-timeout-sec", str(args.prefill_cmd_timeout_sec),
    ]
    if args.exactness_strict_order:
        scenario_args.append("--exactness-strict-order")
    if args.prefill_workers is not None:
        scenario_args.extend(["--prefill-workers", str(args.prefill_workers)])
    if args.passthru_lcores:
        scenario_args.extend(["--passthru-lcores", str(args.passthru_lcores)])
    if args.target_core_mask:
        scenario_args.extend(["--target-core-mask", str(args.target_core_mask)])
    scenario_args.extend(args.extra_vector_arg)

    cmd = [
        "python3",
        str(cpcs_experiments_path),
        "--inventory", str(inventory_path),
        "--artifacts-root", str(artifacts_root),
        "--target-log", str(args.target_log),
        "--run-log", str(run_log),
        "vector-eval",
        "--",
        *scenario_args,
    ]
    run_cmd(cmd, cwd=spdk_root)

    run_config_path = artifacts_root / "vector_eval" / "run_config.json"
    if not run_config_path.exists():
        raise FileNotFoundError(f"run_config.json not found: {run_config_path}")
    run_config = load_json(run_config_path)

    k2_h0_path = Path(run_config["artifacts"]["k2_h0"])
    k2_c0_path = Path(run_config["artifacts"]["k2_c0"])
    k2_h0 = load_json(k2_h0_path)
    k2_c0 = load_json(k2_c0_path)

    host_summary = k2_h0["summary"]
    cpcs_summary = k2_c0["summary"]
    host_bytes = float(host_summary["avg_host_total_bytes"])
    cpcs_bytes = float(cpcs_summary["avg_host_total_bytes"])
    host_p50 = float(host_summary["latency_us"]["p50"])
    cpcs_p50 = float(cpcs_summary["latency_us"]["p50"])

    k2_projection = build_transport_projection(
        host_bytes,
        cpcs_bytes,
        host_p50,
        cpcs_p50,
        bandwidths_gbps,
    )

    k2_payload: Dict[str, Any] = {
        "host_avg_total_bytes": host_bytes,
        "cpcs_avg_total_bytes": cpcs_bytes,
        "byte_reduction_pct": safe_div(host_bytes - cpcs_bytes, host_bytes) * 100.0,
        "host_latency_p50_us": host_p50,
        "cpcs_latency_p50_us": cpcs_p50,
        "measured_speedup_x": safe_div(host_p50, cpcs_p50),
        "transport_projection": k2_projection,
    }

    host_outer = summarize_outer_requests(k2_h0["per_query"], device=False)
    cpcs_outer = summarize_outer_requests(k2_c0["per_query"], device=True)
    k3_payload = None
    if host_outer["expected_rounds"] > 1 and host_outer["outer_request_count"] > 0 and cpcs_outer["outer_request_count"] > 0:
        host_outer_bytes = float(host_outer["avg_total_bytes"])
        cpcs_outer_bytes = float(cpcs_outer["avg_total_bytes"])
        host_outer_p50 = float(host_outer["latency_us"]["p50"])
        cpcs_outer_p50 = float(cpcs_outer["latency_us"]["p50"])
        k3_payload = {
            "expected_rounds": int(host_outer["expected_rounds"]),
            "host_outer_avg_total_bytes": host_outer_bytes,
            "cpcs_outer_avg_total_bytes": cpcs_outer_bytes,
            "byte_reduction_pct": safe_div(host_outer_bytes - cpcs_outer_bytes, host_outer_bytes) * 100.0,
            "host_outer_latency_p50_us": host_outer_p50,
            "cpcs_outer_latency_p50_us": cpcs_outer_p50,
            "measured_speedup_x": safe_div(host_outer_p50, cpcs_outer_p50),
            "transport_projection": build_transport_projection(
                host_outer_bytes,
                cpcs_outer_bytes,
                host_outer_p50,
                cpcs_outer_p50,
                bandwidths_gbps,
            ),
        }

    return {
        "name": spec.name,
        "profile": spec.profile,
        "bandwidth_gbps": bandwidth_gbps,
        "dim": spec.dim,
        "k": spec.k,
        "dataset": {
            "tier": args.tier,
            "canonical_count": spec.canonical_count,
            "desired_count": desired_count,
            "max_fit_count": max_fit_count,
            "effective_count": effective_count,
            "slm_size_mb": args.slm_size_mb,
            "dataset_target_gib": args.dataset_target_gib,
            "layout": layout,
        },
        "k2": k2_payload,
        "k3": k3_payload,
        "artifacts": {
            "run_log": str(run_log),
            "run_config_json": str(run_config_path),
            "k2_h0_json": str(k2_h0_path),
            "k2_c0_json": str(k2_c0_path),
        },
    }


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    spdk_root = Path(args.spdk_root).resolve()
    inventory_path = Path(args.inventory).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    workloads = resolve_workloads(args.workloads)
    bandwidths_gbps = parse_bandwidths(args.transport_bandwidth_gbps)

    cpcs_experiments_path = Path(args.cpcs_experiments)
    if not cpcs_experiments_path.is_absolute():
        cpcs_experiments_path = (spdk_root / cpcs_experiments_path).resolve()

    if not inventory_path.exists():
        raise FileNotFoundError(f"inventory not found: {inventory_path}")
    if not cpcs_experiments_path.exists():
        raise FileNotFoundError(f"cpcs_experiments.py not found: {cpcs_experiments_path}")

    inventory_data: Dict[str, Any] = {}
    if args.measure_real_bandwidth:
        inventory_data = load_inventory_yaml(inventory_path)

    print(f"SPDK root: {spdk_root}")
    print(f"Inventory: {inventory_path}")
    print(f"Output:    {output_dir}")
    print(f"Backend:   {args.backend}, SLM={args.slm_size_mb} MiB", flush=True)

    runs: List[Dict[str, Any]] = []
    failures: List[Dict[str, Any]] = []
    initiator_dev: Optional[str] = None
    target_dev: Optional[str] = None
    target_ssh: Optional[SshThrottleTarget] = None
    if args.measure_real_bandwidth:
        if args.throttle_burst_kb <= 0 or args.throttle_latency_ms <= 0:
            raise ValueError("--throttle-burst-kb and --throttle-latency-ms must be > 0")
        nvmeof = inventory_data.get("nvmeof", {}) if isinstance(inventory_data.get("nvmeof"), dict) else {}
        traddr = nvmeof.get("traddr")
        if args.throttle_initiator_dev:
            initiator_dev = str(args.throttle_initiator_dev)
            initiator_src_ip = None
        else:
            if not traddr:
                raise ValueError("cannot auto-infer initiator dev: inventory nvmeof.traddr is missing")
            initiator_dev, initiator_src_ip = infer_local_route_info(str(traddr))
        target_dev = str(args.throttle_target_dev) if args.throttle_target_dev else None
        target_ssh = resolve_target_ssh_for_throttle(args, inventory_data)
        target_peer_ip: Optional[str] = None

        print(
            f"[throttle] real bandwidth mode enabled; initiator dev={initiator_dev}"
            + (f", target dev={target_dev}" if target_dev else ", target dev=<disabled>"),
            flush=True,
        )
        if target_dev is None and target_ssh is not None:
            # On target side, shape toward initiator source IP.
            # Prefer explicit inventory src_addr; fall back to local route-selected source IP.
            inv_src_addr = nvmeof.get("src_addr")
            if inv_src_addr:
                target_peer_ip = str(inv_src_addr)
            elif initiator_src_ip:
                target_peer_ip = str(initiator_src_ip)
            if target_peer_ip:
                try:
                    target_dev = infer_remote_route_dev(target_ssh, target_peer_ip)
                    print(
                        f"[throttle] auto-inferred target dev={target_dev} using peer ip={target_peer_ip}",
                        flush=True,
                    )
                except Exception as exc:
                    print(f"[throttle] warning: failed to auto-infer target dev: {exc}", flush=True)
            else:
                print(
                    "[throttle] warning: cannot infer target dev; no inventory nvmeof.src_addr and no local route source ip",
                    flush=True,
                )
        if target_dev and target_ssh is None:
            raise ValueError(
                "target throttling requested but target SSH configuration is unavailable; "
                "set --throttle-target-ssh-* or use SSH target inventory."
            )
        if not target_dev:
            print(
                "[throttle] warning: only initiator egress is shaped. "
                "For read-heavy NVMe/TCP workloads, also set --throttle-target-dev for accurate bottleneck emulation.",
                flush=True,
            )

    try:
        if args.measure_real_bandwidth:
            for spec in workloads:
                for bw in bandwidths_gbps:
                    suffix = f"bw_{str(bw).replace('.', 'p')}"
                    t0 = time.perf_counter()
                    try:
                        apply_bandwidth_throttle(
                            gbps=float(bw),
                            initiator_dev=str(initiator_dev),
                            target_dev=target_dev,
                            target_ssh=target_ssh,
                            burst_kb=int(args.throttle_burst_kb),
                            latency_ms=int(args.throttle_latency_ms),
                            local_use_sudo=(not args.throttle_local_no_sudo),
                        )
                        run_payload = run_one_workload(
                            args,
                            spec,
                            spdk_root=spdk_root,
                            inventory_path=inventory_path,
                            cpcs_experiments_path=cpcs_experiments_path,
                            bandwidths_gbps=[bw],
                            output_dir=output_dir,
                            run_suffix=suffix,
                            bandwidth_gbps=float(bw),
                        )
                        elapsed_sec = time.perf_counter() - t0
                        run_payload["throttle_stats"] = collect_throttle_stats(
                            initiator_dev=initiator_dev,
                            target_dev=target_dev,
                            target_ssh=target_ssh,
                            local_use_sudo=(not args.throttle_local_no_sudo),
                            elapsed_sec=elapsed_sec,
                        )
                        runs.append(run_payload)
                    except Exception as exc:
                        elapsed_sec = time.perf_counter() - t0
                        stats = collect_throttle_stats(
                            initiator_dev=initiator_dev,
                            target_dev=target_dev,
                            target_ssh=target_ssh,
                            local_use_sudo=(not args.throttle_local_no_sudo),
                            elapsed_sec=elapsed_sec,
                        )
                        failure = {
                            "workload": spec.name,
                            "profile": spec.profile,
                            "bandwidth_gbps": float(bw),
                            "error": str(exc),
                            "run_log": str((output_dir / spec.name / suffix / "run.log").resolve()),
                            **stats,
                        }
                        failures.append(failure)
                        print(f"[workload-failed] {spec.name}@{bw}Gbps: {exc}", flush=True)
                        if not args.continue_on_failure:
                            raise
                    finally:
                        clear_bandwidth_throttle(
                            initiator_dev=initiator_dev,
                            target_dev=target_dev,
                            target_ssh=target_ssh,
                            local_use_sudo=(not args.throttle_local_no_sudo),
                        )
        else:
            for spec in workloads:
                try:
                    runs.append(
                        run_one_workload(
                            args,
                            spec,
                            spdk_root=spdk_root,
                            inventory_path=inventory_path,
                            cpcs_experiments_path=cpcs_experiments_path,
                            bandwidths_gbps=bandwidths_gbps,
                            output_dir=output_dir,
                        )
                    )
                except Exception as exc:
                    failure = {
                        "workload": spec.name,
                        "profile": spec.profile,
                        "error": str(exc),
                        "run_log": str((output_dir / spec.name / "run.log").resolve()),
                    }
                    failures.append(failure)
                    print(f"[workload-failed] {spec.name}: {exc}", flush=True)
                    if not args.continue_on_failure:
                        raise
    finally:
        if args.measure_real_bandwidth:
            clear_bandwidth_throttle(
                initiator_dev=initiator_dev,
                target_dev=target_dev,
                target_ssh=target_ssh,
                local_use_sudo=(not args.throttle_local_no_sudo),
            )

    summary_rows: List[Dict[str, Any]] = []
    measured_rows: List[Dict[str, Any]] = []
    projection_rows: List[Dict[str, Any]] = []
    for run in runs:
        k2 = run["k2"]
        throttle_stats = run.get("throttle_stats", {})
        k2_summary = {
            "bandwidth_gbps": run["bandwidth_gbps"],
            "workload": run["name"],
            "profile": run["profile"],
            "kernel": "K2",
            "count": run["dataset"]["effective_count"],
            "dim": run["dim"],
            "host_avg_total_bytes": k2["host_avg_total_bytes"],
            "cpcs_avg_total_bytes": k2["cpcs_avg_total_bytes"],
            "byte_reduction_pct": k2["byte_reduction_pct"],
            "host_latency_p50_us": k2["host_latency_p50_us"],
            "cpcs_latency_p50_us": k2["cpcs_latency_p50_us"],
            "measured_speedup_x": k2["measured_speedup_x"],
            "observed_shaped_egress_gbps": throttle_stats.get("observed_shaped_egress_gbps"),
        }
        summary_rows.append(k2_summary)
        if args.measure_real_bandwidth:
            measured_rows.append(
                {
                    "bandwidth_gbps": run["bandwidth_gbps"],
                    "kernel": "K2",
                    "profile": run["profile"],
                    "workload": run["name"],
                    "host_latency_p50_us": k2["host_latency_p50_us"],
                    "cpcs_latency_p50_us": k2["cpcs_latency_p50_us"],
                    "measured_speedup_x": k2["measured_speedup_x"],
                    "host_avg_total_bytes": k2["host_avg_total_bytes"],
                    "cpcs_avg_total_bytes": k2["cpcs_avg_total_bytes"],
                    "byte_reduction_pct": k2["byte_reduction_pct"],
                    "elapsed_sec": throttle_stats.get("elapsed_sec"),
                    "initiator_egress_bytes": throttle_stats.get("initiator_egress_bytes"),
                    "target_egress_bytes": throttle_stats.get("target_egress_bytes"),
                    "total_shaped_egress_bytes": throttle_stats.get("total_shaped_egress_bytes"),
                    "observed_shaped_egress_gbps": throttle_stats.get("observed_shaped_egress_gbps"),
                }
            )
        for proj in k2["transport_projection"]:
            projection_rows.append(
                {
                    "bandwidth_gbps_run": run["bandwidth_gbps"],
                    "workload": run["name"],
                    "profile": run["profile"],
                    "kernel": "K2",
                    **proj,
                }
            )

        if run["k3"] is not None:
            k3 = run["k3"]
            k3_summary = {
                "bandwidth_gbps": run["bandwidth_gbps"],
                "workload": run["name"],
                "profile": run["profile"],
                "kernel": "K3",
                "count": run["dataset"]["effective_count"],
                "dim": run["dim"],
                "host_avg_total_bytes": k3["host_outer_avg_total_bytes"],
                "cpcs_avg_total_bytes": k3["cpcs_outer_avg_total_bytes"],
                "byte_reduction_pct": k3["byte_reduction_pct"],
                "host_latency_p50_us": k3["host_outer_latency_p50_us"],
                "cpcs_latency_p50_us": k3["cpcs_outer_latency_p50_us"],
                "measured_speedup_x": k3["measured_speedup_x"],
                "observed_shaped_egress_gbps": throttle_stats.get("observed_shaped_egress_gbps"),
            }
            summary_rows.append(k3_summary)
            if args.measure_real_bandwidth:
                measured_rows.append(
                    {
                        "bandwidth_gbps": run["bandwidth_gbps"],
                        "kernel": "K3",
                        "profile": run["profile"],
                        "workload": run["name"],
                        "host_latency_p50_us": k3["host_outer_latency_p50_us"],
                        "cpcs_latency_p50_us": k3["cpcs_outer_latency_p50_us"],
                        "measured_speedup_x": k3["measured_speedup_x"],
                        "host_avg_total_bytes": k3["host_outer_avg_total_bytes"],
                        "cpcs_avg_total_bytes": k3["cpcs_outer_avg_total_bytes"],
                        "byte_reduction_pct": k3["byte_reduction_pct"],
                        "elapsed_sec": throttle_stats.get("elapsed_sec"),
                        "initiator_egress_bytes": throttle_stats.get("initiator_egress_bytes"),
                        "target_egress_bytes": throttle_stats.get("target_egress_bytes"),
                        "total_shaped_egress_bytes": throttle_stats.get("total_shaped_egress_bytes"),
                        "observed_shaped_egress_gbps": throttle_stats.get("observed_shaped_egress_gbps"),
                    }
                )
            for proj in k3["transport_projection"]:
                projection_rows.append(
                    {
                        "bandwidth_gbps_run": run["bandwidth_gbps"],
                        "workload": run["name"],
                        "profile": run["profile"],
                        "kernel": "K3",
                        **proj,
                    }
                )

    payload = {
        "config": {
            "inventory": str(inventory_path),
            "spdk_root": str(spdk_root),
            "backend": args.backend,
            "slm_size_mb": args.slm_size_mb,
            "dataset_target_gib": args.dataset_target_gib,
            "tier": args.tier,
            "query_limit": args.query_limit,
            "query_count": args.query_count,
            "transport_bandwidth_gbps": bandwidths_gbps,
            "output_dir": str(output_dir),
            "continue_on_failure": bool(args.continue_on_failure),
            "measure_real_bandwidth": bool(args.measure_real_bandwidth),
            "throttle": {
                "initiator_dev": initiator_dev,
                "target_dev": target_dev,
                "target_ssh_host": (target_ssh.host if target_ssh else None),
                "local_use_sudo": (not args.throttle_local_no_sudo),
                "target_use_sudo": (target_ssh.use_sudo if target_ssh else None),
                "burst_kb": int(args.throttle_burst_kb),
                "latency_ms": int(args.throttle_latency_ms),
            },
        },
        "runs": runs,
        "failures": failures,
        "summary_rows": summary_rows,
        "measured_rows": measured_rows,
        "projection_rows": projection_rows,
    }

    summary_json = output_dir / "summary.json"
    summary_csv = output_dir / "summary.csv"
    measured_csv = output_dir / "measured_bandwidth.csv"
    projection_csv = output_dir / "projection.csv"
    failures_csv = output_dir / "failures.csv"
    summary_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    write_csv(summary_csv, summary_rows)
    write_csv(measured_csv, measured_rows)
    write_csv(projection_csv, projection_rows)
    write_csv(failures_csv, failures)

    print("\nSummary (host vs CPCS)")
    print("workload           kernel  byte_reduction_pct  measured_speedup_x")
    for row in summary_rows:
        print(
            f"{row['workload']:<18} {row['kernel']:<6} "
            f"{row['byte_reduction_pct']:>18.2f} {row['measured_speedup_x']:>19.2f}"
        )

    print(f"\nWrote JSON: {summary_json}")
    print(f"Wrote CSV:  {summary_csv}")
    if measured_rows:
        print(f"Wrote CSV:  {measured_csv}")
    print(f"Wrote CSV:  {projection_csv}")
    if failures:
        print(f"Wrote CSV:  {failures_csv}")
        print(f"Completed with failures: {len(failures)} workload(s)")
    if failures and not runs:
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

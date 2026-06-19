#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import re
import shlex
import subprocess
from typing import Any, Dict, List, Sequence

from experiment_platform import PlatformContext, normalize_scenario_args, resolve_bin


REALAPP_SYSTEMS = ("opensearch", "elasticsearch", "qdrant", "milvus")
HYBRID_MATRIX_RUNNER_NAME = "run_real_app_hybrid_runner.py"
HYBRID_MATRIX_RUNNER_REL = f"experiments/real_apps/common/{HYBRID_MATRIX_RUNNER_NAME}"
DIRECT_CPCS_RUNNER_NAME = "run_real_app_cpcs_direct_passthru.py"
DIRECT_CPCS_RUNNER_REL = f"experiments/real_apps/common/{DIRECT_CPCS_RUNNER_NAME}"

DEFAULT_HOST_BY_SYSTEM = {
    "opensearch": "http://127.0.0.1:9200",
    "elasticsearch": "http://127.0.0.1:9201",
    "qdrant": "http://127.0.0.1:6333",
    "milvus": "127.0.0.1",
}

DEFAULT_PORT_BY_SYSTEM = {
    "milvus": 19530,
}


def _nsid_from_bdev_name(bdev_name: str) -> int | None:
    m = re.search(r"[Nn](\d+)$", str(bdev_name))
    if not m:
        return None
    value = int(m.group(1))
    if value <= 0:
        return None
    return value


def _run_cmd(*, cmd: List[str], cwd: Path, log_path: Path, check: bool = True) -> subprocess.CompletedProcess:
    print(f"+ {' '.join(shlex.quote(token) for token in cmd)}")
    cp = subprocess.run(cmd, cwd=str(cwd), text=True, capture_output=True)
    combined = (
        f"$ {' '.join(shlex.quote(token) for token in cmd)}\n"
        f"[stdout]\n{cp.stdout}\n"
        f"[stderr]\n{cp.stderr}\n"
    )
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(combined, encoding="utf-8")
    if check and cp.returncode != 0:
        raise RuntimeError(
            f"Command failed (rc={cp.returncode}): {' '.join(shlex.quote(token) for token in cmd)}\n"
            f"See log: {log_path}"
        )
    return cp


def _read_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _parse_modes(text: str) -> List[str]:
    return [token.strip() for token in text.split(",") if token.strip()]


def _parse_bandwidths(text: str) -> List[float]:
    out: List[float] = []
    for token in text.split(","):
        value = float(token.strip())
        if value <= 0:
            raise ValueError("bandwidth values must be > 0")
        out.append(value)
    return out


def _parse_int_values(text: str) -> List[int]:
    out: List[int] = []
    for token in text.split(","):
        value = int(token.strip())
        out.append(value)
    return out


def _append_shell_args(base_args: str, *tokens: str) -> str:
    parts: List[str] = []
    base = str(base_args or "").strip()
    if base:
        parts.append(base)

    extras = [shlex.quote(str(token)) for token in tokens if str(token) != ""]
    if extras:
        parts.append(" ".join(extras))
    return " ".join(parts).strip()


def _apply_hybrid_direct_defaults(ctx: PlatformContext, args: argparse.Namespace) -> None:
    inv = ctx.inventory
    runtime = inv.runtime
    initiator_repo = str(ctx.initiator.spec.repo_path)

    if not str(getattr(args, "spdk_nvme_passthru", "") or "").strip():
        args.spdk_nvme_passthru = resolve_bin(initiator_repo, inv.paths.spdk_nvme_passthru)
    if not str(getattr(args, "trtype", "") or "").strip():
        args.trtype = inv.nvmeof.trtype
    if not str(getattr(args, "traddr", "") or "").strip():
        args.traddr = inv.nvmeof.traddr
    if not str(getattr(args, "trsvcid", "") or "").strip():
        args.trsvcid = inv.nvmeof.trsvcid
    if not str(getattr(args, "subnqn", "") or "").strip():
        args.subnqn = inv.nvmeof.nqn
    if not str(getattr(args, "hostnqn", "") or "").strip():
        args.hostnqn = inv.nvmeof.hostnqn
    if not str(getattr(args, "src_addr", "") or "").strip():
        args.src_addr = str(inv.nvmeof.src_addr or "")
    if not str(getattr(args, "src_svcid", "") or "").strip():
        args.src_svcid = str(inv.nvmeof.src_svcid or "")
    if not str(getattr(args, "passthru_lcores", "") or "").strip():
        args.passthru_lcores = str(runtime.initiator_passthru_lcores or "1")

    configured_probe_nsid = int(getattr(args, "direct_probe_nsid", 0) or 0)
    if configured_probe_nsid <= 0:
        runtime_probe_nsid = int(getattr(runtime, "direct_probe_nsid", 0) or 0)
        dataset_probe_nsid = _nsid_from_bdev_name(str(inv.nvmeof.dataset_bdev or ""))
        args.direct_probe_nsid = (
            runtime_probe_nsid
            if runtime_probe_nsid > 0
            else (dataset_probe_nsid if dataset_probe_nsid is not None else 1)
        )

    configured_probe_lba = int(getattr(args, "direct_probe_lba_bytes", 0) or 0)
    if configured_probe_lba <= 0:
        runtime_probe_lba = int(getattr(runtime, "direct_probe_lba_bytes", 0) or 0)
        args.direct_probe_lba_bytes = runtime_probe_lba if runtime_probe_lba > 0 else 4096


def _choose_compare_mode(modes: Sequence[str], *, baseline_mode: str) -> str:
    if "cpcs_vslm" in modes:
        return "cpcs_vslm"
    for mode in modes:
        if mode != baseline_mode:
            return mode
    return baseline_mode


def _mode_summaries(*, output_dir: Path, app: str, modes: Sequence[str]) -> Dict[str, Dict[str, Any]]:
    out: Dict[str, Dict[str, Any]] = {}
    for mode in modes:
        summary_path = output_dir / f"{app}_{mode}_summary.json"
        out[mode] = _read_json(summary_path)
    return out


def _exactness_report(*, output_dir: Path, baseline_mode: str) -> Dict[str, Any]:
    path = output_dir / f"{baseline_mode}_id_exactness_report.json"
    if not path.exists():
        return {}
    return _read_json(path)


def _exactness_gate_pass(
    *,
    report: Dict[str, Any],
    modes: Sequence[str],
    baseline_mode: str,
    threshold: float,
) -> bool:
    non_baseline = [mode for mode in modes if mode != baseline_mode]
    if not non_baseline:
        return True
    if not report:
        return False
    for mode in non_baseline:
        ratio = float(report.get(mode, {}).get("exact_match_ratio", 0.0))
        if ratio < threshold:
            return False
    return True


def _apply_system_defaults(args: argparse.Namespace, *, scenario_name: str) -> None:
    system = str(args.system)
    if system not in REALAPP_SYSTEMS:
        raise ValueError(
            f"unsupported --system value '{system}' for {scenario_name}; "
            f"expected one of {', '.join(REALAPP_SYSTEMS)}"
        )

    if not getattr(args, "host", None):
        args.host = DEFAULT_HOST_BY_SYSTEM[system]

    if system in ("opensearch", "elasticsearch"):
        if not getattr(args, "index", None):
            raise ValueError(f"--index is required for {scenario_name} when --system {system}")

    if system in ("qdrant", "milvus"):
        if not getattr(args, "collection", None):
            raise ValueError(f"--collection is required for {scenario_name} when --system {system}")

    if system == "milvus":
        if getattr(args, "port", None) is None:
            args.port = int(DEFAULT_PORT_BY_SYSTEM["milvus"])


def _run_realapp_load(
    *,
    system: str,
    repo_root: Path,
    host: str,
    index: str | None,
    collection: str | None,
    port: int | None,
    docs_jsonl: str,
    recreate_index: bool,
    engine: str,
    space_type: str,
    log_path: Path,
    manifest_path: Path,
) -> None:
    docs_path = str(Path(docs_jsonl).resolve())
    if system == "opensearch":
        assert index is not None
        script = repo_root / "experiments" / "real_apps" / "opensearch" / "load_opensearch.py"
        cmd = [
            "python3",
            str(script),
            "--host",
            host,
            "--index",
            index,
            "--docs-jsonl",
            docs_path,
            "--engine",
            engine,
            "--space-type",
            space_type,
            "--output-manifest",
            str(manifest_path),
        ]
        if recreate_index:
            cmd.append("--recreate-index")
    elif system == "elasticsearch":
        assert index is not None
        script = repo_root / "experiments" / "real_apps" / "elasticsearch" / "load_elasticsearch.py"
        cmd = [
            "python3",
            str(script),
            "--host",
            host,
            "--index",
            index,
            "--docs-jsonl",
            docs_path,
            "--output-manifest",
            str(manifest_path),
        ]
        if recreate_index:
            cmd.append("--recreate-index")
    elif system == "qdrant":
        assert collection is not None
        script = repo_root / "experiments" / "real_apps" / "qdrant" / "load_qdrant.py"
        cmd = [
            "python3",
            str(script),
            "--host",
            host,
            "--collection",
            collection,
            "--docs-jsonl",
            docs_path,
            "--output-manifest",
            str(manifest_path),
        ]
        if recreate_index:
            cmd.append("--recreate-collection")
    elif system == "milvus":
        assert collection is not None
        if port is None:
            raise ValueError("milvus load requires --port")
        script = repo_root / "experiments" / "real_apps" / "milvus" / "load_milvus.py"
        cmd = [
            "python3",
            str(script),
            "--host",
            host,
            "--port",
            str(int(port)),
            "--collection",
            collection,
            "--docs-jsonl",
            docs_path,
            "--output-manifest",
            str(manifest_path),
        ]
        if recreate_index:
            cmd.append("--recreate-collection")
    else:
        raise ValueError(f"unsupported system for load: {system}")

    _run_cmd(cmd=cmd, cwd=repo_root, log_path=log_path)


def _matrix_extra_args(
    *,
    system: str,
    host: str,
    index: str | None,
    collection: str | None,
    port: int | None,
    timeout_sec: float,
    allow_fallback: bool,
    capture_knn_stats: bool,
    capture_cpcs_counters: bool,
    cpcs_counters_path: str,
    configure_index_flag: bool,
    qdrant_exact: bool,
    cpcs_offload_endpoint: str,
    cpcs_offload_timeout_ms: int,
) -> str:
    tokens = ["--host", host, "--timeout-sec", str(timeout_sec)]
    if system in ("opensearch", "elasticsearch"):
        assert index is not None
        tokens.extend(["--index", index])
    if system in ("qdrant", "milvus"):
        assert collection is not None
        tokens.extend(["--collection", collection])
    if system == "milvus":
        if port is None:
            raise ValueError("milvus matrix arguments require --port")
        tokens.extend(["--port", str(int(port))])

    if allow_fallback:
        tokens.append("--allow-fallback")

    if system == "opensearch":
        if capture_knn_stats:
            tokens.append("--capture-knn-stats")
        else:
            tokens.append("--no-capture-knn-stats")
        if configure_index_flag:
            tokens.append("--configure-index-flag")
        else:
            tokens.append("--no-configure-index-flag")
    elif system in ("elasticsearch", "qdrant"):
        if capture_cpcs_counters:
            tokens.append("--capture-cpcs-counters")
        else:
            tokens.append("--no-capture-cpcs-counters")
        if cpcs_counters_path:
            tokens.extend(["--cpcs-counters-path", cpcs_counters_path])
    elif system == "milvus":
        if cpcs_offload_endpoint:
            tokens.extend(["--cpcs-offload-endpoint", cpcs_offload_endpoint])
            tokens.extend(["--cpcs-offload-timeout-ms", str(int(cpcs_offload_timeout_ms))])

    if system == "qdrant" and qdrant_exact:
        tokens.append("--exact")
    return " ".join(shlex.quote(token) for token in tokens)


def _run_realapp_matrix(
    *,
    repo_root: Path,
    app: str,
    queries_json: Path,
    output_dir: Path,
    modes: Sequence[str],
    exact_baseline_mode: str,
    exact_match_min: float,
    extra_args: str,
    matrix_runner: str | None,
    hybrid_cpcs_policy: str,
    hybrid_direct_runner: str,
    spdk_nvme_passthru: str,
    trtype: str,
    traddr: str,
    trsvcid: str,
    subnqn: str,
    hostnqn: str,
    src_addr: str,
    src_svcid: str,
    passthru_lcores: str,
    direct_probe_nsid: int,
    direct_probe_offset: int,
    direct_probe_length: int,
    direct_probe_lba_bytes: int,
    log_path: Path,
) -> None:
    script = repo_root / "experiments" / "real_apps" / "common" / "run_real_app_matrix.py"
    runner_override = str(matrix_runner or "").strip()
    runner_path = runner_override or str((repo_root / HYBRID_MATRIX_RUNNER_REL).resolve())
    runner_name = Path(runner_path).name

    effective_extra_args = extra_args
    if runner_name == HYBRID_MATRIX_RUNNER_NAME:
        direct_runner = str(hybrid_direct_runner).strip()
        if not direct_runner:
            direct_runner = str((repo_root / DIRECT_CPCS_RUNNER_REL).resolve())

        effective_extra_args = _append_shell_args(
            effective_extra_args,
            "--realapp-system",
            app,
            "--hybrid-cpcs-policy",
            hybrid_cpcs_policy,
            "--hybrid-direct-runner",
            direct_runner,
            "--direct-failure-policy",
            hybrid_cpcs_policy,
            "--spdk-nvme-passthru",
            spdk_nvme_passthru,
            "--trtype",
            trtype,
            "--traddr",
            traddr,
            "--trsvcid",
            trsvcid,
            "--subnqn",
            subnqn,
            "--hostnqn",
            hostnqn,
            "--passthru-lcores",
            passthru_lcores,
            "--direct-probe-nsid",
            str(int(direct_probe_nsid)),
            "--direct-probe-offset",
            str(int(direct_probe_offset)),
            "--direct-probe-length",
            str(int(direct_probe_length)),
            "--direct-probe-lba-bytes",
            str(int(direct_probe_lba_bytes)),
        )
        if str(src_addr).strip():
            effective_extra_args = _append_shell_args(
                effective_extra_args,
                "--src-addr",
                str(src_addr).strip(),
            )
        if str(src_svcid).strip():
            effective_extra_args = _append_shell_args(
                effective_extra_args,
                "--src-svcid",
                str(src_svcid).strip(),
            )

    cmd = [
        "python3",
        str(script),
        "--app",
        app,
        "--queries-json",
        str(queries_json),
        "--output-dir",
        str(output_dir),
        "--modes",
        ",".join(modes),
        "--exact-baseline-mode",
        exact_baseline_mode,
        "--min-id-exact-match-ratio",
        str(exact_match_min),
        "--extra-args",
        effective_extra_args,
        "--runner",
        runner_path,
    ]
    _run_cmd(cmd=cmd, cwd=repo_root, log_path=log_path)


def _shape_clear(*, apply_shape: bool, repo_root: Path, log_path: Path) -> None:
    if not apply_shape:
        return
    _run_cmd(
        cmd=["sudo", "tc", "qdisc", "del", "dev", "lo", "root"],
        cwd=repo_root,
        log_path=log_path,
        check=False,
    )


def _shape_apply(
    *,
    apply_shape: bool,
    repo_root: Path,
    gbps: float,
    rate_burst: str,
    rate_latency: str,
    log_path: Path,
) -> None:
    if not apply_shape:
        return
    _shape_clear(apply_shape=apply_shape, repo_root=repo_root, log_path=log_path)
    _run_cmd(
        cmd=[
            "sudo",
            "tc",
            "qdisc",
            "add",
            "dev",
            "lo",
            "root",
            "tbf",
            "rate",
            f"{gbps}gbit",
            "burst",
            rate_burst,
            "latency",
            rate_latency,
        ],
        cwd=repo_root,
        log_path=log_path,
    )


def _build_selectivity_profile(*, docs_jsonl: Path, flags_values: Sequence[int]) -> Dict[str, Any]:
    buckets: Dict[int, int] = {int(flag): 0 for flag in flags_values}
    total_docs = 0
    with docs_jsonl.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            payload = row.get("payload", {}) if isinstance(row.get("payload"), dict) else {}
            value = payload.get("flags")
            if isinstance(value, bool):
                continue
            try:
                parsed = int(value)
            except (TypeError, ValueError):
                continue
            total_docs += 1
            if parsed in buckets:
                buckets[parsed] += 1

    profile_buckets: List[Dict[str, Any]] = []
    for flag in flags_values:
        count = int(buckets.get(int(flag), 0))
        ratio = float(count) / float(total_docs) if total_docs > 0 else 0.0
        profile_buckets.append(
            {
                "label": f"flags_eq_{int(flag)}",
                "field": "flags",
                "value": int(flag),
                "expected_match_docs": count,
                "expected_selectivity_ratio": ratio,
                "expected_selectivity_pct": ratio * 100.0,
            }
        )

    return {
        "version": 1,
        "docs_jsonl": str(docs_jsonl),
        "total_docs": total_docs,
        "buckets": profile_buckets,
    }


def _write_r3_queries(
    *,
    source_queries_json: Path,
    output_path: Path,
    flag_value: int,
    max_queries: int,
    label: str,
) -> int:
    doc = _read_json(source_queries_json)
    source_queries = list(doc.get("queries", []))
    if not source_queries:
        raise RuntimeError(f"no queries found in {source_queries_json}")
    if max_queries <= 0:
        raise RuntimeError("max_queries_per_bucket must be > 0")

    selected = source_queries[:max_queries]
    queries: List[Dict[str, Any]] = []
    for idx, query in enumerate(selected):
        row = dict(query)
        row["query_id"] = idx
        row["outer_request_id"] = idx
        row["round"] = 0
        row["filters"] = [{"op": "eq", "field": "flags", "value": int(flag_value)}]
        row["target_selectivity_hint"] = label
        queries.append(row)

    payload = {
        "version": int(doc.get("version", 1)),
        "query_count": len(queries),
        "queries": queries,
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return len(queries)


def _write_r4_queries(
    *,
    source_queries_json: Path,
    output_path: Path,
    outer_count: int,
    inner_rounds: int,
) -> int:
    doc = _read_json(source_queries_json)
    source = list(doc.get("queries", []))
    if not source:
        raise RuntimeError(f"no queries found in {source_queries_json}")
    if outer_count <= 0 or inner_rounds <= 0:
        raise RuntimeError("outer_count and inner_rounds must be > 0")
    if outer_count > len(source):
        raise RuntimeError(
            f"outer_count={outer_count} exceeds source query count={len(source)} in {source_queries_json}"
        )

    queries: List[Dict[str, Any]] = []
    query_id = 0
    for outer_id, query in enumerate(source[:outer_count]):
        for round_idx in range(inner_rounds):
            row = dict(query)
            row["query_id"] = query_id
            row["outer_request_id"] = outer_id
            row["round"] = round_idx
            queries.append(row)
            query_id += 1

    payload = {
        "version": int(doc.get("version", 1)),
        "query_count": len(queries),
        "queries": queries,
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return len(queries)


def _percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return float(values[0])
    ordered = sorted(values)
    rank = (len(ordered) - 1) * p
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    frac = rank - low
    return (ordered[low] * (1.0 - frac)) + (ordered[high] * frac)


def _aggregate_outer_metrics(run_jsonl: Path) -> Dict[str, Any]:
    groups: Dict[int, Dict[str, float]] = {}
    with run_jsonl.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            outer_id = int(row.get("outer_request_id", -1))
            group = groups.setdefault(
                outer_id,
                {
                    "latency_us": 0.0,
                    "request_bytes": 0.0,
                    "response_bytes": 0.0,
                    "total_bytes": 0.0,
                },
            )
            req = float(row.get("request_bytes", 0.0))
            resp = float(row.get("response_bytes", 0.0))
            group["latency_us"] += float(row.get("latency_us", 0.0))
            group["request_bytes"] += req
            group["response_bytes"] += resp
            group["total_bytes"] += (req + resp)

    latencies = [float(group["latency_us"]) for group in groups.values()]
    totals = [float(group["total_bytes"]) for group in groups.values()]
    reqs = [float(group["request_bytes"]) for group in groups.values()]
    resps = [float(group["response_bytes"]) for group in groups.values()]

    return {
        "outer_request_count": len(groups),
        "outer_latency_p50_us": _percentile(latencies, 0.50),
        "outer_latency_p95_us": _percentile(latencies, 0.95),
        "outer_latency_p99_us": _percentile(latencies, 0.99),
        "outer_total_bytes_avg": (sum(totals) / len(totals)) if totals else 0.0,
        "outer_request_bytes_avg": (sum(reqs) / len(reqs)) if reqs else 0.0,
        "outer_response_bytes_avg": (sum(resps) / len(resps)) if resps else 0.0,
    }


def _parse_b1b2_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--system", choices=list(REALAPP_SYSTEMS), default="opensearch")
    parser.add_argument("--host", default=None)
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--index", default=None)
    parser.add_argument("--collection", default=None)
    parser.add_argument("--docs-jsonl", default=None)
    parser.add_argument("--queries-json", required=True)
    parser.add_argument("--load-index", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--recreate-index", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--engine", default="faiss", choices=["lucene", "faiss", "nmslib"])
    parser.add_argument("--space-type", default="cosinesimil", choices=["cosinesimil", "l2", "innerproduct", "l1", "linf"])
    parser.add_argument("--bandwidths-gbps", default="1,10,25,100")
    parser.add_argument("--modes", default="baseline,cpcs_fullfit,cpcs_vslm")
    parser.add_argument("--timeout-sec", type=float, default=90.0)
    parser.add_argument("--allow-fallback", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--capture-knn-stats", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--capture-cpcs-counters", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--cpcs-counters-path", default="")
    parser.add_argument("--configure-index-flag", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--qdrant-exact", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--cpcs-offload-endpoint", default="")
    parser.add_argument("--cpcs-offload-timeout-ms", type=int, default=5000)
    parser.add_argument("--exact-baseline-mode", default="baseline")
    parser.add_argument("--exact-match-min", type=float, default=1.0)
    parser.add_argument("--matrix-runner", default="")
    parser.add_argument("--hybrid-cpcs-policy", choices=["strict", "fallback-to-app"], default="strict")
    parser.add_argument("--hybrid-direct-runner", default="")
    parser.add_argument("--spdk-nvme-passthru", default="")
    parser.add_argument("--trtype", default="")
    parser.add_argument("--traddr", default="")
    parser.add_argument("--trsvcid", default="")
    parser.add_argument("--subnqn", default="")
    parser.add_argument("--hostnqn", default="")
    parser.add_argument("--src-addr", dest="src_addr", default="")
    parser.add_argument("--src-svcid", dest="src_svcid", default="")
    parser.add_argument("--passthru-lcores", default="")
    parser.add_argument("--direct-probe-nsid", type=int, default=0)
    parser.add_argument("--direct-probe-offset", type=int, default=0)
    parser.add_argument("--direct-probe-length", type=int, default=4096)
    parser.add_argument("--direct-probe-lba-bytes", type=int, default=0)
    parser.add_argument("--shape-link", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--rate-burst", default="512kb")
    parser.add_argument("--rate-latency", default="50ms")
    parser.add_argument("--dataset-tier", default="T1")
    parser.add_argument("--metric", default="cosine")
    parser.add_argument("--top-k", type=int, default=10)
    return parser.parse_args(list(argv))


def _parse_r3_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--system", choices=list(REALAPP_SYSTEMS), default="opensearch")
    parser.add_argument("--host", default=None)
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--index", default=None)
    parser.add_argument("--collection", default=None)
    parser.add_argument("--docs-jsonl", default=None)
    parser.add_argument("--source-queries-json", required=True)
    parser.add_argument("--flags-values", default="0,1,2,3")
    parser.add_argument("--max-queries-per-bucket", type=int, default=120)
    parser.add_argument("--load-index", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--recreate-index", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--engine", default="faiss", choices=["lucene", "faiss", "nmslib"])
    parser.add_argument("--space-type", default="cosinesimil", choices=["cosinesimil", "l2", "innerproduct", "l1", "linf"])
    parser.add_argument("--modes", default="baseline,cpcs_fullfit,cpcs_vslm")
    parser.add_argument("--timeout-sec", type=float, default=60.0)
    parser.add_argument("--allow-fallback", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--capture-knn-stats", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--capture-cpcs-counters", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--cpcs-counters-path", default="")
    parser.add_argument("--configure-index-flag", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--qdrant-exact", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--cpcs-offload-endpoint", default="")
    parser.add_argument("--cpcs-offload-timeout-ms", type=int, default=5000)
    parser.add_argument("--exact-baseline-mode", default="baseline")
    parser.add_argument("--exact-match-min", type=float, default=1.0)
    parser.add_argument("--matrix-runner", default="")
    parser.add_argument("--hybrid-cpcs-policy", choices=["strict", "fallback-to-app"], default="strict")
    parser.add_argument("--hybrid-direct-runner", default="")
    parser.add_argument("--spdk-nvme-passthru", default="")
    parser.add_argument("--trtype", default="")
    parser.add_argument("--traddr", default="")
    parser.add_argument("--trsvcid", default="")
    parser.add_argument("--subnqn", default="")
    parser.add_argument("--hostnqn", default="")
    parser.add_argument("--src-addr", dest="src_addr", default="")
    parser.add_argument("--src-svcid", dest="src_svcid", default="")
    parser.add_argument("--passthru-lcores", default="")
    parser.add_argument("--direct-probe-nsid", type=int, default=0)
    parser.add_argument("--direct-probe-offset", type=int, default=0)
    parser.add_argument("--direct-probe-length", type=int, default=4096)
    parser.add_argument("--direct-probe-lba-bytes", type=int, default=0)
    parser.add_argument("--dataset-tier", default="T1")
    parser.add_argument("--metric", default="cosine")
    parser.add_argument("--top-k", type=int, default=10)
    return parser.parse_args(list(argv))


def _parse_r4_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--system", choices=list(REALAPP_SYSTEMS), default="opensearch")
    parser.add_argument("--host", default=None)
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--index", default=None)
    parser.add_argument("--collection", default=None)
    parser.add_argument("--docs-jsonl", default=None)
    parser.add_argument("--source-queries-json", required=True)
    parser.add_argument("--outer-count", type=int, default=100)
    parser.add_argument("--inner-rounds", type=int, default=5)
    parser.add_argument("--load-index", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--recreate-index", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--engine", default="faiss", choices=["lucene", "faiss", "nmslib"])
    parser.add_argument("--space-type", default="cosinesimil", choices=["cosinesimil", "l2", "innerproduct", "l1", "linf"])
    parser.add_argument("--modes", default="baseline,cpcs_fullfit,cpcs_vslm")
    parser.add_argument("--timeout-sec", type=float, default=60.0)
    parser.add_argument("--allow-fallback", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--capture-knn-stats", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--capture-cpcs-counters", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--cpcs-counters-path", default="")
    parser.add_argument("--configure-index-flag", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--qdrant-exact", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--cpcs-offload-endpoint", default="")
    parser.add_argument("--cpcs-offload-timeout-ms", type=int, default=5000)
    parser.add_argument("--exact-baseline-mode", default="baseline")
    parser.add_argument("--exact-match-min", type=float, default=1.0)
    parser.add_argument("--matrix-runner", default="")
    parser.add_argument("--hybrid-cpcs-policy", choices=["strict", "fallback-to-app"], default="strict")
    parser.add_argument("--hybrid-direct-runner", default="")
    parser.add_argument("--spdk-nvme-passthru", default="")
    parser.add_argument("--trtype", default="")
    parser.add_argument("--traddr", default="")
    parser.add_argument("--trsvcid", default="")
    parser.add_argument("--subnqn", default="")
    parser.add_argument("--hostnqn", default="")
    parser.add_argument("--src-addr", dest="src_addr", default="")
    parser.add_argument("--src-svcid", dest="src_svcid", default="")
    parser.add_argument("--passthru-lcores", default="")
    parser.add_argument("--direct-probe-nsid", type=int, default=0)
    parser.add_argument("--direct-probe-offset", type=int, default=0)
    parser.add_argument("--direct-probe-length", type=int, default=4096)
    parser.add_argument("--direct-probe-lba-bytes", type=int, default=0)
    parser.add_argument("--dataset-tier", default="T1")
    parser.add_argument("--metric", default="cosine")
    parser.add_argument("--top-k", type=int, default=10)
    return parser.parse_args(list(argv))


def run_realapp_b1b2(
    ctx: PlatformContext,
    *,
    target_log: str = "",
    scenario_args: Sequence[str],
    raw_dir: Path,
    derived_dir: Path,
    logs_dir: Path,
) -> Dict[str, Any]:
    _ = target_log
    args = _parse_b1b2_args(normalize_scenario_args(scenario_args))
    _apply_system_defaults(args, scenario_name="realapp-b1b2")
    _apply_hybrid_direct_defaults(ctx, args)

    if args.load_index and not args.docs_jsonl:
        raise ValueError("--docs-jsonl is required when --load-index is enabled")

    raw_dir.mkdir(parents=True, exist_ok=True)
    derived_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)

    repo_root = ctx.repo_root
    modes = _parse_modes(args.modes)
    bandwidths = _parse_bandwidths(args.bandwidths_gbps)
    compare_mode = _choose_compare_mode(modes, baseline_mode=args.exact_baseline_mode)

    if args.load_index:
        _run_realapp_load(
            system=args.system,
            repo_root=repo_root,
            host=args.host,
            index=(str(args.index) if args.index else None),
            collection=(str(args.collection) if args.collection else None),
            port=(int(args.port) if args.port is not None else None),
            docs_jsonl=str(args.docs_jsonl),
            recreate_index=bool(args.recreate_index),
            engine=str(args.engine),
            space_type=str(args.space_type),
            log_path=logs_dir / "realapp_b1b2_load.log",
            manifest_path=raw_dir / "realapp_b1b2_load_manifest.json",
        )

    extra_args = _matrix_extra_args(
        system=args.system,
        host=args.host,
        index=(str(args.index) if args.index else None),
        collection=(str(args.collection) if args.collection else None),
        port=(int(args.port) if args.port is not None else None),
        timeout_sec=float(args.timeout_sec),
        allow_fallback=bool(args.allow_fallback),
        capture_knn_stats=bool(args.capture_knn_stats),
        capture_cpcs_counters=bool(args.capture_cpcs_counters),
        cpcs_counters_path=str(args.cpcs_counters_path),
        configure_index_flag=bool(args.configure_index_flag),
        qdrant_exact=bool(args.qdrant_exact),
        cpcs_offload_endpoint=str(args.cpcs_offload_endpoint),
        cpcs_offload_timeout_ms=int(args.cpcs_offload_timeout_ms),
    )

    b1_dir = raw_dir / "b1_unshaped"
    _shape_clear(apply_shape=bool(args.shape_link), repo_root=repo_root, log_path=logs_dir / "shape_clear.log")
    _run_realapp_matrix(
        repo_root=repo_root,
        app=args.system,
        queries_json=Path(args.queries_json).resolve(),
        output_dir=b1_dir,
        modes=modes,
        exact_baseline_mode=args.exact_baseline_mode,
        exact_match_min=float(args.exact_match_min),
        extra_args=extra_args,
        matrix_runner=(str(args.matrix_runner).strip() or None),
        hybrid_cpcs_policy=str(args.hybrid_cpcs_policy),
        hybrid_direct_runner=str(args.hybrid_direct_runner),
        spdk_nvme_passthru=str(args.spdk_nvme_passthru),
        trtype=str(args.trtype),
        traddr=str(args.traddr),
        trsvcid=str(args.trsvcid),
        subnqn=str(args.subnqn),
        hostnqn=str(args.hostnqn),
        src_addr=str(args.src_addr),
        src_svcid=str(args.src_svcid),
        passthru_lcores=str(args.passthru_lcores),
        direct_probe_nsid=int(args.direct_probe_nsid),
        direct_probe_offset=int(args.direct_probe_offset),
        direct_probe_length=int(args.direct_probe_length),
        direct_probe_lba_bytes=int(args.direct_probe_lba_bytes),
        log_path=logs_dir / "realapp_b1b2_b1.log",
    )

    b2_points: List[Dict[str, Any]] = []
    for link in bandwidths:
        _shape_apply(
            apply_shape=bool(args.shape_link),
            repo_root=repo_root,
            gbps=float(link),
            rate_burst=str(args.rate_burst),
            rate_latency=str(args.rate_latency),
            log_path=logs_dir / f"shape_{link}gbps.log",
        )
        label_dir = raw_dir / f"b2_{int(link)}gbps"
        _run_realapp_matrix(
            repo_root=repo_root,
            app=args.system,
            queries_json=Path(args.queries_json).resolve(),
            output_dir=label_dir,
            modes=modes,
            exact_baseline_mode=args.exact_baseline_mode,
            exact_match_min=float(args.exact_match_min),
            extra_args=extra_args,
            matrix_runner=(str(args.matrix_runner).strip() or None),
            hybrid_cpcs_policy=str(args.hybrid_cpcs_policy),
            hybrid_direct_runner=str(args.hybrid_direct_runner),
            spdk_nvme_passthru=str(args.spdk_nvme_passthru),
            trtype=str(args.trtype),
            traddr=str(args.traddr),
            trsvcid=str(args.trsvcid),
            subnqn=str(args.subnqn),
            hostnqn=str(args.hostnqn),
            src_addr=str(args.src_addr),
            src_svcid=str(args.src_svcid),
            passthru_lcores=str(args.passthru_lcores),
            direct_probe_nsid=int(args.direct_probe_nsid),
            direct_probe_offset=int(args.direct_probe_offset),
            direct_probe_length=int(args.direct_probe_length),
            direct_probe_lba_bytes=int(args.direct_probe_lba_bytes),
            log_path=logs_dir / f"realapp_b1b2_b2_{link}gbps.log",
        )

        summaries = _mode_summaries(output_dir=label_dir, app=args.system, modes=modes)
        baseline = summaries[args.exact_baseline_mode]
        compare = summaries[compare_mode]
        b2_points.append(
            {
                "link_gbps": float(link),
                "baseline_p50_us": float(baseline.get("latency_us", {}).get("p50", 0.0)),
                "compare_p50_us": float(compare.get("latency_us", {}).get("p50", 0.0)),
                "baseline_total_bytes_per_query": float(baseline.get("bytes", {}).get("total_per_query", 0.0)),
                "compare_total_bytes_per_query": float(compare.get("bytes", {}).get("total_per_query", 0.0)),
            }
        )

    _shape_clear(apply_shape=bool(args.shape_link), repo_root=repo_root, log_path=logs_dir / "shape_cleanup.log")

    b1_summaries = _mode_summaries(output_dir=b1_dir, app=args.system, modes=modes)
    b1_exactness = _exactness_report(output_dir=b1_dir, baseline_mode=args.exact_baseline_mode)
    exactness_gate = _exactness_gate_pass(
        report=b1_exactness,
        modes=modes,
        baseline_mode=args.exact_baseline_mode,
        threshold=float(args.exact_match_min),
    )

    baseline_summary = b1_summaries[args.exact_baseline_mode]
    compare_summary = b1_summaries[compare_mode]
    baseline_bytes = float(baseline_summary.get("bytes", {}).get("total_per_query", 0.0))
    compare_bytes = float(compare_summary.get("bytes", {}).get("total_per_query", 0.0))
    byte_reduction_x = (baseline_bytes / compare_bytes) if compare_bytes > 0 else 0.0

    metrics = {
        "latency": {
            "b1_baseline_p50_us": float(baseline_summary.get("latency_us", {}).get("p50", 0.0)),
            "b1_compare_p50_us": float(compare_summary.get("latency_us", {}).get("p50", 0.0)),
        },
        "host_visible_bytes": {
            "b1_baseline_total_per_query": baseline_bytes,
            "b1_compare_total_per_query": compare_bytes,
            "b1_byte_reduction_x": byte_reduction_x,
        },
        "realapp_b1b2": {
            "system": args.system,
            "compare_mode": compare_mode,
            "modes": modes,
            "bandwidths_gbps": bandwidths,
            "b2_points": b2_points,
            "exactness_gate_pass": exactness_gate,
            "exact_match_min": float(args.exact_match_min),
        },
    }

    summary_path = derived_dir / "realapp_b1b2_summary.json"
    summary_path.write_text(json.dumps(metrics["realapp_b1b2"], indent=2), encoding="utf-8")

    return {
        "payload": {
            "system": args.system,
            "host": args.host,
            "queries_json": str(Path(args.queries_json).resolve()),
            "dataset_tier": args.dataset_tier,
            **({"index": args.index} if args.index else {}),
            **({"collection": args.collection} if args.collection else {}),
            **({"port": int(args.port)} if args.port is not None else {}),
        },
        "metrics": metrics,
        "artifacts": {
            "realapp_b1_dir": str(b1_dir),
            "realapp_b1_exactness_json": str(b1_dir / f"{args.exact_baseline_mode}_id_exactness_report.json"),
            "realapp_b1b2_summary_json": str(summary_path),
            "realapp_b1b2_logs_dir": str(logs_dir),
        },
        "dataset_tier": str(args.dataset_tier),
        "mode": str(compare_mode),
        "query_count": int(baseline_summary.get("query_count", 0)),
        "concurrency": 1,
        "extra": {
            "app_system": args.system,
            "metric": args.metric,
            "top_k": int(args.top_k),
        },
    }


def run_realapp_r3(
    ctx: PlatformContext,
    *,
    target_log: str = "",
    scenario_args: Sequence[str],
    raw_dir: Path,
    derived_dir: Path,
    logs_dir: Path,
) -> Dict[str, Any]:
    _ = target_log
    args = _parse_r3_args(normalize_scenario_args(scenario_args))
    _apply_system_defaults(args, scenario_name="realapp-r3")
    _apply_hybrid_direct_defaults(ctx, args)

    if args.load_index and not args.docs_jsonl:
        raise ValueError("--docs-jsonl is required when --load-index is enabled")

    raw_dir.mkdir(parents=True, exist_ok=True)
    derived_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)

    repo_root = ctx.repo_root
    modes = _parse_modes(args.modes)
    flags_values = _parse_int_values(args.flags_values)
    compare_mode = _choose_compare_mode(modes, baseline_mode=args.exact_baseline_mode)

    if args.load_index:
        _run_realapp_load(
            system=args.system,
            repo_root=repo_root,
            host=args.host,
            index=(str(args.index) if args.index else None),
            collection=(str(args.collection) if args.collection else None),
            port=(int(args.port) if args.port is not None else None),
            docs_jsonl=str(args.docs_jsonl),
            recreate_index=bool(args.recreate_index),
            engine=str(args.engine),
            space_type=str(args.space_type),
            log_path=logs_dir / "realapp_r3_load.log",
            manifest_path=raw_dir / "realapp_r3_load_manifest.json",
        )

    if not args.docs_jsonl:
        raise ValueError("--docs-jsonl is required for realapp-r3 selectivity profiling")

    profile = _build_selectivity_profile(
        docs_jsonl=Path(args.docs_jsonl).resolve(),
        flags_values=flags_values,
    )
    profile_path = derived_dir / "r3_selectivity_profile.json"
    profile_path.write_text(json.dumps(profile, indent=2), encoding="utf-8")

    extra_args = _matrix_extra_args(
        system=args.system,
        host=args.host,
        index=(str(args.index) if args.index else None),
        collection=(str(args.collection) if args.collection else None),
        port=(int(args.port) if args.port is not None else None),
        timeout_sec=float(args.timeout_sec),
        allow_fallback=bool(args.allow_fallback),
        capture_knn_stats=bool(args.capture_knn_stats),
        capture_cpcs_counters=bool(args.capture_cpcs_counters),
        cpcs_counters_path=str(args.cpcs_counters_path),
        configure_index_flag=bool(args.configure_index_flag),
        qdrant_exact=bool(args.qdrant_exact),
        cpcs_offload_endpoint=str(args.cpcs_offload_endpoint),
        cpcs_offload_timeout_ms=int(args.cpcs_offload_timeout_ms),
    )

    rows: List[Dict[str, Any]] = []
    exactness_gate = True
    total_queries = 0

    for flag in flags_values:
        label = f"flags_eq_{int(flag)}"
        bucket_dir = raw_dir / label
        bucket_dir.mkdir(parents=True, exist_ok=True)

        bucket_queries = bucket_dir / f"queries_{label}.json"
        _write_r3_queries(
            source_queries_json=Path(args.source_queries_json).resolve(),
            output_path=bucket_queries,
            flag_value=int(flag),
            max_queries=int(args.max_queries_per_bucket),
            label=label,
        )

        _run_realapp_matrix(
            repo_root=repo_root,
            app=args.system,
            queries_json=bucket_queries,
            output_dir=bucket_dir,
            modes=modes,
            exact_baseline_mode=args.exact_baseline_mode,
            exact_match_min=float(args.exact_match_min),
            extra_args=extra_args,
            matrix_runner=(str(args.matrix_runner).strip() or None),
            hybrid_cpcs_policy=str(args.hybrid_cpcs_policy),
            hybrid_direct_runner=str(args.hybrid_direct_runner),
            spdk_nvme_passthru=str(args.spdk_nvme_passthru),
            trtype=str(args.trtype),
            traddr=str(args.traddr),
            trsvcid=str(args.trsvcid),
            subnqn=str(args.subnqn),
            hostnqn=str(args.hostnqn),
            src_addr=str(args.src_addr),
            src_svcid=str(args.src_svcid),
            passthru_lcores=str(args.passthru_lcores),
            direct_probe_nsid=int(args.direct_probe_nsid),
            direct_probe_offset=int(args.direct_probe_offset),
            direct_probe_length=int(args.direct_probe_length),
            direct_probe_lba_bytes=int(args.direct_probe_lba_bytes),
            log_path=logs_dir / f"realapp_r3_{label}.log",
        )

        summaries = _mode_summaries(output_dir=bucket_dir, app=args.system, modes=modes)
        exactness = _exactness_report(output_dir=bucket_dir, baseline_mode=args.exact_baseline_mode)
        bucket_exactness_pass = _exactness_gate_pass(
            report=exactness,
            modes=modes,
            baseline_mode=args.exact_baseline_mode,
            threshold=float(args.exact_match_min),
        )
        exactness_gate = exactness_gate and bucket_exactness_pass

        expected = next((entry for entry in profile.get("buckets", []) if int(entry.get("value", -1)) == int(flag)), {})

        for mode in modes:
            summary = summaries[mode]
            mode_exact = exactness.get(mode, {})
            rows.append(
                {
                    "bucket_label": label,
                    "flags_value": int(flag),
                    "expected_match_docs": int(expected.get("expected_match_docs", 0)),
                    "expected_selectivity_pct": float(expected.get("expected_selectivity_pct", 0.0)),
                    "mode": mode,
                    "query_count": int(summary.get("query_count", 0)),
                    "latency_p50_us": float(summary.get("latency_us", {}).get("p50", 0.0)),
                    "latency_p95_us": float(summary.get("latency_us", {}).get("p95", 0.0)),
                    "host_total_bytes_per_query": float(summary.get("bytes", {}).get("total_per_query", 0.0)),
                    "exact_match_ratio_vs_baseline": float(
                        mode_exact.get("exact_match_ratio", 1.0 if mode == args.exact_baseline_mode else 0.0)),
                }
            )

        total_queries += int(summaries[args.exact_baseline_mode].get("query_count", 0))

    summary_json = derived_dir / "r3_selectivity_summary.json"
    summary_json.write_text(json.dumps(rows, indent=2), encoding="utf-8")

    summary_csv = derived_dir / "r3_selectivity_summary.csv"
    if rows:
        with summary_csv.open("w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)

    baseline_rows = [row for row in rows if row.get("mode") == args.exact_baseline_mode]
    compare_rows = [row for row in rows if row.get("mode") == compare_mode]

    def _avg(key: str, src: Sequence[Dict[str, Any]]) -> float:
        if not src:
            return 0.0
        return sum(float(row.get(key, 0.0)) for row in src) / float(len(src))

    metrics = {
        "latency": {
            "r3_baseline_p50_us_avg": _avg("latency_p50_us", baseline_rows),
            "r3_compare_p50_us_avg": _avg("latency_p50_us", compare_rows),
        },
        "host_visible_bytes": {
            "r3_baseline_total_per_query_avg": _avg("host_total_bytes_per_query", baseline_rows),
            "r3_compare_total_per_query_avg": _avg("host_total_bytes_per_query", compare_rows),
        },
        "realapp_r3": {
            "system": args.system,
            "compare_mode": compare_mode,
            "modes": modes,
            "bucket_count": len(flags_values),
            "rows": rows,
            "exactness_gate_pass": exactness_gate,
            "exact_match_min": float(args.exact_match_min),
        },
    }

    return {
        "payload": {
            "system": args.system,
            "host": args.host,
            "source_queries_json": str(Path(args.source_queries_json).resolve()),
            "dataset_tier": args.dataset_tier,
            **({"index": args.index} if args.index else {}),
            **({"collection": args.collection} if args.collection else {}),
            **({"port": int(args.port)} if args.port is not None else {}),
        },
        "metrics": metrics,
        "artifacts": {
            "r3_selectivity_profile_json": str(profile_path),
            "r3_selectivity_summary_json": str(summary_json),
            "r3_selectivity_summary_csv": str(summary_csv),
            "r3_logs_dir": str(logs_dir),
        },
        "dataset_tier": str(args.dataset_tier),
        "mode": str(compare_mode),
        "query_count": total_queries,
        "concurrency": 1,
        "extra": {
            "app_system": args.system,
            "metric": args.metric,
            "top_k": int(args.top_k),
        },
    }


def run_realapp_r4(
    ctx: PlatformContext,
    *,
    target_log: str = "",
    scenario_args: Sequence[str],
    raw_dir: Path,
    derived_dir: Path,
    logs_dir: Path,
) -> Dict[str, Any]:
    _ = target_log
    args = _parse_r4_args(normalize_scenario_args(scenario_args))
    _apply_system_defaults(args, scenario_name="realapp-r4")
    _apply_hybrid_direct_defaults(ctx, args)

    if args.load_index and not args.docs_jsonl:
        raise ValueError("--docs-jsonl is required when --load-index is enabled")

    raw_dir.mkdir(parents=True, exist_ok=True)
    derived_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)

    repo_root = ctx.repo_root
    modes = _parse_modes(args.modes)
    compare_mode = _choose_compare_mode(modes, baseline_mode=args.exact_baseline_mode)

    if args.load_index:
        _run_realapp_load(
            system=args.system,
            repo_root=repo_root,
            host=args.host,
            index=(str(args.index) if args.index else None),
            collection=(str(args.collection) if args.collection else None),
            port=(int(args.port) if args.port is not None else None),
            docs_jsonl=str(args.docs_jsonl),
            recreate_index=bool(args.recreate_index),
            engine=str(args.engine),
            space_type=str(args.space_type),
            log_path=logs_dir / "realapp_r4_load.log",
            manifest_path=raw_dir / "realapp_r4_load_manifest.json",
        )

    queries_json = raw_dir / f"queries_k3_{int(args.outer_count)}x{int(args.inner_rounds)}.json"
    _write_r4_queries(
        source_queries_json=Path(args.source_queries_json).resolve(),
        output_path=queries_json,
        outer_count=int(args.outer_count),
        inner_rounds=int(args.inner_rounds),
    )

    output_dir = raw_dir / "r4_matrix"
    extra_args = _matrix_extra_args(
        system=args.system,
        host=args.host,
        index=(str(args.index) if args.index else None),
        collection=(str(args.collection) if args.collection else None),
        port=(int(args.port) if args.port is not None else None),
        timeout_sec=float(args.timeout_sec),
        allow_fallback=bool(args.allow_fallback),
        capture_knn_stats=bool(args.capture_knn_stats),
        capture_cpcs_counters=bool(args.capture_cpcs_counters),
        cpcs_counters_path=str(args.cpcs_counters_path),
        configure_index_flag=bool(args.configure_index_flag),
        qdrant_exact=bool(args.qdrant_exact),
        cpcs_offload_endpoint=str(args.cpcs_offload_endpoint),
        cpcs_offload_timeout_ms=int(args.cpcs_offload_timeout_ms),
    )
    _run_realapp_matrix(
        repo_root=repo_root,
        app=args.system,
        queries_json=queries_json,
        output_dir=output_dir,
        modes=modes,
        exact_baseline_mode=args.exact_baseline_mode,
        exact_match_min=float(args.exact_match_min),
        extra_args=extra_args,
        matrix_runner=(str(args.matrix_runner).strip() or None),
        hybrid_cpcs_policy=str(args.hybrid_cpcs_policy),
        hybrid_direct_runner=str(args.hybrid_direct_runner),
        spdk_nvme_passthru=str(args.spdk_nvme_passthru),
        trtype=str(args.trtype),
        traddr=str(args.traddr),
        trsvcid=str(args.trsvcid),
        subnqn=str(args.subnqn),
        hostnqn=str(args.hostnqn),
        src_addr=str(args.src_addr),
        src_svcid=str(args.src_svcid),
        passthru_lcores=str(args.passthru_lcores),
        direct_probe_nsid=int(args.direct_probe_nsid),
        direct_probe_offset=int(args.direct_probe_offset),
        direct_probe_length=int(args.direct_probe_length),
        direct_probe_lba_bytes=int(args.direct_probe_lba_bytes),
        log_path=logs_dir / "realapp_r4_matrix.log",
    )

    exactness = _exactness_report(output_dir=output_dir, baseline_mode=args.exact_baseline_mode)
    exactness_gate = _exactness_gate_pass(
        report=exactness,
        modes=modes,
        baseline_mode=args.exact_baseline_mode,
        threshold=float(args.exact_match_min),
    )

    outer_by_mode: Dict[str, Dict[str, Any]] = {}
    for mode in modes:
        run_jsonl = output_dir / f"{args.system}_{mode}.jsonl"
        outer_by_mode[mode] = _aggregate_outer_metrics(run_jsonl)

    baseline_outer = outer_by_mode[args.exact_baseline_mode]
    compare_outer = outer_by_mode[compare_mode]

    summary_path = derived_dir / "r4_outer_summary.json"
    summary_path.write_text(json.dumps(outer_by_mode, indent=2), encoding="utf-8")

    metrics = {
        "latency": {
            "r4_baseline_outer_p50_us": float(baseline_outer.get("outer_latency_p50_us", 0.0)),
            "r4_compare_outer_p50_us": float(compare_outer.get("outer_latency_p50_us", 0.0)),
        },
        "host_visible_bytes": {
            "r4_baseline_outer_total_bytes_avg": float(baseline_outer.get("outer_total_bytes_avg", 0.0)),
            "r4_compare_outer_total_bytes_avg": float(compare_outer.get("outer_total_bytes_avg", 0.0)),
        },
        "realapp_r4": {
            "system": args.system,
            "compare_mode": compare_mode,
            "modes": modes,
            "outer_request_count": int(baseline_outer.get("outer_request_count", 0)),
            "inner_rounds": int(args.inner_rounds),
            "outer_by_mode": outer_by_mode,
            "exactness_gate_pass": exactness_gate,
            "exact_match_min": float(args.exact_match_min),
        },
    }

    summaries = _mode_summaries(output_dir=output_dir, app=args.system, modes=modes)
    query_count = int(summaries[args.exact_baseline_mode].get("query_count", 0))

    return {
        "payload": {
            "system": args.system,
            "host": args.host,
            "source_queries_json": str(Path(args.source_queries_json).resolve()),
            "dataset_tier": args.dataset_tier,
            **({"index": args.index} if args.index else {}),
            **({"collection": args.collection} if args.collection else {}),
            **({"port": int(args.port)} if args.port is not None else {}),
        },
        "metrics": metrics,
        "artifacts": {
            "r4_queries_json": str(queries_json),
            "r4_matrix_dir": str(output_dir),
            "r4_outer_summary_json": str(summary_path),
            "r4_logs_dir": str(logs_dir),
        },
        "dataset_tier": str(args.dataset_tier),
        "mode": str(compare_mode),
        "query_count": query_count,
        "concurrency": 1,
        "extra": {
            "app_system": args.system,
            "metric": args.metric,
            "top_k": int(args.top_k),
            "inner_rounds": int(args.inner_rounds),
        },
    }

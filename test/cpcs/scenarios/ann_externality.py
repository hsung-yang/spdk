#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex
import subprocess
from typing import Any, Dict, List, Optional, Sequence

from experiment_platform import PlatformContext, normalize_scenario_args
from experiment_validation import compute_ann_quality


def _read_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    argv_list = list(argv)

    def _has_arg(flag: str) -> bool:
        return any(token == flag or token.startswith(f"{flag}=") for token in argv_list)

    parser = argparse.ArgumentParser(description="Run OpenSearch ANN externality scenario")
    parser.add_argument("--system", default="opensearch", choices=["opensearch"])
    parser.add_argument("--host", default="http://127.0.0.1:9200")
    parser.add_argument("--index", required=True)
    parser.add_argument("--queries-json", required=True)
    parser.add_argument("--docs-jsonl", default=None)
    parser.add_argument("--load-index", action="store_true")
    parser.add_argument("--recreate-index", action="store_true")
    parser.add_argument("--engine", default="lucene", choices=["lucene", "faiss", "nmslib"])
    parser.add_argument("--space-type", default="cosinesimil", choices=["cosinesimil", "l2", "innerproduct", "l1", "linf"])
    parser.add_argument("--user", default=None)
    parser.add_argument("--password", default=None)
    parser.add_argument("--baseline-mode", default="baseline")
    parser.add_argument("--ann-mode", default="cpcs_vslm")
    parser.add_argument("--mode", default=None, help=argparse.SUPPRESS)
    parser.add_argument("--modes", default=None, help=argparse.SUPPRESS)
    parser.add_argument("--allow-fallback", action="store_true")
    parser.add_argument("--capture-knn-stats", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--configure-index-flag", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--timeout-sec", type=float, default=30.0)
    parser.add_argument("--limit-queries", type=int, default=None)
    parser.add_argument("--top-k", type=int, default=10)
    parser.add_argument("--dataset-tier", default="T1")
    parser.add_argument("--metric", default="cosine")
    parser.add_argument("--filter-selectivity", default="unknown")
    parser.add_argument("--recall-threshold", type=float, default=0.90)
    parser.add_argument("--overlap-threshold", type=float, default=0.80)
    parser.add_argument("--concurrency", type=int, default=1)
    args = parser.parse_args(argv_list)

    explicit_baseline = _has_arg("--baseline-mode")
    explicit_ann = _has_arg("--ann-mode")

    if args.modes:
        modes = [entry.strip() for entry in str(args.modes).split(",") if entry.strip()]
        if len(modes) >= 2:
            if not explicit_baseline:
                args.baseline_mode = modes[0]
            if not explicit_ann:
                args.ann_mode = modes[1]
        elif len(modes) == 1 and not explicit_ann:
            args.ann_mode = modes[0]

    if args.mode and not explicit_ann:
        args.ann_mode = str(args.mode).strip()

    return args


def _run_cmd(*, cmd: List[str], cwd: Path, log_path: Path) -> None:
    print(f"+ {' '.join(shlex.quote(token) for token in cmd)}")
    cp = subprocess.run(cmd, cwd=str(cwd), text=True, capture_output=True)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    combined = (
        f"$ {' '.join(shlex.quote(token) for token in cmd)}\n"
        f"[stdout]\n{cp.stdout}\n"
        f"[stderr]\n{cp.stderr}\n"
    )
    log_path.write_text(combined, encoding="utf-8")
    if cp.returncode != 0:
        raise RuntimeError(
            f"Command failed (rc={cp.returncode}): {' '.join(shlex.quote(token) for token in cmd)}\n"
            f"See log: {log_path}"
        )


def _avg_result_count(path: Path) -> float:
    total = 0
    n = 0
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            total += int(row.get("result_count", 0))
            n += 1
    return float(total) / float(n) if n > 0 else 0.0


def run_ann_search(
    ctx: PlatformContext,
    *,
    target_log: str = "",
    scenario_args: Sequence[str],
    raw_dir: Path,
    derived_dir: Path,
    logs_dir: Path,
) -> Dict[str, Any]:
    args = _parse_args(normalize_scenario_args(scenario_args))

    if args.system != "opensearch":
        raise ValueError("Wave 1 ann-search supports only --system opensearch")

    repo_root = ctx.repo_root
    opensearch_dir = repo_root / "experiments" / "real_apps" / "opensearch"
    common_dir = repo_root / "experiments" / "real_apps" / "common"
    run_script = opensearch_dir / "run_opensearch_queries.py"
    load_script = opensearch_dir / "load_opensearch.py"
    collect_script = common_dir / "collect_real_app_metrics.py"

    raw_dir.mkdir(parents=True, exist_ok=True)
    derived_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)

    load_manifest = raw_dir / "opensearch_load_manifest.json"
    baseline_jsonl = raw_dir / f"opensearch_{args.baseline_mode}.jsonl"
    ann_jsonl = raw_dir / f"opensearch_{args.ann_mode}.jsonl"

    baseline_summary_json = derived_dir / f"opensearch_{args.baseline_mode}_summary.json"
    baseline_summary_csv = derived_dir / f"opensearch_{args.baseline_mode}_summary.csv"
    ann_summary_json = derived_dir / f"opensearch_{args.ann_mode}_summary.json"
    ann_summary_csv = derived_dir / f"opensearch_{args.ann_mode}_summary.csv"

    common_query_cmd = [
        "python3",
        str(run_script),
        "--host",
        args.host,
        "--index",
        args.index,
        "--queries-json",
        str(Path(args.queries_json).resolve()),
        "--timeout-sec",
        str(args.timeout_sec),
    ]
    if args.allow_fallback:
        common_query_cmd.append("--allow-fallback")
    if args.limit_queries is not None:
        common_query_cmd.extend(["--limit-queries", str(args.limit_queries)])
    if args.user:
        common_query_cmd.extend(["--user", args.user])
    if args.password:
        common_query_cmd.extend(["--password", args.password])
    if not args.capture_knn_stats:
        common_query_cmd.append("--no-capture-knn-stats")
    if not args.configure_index_flag:
        common_query_cmd.append("--no-configure-index-flag")

    if args.load_index:
        if not args.docs_jsonl:
            raise ValueError("--docs-jsonl is required when --load-index is set")
        load_cmd = [
            "python3",
            str(load_script),
            "--host",
            args.host,
            "--index",
            args.index,
            "--docs-jsonl",
            str(Path(args.docs_jsonl).resolve()),
            "--engine",
            args.engine,
            "--space-type",
            args.space_type,
            "--output-manifest",
            str(load_manifest),
        ]
        if args.recreate_index:
            load_cmd.append("--recreate-index")
        if args.user:
            load_cmd.extend(["--user", args.user])
        if args.password:
            load_cmd.extend(["--password", args.password])
        _run_cmd(cmd=load_cmd, cwd=repo_root, log_path=logs_dir / "load_opensearch.log")

    baseline_cmd = common_query_cmd + [
        "--mode",
        args.baseline_mode,
        "--output-jsonl",
        str(baseline_jsonl),
    ]
    _run_cmd(cmd=baseline_cmd, cwd=repo_root, log_path=logs_dir / f"query_{args.baseline_mode}.log")

    ann_cmd = common_query_cmd + [
        "--mode",
        args.ann_mode,
        "--output-jsonl",
        str(ann_jsonl),
    ]
    _run_cmd(cmd=ann_cmd, cwd=repo_root, log_path=logs_dir / f"query_{args.ann_mode}.log")

    collect_baseline_cmd = [
        "python3",
        str(collect_script),
        "--run-jsonl",
        str(baseline_jsonl),
        "--output-json",
        str(baseline_summary_json),
        "--output-csv",
        str(baseline_summary_csv),
        "--label",
        f"opensearch:{args.baseline_mode}",
    ]
    _run_cmd(cmd=collect_baseline_cmd, cwd=repo_root, log_path=logs_dir / f"collect_{args.baseline_mode}.log")

    collect_ann_cmd = [
        "python3",
        str(collect_script),
        "--run-jsonl",
        str(ann_jsonl),
        "--output-json",
        str(ann_summary_json),
        "--output-csv",
        str(ann_summary_csv),
        "--label",
        f"opensearch:{args.ann_mode}",
    ]
    _run_cmd(cmd=collect_ann_cmd, cwd=repo_root, log_path=logs_dir / f"collect_{args.ann_mode}.log")

    ann_quality = compute_ann_quality(
        exact_baseline_jsonl=baseline_jsonl,
        approx_jsonl=ann_jsonl,
        top_k=int(args.top_k),
        recall_threshold=float(args.recall_threshold),
        overlap_threshold=float(args.overlap_threshold),
    )
    ann_quality["ann_exact_reference_mode"] = args.baseline_mode

    ann_quality_path = derived_dir / "ann_quality.json"
    ann_quality_path.write_text(json.dumps(ann_quality, indent=2), encoding="utf-8")

    baseline_summary = _read_json(baseline_summary_json)
    ann_summary = _read_json(ann_summary_json)

    metrics = {
        "latency": {
            "baseline_p50_us": float(baseline_summary.get("latency_us", {}).get("p50", 0.0)),
            "ann_p50_us": float(ann_summary.get("latency_us", {}).get("p50", 0.0)),
            "ann_p95_us": float(ann_summary.get("latency_us", {}).get("p95", 0.0)),
        },
        "host_visible_bytes": {
            "baseline_total_per_query": float(baseline_summary.get("bytes", {}).get("total_per_query", 0.0)),
            "ann_total_per_query": float(ann_summary.get("bytes", {}).get("total_per_query", 0.0)),
        },
        "result_count": {
            "baseline_avg": _avg_result_count(baseline_jsonl),
            "ann_avg": _avg_result_count(ann_jsonl),
        },
        "ann_quality": ann_quality,
    }

    payload = {
        "system": args.system,
        "host": args.host,
        "index": args.index,
        "baseline_mode": args.baseline_mode,
        "ann_mode": args.ann_mode,
        "dataset_tier": args.dataset_tier,
        "metric": args.metric,
        "filter_selectivity": args.filter_selectivity,
        "top_k": args.top_k,
        "queries_json": str(Path(args.queries_json).resolve()),
        "docs_jsonl": str(Path(args.docs_jsonl).resolve()) if args.docs_jsonl else None,
        "load_index": args.load_index,
    }

    return {
        "payload": payload,
        "metrics": metrics,
        "artifacts": {
            "load_manifest_json": str(load_manifest) if load_manifest.exists() else None,
            "baseline_jsonl": str(baseline_jsonl),
            "ann_jsonl": str(ann_jsonl),
            "baseline_summary_json": str(baseline_summary_json),
            "ann_summary_json": str(ann_summary_json),
            "ann_quality_json": str(ann_quality_path),
            "logs_dir": str(logs_dir),
        },
        "dataset_tier": str(args.dataset_tier),
        "mode": str(args.ann_mode),
        "query_count": int(ann_quality.get("query_count_compared", 0)),
        "concurrency": int(args.concurrency),
        "extra": {
            "app_system": args.system,
            "metric": args.metric,
            "top_k": int(args.top_k),
            "filter_selectivity": args.filter_selectivity,
            "recall_threshold": float(args.recall_threshold),
            "overlap_threshold": float(args.overlap_threshold),
            "exact_reference_mode": args.baseline_mode,
        },
    }

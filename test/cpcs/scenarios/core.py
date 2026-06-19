#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

import cpcs_slm_capacity_gap_compare as capacity_gap
import cpcs_transport_bottleneck_compare as transport_bottleneck
import cpcs_vector_eval_compare as vector_eval
import vslm_pslm_perf_compare as perf_compare

from experiment_platform import (
    PlatformContext,
    apply_inventory_runtime_overrides,
    has_flag,
    normalize_scenario_args,
    scenario_common_argv,
)


def _arg_value(argv: Sequence[str], key: str) -> Optional[str]:
    for idx, token in enumerate(argv):
        if token == key and idx + 1 < len(argv):
            return argv[idx + 1]
        if token.startswith(f"{key}="):
            return token.split("=", 1)[1]
    return None


def _read_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def run_perf_compare(
    ctx: PlatformContext,
    *,
    target_log: str,
    scenario_args: Sequence[str],
    raw_dir: Path,
    derived_dir: Optional[Path] = None,
    logs_dir: Optional[Path] = None,
) -> Dict[str, Any]:
    argv = scenario_common_argv(ctx, target_log) + normalize_scenario_args(scenario_args)
    argv = apply_inventory_runtime_overrides(ctx, argv, vector_mode=False)
    raw_path = raw_dir / "perf_compare.json"
    if not has_flag(argv, "--output-json"):
        argv.extend(["--output-json", str(raw_path)])

    payload = perf_compare.run_benchmark(perf_compare.parse_args(argv))
    agg = payload.get("aggregate", {})
    pslm = agg.get("pslm", {})
    vslm = agg.get("vslm", {})

    metrics = {
        "latency": {
            "pslm_mean_s": float(pslm.get("end_to_end_seconds", {}).get("mean", 0.0)),
            "vslm_mean_s": float(vslm.get("end_to_end_seconds", {}).get("mean", 0.0)),
        },
        "command_count": {
            "pslm_copy_mean": float(pslm.get("copy_cmd_count", {}).get("mean", 0.0)),
            "vslm_copy_mean": float(vslm.get("copy_cmd_count", {}).get("mean", 0.0)),
            "pslm_execute_mean": float(pslm.get("execute_cmd_count", {}).get("mean", 0.0)),
            "vslm_execute_mean": float(vslm.get("execute_cmd_count", {}).get("mean", 0.0)),
        },
        "runs": int(payload.get("config", {}).get("runs", 0)),
    }

    return {
        "payload": payload,
        "metrics": metrics,
        "artifacts": {
            "raw_perf_compare_json": str(raw_path),
        },
        "dataset_tier": str(_arg_value(argv, "--tier") or "unknown"),
        "mode": "pslm_vs_vslm",
        "query_count": int(payload.get("config", {}).get("runs", 0)),
        "concurrency": 1,
    }


def run_capacity_gap(
    ctx: PlatformContext,
    *,
    target_log: str,
    scenario_args: Sequence[str],
    raw_dir: Path,
    derived_dir: Optional[Path] = None,
    logs_dir: Optional[Path] = None,
) -> Dict[str, Any]:
    argv = scenario_common_argv(ctx, target_log) + normalize_scenario_args(scenario_args)
    argv = apply_inventory_runtime_overrides(ctx, argv, vector_mode=False)
    raw_path = raw_dir / "capacity_gap.json"
    if not has_flag(argv, "--output-json"):
        argv.extend(["--output-json", str(raw_path)])

    payload = capacity_gap.run_capacity_gap(capacity_gap.parse_args(argv))

    means = payload.get("means", {}) if isinstance(payload.get("means"), dict) else {}
    small_pslm = means.get("small_pslm", {})
    small_vslm = means.get("small_vslm", {})

    metrics = {
        "latency": {
            "small_pslm_end_to_end_s": float(small_pslm.get("end_to_end_seconds", 0.0)),
            "small_vslm_end_to_end_s": float(small_vslm.get("end_to_end_seconds", 0.0)),
        },
        "command_count": {
            "small_pslm_total_control_cmd_count": float(small_pslm.get("total_control_cmd_count", 0.0)),
            "small_vslm_total_control_cmd_count": float(small_vslm.get("total_control_cmd_count", 0.0)),
        },
        "ratios": payload.get("ratios", {}),
    }

    return {
        "payload": payload,
        "metrics": metrics,
        "artifacts": {
            "raw_capacity_gap_json": str(raw_path),
        },
        "dataset_tier": str(_arg_value(argv, "--tier") or "unknown"),
        "mode": "c1_vs_c2_vs_c0",
        "query_count": int(payload.get("config", {}).get("runs", 0)),
        "concurrency": 1,
    }


def run_transport_bottleneck(
    ctx: PlatformContext,
    *,
    target_log: str,
    scenario_args: Sequence[str],
    raw_dir: Path,
    derived_dir: Optional[Path] = None,
    logs_dir: Optional[Path] = None,
) -> Dict[str, Any]:
    argv = scenario_common_argv(ctx, target_log) + normalize_scenario_args(scenario_args)
    argv = apply_inventory_runtime_overrides(ctx, argv, vector_mode=False)
    raw_path = raw_dir / "transport_bottleneck.json"
    if not has_flag(argv, "--output-json"):
        argv.extend(["--output-json", str(raw_path)])

    payload = transport_bottleneck.run_transport_study(transport_bottleneck.parse_args(argv))
    compare = payload.get("compare", {}) if isinstance(payload.get("compare"), dict) else {}
    host = compare.get("host", {})
    cpcs = compare.get("cpcs", {})

    metrics = {
        "transport": {
            "host_target_bytes_host": float(host.get("host_target_bytes", 0.0)),
            "host_target_bytes_cpcs": float(cpcs.get("host_target_bytes", 0.0)),
            "byte_reduction_pct": float(compare.get("byte_reduction_pct", 0.0)),
        },
        "projections": payload.get("projections", []),
    }

    return {
        "payload": payload,
        "metrics": metrics,
        "artifacts": {
            "raw_transport_bottleneck_json": str(raw_path),
        },
        "dataset_tier": str(_arg_value(argv, "--tier") or "unknown"),
        "mode": str(_arg_value(argv, "--backend") or "pslm"),
        "query_count": 1,
        "concurrency": 1,
    }


def run_vector_eval(
    ctx: PlatformContext,
    *,
    target_log: str,
    scenario_args: Sequence[str],
    raw_dir: Path,
    derived_dir: Path,
    logs_dir: Optional[Path] = None,
) -> Dict[str, Any]:
    argv = scenario_common_argv(ctx, target_log) + normalize_scenario_args(scenario_args)
    argv = apply_inventory_runtime_overrides(ctx, argv, vector_mode=True)
    out_dir = raw_dir / "vector_eval"
    if not has_flag(argv, "--output-dir"):
        argv.extend(["--output-dir", str(out_dir)])

    payload = vector_eval.run_vector_eval(vector_eval.parse_args(argv))

    k2_h0_path = Path(payload["artifacts"]["k2_h0"])
    k2_c0_path = Path(payload["artifacts"]["k2_c0"])
    exactness_path = Path(payload["artifacts"]["exactness"])

    k2_h0 = _read_json(k2_h0_path)
    k2_c0 = _read_json(k2_c0_path)
    exactness = _read_json(exactness_path)

    h_summary = k2_h0.get("summary", {})
    c_summary = k2_c0.get("summary", {})

    metrics = {
        "latency": {
            "host_p50_us": float(h_summary.get("latency_us", {}).get("p50", 0.0)),
            "cpcs_p50_us": float(c_summary.get("latency_us", {}).get("p50", 0.0)),
        },
        "host_visible_bytes": {
            "host_avg_total_bytes": float(h_summary.get("avg_host_total_bytes", 0.0)),
            "cpcs_avg_total_bytes": float(c_summary.get("avg_host_total_bytes", 0.0)),
        },
        "exactness": {
            "k1_mismatch_count": int(exactness.get("k1_mismatch_count", 0)),
            "k2_mismatch_count": int(exactness.get("k2_mismatch_count", 0)),
        },
        "query_count": int(h_summary.get("query_count", 0)),
    }

    exactness_copy = derived_dir / "vector_exactness.json"
    exactness_copy.write_text(json.dumps(exactness, indent=2), encoding="utf-8")

    return {
        "payload": payload,
        "metrics": metrics,
        "artifacts": {
            "vector_run_config_json": str(out_dir / "run_config.json"),
            "k2_h0_json": str(k2_h0_path),
            "k2_c0_json": str(k2_c0_path),
            "exactness_json": str(exactness_path),
            "derived_exactness_json": str(exactness_copy),
        },
        "dataset_tier": str(_arg_value(argv, "--tier") or "unknown"),
        "mode": str(_arg_value(argv, "--backend") or "pslm"),
        "query_count": int(h_summary.get("query_count", 0)),
        "concurrency": 1,
    }

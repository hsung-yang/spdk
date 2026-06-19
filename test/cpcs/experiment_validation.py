#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, Iterable, List, Sequence


def _get_by_path(obj: Dict[str, Any], path: str) -> Any:
    cur: Any = obj
    for part in path.split("."):
        if not isinstance(cur, dict) or part not in cur:
            raise KeyError(path)
        cur = cur[part]
    return cur


def validate_required_metrics(metrics: Dict[str, Any], required_paths: Sequence[str]) -> Dict[str, Any]:
    missing: List[str] = []
    for path in required_paths:
        try:
            value = _get_by_path(metrics, path)
            if value is None:
                missing.append(path)
        except KeyError:
            missing.append(path)
    return {
        "required_metric_paths": list(required_paths),
        "missing_metric_paths": missing,
        "required_metrics_pass": len(missing) == 0,
    }


def validate_locality_metrics(metrics: Dict[str, Any]) -> Dict[str, Any]:
    locality = metrics.get("locality", {})
    patterns = locality.get("patterns", {}) if isinstance(locality, dict) else {}
    required_patterns = ["sequential", "clustered", "strided", "random"]
    missing_patterns: List[str] = []
    failed_patterns: List[str] = []

    for pattern in required_patterns:
        data = patterns.get(pattern) if isinstance(patterns, dict) else None
        if not isinstance(data, dict):
            missing_patterns.append(pattern)
            continue
        speedup = float(data.get("vslm_speedup_x", 0.0))
        if speedup <= 0.0:
            failed_patterns.append(pattern)

    gate_pass = (len(missing_patterns) == 0) and (len(failed_patterns) == 0)
    return {
        "locality_required_patterns": required_patterns,
        "locality_missing_patterns": missing_patterns,
        "locality_failed_patterns": failed_patterns,
        "locality_gate_pass": gate_pass,
    }


def validate_lease_correctness_metrics(metrics: Dict[str, Any]) -> Dict[str, Any]:
    lease = metrics.get("lease_correctness", {})
    checks = lease.get("checks", {}) if isinstance(lease, dict) else {}
    required_checks = [
        "execute_host_read_allowed",
        "execute_host_read_committed_view",
        "execute_overlapping_write_blocked",
        "execute_overlap_95h_rejected",
        "execute_nonoverlapping_write_allowed",
        "publish_on_success",
        "discard_on_failure",
        "eviction_writeback_not_published",
    ]
    missing: List[str] = []
    failed: List[str] = []
    for key in required_checks:
        if key not in checks:
            missing.append(key)
            continue
        if not bool(checks.get(key)):
            failed.append(key)

    gate_pass = (len(missing) == 0) and (len(failed) == 0)
    return {
        "lease_correctness_required_checks": required_checks,
        "lease_correctness_missing_checks": missing,
        "lease_correctness_failed_checks": failed,
        "lease_correctness_gate_pass": gate_pass,
    }


def validate_lease_contention_metrics(metrics: Dict[str, Any]) -> Dict[str, Any]:
    contention = metrics.get("lease_contention", {})
    levels = contention.get("levels", {}) if isinstance(contention, dict) else {}
    required_levels = ["none", "light", "moderate", "heavy"]
    missing_levels: List[str] = []
    blocked_counts: List[int] = []
    nonzero_return_levels: List[str] = []
    for level in required_levels:
        row = levels.get(level) if isinstance(levels, dict) else None
        if not isinstance(row, dict):
            missing_levels.append(level)
            blocked_counts.append(-1)
            continue
        blocked_counts.append(int(row.get("blocked_count", 0)))
        return_code = int(row.get("return_code", 1))
        if return_code != 0:
            nonzero_return_levels.append(level)

    monotonic = True
    valid_counts = [value for value in blocked_counts if value >= 0]
    for idx in range(1, len(valid_counts)):
        if valid_counts[idx] < valid_counts[idx - 1]:
            monotonic = False
            break

    gate_pass = (len(missing_levels) == 0) and monotonic and (len(nonzero_return_levels) == 0)
    return {
        "lease_contention_required_levels": required_levels,
        "lease_contention_missing_levels": missing_levels,
        "lease_contention_blocked_counts": blocked_counts,
        "lease_contention_nonzero_return_levels": nonzero_return_levels,
        "lease_contention_monotonic_blocked_count": monotonic,
        "lease_contention_gate_pass": gate_pass,
    }


def validate_realapp_b1b2_metrics(metrics: Dict[str, Any]) -> Dict[str, Any]:
    section = metrics.get("realapp_b1b2", {})
    b2_points = section.get("b2_points", []) if isinstance(section, dict) else []
    exactness_gate = bool(section.get("exactness_gate_pass", False)) if isinstance(section, dict) else False
    gate_pass = (len(b2_points) > 0) and exactness_gate
    return {
        "realapp_b1b2_point_count": len(b2_points),
        "realapp_b1b2_exactness_gate_pass": exactness_gate,
        "realapp_b1b2_gate_pass": gate_pass,
    }


def validate_realapp_r3_metrics(metrics: Dict[str, Any]) -> Dict[str, Any]:
    section = metrics.get("realapp_r3", {})
    bucket_count = int(section.get("bucket_count", 0)) if isinstance(section, dict) else 0
    exactness_gate = bool(section.get("exactness_gate_pass", False)) if isinstance(section, dict) else False
    gate_pass = bucket_count > 0 and exactness_gate
    return {
        "realapp_r3_bucket_count": bucket_count,
        "realapp_r3_exactness_gate_pass": exactness_gate,
        "realapp_r3_gate_pass": gate_pass,
    }


def validate_realapp_r4_metrics(metrics: Dict[str, Any]) -> Dict[str, Any]:
    section = metrics.get("realapp_r4", {})
    outer_count = int(section.get("outer_request_count", 0)) if isinstance(section, dict) else 0
    exactness_gate = bool(section.get("exactness_gate_pass", False)) if isinstance(section, dict) else False
    gate_pass = outer_count > 0 and exactness_gate
    return {
        "realapp_r4_outer_request_count": outer_count,
        "realapp_r4_exactness_gate_pass": exactness_gate,
        "realapp_r4_gate_pass": gate_pass,
    }


def load_query_ids_by_mode_jsonl(path: Path, *, top_k: int) -> Dict[int, List[str]]:
    out: Dict[int, List[str]] = {}
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            query_id = int(row.get("query_id", -1))
            if query_id < 0:
                continue
            ids = [str(item.get("id")) for item in row.get("results", [])][:top_k]
            out[query_id] = ids
    return out


def compute_ann_quality(
    *,
    exact_baseline_jsonl: Path,
    approx_jsonl: Path,
    top_k: int,
    recall_threshold: float,
    overlap_threshold: float,
) -> Dict[str, Any]:
    baseline = load_query_ids_by_mode_jsonl(exact_baseline_jsonl, top_k=top_k)
    approx = load_query_ids_by_mode_jsonl(approx_jsonl, top_k=top_k)

    if not baseline:
        raise RuntimeError(f"No baseline rows found: {exact_baseline_jsonl}")

    recalls: List[float] = []
    overlaps: List[float] = []
    missing = 0
    compared = 0

    for query_id, b_ids in baseline.items():
        a_ids = approx.get(query_id)
        if a_ids is None:
            missing += 1
            continue
        compared += 1

        b_set = set(b_ids)
        a_set = set(a_ids)
        inter = len(b_set & a_set)

        recall = float(inter) / float(len(b_set)) if b_set else 0.0
        overlap = float(inter) / float(max(min(len(a_ids), len(b_ids)), 1))
        recalls.append(recall)
        overlaps.append(overlap)

    avg_recall = (sum(recalls) / len(recalls)) if recalls else 0.0
    avg_overlap = (sum(overlaps) / len(overlaps)) if overlaps else 0.0

    pass_gate = avg_recall >= recall_threshold and avg_overlap >= overlap_threshold

    return {
        "query_count_baseline": len(baseline),
        "query_count_compared": compared,
        "missing_query_count": missing,
        "ann_recall_at_k": avg_recall,
        "ann_overlap_at_k": avg_overlap,
        "ann_recall_threshold": recall_threshold,
        "ann_overlap_threshold": overlap_threshold,
        "ann_quality_gate_pass": pass_gate,
        "top_k": top_k,
    }


def discover_run_manifests(artifacts_root: Path) -> List[Path]:
    manifests: List[Path] = []
    for path in artifacts_root.rglob("run_manifest.json"):
        manifests.append(path)
    return sorted(manifests)


def load_manifests(paths: Iterable[Path]) -> List[Dict[str, Any]]:
    manifests: List[Dict[str, Any]] = []
    for path in paths:
        try:
            manifests.append(json.loads(path.read_text(encoding="utf-8")))
        except Exception:
            continue
    return manifests


def _normalize_claim_registry(claim_registry: Dict[str, Any]) -> Dict[str, Dict[str, List[str]]]:
    raw_claims = claim_registry.get("claims") if isinstance(claim_registry, dict) else None
    if isinstance(raw_claims, dict):
        source = raw_claims
    else:
        source = claim_registry if isinstance(claim_registry, dict) else {}

    normalized: Dict[str, Dict[str, List[str]]] = {}
    for claim, entry in source.items():
        if isinstance(entry, dict):
            required = [str(name) for name in entry.get("required", [])]
            optional = [str(name) for name in entry.get("optional", [])]
        elif isinstance(entry, list):
            required = [str(name) for name in entry]
            optional = []
        else:
            continue
        normalized[str(claim)] = {
            "required": required,
            "optional": optional,
        }
    return normalized


def generate_paper_validity_report(
    *,
    claim_registry: Dict[str, Any],
    manifests: Sequence[Dict[str, Any]],
) -> Dict[str, Any]:
    scenario_status: Dict[str, Dict[str, Any]] = {}
    for manifest in manifests:
        scenario = str(manifest.get("scenario_name", ""))
        if not scenario:
            continue
        validation = manifest.get("validation", {}) if isinstance(manifest.get("validation"), dict) else {}
        required_ok = bool(validation.get("required_metrics_pass", False))
        gate_ok = bool(validation.get("overall_pass", required_ok))
        existing = scenario_status.get(scenario)
        if existing is None:
            scenario_status[scenario] = {
                "pass": gate_ok,
                "required_metrics_pass": required_ok,
                "claim_tag": manifest.get("claim_tag"),
                "lane": manifest.get("lane"),
                "claim_policy": manifest.get("claim_policy", "required"),
                "runs_seen": 1,
            }
            continue
        existing["pass"] = bool(existing.get("pass", False)) or gate_ok
        existing["required_metrics_pass"] = bool(existing.get("required_metrics_pass", False)) or required_ok
        existing["runs_seen"] = int(existing.get("runs_seen", 1)) + 1

    normalized_claims = _normalize_claim_registry(claim_registry)
    claims: Dict[str, Any] = {}
    under_supported: List[str] = []
    for claim, policy in normalized_claims.items():
        required_scenarios = list(policy.get("required", []))
        optional_scenarios = list(policy.get("optional", []))
        observed: List[Dict[str, Any]] = []
        required_pass_count = 0
        optional_pass_count = 0
        required_seen_count = 0
        optional_seen_count = 0

        for scenario in required_scenarios:
            status = scenario_status.get(scenario)
            observed.append({
                "scenario": scenario,
                "policy": "required",
                "seen": status is not None,
                "pass": bool(status.get("pass", False)) if status else False,
            })
            if status and status.get("pass"):
                required_pass_count += 1
            if status is not None:
                required_seen_count += 1

        for scenario in optional_scenarios:
            status = scenario_status.get(scenario)
            observed.append({
                "scenario": scenario,
                "policy": "optional",
                "seen": status is not None,
                "pass": bool(status.get("pass", False)) if status else False,
            })
            if status and status.get("pass"):
                optional_pass_count += 1
            if status is not None:
                optional_seen_count += 1

        required_satisfied = (len(required_scenarios) == 0) or (required_pass_count == len(required_scenarios))
        supported = required_satisfied
        if not required_satisfied:
            under_supported.append(claim)
        claims[claim] = {
            "supported": supported,
            "required_satisfied": required_satisfied,
            "required": {
                "total": len(required_scenarios),
                "seen": required_seen_count,
                "passed": required_pass_count,
                "scenarios": required_scenarios,
            },
            "optional": {
                "total": len(optional_scenarios),
                "seen": optional_seen_count,
                "passed": optional_pass_count,
                "scenarios": optional_scenarios,
            },
            "expected_scenarios": required_scenarios + optional_scenarios,
            "passed_scenario_count": required_pass_count + optional_pass_count,
            "observed": observed,
        }

    return {
        "claim_coverage": claims,
        "under_supported_claims": under_supported,
        "scenario_status": scenario_status,
        "total_scenarios_seen": len(scenario_status),
        "claim_registry_normalized": normalized_claims,
    }


def combine_validation_reports(reports: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    merged: Dict[str, Any] = {}
    overall = True
    for report in reports:
        merged.update(report)
        if "required_metrics_pass" in report:
            overall = overall and bool(report.get("required_metrics_pass"))
        for key, value in report.items():
            if key.endswith("_gate_pass") and isinstance(value, bool):
                overall = overall and value
    merged["overall_pass"] = overall
    return merged

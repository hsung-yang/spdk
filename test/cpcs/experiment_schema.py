#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional


CLAIM_TAG_MAIN = "main_claim"
CLAIM_TAG_SUPPORTING = "supporting_claim"
CLAIM_TAG_SECONDARY = "secondary_externality"
CLAIM_TAG_APPENDIX = "appendix_only"

CLAIM_TAGS = {
    CLAIM_TAG_MAIN,
    CLAIM_TAG_SUPPORTING,
    CLAIM_TAG_SECONDARY,
    CLAIM_TAG_APPENDIX,
}

LANE_CORE = "core_cpcs_vslm"
LANE_APP_ANN = "application_ann"
LANE_VALIDATION = "validation"
LANE_INFRA = "infra"

CLAIM_POLICY_REQUIRED = "required"
CLAIM_POLICY_OPTIONAL = "optional"
CLAIM_POLICIES = {CLAIM_POLICY_REQUIRED, CLAIM_POLICY_OPTIONAL}


@dataclass
class ScenarioSpec:
    name: str
    lane: str
    claim_tag: str
    workload: str
    required_metrics: List[str]
    handler: Callable[..., Dict[str, Any]]
    validator: Optional[Callable[..., Dict[str, Any]]] = None
    claim_policy: str = CLAIM_POLICY_REQUIRED


def write_json(path: Path, payload: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def build_manifest(
    *,
    run_id: str,
    scenario_name: str,
    lane: str,
    claim_tag: str,
    workload: str,
    dataset_tier: str,
    mode: str,
    topology: str,
    inventory_path: str,
    git_commit: str,
    query_count: int,
    concurrency: int,
    metrics: Dict[str, Any],
    validation: Dict[str, Any],
    artifacts: Dict[str, Any],
    extra: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    manifest: Dict[str, Any] = {
        "run_id": run_id,
        "scenario_name": scenario_name,
        "lane": lane,
        "claim_tag": claim_tag,
        "workload": workload,
        "dataset_tier": dataset_tier,
        "mode": mode,
        "topology": topology,
        "inventory": inventory_path,
        "git_commit": git_commit,
        "query_count": int(query_count),
        "concurrency": int(concurrency),
        "metrics": metrics,
        "validation": validation,
        "artifacts": artifacts,
    }
    if extra:
        manifest.update(extra)
    return manifest


def ensure_claim_tag(tag: str) -> None:
    if tag not in CLAIM_TAGS:
        raise ValueError(f"Unsupported claim_tag: {tag}")


def ensure_claim_policy(policy: str) -> None:
    if policy not in CLAIM_POLICIES:
        raise ValueError(f"Unsupported claim_policy: {policy}")

#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 CPCS Implementation Team.
# All rights reserved.

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any, Dict, Optional, Sequence

from experiment_platform import (
    DEFAULT_TARGET_LOG,
    build_platform_context,
    load_inventory,
    run_prepare,
    tee_output,
)
from experiment_registry import build_claim_registry, build_scenario_registry
from experiment_schema import build_manifest, ensure_claim_policy, ensure_claim_tag, write_json
from experiment_validation import (
    combine_validation_reports,
    discover_run_manifests,
    generate_paper_validity_report,
    load_manifests,
    validate_required_metrics,
)


SCENARIO_REGISTRY = build_scenario_registry()
CLAIM_REGISTRY = build_claim_registry()


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Unified CPCS experiment platform for loopback and split initiator-target topologies"
        ),
    )
    parser.add_argument("--inventory", required=True, help="Path to inventory YAML")
    parser.add_argument(
        "--artifacts-root",
        default=None,
        help="Local artifacts directory (default: /tmp/cpcs_experiments/<timestamp>)",
    )
    parser.add_argument(
        "--target-log",
        default=DEFAULT_TARGET_LOG,
        help=f"spdk_tgt log path forwarded to scenario runners (default: {DEFAULT_TARGET_LOG})",
    )
    parser.add_argument(
        "--run-log",
        default=None,
        help="Combined stdout/stderr log file for this cpcs_experiments run",
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare = subparsers.add_parser("prepare", help="Provision and bootstrap required hosts")
    prepare.add_argument("--hugepages", type=int, default=2048, help="Hugepages to configure via sysctl")
    prepare.add_argument("--skip-build", action="store_true", help="Skip ./configure && make")
    prepare.add_argument("--skip-runtime", action="store_true", help="Skip runtime setup (modprobe/sysctl/cleanup)")

    for command in sorted(SCENARIO_REGISTRY.keys()):
        subparsers.add_parser(command, help=f"Run scenario: {command}")

    args, extra = parser.parse_known_args(argv)
    if args.command in SCENARIO_REGISTRY:
        args.scenario_args = extra
    else:
        if extra:
            parser.error(f"unrecognized arguments: {' '.join(extra)}")
        args.scenario_args = []
    return args


def _scenario_dirs(root: Path, scenario_name: str) -> Dict[str, Path]:
    scenario_root = root / scenario_name
    raw_dir = scenario_root / "raw"
    derived_dir = scenario_root / "derived"
    logs_dir = scenario_root / "logs"
    for path in (scenario_root, raw_dir, derived_dir, logs_dir):
        path.mkdir(parents=True, exist_ok=True)
    return {
        "scenario_root": scenario_root,
        "raw_dir": raw_dir,
        "derived_dir": derived_dir,
        "logs_dir": logs_dir,
    }


def _run_registered_scenario(ctx: Any, args: argparse.Namespace) -> Dict[str, Any]:
    spec = SCENARIO_REGISTRY[args.command]
    ensure_claim_tag(spec.claim_tag)
    ensure_claim_policy(spec.claim_policy)

    dirs = _scenario_dirs(ctx.artifacts_root, spec.name)
    payload = spec.handler(
        ctx,
        target_log=args.target_log,
        scenario_args=args.scenario_args,
        raw_dir=dirs["raw_dir"],
        derived_dir=dirs["derived_dir"],
        logs_dir=dirs["logs_dir"],
    )

    scenario_payload = payload.get("payload", {}) if isinstance(payload, dict) else {}
    metrics = payload.get("metrics", {}) if isinstance(payload, dict) else {}
    artifacts = payload.get("artifacts", {}) if isinstance(payload, dict) else {}

    if not isinstance(metrics, dict):
        raise RuntimeError(f"Scenario '{spec.name}' returned invalid metrics payload")
    if not isinstance(artifacts, dict):
        raise RuntimeError(f"Scenario '{spec.name}' returned invalid artifacts payload")

    validation_reports = [
        validate_required_metrics(metrics, spec.required_metrics),
    ]
    if spec.validator is not None:
        validation_reports.append(spec.validator(metrics))

    # ANN scenarios expose quality gates inside metrics. Promote them into validation.
    ann_quality = metrics.get("ann_quality")
    if isinstance(ann_quality, dict):
        validation_reports.append(ann_quality)

    validation = combine_validation_reports(validation_reports)

    metrics_summary_path = dirs["scenario_root"] / "metrics_summary.json"
    validation_report_path = dirs["scenario_root"] / "validation_report.json"
    run_manifest_path = dirs["scenario_root"] / "run_manifest.json"
    scenario_payload_path = dirs["raw_dir"] / "scenario_payload.json"

    write_json(metrics_summary_path, metrics)
    write_json(validation_report_path, validation)
    if isinstance(scenario_payload, dict):
        write_json(scenario_payload_path, scenario_payload)

    manifest = build_manifest(
        run_id=ctx.run_id,
        scenario_name=spec.name,
        lane=spec.lane,
        claim_tag=spec.claim_tag,
        workload=spec.workload,
        dataset_tier=str(payload.get("dataset_tier", "unknown")),
        mode=str(payload.get("mode", "default")),
        topology=("split" if ctx.split_mode else "loopback"),
        inventory_path=str(Path(args.inventory).resolve()),
        git_commit=ctx.git_commit,
        query_count=int(payload.get("query_count", 0)),
        concurrency=int(payload.get("concurrency", 1)),
        metrics=metrics,
        validation=validation,
        artifacts={
            **artifacts,
            "metrics_summary_json": str(metrics_summary_path),
            "validation_report_json": str(validation_report_path),
            "scenario_payload_json": str(scenario_payload_path),
        },
        extra=(
            {
                **(payload.get("extra") if isinstance(payload.get("extra"), dict) else {}),
                "claim_policy": spec.claim_policy,
            }
        ),
    )
    write_json(run_manifest_path, manifest)

    # Emit claim registry + paper-validity report for every run.
    claim_registry_path = ctx.artifacts_root / "claim_registry.json"
    write_json(claim_registry_path, CLAIM_REGISTRY)

    all_manifest_paths = discover_run_manifests(ctx.artifacts_root)
    all_manifests = load_manifests(all_manifest_paths)
    paper_validity = generate_paper_validity_report(
        claim_registry=CLAIM_REGISTRY,
        manifests=all_manifests,
    )
    paper_validity_path = ctx.artifacts_root / "paper_validity_report.json"
    write_json(paper_validity_path, paper_validity)

    return {
        "scenario": spec.name,
        "run_manifest": str(run_manifest_path),
        "metrics_summary": str(metrics_summary_path),
        "validation_report": str(validation_report_path),
        "paper_validity_report": str(paper_validity_path),
        "validation": validation,
    }


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    run_log = Path(args.run_log).expanduser().resolve() if args.run_log else None
    with tee_output(run_log):
        if run_log is not None:
            print(f"[cpcs_experiments] tee stdout/stderr -> {run_log}")
        try:
            inventory = load_inventory(args.inventory)
            ctx = build_platform_context(inventory, args.artifacts_root)

            if args.command == "prepare":
                result = run_prepare(ctx, args)
            else:
                result = _run_registered_scenario(ctx, args)

            meta = {
                "command": args.command,
                "inventory": str(Path(args.inventory).resolve()),
                "artifacts_root": str(ctx.artifacts_root),
                "topology": "split" if ctx.split_mode else "loopback",
                "run_log": (str(run_log) if run_log is not None else None),
                "git_commit": ctx.git_commit,
                "result": result,
            }
            meta_path = ctx.artifacts_root / f"{args.command}_meta.json"
            with meta_path.open("w", encoding="utf-8") as fh:
                json.dump(meta, fh, indent=2)
            print(f"\nWrote metadata: {meta_path}")
            return 0
        except Exception as exc:
            print(f"\nERROR: {exc}", file=sys.stderr)
            return 1


if __name__ == "__main__":
    sys.exit(main())

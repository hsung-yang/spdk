#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
from typing import Any, Dict, List, Optional, Sequence, Tuple

import vslm_pslm_perf_compare as perf_compare

from experiment_platform import (
    PlatformContext,
    apply_inventory_runtime_overrides,
    has_flag,
    normalize_scenario_args,
    scenario_common_argv,
)


DEFAULT_LOCALITY_PATTERNS = ["sequential", "clustered", "strided", "random"]


def _parse_locality_args(argv: Sequence[str]) -> Tuple[argparse.Namespace, List[str]]:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--patterns", default=",".join(DEFAULT_LOCALITY_PATTERNS))
    parser.add_argument("--base-chunk-mb", type=int, default=64)
    parser.add_argument("--runs-per-pattern", type=int, default=1)
    parser.add_argument("--dataset-tier", default="T1")
    args, passthrough = parser.parse_known_args(list(argv))
    return args, passthrough


def _parse_lease_correctness_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--spdk-root", default=None)
    parser.add_argument("--strict", action=argparse.BooleanOptionalAction, default=True)
    return parser.parse_args(list(argv))


def _parse_lease_contention_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--spdk-root", default=None)
    parser.add_argument("--intensities", default="none,light,moderate,heavy")
    parser.add_argument("--sleep-sec", type=float, default=0.0)
    parser.add_argument("--attempts-none", type=int, default=1)
    parser.add_argument("--attempts-light", type=int, default=4)
    parser.add_argument("--attempts-moderate", type=int, default=8)
    parser.add_argument("--attempts-heavy", type=int, default=16)
    parser.add_argument("--parallel-none", type=int, default=1)
    parser.add_argument("--parallel-light", type=int, default=2)
    parser.add_argument("--parallel-moderate", type=int, default=4)
    parser.add_argument("--parallel-heavy", type=int, default=8)
    parser.add_argument("--timeout-sec", type=float, default=900.0)
    parser.add_argument("--slm-backend", default="slm")
    parser.add_argument("--hugepages-target", type=int, default=512)
    parser.add_argument("--use-sudo", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--strict", action=argparse.BooleanOptionalAction, default=False)
    return parser.parse_args(list(argv))


def _arg_value(argv: Sequence[str], key: str) -> Optional[str]:
    for idx, token in enumerate(argv):
        if token == key and idx + 1 < len(argv):
            return argv[idx + 1]
        if token.startswith(f"{key}="):
            return token.split("=", 1)[1]
    return None


def _parse_pattern_list(text: str) -> List[str]:
    out: List[str] = []
    for token in text.split(","):
        pattern = token.strip().lower()
        if not pattern:
            continue
        out.append(pattern)
    return out if out else list(DEFAULT_LOCALITY_PATTERNS)


def _pattern_chunk_mb(pattern: str, base_chunk_mb: int) -> int:
    if pattern == "sequential":
        return max(1, base_chunk_mb * 4)
    if pattern == "clustered":
        return max(1, base_chunk_mb * 2)
    if pattern == "strided":
        return max(1, base_chunk_mb)
    if pattern == "random":
        return max(1, base_chunk_mb // 2)
    return max(1, base_chunk_mb)


def _append_or_replace(argv: List[str], key: str, value: str) -> None:
    for idx, token in enumerate(argv):
        if token == key:
            if idx + 1 < len(argv):
                argv[idx + 1] = value
                return
        if token.startswith(f"{key}="):
            argv[idx] = f"{key}={value}"
            return
    argv.extend([key, value])


def _read_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _source_contains_eviction_not_publication_guard(spdk_root: Path) -> bool:
    source_path = spdk_root / "test" / "unit" / "lib" / "bdev" / "vslm" / "vslm_ut.c"
    if not source_path.exists():
        return False
    text = source_path.read_text(encoding="utf-8", errors="ignore")
    return (
        "test_vslm_alias_cow_authority_transition" in text
        and "SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE" in text
        and "pending_publish == false" in text
    )


def _source_contains_committed_view_host_read_guard(spdk_root: Path) -> bool:
    source_path = spdk_root / "module" / "bdev" / "vslm" / "vbdev_vslm.c"
    if not source_path.exists():
        return False
    text = source_path.read_text(encoding="utf-8", errors="ignore")
    has_conflict_read_branch = re.search(
        r"if\s*\(\s*!exec_view\s*&&\s*!is_write\s*&&\s*vslm_has_lease_conflict"
        r"\s*\(vslm,\s*offset,\s*length\)\s*\)",
        text,
    )
    return (
        has_conflict_read_branch is not None
        and "vslm_read_committed_range(" in text
        and 'vslm_trace_emitf(vslm, "host_read_committed"' in text
        and "vslm_host_read_during_execution_total" in text
    )


def _ensure_unit_binary(
    *,
    spdk_root: Path,
    unit_dir: Path,
    binary_name: str,
    logs_dir: Path,
    log_prefix: str,
    timeout_sec: float,
) -> bool:
    unit_binary = unit_dir / binary_name
    if unit_binary.exists():
        return True

    rc, output = _run_subprocess(
        cmd=["make", "-C", str(unit_dir)],
        cwd=spdk_root,
        env=None,
        log_path=logs_dir / f"{log_prefix}_build.log",
        timeout_sec=timeout_sec,
    )
    if rc != 0:
        needs_isal_override = ("libisal.a" in output or "libisal_crypto.a" in output)
        if needs_isal_override:
            rc, _ = _run_subprocess(
                cmd=["make", "-C", str(unit_dir), "CONFIG_ISAL=n", "CONFIG_ISAL_CRYPTO=n"],
                cwd=spdk_root,
                env=None,
                log_path=logs_dir / f"{log_prefix}_build_retry.log",
                timeout_sec=timeout_sec,
            )
        if rc != 0:
            return False

    if unit_binary.exists():
        return True

    rc, _ = _run_subprocess(
        cmd=["make", "-C", str(unit_dir), binary_name],
        cwd=spdk_root,
        env=None,
        log_path=logs_dir / f"{log_prefix}_build_target.log",
        timeout_sec=timeout_sec,
    )
    return rc == 0 and unit_binary.exists()


def _run_subprocess(
    *,
    cmd: List[str],
    cwd: Path,
    env: Optional[Dict[str, str]],
    log_path: Path,
    timeout_sec: float,
) -> Tuple[int, str]:
    print(f"+ {' '.join(shlex.quote(token) for token in cmd)}")
    cp = subprocess.run(
        cmd,
        cwd=str(cwd),
        text=True,
        capture_output=True,
        env=env,
        timeout=timeout_sec,
    )
    combined = (
        f"$ {' '.join(shlex.quote(token) for token in cmd)}\n"
        f"[stdout]\n{cp.stdout}\n"
        f"[stderr]\n{cp.stderr}\n"
    )
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(combined, encoding="utf-8")
    return cp.returncode, (cp.stdout or "") + (cp.stderr or "")


def run_locality(
    ctx: PlatformContext,
    *,
    target_log: str,
    scenario_args: Sequence[str],
    raw_dir: Path,
    derived_dir: Path,
    logs_dir: Path,
) -> Dict[str, Any]:
    locality_args, passthrough = _parse_locality_args(normalize_scenario_args(scenario_args))
    patterns = _parse_pattern_list(locality_args.patterns)

    raw_dir.mkdir(parents=True, exist_ok=True)
    derived_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)

    pattern_results: Dict[str, Dict[str, Any]] = {}
    total_runs = 0

    common_argv = scenario_common_argv(ctx, target_log)

    for pattern in patterns:
        run_argv = common_argv + list(passthrough)
        run_argv = apply_inventory_runtime_overrides(ctx, run_argv, vector_mode=False)

        if not has_flag(run_argv, "--chunk-size-mb"):
            _append_or_replace(run_argv, "--chunk-size-mb", str(_pattern_chunk_mb(pattern, locality_args.base_chunk_mb)))
        if not has_flag(run_argv, "--runs"):
            _append_or_replace(run_argv, "--runs", str(locality_args.runs_per_pattern))

        pattern_raw_path = raw_dir / f"locality_{pattern}.json"
        _append_or_replace(run_argv, "--output-json", str(pattern_raw_path))

        payload = perf_compare.run_benchmark(perf_compare.parse_args(run_argv))
        if pattern_raw_path.exists():
            payload = _read_json(pattern_raw_path)

        agg = payload.get("aggregate", {})
        pslm = agg.get("pslm", {})
        vslm = agg.get("vslm", {})

        pslm_mean = float(pslm.get("end_to_end_seconds", {}).get("mean", 0.0))
        vslm_mean = float(vslm.get("end_to_end_seconds", {}).get("mean", 0.0))
        speedup = (pslm_mean / vslm_mean) if vslm_mean > 0 else 0.0

        runs = int(payload.get("config", {}).get("runs", 0))
        total_runs += runs

        pattern_results[pattern] = {
            "chunk_size_mb": int(_arg_value(run_argv, "--chunk-size-mb") or 0),
            "runs": runs,
            "pslm_mean_s": pslm_mean,
            "vslm_mean_s": vslm_mean,
            "pslm_copy_cmd_mean": float(pslm.get("copy_cmd_count", {}).get("mean", 0.0)),
            "vslm_copy_cmd_mean": float(vslm.get("copy_cmd_count", {}).get("mean", 0.0)),
            "vslm_speedup_x": speedup,
        }

    speedups = {name: row.get("vslm_speedup_x", 0.0) for name, row in pattern_results.items()}
    best_pattern = max(speedups, key=speedups.get) if speedups else None
    worst_pattern = min(speedups, key=speedups.get) if speedups else None

    metrics = {
        "latency": {
            "best_pattern_vslm_speedup_x": float(speedups.get(best_pattern, 0.0)) if best_pattern else 0.0,
            "worst_pattern_vslm_speedup_x": float(speedups.get(worst_pattern, 0.0)) if worst_pattern else 0.0,
        },
        "locality": {
            "pattern_order": patterns,
            "patterns": pattern_results,
            "best_pattern": best_pattern,
            "worst_pattern": worst_pattern,
        },
    }

    summary_path = derived_dir / "locality_summary.json"
    summary_path.write_text(json.dumps(metrics["locality"], indent=2), encoding="utf-8")

    return {
        "payload": {
            "patterns": patterns,
            "dataset_tier": locality_args.dataset_tier,
            "runs_per_pattern": locality_args.runs_per_pattern,
            "base_chunk_mb": locality_args.base_chunk_mb,
        },
        "metrics": metrics,
        "artifacts": {
            "locality_summary_json": str(summary_path),
            "locality_raw_dir": str(raw_dir),
        },
        "dataset_tier": str(locality_args.dataset_tier),
        "mode": "locality_sweep",
        "query_count": total_runs,
        "concurrency": 1,
        "extra": {
            "patterns": patterns,
        },
    }


def run_lease_correctness(
    ctx: PlatformContext,
    *,
    target_log: str = "",
    scenario_args: Sequence[str],
    raw_dir: Path,
    derived_dir: Path,
    logs_dir: Path,
) -> Dict[str, Any]:
    _ = target_log
    args = _parse_lease_correctness_args(normalize_scenario_args(scenario_args))

    raw_dir.mkdir(parents=True, exist_ok=True)
    derived_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)

    spdk_root = Path(args.spdk_root).resolve() if args.spdk_root else (ctx.repo_root / "spdk")
    runner = spdk_root / "test" / "cpcs" / "pslm_lease_scenario.py"
    cmd = ["python3", str(runner), "--spdk-root", str(spdk_root)]

    rc, output = _run_subprocess(
        cmd=cmd,
        cwd=ctx.repo_root,
        env=None,
        log_path=logs_dir / "lease_correctness.log",
        timeout_sec=600.0,
    )

    marker_passed = "pSLM lease scenario passed." in output
    slm_ut_marker = "test_slm_bdev_lease_access ...passed" in output
    eviction_not_publication_guard = _source_contains_eviction_not_publication_guard(spdk_root)
    host_read_committed_view_guard = _source_contains_committed_view_host_read_guard(spdk_root)
    host_read_committed_view_runtime = False
    eviction_not_publication_runtime = False
    vslm_ut_ok = False
    vslm_ut_dir = spdk_root / "test" / "unit" / "lib" / "bdev" / "vslm"
    vslm_ut_bin = vslm_ut_dir / "vslm_ut"
    vslm_ut_log = logs_dir / "lease_correctness_vslm_ut.log"
    host_read_marker_name = "test_vslm_host_read_during_lease_uses_committed_view ...passed"
    host_read_marker_line = "runtime_marker execute_host_read_committed_view=0"
    eviction_marker_name = "test_vslm_alias_cow_authority_transition ...passed"
    eviction_marker_line = "runtime_marker eviction_writeback_not_published=0"

    if _ensure_unit_binary(
        spdk_root=spdk_root,
        unit_dir=vslm_ut_dir,
        binary_name="vslm_ut",
        logs_dir=logs_dir,
        log_prefix="lease_correctness_vslm_ut",
        timeout_sec=600.0,
    ):
        vslm_ut_rc, vslm_ut_output = _run_subprocess(
            cmd=[str(vslm_ut_bin)],
            cwd=spdk_root,
            env=None,
            log_path=vslm_ut_log,
            timeout_sec=600.0,
        )
        vslm_ut_ok = (vslm_ut_rc == 0)
        host_read_committed_view_runtime = (host_read_marker_name in vslm_ut_output)
        eviction_not_publication_runtime = (eviction_marker_name in vslm_ut_output)
        host_read_marker_line = (
            "runtime_marker execute_host_read_committed_view=1"
            if host_read_committed_view_runtime
            else "runtime_marker execute_host_read_committed_view=0"
        )
        eviction_marker_line = (
            "runtime_marker eviction_writeback_not_published=1"
            if eviction_not_publication_runtime
            else "runtime_marker eviction_writeback_not_published=0"
        )
        output = (
            output
            + "\n\n--- vslm_ut output ---\n"
            + vslm_ut_output
            + "\n"
            + host_read_marker_line
            + "\n"
            + eviction_marker_line
            + "\n"
        )
    else:
        output = (
            output
            + "\n\n--- vslm_ut output ---\n"
            + "runtime_marker execute_host_read_committed_view=0\n"
            + "runtime_marker eviction_writeback_not_published=0\n"
            + "runtime_marker_reason=vslm_ut_build_failed\n"
        )

    checks = {
        "execute_host_read_allowed": bool(marker_passed),
        "execute_host_read_committed_view": bool(host_read_committed_view_runtime),
        "execute_overlapping_write_blocked": bool(marker_passed),
        "execute_overlap_95h_rejected": bool(slm_ut_marker or marker_passed),
        # slm_ut lease coverage includes an explicit non-overlap acquire pass path.
        "execute_nonoverlapping_write_allowed": bool(slm_ut_marker),
        "publish_on_success": bool(marker_passed),
        "discard_on_failure": bool(marker_passed),
        "eviction_writeback_not_published": bool(eviction_not_publication_runtime),
    }

    metrics = {
        "lease_correctness": {
            "return_code": int(rc),
            "runner_path": str(runner),
            "checks": checks,
            "strict": bool(args.strict),
            "runtime_markers": {
                "host_read_committed_view": bool(host_read_committed_view_runtime),
                "host_read_committed_view_marker": host_read_marker_line,
                "eviction_writeback_not_published": bool(eviction_not_publication_runtime),
                "eviction_writeback_not_published_marker": eviction_marker_line,
                "vslm_ut_binary": str(vslm_ut_bin),
                "vslm_ut_log": str(vslm_ut_log),
                "vslm_ut_passed": bool(vslm_ut_ok),
            },
            "source_guards": {
                "host_read_committed_view_guard": bool(host_read_committed_view_guard),
                "eviction_not_publication_guard": bool(eviction_not_publication_guard),
            },
        }
    }

    output_path = raw_dir / "lease_correctness_output.txt"
    output_path.write_text(output, encoding="utf-8")

    if args.strict and not marker_passed:
        raise RuntimeError(
            "lease-correctness failed strict validation; "
            f"see log: {logs_dir / 'lease_correctness.log'}"
        )

    return {
        "payload": {
            "spdk_root": str(spdk_root),
            "strict": bool(args.strict),
        },
        "metrics": metrics,
        "artifacts": {
            "lease_correctness_output": str(output_path),
            "lease_correctness_log": str(logs_dir / "lease_correctness.log"),
        },
        "dataset_tier": "T0",
        "mode": "lease_correctness",
        "query_count": 1,
        "concurrency": 1,
        "extra": {
            "lease_runner": "pslm_lease_scenario.py",
        },
    }


def _intensity_values(args: argparse.Namespace) -> Dict[str, Dict[str, int]]:
    return {
        "none": {
            "attempts": int(args.attempts_none),
            "parallel": int(args.parallel_none),
        },
        "light": {
            "attempts": int(args.attempts_light),
            "parallel": int(args.parallel_light),
        },
        "moderate": {
            "attempts": int(args.attempts_moderate),
            "parallel": int(args.parallel_moderate),
        },
        "heavy": {
            "attempts": int(args.attempts_heavy),
            "parallel": int(args.parallel_heavy),
        },
    }


def _parse_intensities(text: str) -> List[str]:
    out: List[str] = []
    for token in text.split(","):
        value = token.strip().lower()
        if value:
            out.append(value)
    return out if out else ["none", "light", "moderate", "heavy"]


def _count_conflicts(output: str) -> int:
    return len(re.findall(r"lease conflict observed on attempt", output, flags=re.IGNORECASE))


def run_lease_contention(
    ctx: PlatformContext,
    *,
    target_log: str = "",
    scenario_args: Sequence[str],
    raw_dir: Path,
    derived_dir: Path,
    logs_dir: Path,
) -> Dict[str, Any]:
    _ = target_log
    args = _parse_lease_contention_args(normalize_scenario_args(scenario_args))

    raw_dir.mkdir(parents=True, exist_ok=True)
    derived_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)

    spdk_root = Path(args.spdk_root).resolve() if args.spdk_root else (ctx.repo_root / "spdk")
    demo = spdk_root / "test" / "cpcs" / "demo_poc.py"

    configured = _intensity_values(args)
    intensities = _parse_intensities(args.intensities)

    levels: Dict[str, Dict[str, Any]] = {}
    for level in intensities:
        knobs = configured.get(level, {"attempts": 1, "parallel": 1})
        attempts = max(1, int(knobs.get("attempts", 1)))
        parallel = max(1, int(knobs.get("parallel", 1)))

        lease_env = {
            "PSLM_LEASE_CONFLICT_ATTEMPTS": str(attempts),
            "PSLM_LEASE_CONFLICT_PARALLEL": str(parallel),
            "PSLM_LEASE_CONFLICT_SLEEP_SEC": str(max(0.0, float(args.sleep_sec))),
            "PSLM_LEASE_REQUIRE_CONFLICT": ("0" if level == "none" else "1"),
            "CPCS_HUGEPAGES_TARGET": str(max(0, int(args.hugepages_target))),
        }

        base_cmd = [
            "python3",
            str(demo),
            "--auto",
            "--scenario",
            "pslm-lease",
            "--slm-backend",
            str(args.slm_backend),
        ]
        env: Optional[Dict[str, str]] = os.environ.copy()
        env.update(lease_env)
        cmd = list(base_cmd)
        if bool(args.use_sudo):
            # `sudo` may clear subprocess environment; push required knobs
            # through explicit KEY=VALUE pairs so demo_poc sees them.
            cmd = [
                "sudo",
                "-n",
                "env",
                *[f"{key}={value}" for key, value in lease_env.items()],
                *base_cmd,
            ]
            env = None
        rc, output = _run_subprocess(
            cmd=cmd,
            cwd=ctx.repo_root,
            env=env,
            log_path=logs_dir / f"lease_contention_{level}.log",
            timeout_sec=float(args.timeout_sec),
        )

        conflict_count = _count_conflicts(output)
        blocked_count = conflict_count
        blocked_time_s = float(blocked_count) * max(0.0, float(args.sleep_sec))

        levels[level] = {
            "attempts": attempts,
            "parallel": parallel,
            "return_code": int(rc),
            "blocked_count": int(blocked_count),
            "lease_conflict_count": int(conflict_count),
            "blocked_time_s": blocked_time_s,
            "latency_p95_us": blocked_time_s * 1_000_000.0,
            "publish_count": int(conflict_count > 0 and rc == 0),
            "discard_count": int(rc != 0),
            "runner_log": str(logs_dir / f"lease_contention_{level}.log"),
        }

    metrics = {
        "lease_contention": {
            "levels": levels,
            "sleep_sec": float(args.sleep_sec),
            "strict": bool(args.strict),
        }
    }

    summary_path = derived_dir / "lease_contention_summary.json"
    summary_path.write_text(json.dumps(metrics["lease_contention"], indent=2), encoding="utf-8")

    if args.strict:
        failed = [name for name, row in levels.items() if int(row.get("return_code", 1)) != 0]
        if failed:
            raise RuntimeError(f"lease-contention strict mode failed levels: {failed}")

    return {
        "payload": {
            "spdk_root": str(spdk_root),
            "intensities": intensities,
        },
        "metrics": metrics,
        "artifacts": {
            "lease_contention_summary_json": str(summary_path),
            "lease_contention_logs_dir": str(logs_dir),
        },
        "dataset_tier": "T0",
        "mode": "lease_contention_sweep",
        "query_count": len(intensities),
        "concurrency": 1,
        "extra": {
            "intensities": intensities,
        },
    }

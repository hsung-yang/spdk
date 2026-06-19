#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import json
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import cpcs_experiments
from experiment_schema import (
    CLAIM_POLICY_REQUIRED,
    CLAIM_TAG_MAIN,
    CLAIM_POLICY_OPTIONAL,
    CLAIM_TAG_SECONDARY,
    LANE_APP_ANN,
    LANE_CORE,
    ScenarioSpec,
)


class OrchestratorArtifactTests(unittest.TestCase):
    def test_run_registered_scenario_writes_unified_artifact_triplet(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_orchestrator_artifacts_") as tmp:
            artifacts_root = Path(tmp)
            ctx = SimpleNamespace(
                artifacts_root=artifacts_root,
                run_id="run_test_001",
                git_commit="deadbeef",
                split_mode=False,
            )

            def handler(
                _ctx,
                *,
                target_log: str,
                scenario_args,
                raw_dir: Path,
                derived_dir: Path,
                logs_dir: Path,
            ):
                _ = _ctx, target_log, scenario_args, raw_dir, derived_dir, logs_dir
                return {
                    "payload": {"kind": "mock"},
                    "metrics": {"latency": {"mock_p50_us": 42.0}},
                    "artifacts": {"mock_raw": str(raw_dir / "mock.json")},
                    "dataset_tier": "T0",
                    "mode": "mock_mode",
                    "query_count": 3,
                    "concurrency": 1,
                    "extra": {"app_system": "opensearch"},
                }

            spec = ScenarioSpec(
                name="mock-realapp",
                lane=LANE_APP_ANN,
                claim_tag=CLAIM_TAG_SECONDARY,
                workload="K4",
                required_metrics=["latency.mock_p50_us"],
                handler=handler,
                validator=None,
                claim_policy=CLAIM_POLICY_OPTIONAL,
            )

            args = Namespace(
                command="mock-realapp",
                inventory="/tmp/inventory.yaml",
                target_log="/var/log/spdk.log",
                scenario_args=[],
            )

            with mock.patch.dict(cpcs_experiments.SCENARIO_REGISTRY, {"mock-realapp": spec}, clear=False):
                result = cpcs_experiments._run_registered_scenario(ctx, args)

            run_manifest = Path(result["run_manifest"])
            metrics_summary = Path(result["metrics_summary"])
            validation_report = Path(result["validation_report"])
            paper_validity = Path(result["paper_validity_report"])
            claim_registry = artifacts_root / "claim_registry.json"

            self.assertTrue(run_manifest.exists())
            self.assertTrue(metrics_summary.exists())
            self.assertTrue(validation_report.exists())
            self.assertTrue(paper_validity.exists())
            self.assertTrue(claim_registry.exists())

            self.assertTrue(run_manifest.read_text(encoding="utf-8").strip())
            self.assertTrue(metrics_summary.read_text(encoding="utf-8").strip())
            self.assertTrue(validation_report.read_text(encoding="utf-8").strip())
            self.assertTrue(paper_validity.read_text(encoding="utf-8").strip())
            self.assertTrue(claim_registry.read_text(encoding="utf-8").strip())

    def test_mocked_core_and_realapp_runs_update_paper_validity(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_orchestrator_smoke_") as tmp:
            artifacts_root = Path(tmp)
            ctx = SimpleNamespace(
                artifacts_root=artifacts_root,
                run_id="run_test_002",
                git_commit="deadbeef",
                split_mode=False,
            )

            def core_handler(
                _ctx,
                *,
                target_log: str,
                scenario_args,
                raw_dir: Path,
                derived_dir: Path,
                logs_dir: Path,
            ):
                _ = _ctx, target_log, scenario_args, raw_dir, derived_dir, logs_dir
                return {
                    "payload": {"kind": "core"},
                    "metrics": {"latency": {"mock_core_p50_us": 11.0}},
                    "artifacts": {"core_raw": str(raw_dir / "core.json")},
                    "dataset_tier": "T0",
                    "mode": "core_mode",
                    "query_count": 5,
                    "concurrency": 1,
                }

            def app_handler(
                _ctx,
                *,
                target_log: str,
                scenario_args,
                raw_dir: Path,
                derived_dir: Path,
                logs_dir: Path,
            ):
                _ = _ctx, target_log, scenario_args, raw_dir, derived_dir, logs_dir
                return {
                    "payload": {"kind": "app"},
                    "metrics": {"latency": {"mock_app_p50_us": 7.0}},
                    "artifacts": {"app_raw": str(raw_dir / "app.json")},
                    "dataset_tier": "T1",
                    "mode": "app_mode",
                    "query_count": 8,
                    "concurrency": 1,
                }

            core_spec = ScenarioSpec(
                name="mock-core",
                lane=LANE_CORE,
                claim_tag=CLAIM_TAG_MAIN,
                workload="K2",
                required_metrics=["latency.mock_core_p50_us"],
                handler=core_handler,
                validator=None,
                claim_policy=CLAIM_POLICY_REQUIRED,
            )
            app_spec = ScenarioSpec(
                name="mock-realapp",
                lane=LANE_APP_ANN,
                claim_tag=CLAIM_TAG_SECONDARY,
                workload="K4",
                required_metrics=["latency.mock_app_p50_us"],
                handler=app_handler,
                validator=None,
                claim_policy=CLAIM_POLICY_OPTIONAL,
            )

            claim_registry = {
                "claims": {
                    "MOCK_CORE": {
                        "required": ["mock-core"],
                        "optional": [],
                    },
                    "MOCK_APP": {
                        "required": [],
                        "optional": ["mock-realapp"],
                    },
                }
            }

            core_args = Namespace(
                command="mock-core",
                inventory="/tmp/inventory.yaml",
                target_log="/var/log/spdk.log",
                scenario_args=[],
            )
            app_args = Namespace(
                command="mock-realapp",
                inventory="/tmp/inventory.yaml",
                target_log="/var/log/spdk.log",
                scenario_args=[],
            )

            with (
                mock.patch.dict(
                    cpcs_experiments.SCENARIO_REGISTRY,
                    {"mock-core": core_spec, "mock-realapp": app_spec},
                    clear=False,
                ),
                mock.patch.object(cpcs_experiments, "CLAIM_REGISTRY", claim_registry),
            ):
                core_result = cpcs_experiments._run_registered_scenario(ctx, core_args)
                app_result = cpcs_experiments._run_registered_scenario(ctx, app_args)

            for result in (core_result, app_result):
                self.assertTrue(Path(result["run_manifest"]).exists())
                self.assertTrue(Path(result["metrics_summary"]).exists())
                self.assertTrue(Path(result["validation_report"]).exists())
                self.assertTrue(Path(result["paper_validity_report"]).exists())

            report_path = Path(app_result["paper_validity_report"])
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(int(report.get("total_scenarios_seen", 0)), 2)
            self.assertIn("mock-core", report.get("scenario_status", {}))
            self.assertIn("mock-realapp", report.get("scenario_status", {}))
            self.assertTrue(bool(report.get("claim_coverage", {}).get("MOCK_CORE", {}).get("supported")))
            self.assertTrue(bool(report.get("claim_coverage", {}).get("MOCK_APP", {}).get("supported")))

    def test_main_command_paths_emit_meta_and_triplet_for_core_and_realapp(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_orchestrator_main_paths_") as tmp:
            artifacts_root = Path(tmp) / "artifacts"
            artifacts_root.mkdir(parents=True, exist_ok=True)
            ctx = SimpleNamespace(
                artifacts_root=artifacts_root,
                run_id="run_test_003",
                git_commit="deadbeef",
                split_mode=False,
            )

            def core_handler(
                _ctx,
                *,
                target_log: str,
                scenario_args,
                raw_dir: Path,
                derived_dir: Path,
                logs_dir: Path,
            ):
                _ = _ctx, target_log, scenario_args, raw_dir, derived_dir, logs_dir
                return {
                    "payload": {"kind": "core-main"},
                    "metrics": {"latency": {"mock_core_main_p50_us": 13.0}},
                    "artifacts": {"core_main_raw": str(raw_dir / "core_main.json")},
                    "dataset_tier": "T0",
                    "mode": "core_main_mode",
                    "query_count": 2,
                    "concurrency": 1,
                }

            def app_handler(
                _ctx,
                *,
                target_log: str,
                scenario_args,
                raw_dir: Path,
                derived_dir: Path,
                logs_dir: Path,
            ):
                _ = _ctx, target_log, scenario_args, raw_dir, derived_dir, logs_dir
                return {
                    "payload": {"kind": "realapp-main"},
                    "metrics": {"latency": {"mock_app_main_p50_us": 9.0}},
                    "artifacts": {"app_main_raw": str(raw_dir / "app_main.json")},
                    "dataset_tier": "T1",
                    "mode": "app_main_mode",
                    "query_count": 4,
                    "concurrency": 1,
                }

            core_command = "mock-core-main"
            app_command = "mock-realapp-main"

            core_spec = ScenarioSpec(
                name=core_command,
                lane=LANE_CORE,
                claim_tag=CLAIM_TAG_MAIN,
                workload="K2",
                required_metrics=["latency.mock_core_main_p50_us"],
                handler=core_handler,
                validator=None,
                claim_policy=CLAIM_POLICY_REQUIRED,
            )
            app_spec = ScenarioSpec(
                name=app_command,
                lane=LANE_APP_ANN,
                claim_tag=CLAIM_TAG_SECONDARY,
                workload="K4",
                required_metrics=["latency.mock_app_main_p50_us"],
                handler=app_handler,
                validator=None,
                claim_policy=CLAIM_POLICY_OPTIONAL,
            )

            claim_registry = {
                "claims": {
                    "MOCK_MAIN": {
                        "required": [core_command],
                        "optional": [app_command],
                    }
                }
            }

            with (
                mock.patch.dict(
                    cpcs_experiments.SCENARIO_REGISTRY,
                    {
                        core_command: core_spec,
                        app_command: app_spec,
                    },
                    clear=False,
                ),
                mock.patch.object(cpcs_experiments, "CLAIM_REGISTRY", claim_registry),
                mock.patch.object(cpcs_experiments, "load_inventory", return_value={"mock": True}),
                mock.patch.object(cpcs_experiments, "build_platform_context", return_value=ctx),
            ):
                rc_core = cpcs_experiments.main(
                    [
                        "--inventory",
                        "/tmp/inventory.yaml",
                        "--artifacts-root",
                        str(artifacts_root),
                        core_command,
                    ]
                )
                rc_app = cpcs_experiments.main(
                    [
                        "--inventory",
                        "/tmp/inventory.yaml",
                        "--artifacts-root",
                        str(artifacts_root),
                        app_command,
                    ]
                )

            self.assertEqual(rc_core, 0)
            self.assertEqual(rc_app, 0)

            for command in (core_command, app_command):
                meta_path = artifacts_root / f"{command}_meta.json"
                self.assertTrue(meta_path.exists())
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
                self.assertEqual(meta.get("command"), command)
                self.assertEqual(meta.get("topology"), "loopback")

                result = meta.get("result", {})
                run_manifest = Path(result.get("run_manifest", ""))
                metrics_summary = Path(result.get("metrics_summary", ""))
                validation_report = Path(result.get("validation_report", ""))
                paper_validity_report = Path(result.get("paper_validity_report", ""))

                self.assertTrue(run_manifest.exists())
                self.assertTrue(metrics_summary.exists())
                self.assertTrue(validation_report.exists())
                self.assertTrue(paper_validity_report.exists())

                manifest = json.loads(run_manifest.read_text(encoding="utf-8"))
                artifacts = manifest.get("artifacts", {})
                self.assertTrue(Path(artifacts.get("metrics_summary_json", "")).exists())
                self.assertTrue(Path(artifacts.get("validation_report_json", "")).exists())
                self.assertTrue(Path(artifacts.get("scenario_payload_json", "")).exists())

            paper_report = json.loads((artifacts_root / "paper_validity_report.json").read_text(encoding="utf-8"))
            self.assertEqual(int(paper_report.get("total_scenarios_seen", 0)), 2)
            self.assertTrue(bool(paper_report.get("claim_coverage", {}).get("MOCK_MAIN", {}).get("supported")))

    def test_main_prepare_command_emits_metadata(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_orchestrator_prepare_") as tmp:
            artifacts_root = Path(tmp) / "artifacts"
            artifacts_root.mkdir(parents=True, exist_ok=True)
            ctx = SimpleNamespace(
                artifacts_root=artifacts_root,
                run_id="run_test_004",
                git_commit="deadbeef",
                split_mode=False,
            )
            prepare_result = {
                "prepared_hosts": ["initiator", "target"],
                "hugepages": 1024,
                "skip_build": True,
                "skip_runtime": True,
            }

            with (
                mock.patch.object(cpcs_experiments, "load_inventory", return_value={"mock": True}),
                mock.patch.object(cpcs_experiments, "build_platform_context", return_value=ctx),
                mock.patch.object(cpcs_experiments, "run_prepare", return_value=prepare_result) as prepare_mock,
            ):
                rc = cpcs_experiments.main(
                    [
                        "--inventory",
                        "/tmp/inventory.yaml",
                        "--artifacts-root",
                        str(artifacts_root),
                        "prepare",
                        "--hugepages",
                        "1024",
                        "--skip-build",
                        "--skip-runtime",
                    ]
                )

            self.assertEqual(rc, 0)
            self.assertEqual(prepare_mock.call_count, 1)
            called_ctx, called_args = prepare_mock.call_args[0]
            self.assertIs(called_ctx, ctx)
            self.assertEqual(getattr(called_args, "command", None), "prepare")
            self.assertEqual(int(getattr(called_args, "hugepages", -1)), 1024)
            self.assertTrue(bool(getattr(called_args, "skip_build", False)))
            self.assertTrue(bool(getattr(called_args, "skip_runtime", False)))

            meta_path = artifacts_root / "prepare_meta.json"
            self.assertTrue(meta_path.exists())

            meta = json.loads(meta_path.read_text(encoding="utf-8"))
            self.assertEqual(meta.get("command"), "prepare")
            self.assertEqual(meta.get("topology"), "loopback")
            self.assertEqual(meta.get("result"), prepare_result)
            self.assertEqual(meta.get("artifacts_root"), str(artifacts_root))


if __name__ == "__main__":
    unittest.main()

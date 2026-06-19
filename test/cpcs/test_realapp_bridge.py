#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from scenarios import realapp_bridge


class RealAppBridgeTests(unittest.TestCase):
    def _make_ctx(self, root: Path) -> SimpleNamespace:
        inventory = SimpleNamespace(
            paths=SimpleNamespace(spdk_nvme_passthru="build/bin/spdk_nvme_passthru"),
            nvmeof=SimpleNamespace(
                trtype="TCP",
                traddr="127.0.0.1",
                trsvcid="4420",
                nqn="nqn.2026-01.io.spdk:cnode0",
                hostnqn="nqn.2014-08.org.nvmexpress:uuid:test",
                src_addr=None,
                src_svcid=None,
                dataset_bdev="Nvme0n1",
            ),
            runtime=SimpleNamespace(
                initiator_passthru_lcores="1",
                direct_probe_nsid=None,
                direct_probe_lba_bytes=None,
            ),
        )
        initiator = SimpleNamespace(spec=SimpleNamespace(repo_path=str(root)))
        return SimpleNamespace(repo_root=root, inventory=inventory, initiator=initiator)

    def _write_queries(self, root: Path, count: int) -> Path:
        payload = {
            "version": 1,
            "query_count": count,
            "queries": [{"text": f"q{idx}", "vector": [float(idx), float(idx + 1)]} for idx in range(count)],
        }
        path = root / "queries.json"
        path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        return path

    def _write_docs(self, root: Path) -> Path:
        path = root / "docs.jsonl"
        rows = [
            {"id": "a", "payload": {"flags": 0}},
            {"id": "b", "payload": {"flags": 0}},
            {"id": "c", "payload": {"flags": 1}},
            {"id": "d", "payload": {"flags": 1}},
        ]
        with path.open("w", encoding="utf-8") as fh:
            for row in rows:
                fh.write(json.dumps(row, separators=(",", ":")) + "\n")
        return path

    def test_write_r3_queries_applies_filters_and_limit(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_r3_") as tmp:
            root = Path(tmp)
            source = self._write_queries(root, count=5)
            output = root / "out" / "r3_queries.json"

            count = realapp_bridge._write_r3_queries(
                source_queries_json=source,
                output_path=output,
                flag_value=7,
                max_queries=3,
                label="flags_eq_7",
            )

            self.assertEqual(count, 3)
            doc = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(doc["query_count"], 3)
            self.assertEqual(len(doc["queries"]), 3)
            for idx, row in enumerate(doc["queries"]):
                self.assertEqual(int(row["query_id"]), idx)
                self.assertEqual(int(row["outer_request_id"]), idx)
                self.assertEqual(int(row["round"]), 0)
                self.assertEqual(
                    row["filters"],
                    [{"op": "eq", "field": "flags", "value": 7}],
                )
                self.assertEqual(row["target_selectivity_hint"], "flags_eq_7")

    def test_write_r4_queries_expands_outer_and_rounds(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_r4_") as tmp:
            root = Path(tmp)
            source = self._write_queries(root, count=4)
            output = root / "out" / "r4_queries.json"

            count = realapp_bridge._write_r4_queries(
                source_queries_json=source,
                output_path=output,
                outer_count=2,
                inner_rounds=3,
            )

            self.assertEqual(count, 6)
            doc = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(doc["query_count"], 6)
            self.assertEqual(
                [int(row["outer_request_id"]) for row in doc["queries"]],
                [0, 0, 0, 1, 1, 1],
            )
            self.assertEqual(
                [int(row["round"]) for row in doc["queries"]],
                [0, 1, 2, 0, 1, 2],
            )
            self.assertEqual(
                [int(row["query_id"]) for row in doc["queries"]],
                [0, 1, 2, 3, 4, 5],
            )

    def test_write_r4_queries_rejects_outer_count_overflow(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_r4_err_") as tmp:
            root = Path(tmp)
            source = self._write_queries(root, count=2)
            output = root / "out" / "r4_queries.json"

            with self.assertRaises(RuntimeError):
                realapp_bridge._write_r4_queries(
                    source_queries_json=source,
                    output_path=output,
                    outer_count=3,
                    inner_rounds=2,
                )

    def test_run_realapp_r3_mocked_matrix_outputs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_r3_run_") as tmp:
            root = Path(tmp)
            source_queries = self._write_queries(root, count=3)
            docs_jsonl = self._write_docs(root)
            raw_dir = root / "raw"
            derived_dir = root / "derived"
            logs_dir = root / "logs"
            ctx = self._make_ctx(root)

            def fake_run_realapp_matrix(
                *,
                repo_root: Path,
                app: str,
                queries_json: Path,
                output_dir: Path,
                modes,
                exact_baseline_mode: str,
                exact_match_min: float,
                extra_args: str,
                matrix_runner: str | None,
                **kwargs,
            ) -> None:
                _ = (
                    repo_root,
                    exact_match_min,
                    extra_args,
                    matrix_runner,
                    kwargs,
                )
                doc = json.loads(Path(queries_json).read_text(encoding="utf-8"))
                qcount = int(doc.get("query_count", 0))
                output_dir.mkdir(parents=True, exist_ok=True)
                for mode in modes:
                    summary = {
                        "query_count": qcount,
                        "latency_us": {"p50": 100.0 if mode == exact_baseline_mode else 80.0, "p95": 150.0},
                        "bytes": {"total_per_query": 1000.0 if mode == exact_baseline_mode else 700.0},
                    }
                    (output_dir / f"{app}_{mode}_summary.json").write_text(
                        json.dumps(summary, indent=2),
                        encoding="utf-8",
                    )
                report = {
                    mode: {"exact_match_ratio": 1.0}
                    for mode in modes
                    if mode != exact_baseline_mode
                }
                (output_dir / f"{exact_baseline_mode}_id_exactness_report.json").write_text(
                    json.dumps(report, indent=2),
                    encoding="utf-8",
                )

            with mock.patch.object(realapp_bridge, "_run_realapp_matrix", side_effect=fake_run_realapp_matrix):
                result = realapp_bridge.run_realapp_r3(
                    ctx,
                    scenario_args=[
                        "--system",
                        "opensearch",
                        "--index",
                        "cpcs_realapps",
                        "--docs-jsonl",
                        str(docs_jsonl),
                        "--source-queries-json",
                        str(source_queries),
                        "--flags-values",
                        "0,1",
                        "--max-queries-per-bucket",
                        "2",
                        "--modes",
                        "baseline,cpcs_vslm",
                        "--exact-baseline-mode",
                        "baseline",
                        "--exact-match-min",
                        "0.90",
                    ],
                    raw_dir=raw_dir,
                    derived_dir=derived_dir,
                    logs_dir=logs_dir,
                )

            self.assertEqual(result["query_count"], 4)
            self.assertEqual(int(result["metrics"]["realapp_r3"]["bucket_count"]), 2)
            self.assertTrue(bool(result["metrics"]["realapp_r3"]["exactness_gate_pass"]))

            profile_path = Path(result["artifacts"]["r3_selectivity_profile_json"])
            summary_json = Path(result["artifacts"]["r3_selectivity_summary_json"])
            summary_csv = Path(result["artifacts"]["r3_selectivity_summary_csv"])
            self.assertTrue(profile_path.exists())
            self.assertTrue(summary_json.exists())
            self.assertTrue(summary_csv.exists())

            profile = json.loads(profile_path.read_text(encoding="utf-8"))
            self.assertEqual(int(profile["total_docs"]), 4)
            self.assertEqual(len(profile["buckets"]), 2)

            rows = json.loads(summary_json.read_text(encoding="utf-8"))
            self.assertEqual(len(rows), 4)

    def test_run_realapp_r4_mocked_matrix_outputs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_r4_run_") as tmp:
            root = Path(tmp)
            source_queries = self._write_queries(root, count=3)
            raw_dir = root / "raw"
            derived_dir = root / "derived"
            logs_dir = root / "logs"
            ctx = self._make_ctx(root)

            def fake_run_realapp_matrix(
                *,
                repo_root: Path,
                app: str,
                queries_json: Path,
                output_dir: Path,
                modes,
                exact_baseline_mode: str,
                exact_match_min: float,
                extra_args: str,
                matrix_runner: str | None,
                **kwargs,
            ) -> None:
                _ = (
                    repo_root,
                    exact_match_min,
                    extra_args,
                    matrix_runner,
                    kwargs,
                )
                doc = json.loads(Path(queries_json).read_text(encoding="utf-8"))
                queries = list(doc.get("queries", []))
                qcount = int(doc.get("query_count", len(queries)))
                output_dir.mkdir(parents=True, exist_ok=True)
                for mode in modes:
                    summary = {
                        "query_count": qcount,
                        "latency_us": {"p50": 120.0 if mode == exact_baseline_mode else 90.0, "p95": 160.0},
                        "bytes": {"total_per_query": 1100.0 if mode == exact_baseline_mode else 800.0},
                    }
                    (output_dir / f"{app}_{mode}_summary.json").write_text(
                        json.dumps(summary, indent=2),
                        encoding="utf-8",
                    )

                    run_path = output_dir / f"{app}_{mode}.jsonl"
                    with run_path.open("w", encoding="utf-8") as fh:
                        for row in queries:
                            record = {
                                "outer_request_id": int(row.get("outer_request_id", -1)),
                                "latency_us": (10.0 if mode == exact_baseline_mode else 8.0),
                                "request_bytes": 100.0,
                                "response_bytes": 50.0,
                            }
                            fh.write(json.dumps(record, separators=(",", ":")) + "\n")

                report = {
                    mode: {"exact_match_ratio": 1.0}
                    for mode in modes
                    if mode != exact_baseline_mode
                }
                (output_dir / f"{exact_baseline_mode}_id_exactness_report.json").write_text(
                    json.dumps(report, indent=2),
                    encoding="utf-8",
                )

            with mock.patch.object(realapp_bridge, "_run_realapp_matrix", side_effect=fake_run_realapp_matrix):
                result = realapp_bridge.run_realapp_r4(
                    ctx,
                    scenario_args=[
                        "--system",
                        "opensearch",
                        "--index",
                        "cpcs_realapps",
                        "--source-queries-json",
                        str(source_queries),
                        "--outer-count",
                        "2",
                        "--inner-rounds",
                        "3",
                        "--modes",
                        "baseline,cpcs_vslm",
                        "--exact-baseline-mode",
                        "baseline",
                        "--exact-match-min",
                        "0.90",
                    ],
                    raw_dir=raw_dir,
                    derived_dir=derived_dir,
                    logs_dir=logs_dir,
                )

            self.assertEqual(result["query_count"], 6)
            self.assertTrue(bool(result["metrics"]["realapp_r4"]["exactness_gate_pass"]))
            self.assertEqual(int(result["metrics"]["realapp_r4"]["outer_request_count"]), 2)
            self.assertEqual(int(result["metrics"]["realapp_r4"]["inner_rounds"]), 3)

            queries_path = Path(result["artifacts"]["r4_queries_json"])
            summary_path = Path(result["artifacts"]["r4_outer_summary_json"])
            self.assertTrue(queries_path.exists())
            self.assertTrue(summary_path.exists())

            queries_doc = json.loads(queries_path.read_text(encoding="utf-8"))
            self.assertEqual(int(queries_doc["query_count"]), 6)

            outer = json.loads(summary_path.read_text(encoding="utf-8"))
            self.assertIn("baseline", outer)
            self.assertIn("cpcs_vslm", outer)
            self.assertEqual(int(outer["baseline"]["outer_request_count"]), 2)

    def test_apply_system_defaults_milvus_sets_host_and_port(self) -> None:
        args = SimpleNamespace(
            system="milvus",
            host=None,
            port=None,
            index=None,
            collection="cpcs_realapps_milvus",
        )
        realapp_bridge._apply_system_defaults(args, scenario_name="realapp-r4")
        self.assertEqual(args.host, "127.0.0.1")
        self.assertEqual(int(args.port), 19530)

    def test_apply_hybrid_direct_defaults_auto_probe_from_inventory(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_probe_defaults_") as tmp:
            root = Path(tmp)
            ctx = self._make_ctx(root)
            ctx.inventory.nvmeof.dataset_bdev = "Nvme0n2"
            ctx.inventory.runtime.direct_probe_lba_bytes = 512
            args = SimpleNamespace(
                spdk_nvme_passthru="",
                trtype="",
                traddr="",
                trsvcid="",
                subnqn="",
                hostnqn="",
                src_addr="",
                src_svcid="",
                passthru_lcores="",
                direct_probe_nsid=0,
                direct_probe_lba_bytes=0,
            )

            realapp_bridge._apply_hybrid_direct_defaults(ctx, args)

            self.assertEqual(int(args.direct_probe_nsid), 2)
            self.assertEqual(int(args.direct_probe_lba_bytes), 512)

    def test_run_realapp_matrix_defaults_to_hybrid_runner(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_matrix_default_runner_") as tmp:
            root = Path(tmp)
            captured = {}

            def fake_run_cmd(*, cmd, cwd, log_path, check=True):
                _ = cwd, log_path, check
                captured["cmd"] = list(cmd)
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            with mock.patch.object(realapp_bridge, "_run_cmd", side_effect=fake_run_cmd):
                realapp_bridge._run_realapp_matrix(
                    repo_root=root,
                    app="qdrant",
                    queries_json=root / "queries.json",
                    output_dir=root / "out",
                    modes=["baseline", "cpcs_vslm"],
                    exact_baseline_mode="baseline",
                    exact_match_min=1.0,
                    extra_args="--host http://127.0.0.1:6333 --collection cpcs_realapps_qdrant",
                    matrix_runner=None,
                    hybrid_cpcs_policy="strict",
                    hybrid_direct_runner="",
                    spdk_nvme_passthru="/tmp/spdk_nvme_passthru",
                    trtype="TCP",
                    traddr="127.0.0.1",
                    trsvcid="4420",
                    subnqn="nqn.2026-01.io.spdk:cnode0",
                    hostnqn="nqn.2014-08.org.nvmexpress:uuid:test-host",
                    src_addr="",
                    src_svcid="",
                    passthru_lcores="1",
                    direct_probe_nsid=1,
                    direct_probe_offset=0,
                    direct_probe_length=4096,
                    direct_probe_lba_bytes=4096,
                    log_path=root / "matrix.log",
                )

            cmd = captured["cmd"]
            extra_args = cmd[cmd.index("--extra-args") + 1]
            runner = cmd[cmd.index("--runner") + 1]
            self.assertIn("--realapp-system qdrant", extra_args)
            self.assertIn("--hybrid-cpcs-policy strict", extra_args)
            self.assertEqual(
                runner,
                str((root / realapp_bridge.HYBRID_MATRIX_RUNNER_REL).resolve()),
            )

    def test_run_realapp_matrix_override_runner_skips_hybrid_args(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_matrix_override_runner_") as tmp:
            root = Path(tmp)
            captured = {}

            def fake_run_cmd(*, cmd, cwd, log_path, check=True):
                _ = cwd, log_path, check
                captured["cmd"] = list(cmd)
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            with mock.patch.object(realapp_bridge, "_run_cmd", side_effect=fake_run_cmd):
                realapp_bridge._run_realapp_matrix(
                    repo_root=root,
                    app="qdrant",
                    queries_json=root / "queries.json",
                    output_dir=root / "out",
                    modes=["baseline", "cpcs_vslm"],
                    exact_baseline_mode="baseline",
                    exact_match_min=1.0,
                    extra_args="--host http://127.0.0.1:6333 --collection cpcs_realapps_qdrant",
                    matrix_runner="/tmp/mock_matrix_runner.py",
                    hybrid_cpcs_policy="strict",
                    hybrid_direct_runner="/tmp/direct.py",
                    spdk_nvme_passthru="/tmp/spdk_nvme_passthru",
                    trtype="TCP",
                    traddr="127.0.0.1",
                    trsvcid="4420",
                    subnqn="nqn.2026-01.io.spdk:cnode0",
                    hostnqn="nqn.2014-08.org.nvmexpress:uuid:test-host",
                    src_addr="",
                    src_svcid="",
                    passthru_lcores="1",
                    direct_probe_nsid=1,
                    direct_probe_offset=0,
                    direct_probe_length=4096,
                    direct_probe_lba_bytes=4096,
                    log_path=root / "matrix.log",
                )

            cmd = captured["cmd"]
            extra_args = cmd[cmd.index("--extra-args") + 1]
            runner = cmd[cmd.index("--runner") + 1]
            self.assertEqual(extra_args, "--host http://127.0.0.1:6333 --collection cpcs_realapps_qdrant")
            self.assertEqual(runner, "/tmp/mock_matrix_runner.py")

    def test_run_realapp_b1b2_elasticsearch_routes_index_and_counters(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_b1b2_elasticsearch_") as tmp:
            root = Path(tmp)
            source_queries = self._write_queries(root, count=3)
            raw_dir = root / "raw"
            derived_dir = root / "derived"
            logs_dir = root / "logs"
            ctx = self._make_ctx(root)
            captured_extra_args = []

            def fake_run_realapp_matrix(
                *,
                repo_root: Path,
                app: str,
                queries_json: Path,
                output_dir: Path,
                modes,
                exact_baseline_mode: str,
                exact_match_min: float,
                extra_args: str,
                matrix_runner: str | None,
                **kwargs,
            ) -> None:
                _ = (
                    repo_root,
                    queries_json,
                    exact_match_min,
                    kwargs,
                )
                captured_extra_args.append((app, extra_args, matrix_runner))
                output_dir.mkdir(parents=True, exist_ok=True)
                for mode in modes:
                    summary = {
                        "query_count": 3,
                        "latency_us": {"p50": 55.0 if mode == exact_baseline_mode else 44.0},
                        "bytes": {"total_per_query": 550.0 if mode == exact_baseline_mode else 330.0},
                    }
                    (output_dir / f"{app}_{mode}_summary.json").write_text(
                        json.dumps(summary, indent=2),
                        encoding="utf-8",
                    )
                report = {
                    mode: {"exact_match_ratio": 1.0}
                    for mode in modes
                    if mode != exact_baseline_mode
                }
                (output_dir / f"{exact_baseline_mode}_id_exactness_report.json").write_text(
                    json.dumps(report, indent=2),
                    encoding="utf-8",
                )

            with mock.patch.object(realapp_bridge, "_run_realapp_matrix", side_effect=fake_run_realapp_matrix):
                result = realapp_bridge.run_realapp_b1b2(
                    ctx,
                    scenario_args=[
                        "--system",
                        "elasticsearch",
                        "--index",
                        "cpcs_realapps_es",
                        "--queries-json",
                        str(source_queries),
                        "--bandwidths-gbps",
                        "25",
                        "--modes",
                        "baseline,cpcs_vslm",
                        "--cpcs-counters-path",
                        "/tmp/es_counters.json",
                        "--matrix-runner",
                        "/tmp/mock_runner.py",
                    ],
                    raw_dir=raw_dir,
                    derived_dir=derived_dir,
                    logs_dir=logs_dir,
                )

            self.assertEqual(result["payload"]["system"], "elasticsearch")
            self.assertEqual(result["payload"]["index"], "cpcs_realapps_es")
            self.assertEqual(result["payload"]["host"], "http://127.0.0.1:9201")
            self.assertEqual(len(captured_extra_args), 2)
            for app, extra_args, matrix_runner in captured_extra_args:
                self.assertEqual(app, "elasticsearch")
                self.assertIn("--index cpcs_realapps_es", extra_args)
                self.assertIn("--capture-cpcs-counters", extra_args)
                self.assertIn("--cpcs-counters-path /tmp/es_counters.json", extra_args)
                self.assertNotIn("--collection", extra_args)
                self.assertEqual(matrix_runner, "/tmp/mock_runner.py")

    def test_run_realapp_b1b2_qdrant_routes_collection_and_exact(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_b1b2_qdrant_") as tmp:
            root = Path(tmp)
            source_queries = self._write_queries(root, count=3)
            raw_dir = root / "raw"
            derived_dir = root / "derived"
            logs_dir = root / "logs"
            ctx = self._make_ctx(root)
            captured_extra_args = []

            def fake_run_realapp_matrix(
                *,
                repo_root: Path,
                app: str,
                queries_json: Path,
                output_dir: Path,
                modes,
                exact_baseline_mode: str,
                exact_match_min: float,
                extra_args: str,
                matrix_runner: str | None,
                **kwargs,
            ) -> None:
                _ = (
                    repo_root,
                    queries_json,
                    exact_match_min,
                    matrix_runner,
                    kwargs,
                )
                captured_extra_args.append((app, extra_args))
                output_dir.mkdir(parents=True, exist_ok=True)
                for mode in modes:
                    summary = {
                        "query_count": 3,
                        "latency_us": {"p50": 50.0 if mode == exact_baseline_mode else 40.0},
                        "bytes": {"total_per_query": 500.0 if mode == exact_baseline_mode else 300.0},
                    }
                    (output_dir / f"{app}_{mode}_summary.json").write_text(
                        json.dumps(summary, indent=2),
                        encoding="utf-8",
                    )
                report = {
                    mode: {"exact_match_ratio": 1.0}
                    for mode in modes
                    if mode != exact_baseline_mode
                }
                (output_dir / f"{exact_baseline_mode}_id_exactness_report.json").write_text(
                    json.dumps(report, indent=2),
                    encoding="utf-8",
                )

            with mock.patch.object(realapp_bridge, "_run_realapp_matrix", side_effect=fake_run_realapp_matrix):
                result = realapp_bridge.run_realapp_b1b2(
                    ctx,
                    scenario_args=[
                        "--system",
                        "qdrant",
                        "--collection",
                        "cpcs_realapps_qdrant",
                        "--queries-json",
                        str(source_queries),
                        "--bandwidths-gbps",
                        "10",
                        "--modes",
                        "baseline,cpcs_vslm",
                    ],
                    raw_dir=raw_dir,
                    derived_dir=derived_dir,
                    logs_dir=logs_dir,
                )

            self.assertEqual(result["payload"]["system"], "qdrant")
            self.assertEqual(result["payload"]["collection"], "cpcs_realapps_qdrant")
            self.assertEqual(len(captured_extra_args), 2)
            for app, extra_args in captured_extra_args:
                self.assertEqual(app, "qdrant")
                self.assertIn("--collection cpcs_realapps_qdrant", extra_args)
                self.assertIn("--exact", extra_args)
                self.assertNotIn("--index", extra_args)

    def test_run_realapp_r4_milvus_routes_port_and_offload_endpoint(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_r4_milvus_") as tmp:
            root = Path(tmp)
            source_queries = self._write_queries(root, count=3)
            raw_dir = root / "raw"
            derived_dir = root / "derived"
            logs_dir = root / "logs"
            ctx = self._make_ctx(root)
            captured_extra_args = []

            def fake_run_realapp_matrix(
                *,
                repo_root: Path,
                app: str,
                queries_json: Path,
                output_dir: Path,
                modes,
                exact_baseline_mode: str,
                exact_match_min: float,
                extra_args: str,
                matrix_runner: str | None,
                **kwargs,
            ) -> None:
                _ = (
                    repo_root,
                    exact_match_min,
                    matrix_runner,
                    kwargs,
                )
                captured_extra_args.append((app, extra_args))
                doc = json.loads(Path(queries_json).read_text(encoding="utf-8"))
                queries = list(doc.get("queries", []))
                output_dir.mkdir(parents=True, exist_ok=True)
                for mode in modes:
                    summary = {
                        "query_count": len(queries),
                        "latency_us": {"p50": 120.0 if mode == exact_baseline_mode else 95.0, "p95": 160.0},
                        "bytes": {"total_per_query": 1000.0 if mode == exact_baseline_mode else 700.0},
                    }
                    (output_dir / f"{app}_{mode}_summary.json").write_text(
                        json.dumps(summary, indent=2),
                        encoding="utf-8",
                    )
                    run_path = output_dir / f"{app}_{mode}.jsonl"
                    with run_path.open("w", encoding="utf-8") as fh:
                        for row in queries:
                            record = {
                                "outer_request_id": int(row.get("outer_request_id", -1)),
                                "latency_us": (9.0 if mode == exact_baseline_mode else 7.0),
                                "request_bytes": 90.0,
                                "response_bytes": 40.0,
                            }
                            fh.write(json.dumps(record, separators=(",", ":")) + "\n")

                report = {
                    mode: {"exact_match_ratio": 1.0}
                    for mode in modes
                    if mode != exact_baseline_mode
                }
                (output_dir / f"{exact_baseline_mode}_id_exactness_report.json").write_text(
                    json.dumps(report, indent=2),
                    encoding="utf-8",
                )

            with mock.patch.object(realapp_bridge, "_run_realapp_matrix", side_effect=fake_run_realapp_matrix):
                result = realapp_bridge.run_realapp_r4(
                    ctx,
                    scenario_args=[
                        "--system",
                        "milvus",
                        "--host",
                        "127.0.0.1",
                        "--port",
                        "19531",
                        "--collection",
                        "cpcs_realapps_milvus",
                        "--source-queries-json",
                        str(source_queries),
                        "--outer-count",
                        "2",
                        "--inner-rounds",
                        "2",
                        "--modes",
                        "baseline,cpcs_vslm",
                        "--cpcs-offload-endpoint",
                        "http://127.0.0.1:18082/offload",
                        "--cpcs-offload-timeout-ms",
                        "3210",
                    ],
                    raw_dir=raw_dir,
                    derived_dir=derived_dir,
                    logs_dir=logs_dir,
                )

            self.assertEqual(result["payload"]["system"], "milvus")
            self.assertEqual(result["payload"]["collection"], "cpcs_realapps_milvus")
            self.assertEqual(int(result["payload"]["port"]), 19531)
            self.assertEqual(len(captured_extra_args), 1)
            app, extra_args = captured_extra_args[0]
            self.assertEqual(app, "milvus")
            self.assertIn("--collection cpcs_realapps_milvus", extra_args)
            self.assertIn("--port 19531", extra_args)
            self.assertIn("--cpcs-offload-endpoint http://127.0.0.1:18082/offload", extra_args)

    def test_realapp_b1b2_elasticsearch_requires_index(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_b1b2_elasticsearch_missing_index_") as tmp:
            root = Path(tmp)
            source_queries = self._write_queries(root, count=2)
            ctx = self._make_ctx(root)

            with self.assertRaises(ValueError):
                realapp_bridge.run_realapp_b1b2(
                    ctx,
                    scenario_args=[
                        "--system",
                        "elasticsearch",
                        "--queries-json",
                        str(source_queries),
                    ],
                    raw_dir=root / "raw",
                    derived_dir=root / "derived",
                    logs_dir=root / "logs",
                )

    def test_realapp_b1b2_qdrant_requires_collection(self) -> None:
        with tempfile.TemporaryDirectory(prefix="test_realapp_b1b2_qdrant_missing_collection_") as tmp:
            root = Path(tmp)
            source_queries = self._write_queries(root, count=2)
            ctx = self._make_ctx(root)

            with self.assertRaises(ValueError):
                realapp_bridge.run_realapp_b1b2(
                    ctx,
                    scenario_args=[
                        "--system",
                        "qdrant",
                        "--queries-json",
                        str(source_queries),
                    ],
                    raw_dir=root / "raw",
                    derived_dir=root / "derived",
                    logs_dir=root / "logs",
                )


if __name__ == "__main__":
    unittest.main()

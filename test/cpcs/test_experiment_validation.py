#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import json
import tempfile
import unittest
from pathlib import Path

from experiment_validation import (
    combine_validation_reports,
    compute_ann_quality,
    generate_paper_validity_report,
    validate_lease_correctness_metrics,
    validate_lease_contention_metrics,
    validate_locality_metrics,
    validate_required_metrics,
)


class ExperimentValidationTests(unittest.TestCase):
    def _write_jsonl(self, rows):
        tmp = tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False)
        for row in rows:
            tmp.write(json.dumps(row, separators=(",", ":")) + "\n")
        tmp.flush()
        tmp.close()
        self.addCleanup(lambda: Path(tmp.name).unlink(missing_ok=True))
        return Path(tmp.name)

    def test_validate_required_metrics(self) -> None:
        metrics = {
            "latency": {"ann_p50_us": 123.0},
            "ann_quality": {"ann_recall_at_k": 0.95},
        }
        report = validate_required_metrics(
            metrics,
            [
                "latency.ann_p50_us",
                "ann_quality.ann_recall_at_k",
                "ann_quality.ann_overlap_at_k",
            ],
        )
        self.assertFalse(report["required_metrics_pass"])
        self.assertEqual(report["missing_metric_paths"], ["ann_quality.ann_overlap_at_k"])

    def test_compute_ann_quality_gate_pass(self) -> None:
        baseline = self._write_jsonl(
            [
                {"query_id": 0, "results": [{"id": "1"}, {"id": "2"}, {"id": "3"}]},
                {"query_id": 1, "results": [{"id": "a"}, {"id": "b"}, {"id": "c"}]},
            ]
        )
        approx = self._write_jsonl(
            [
                {"query_id": 0, "results": [{"id": "1"}, {"id": "2"}, {"id": "9"}]},
                {"query_id": 1, "results": [{"id": "a"}, {"id": "b"}, {"id": "x"}]},
            ]
        )

        quality = compute_ann_quality(
            exact_baseline_jsonl=baseline,
            approx_jsonl=approx,
            top_k=3,
            recall_threshold=0.60,
            overlap_threshold=0.60,
        )

        self.assertAlmostEqual(quality["ann_recall_at_k"], 2.0 / 3.0, places=6)
        self.assertAlmostEqual(quality["ann_overlap_at_k"], 2.0 / 3.0, places=6)
        self.assertTrue(quality["ann_quality_gate_pass"])

    def test_compute_ann_quality_gate_fail(self) -> None:
        baseline = self._write_jsonl(
            [
                {"query_id": 0, "results": [{"id": "1"}, {"id": "2"}, {"id": "3"}]},
            ]
        )
        approx = self._write_jsonl(
            [
                {"query_id": 0, "results": [{"id": "4"}, {"id": "5"}, {"id": "6"}]},
            ]
        )

        quality = compute_ann_quality(
            exact_baseline_jsonl=baseline,
            approx_jsonl=approx,
            top_k=3,
            recall_threshold=0.90,
            overlap_threshold=0.80,
        )
        self.assertEqual(quality["ann_recall_at_k"], 0.0)
        self.assertEqual(quality["ann_overlap_at_k"], 0.0)
        self.assertFalse(quality["ann_quality_gate_pass"])

    def test_locality_validator_pass(self) -> None:
        metrics = {
            "locality": {
                "patterns": {
                    "sequential": {"vslm_speedup_x": 2.0},
                    "clustered": {"vslm_speedup_x": 1.7},
                    "strided": {"vslm_speedup_x": 1.3},
                    "random": {"vslm_speedup_x": 1.1},
                }
            }
        }
        report = validate_locality_metrics(metrics)
        self.assertTrue(report["locality_gate_pass"])

    def test_lease_correctness_validator_pass(self) -> None:
        metrics = {
            "lease_correctness": {
                "checks": {
                    "execute_host_read_allowed": True,
                    "execute_host_read_committed_view": True,
                    "execute_overlapping_write_blocked": True,
                    "execute_overlap_95h_rejected": True,
                    "execute_nonoverlapping_write_allowed": True,
                    "publish_on_success": True,
                    "discard_on_failure": True,
                    "eviction_writeback_not_published": True,
                }
            }
        }
        report = validate_lease_correctness_metrics(metrics)
        self.assertTrue(report["lease_correctness_gate_pass"])
        self.assertEqual(report["lease_correctness_missing_checks"], [])
        self.assertEqual(report["lease_correctness_failed_checks"], [])

    def test_lease_correctness_validator_fails_missing_nonoverlap(self) -> None:
        metrics = {
            "lease_correctness": {
                "checks": {
                    "execute_host_read_allowed": True,
                    "execute_host_read_committed_view": True,
                    "execute_overlapping_write_blocked": True,
                    "execute_overlap_95h_rejected": True,
                    "eviction_writeback_not_published": True,
                    "publish_on_success": True,
                    "discard_on_failure": True,
                }
            }
        }
        report = validate_lease_correctness_metrics(metrics)
        self.assertFalse(report["lease_correctness_gate_pass"])
        self.assertEqual(report["lease_correctness_missing_checks"], ["execute_nonoverlapping_write_allowed"])

    def test_lease_correctness_validator_fails_missing_host_read_committed_view(self) -> None:
        metrics = {
            "lease_correctness": {
                "checks": {
                    "execute_host_read_allowed": True,
                    "execute_overlapping_write_blocked": True,
                    "execute_overlap_95h_rejected": True,
                    "execute_nonoverlapping_write_allowed": True,
                    "publish_on_success": True,
                    "discard_on_failure": True,
                    "eviction_writeback_not_published": True,
                }
            }
        }
        report = validate_lease_correctness_metrics(metrics)
        self.assertFalse(report["lease_correctness_gate_pass"])
        self.assertEqual(report["lease_correctness_missing_checks"], ["execute_host_read_committed_view"])

    def test_lease_correctness_validator_fails_missing_eviction_guard(self) -> None:
        metrics = {
            "lease_correctness": {
                "checks": {
                    "execute_host_read_allowed": True,
                    "execute_host_read_committed_view": True,
                    "execute_overlapping_write_blocked": True,
                    "execute_overlap_95h_rejected": True,
                    "execute_nonoverlapping_write_allowed": True,
                    "publish_on_success": True,
                    "discard_on_failure": True,
                }
            }
        }
        report = validate_lease_correctness_metrics(metrics)
        self.assertFalse(report["lease_correctness_gate_pass"])
        self.assertEqual(report["lease_correctness_missing_checks"], ["eviction_writeback_not_published"])

    def test_lease_contention_validator_pass(self) -> None:
        metrics = {
            "lease_contention": {
                "levels": {
                    "none": {"blocked_count": 0, "return_code": 0},
                    "light": {"blocked_count": 1, "return_code": 0},
                    "moderate": {"blocked_count": 2, "return_code": 0},
                    "heavy": {"blocked_count": 3, "return_code": 0},
                }
            }
        }
        report = validate_lease_contention_metrics(metrics)
        self.assertTrue(report["lease_contention_gate_pass"])
        self.assertEqual(report["lease_contention_nonzero_return_levels"], [])

    def test_lease_contention_validator_fails_nonzero_return(self) -> None:
        metrics = {
            "lease_contention": {
                "levels": {
                    "none": {"blocked_count": 0, "return_code": 0},
                    "light": {"blocked_count": 1, "return_code": 1},
                    "moderate": {"blocked_count": 2, "return_code": 0},
                    "heavy": {"blocked_count": 3, "return_code": 0},
                }
            }
        }
        report = validate_lease_contention_metrics(metrics)
        self.assertFalse(report["lease_contention_gate_pass"])
        self.assertEqual(report["lease_contention_nonzero_return_levels"], ["light"])

    def test_claim_policy_required_optional(self) -> None:
        claim_registry = {
            "claims": {
                "RQ1": {
                    "required": ["capacity-gap", "locality"],
                    "optional": ["perf-compare"],
                }
            }
        }
        manifests = [
            {
                "scenario_name": "capacity-gap",
                "validation": {"required_metrics_pass": True, "overall_pass": True},
                "claim_tag": "main_claim",
                "lane": "core_cpcs_vslm",
            },
            {
                "scenario_name": "locality",
                "validation": {"required_metrics_pass": False, "overall_pass": False},
                "claim_tag": "main_claim",
                "lane": "core_cpcs_vslm",
            },
            {
                "scenario_name": "perf-compare",
                "validation": {"required_metrics_pass": True, "overall_pass": True},
                "claim_tag": "supporting_claim",
                "lane": "core_cpcs_vslm",
            },
        ]
        report = generate_paper_validity_report(claim_registry=claim_registry, manifests=manifests)
        rq1 = report["claim_coverage"]["RQ1"]
        self.assertFalse(rq1["supported"])
        self.assertFalse(rq1["required_satisfied"])
        self.assertEqual(rq1["required"]["passed"], 1)
        self.assertEqual(rq1["optional"]["passed"], 1)
        self.assertIn("RQ1", report["under_supported_claims"])

    def test_combine_validation_reports_gate_suffix(self) -> None:
        merged = combine_validation_reports(
            [
                {"required_metrics_pass": True},
                {"locality_gate_pass": True},
                {"realapp_r3_gate_pass": False},
            ]
        )
        self.assertFalse(merged["overall_pass"])


if __name__ == "__main__":
    unittest.main()

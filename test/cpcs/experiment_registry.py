#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict

from experiment_schema import (
    CLAIM_POLICY_OPTIONAL,
    CLAIM_POLICY_REQUIRED,
    CLAIM_TAG_MAIN,
    CLAIM_TAG_SECONDARY,
    CLAIM_TAG_SUPPORTING,
    LANE_APP_ANN,
    LANE_CORE,
    ScenarioSpec,
)
from experiment_validation import (
    validate_lease_contention_metrics,
    validate_lease_correctness_metrics,
    validate_locality_metrics,
    validate_realapp_b1b2_metrics,
    validate_realapp_r3_metrics,
    validate_realapp_r4_metrics,
)
from scenarios.ann_externality import run_ann_search
from scenarios.core import (
    run_capacity_gap,
    run_perf_compare,
    run_transport_bottleneck,
    run_vector_eval,
)
from scenarios.lease_locality import (
    run_lease_contention,
    run_lease_correctness,
    run_locality,
)
from scenarios.realapp_bridge import (
    run_realapp_b1b2,
    run_realapp_r3,
    run_realapp_r4,
)


def build_scenario_registry() -> Dict[str, ScenarioSpec]:
    return {
        "perf-compare": ScenarioSpec(
            name="perf-compare",
            lane=LANE_CORE,
            claim_tag=CLAIM_TAG_MAIN,
            workload="K2",
            required_metrics=[
                "latency.pslm_mean_s",
                "latency.vslm_mean_s",
                "command_count.pslm_copy_mean",
                "command_count.vslm_copy_mean",
            ],
            handler=run_perf_compare,
            validator=None,
            claim_policy=CLAIM_POLICY_OPTIONAL,
        ),
        "capacity-gap": ScenarioSpec(
            name="capacity-gap",
            lane=LANE_CORE,
            claim_tag=CLAIM_TAG_MAIN,
            workload="K2",
            required_metrics=[
                "latency.small_pslm_end_to_end_s",
                "latency.small_vslm_end_to_end_s",
                "command_count.small_pslm_total_control_cmd_count",
                "command_count.small_vslm_total_control_cmd_count",
            ],
            handler=run_capacity_gap,
            validator=None,
            claim_policy=CLAIM_POLICY_REQUIRED,
        ),
        "transport-bottleneck": ScenarioSpec(
            name="transport-bottleneck",
            lane=LANE_CORE,
            claim_tag=CLAIM_TAG_SUPPORTING,
            workload="K2",
            required_metrics=[
                "transport.host_target_bytes_host",
                "transport.host_target_bytes_cpcs",
                "transport.byte_reduction_pct",
            ],
            handler=run_transport_bottleneck,
            validator=None,
            claim_policy=CLAIM_POLICY_REQUIRED,
        ),
        "vector-eval": ScenarioSpec(
            name="vector-eval",
            lane=LANE_CORE,
            claim_tag=CLAIM_TAG_SUPPORTING,
            workload="K2",
            required_metrics=[
                "latency.host_p50_us",
                "latency.cpcs_p50_us",
                "host_visible_bytes.host_avg_total_bytes",
                "host_visible_bytes.cpcs_avg_total_bytes",
                "exactness.k2_mismatch_count",
            ],
            handler=run_vector_eval,
            validator=None,
            claim_policy=CLAIM_POLICY_OPTIONAL,
        ),
        "locality": ScenarioSpec(
            name="locality",
            lane=LANE_CORE,
            claim_tag=CLAIM_TAG_MAIN,
            workload="K2",
            required_metrics=[
                "latency.best_pattern_vslm_speedup_x",
                "latency.worst_pattern_vslm_speedup_x",
                "locality.patterns.sequential.vslm_speedup_x",
                "locality.patterns.random.vslm_speedup_x",
            ],
            handler=run_locality,
            validator=validate_locality_metrics,
            claim_policy=CLAIM_POLICY_REQUIRED,
        ),
        "lease-correctness": ScenarioSpec(
            name="lease-correctness",
            lane=LANE_CORE,
            claim_tag=CLAIM_TAG_MAIN,
            workload="K2",
            required_metrics=[
                "lease_correctness.return_code",
                "lease_correctness.checks.execute_overlap_95h_rejected",
                "lease_correctness.checks.execute_host_read_committed_view",
                "lease_correctness.checks.execute_nonoverlapping_write_allowed",
                "lease_correctness.checks.publish_on_success",
                "lease_correctness.checks.discard_on_failure",
                "lease_correctness.checks.eviction_writeback_not_published",
            ],
            handler=run_lease_correctness,
            validator=validate_lease_correctness_metrics,
            claim_policy=CLAIM_POLICY_REQUIRED,
        ),
        "lease-contention": ScenarioSpec(
            name="lease-contention",
            lane=LANE_CORE,
            claim_tag=CLAIM_TAG_MAIN,
            workload="K2",
            required_metrics=[
                "lease_contention.levels.none.blocked_count",
                "lease_contention.levels.light.blocked_count",
                "lease_contention.levels.moderate.blocked_count",
                "lease_contention.levels.heavy.blocked_count",
            ],
            handler=run_lease_contention,
            validator=validate_lease_contention_metrics,
            claim_policy=CLAIM_POLICY_REQUIRED,
        ),
        "ann-search": ScenarioSpec(
            name="ann-search",
            lane=LANE_APP_ANN,
            claim_tag=CLAIM_TAG_SECONDARY,
            workload="K4",
            required_metrics=[
                "latency.ann_p50_us",
                "host_visible_bytes.ann_total_per_query",
                "result_count.ann_avg",
                "ann_quality.ann_recall_at_k",
                "ann_quality.ann_overlap_at_k",
            ],
            handler=run_ann_search,
            validator=None,
            claim_policy=CLAIM_POLICY_REQUIRED,
        ),
        "realapp-b1b2": ScenarioSpec(
            name="realapp-b1b2",
            lane=LANE_APP_ANN,
            claim_tag=CLAIM_TAG_SECONDARY,
            workload="K4",
            required_metrics=[
                "latency.b1_baseline_p50_us",
                "latency.b1_compare_p50_us",
                "host_visible_bytes.b1_baseline_total_per_query",
                "host_visible_bytes.b1_compare_total_per_query",
                "realapp_b1b2.exactness_gate_pass",
            ],
            handler=run_realapp_b1b2,
            validator=validate_realapp_b1b2_metrics,
            claim_policy=CLAIM_POLICY_OPTIONAL,
        ),
        "realapp-r3": ScenarioSpec(
            name="realapp-r3",
            lane=LANE_APP_ANN,
            claim_tag=CLAIM_TAG_SECONDARY,
            workload="K4",
            required_metrics=[
                "latency.r3_baseline_p50_us_avg",
                "latency.r3_compare_p50_us_avg",
                "host_visible_bytes.r3_baseline_total_per_query_avg",
                "host_visible_bytes.r3_compare_total_per_query_avg",
                "realapp_r3.bucket_count",
                "realapp_r3.exactness_gate_pass",
            ],
            handler=run_realapp_r3,
            validator=validate_realapp_r3_metrics,
            claim_policy=CLAIM_POLICY_OPTIONAL,
        ),
        "realapp-r4": ScenarioSpec(
            name="realapp-r4",
            lane=LANE_APP_ANN,
            claim_tag=CLAIM_TAG_SECONDARY,
            workload="K4",
            required_metrics=[
                "latency.r4_baseline_outer_p50_us",
                "latency.r4_compare_outer_p50_us",
                "host_visible_bytes.r4_baseline_outer_total_bytes_avg",
                "host_visible_bytes.r4_compare_outer_total_bytes_avg",
                "realapp_r4.outer_request_count",
                "realapp_r4.exactness_gate_pass",
            ],
            handler=run_realapp_r4,
            validator=validate_realapp_r4_metrics,
            claim_policy=CLAIM_POLICY_OPTIONAL,
        ),
    }


def build_claim_registry() -> Dict[str, Any]:
    path = Path(__file__).with_name("claim_registry.json")
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError(f"Invalid claim registry payload: {path}")
    return payload

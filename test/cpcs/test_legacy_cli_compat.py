#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import unittest
from argparse import Namespace
from typing import Any, Dict
from unittest import mock

import cpcs_data_movement_compare as data_move
import cpcs_slm_capacity_gap_compare as capacity_gap
import cpcs_transport_bottleneck_compare as transport
import cpcs_vector_eval_compare as vector_eval
import vslm_pslm_perf_compare as perf_compare


class LegacyCliCompatibilityTests(unittest.TestCase):
    def _assert_wrapper(
        self,
        *,
        module: Any,
        wrapper_name: str,
        runner_name: str,
    ) -> None:
        argv = ["--dummy", "1"]
        parsed = Namespace(parsed=True)
        payload: Dict[str, Any] = {"module": module.__name__, "ok": True}

        with (
            mock.patch.object(module, "parse_args", return_value=parsed) as parse_mock,
            mock.patch.object(module, runner_name, return_value=payload) as run_mock,
        ):
            wrapper = getattr(module, wrapper_name)
            result = wrapper(argv)

        parse_mock.assert_called_once_with(argv)
        run_mock.assert_called_once_with(parsed)
        self.assertEqual(result, payload)

    def test_perf_compare_run_benchmark_from_argv_wrapper(self) -> None:
        self._assert_wrapper(
            module=perf_compare,
            wrapper_name="run_benchmark_from_argv",
            runner_name="run_benchmark",
        )

    def test_capacity_gap_run_capacity_gap_from_argv_wrapper(self) -> None:
        self._assert_wrapper(
            module=capacity_gap,
            wrapper_name="run_capacity_gap_from_argv",
            runner_name="run_capacity_gap",
        )

    def test_transport_run_transport_study_from_argv_wrapper(self) -> None:
        self._assert_wrapper(
            module=transport,
            wrapper_name="run_transport_study_from_argv",
            runner_name="run_transport_study",
        )

    def test_data_move_run_comparison_from_argv_wrapper(self) -> None:
        self._assert_wrapper(
            module=data_move,
            wrapper_name="run_comparison_from_argv",
            runner_name="run_comparison",
        )

    def test_vector_eval_run_vector_eval_from_argv_wrapper(self) -> None:
        self._assert_wrapper(
            module=vector_eval,
            wrapper_name="run_vector_eval_from_argv",
            runner_name="run_vector_eval",
        )


if __name__ == "__main__":
    unittest.main()

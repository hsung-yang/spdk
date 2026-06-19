#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import unittest

from scenarios.ann_externality import _parse_args


class AnnExternalityArgCompatTests(unittest.TestCase):
    def _base_argv(self):
        return [
            "--index",
            "cpcs_realapps",
            "--queries-json",
            "/tmp/queries.json",
        ]

    def test_mode_alias_sets_ann_mode(self) -> None:
        args = _parse_args(self._base_argv() + ["--mode", "cpcs_fullfit"])
        self.assertEqual(args.baseline_mode, "baseline")
        self.assertEqual(args.ann_mode, "cpcs_fullfit")

    def test_modes_alias_sets_baseline_and_ann(self) -> None:
        args = _parse_args(self._base_argv() + ["--modes", "baseline,cpcs_vslm"])
        self.assertEqual(args.baseline_mode, "baseline")
        self.assertEqual(args.ann_mode, "cpcs_vslm")

    def test_explicit_ann_mode_overrides_aliases(self) -> None:
        args = _parse_args(
            self._base_argv()
            + [
                "--modes",
                "baseline,cpcs_fullfit",
                "--mode",
                "cpcs_fullfit",
                "--ann-mode",
                "cpcs_vslm",
            ]
        )
        self.assertEqual(args.ann_mode, "cpcs_vslm")
        self.assertEqual(args.baseline_mode, "baseline")


if __name__ == "__main__":
    unittest.main()

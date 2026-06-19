#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause

"""
How to run:

1) From SPDK root:
   python3 -m unittest discover -s test/cpcs -p 'test_cpcs_experiments.py' -v

2) From test/cpcs:
   python3 -m unittest -v test_cpcs_experiments.py
"""

import tempfile
import textwrap
import unittest
import inspect
from pathlib import Path

from cpcs_experiments import SCENARIO_REGISTRY, build_platform_context, load_inventory, parse_args

try:
    import yaml  # type: ignore  # noqa: F401
    HAS_YAML = True
except Exception:
    HAS_YAML = False


@unittest.skipUnless(HAS_YAML, "PyYAML is required for inventory parsing tests")
class CpcsExperimentsInventoryTests(unittest.TestCase):
    def _write_inventory(self, body: str) -> str:
        tmp = tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False)
        tmp.write(textwrap.dedent(body))
        tmp.flush()
        tmp.close()
        self.addCleanup(lambda: Path(tmp.name).unlink(missing_ok=True))
        return tmp.name

    def test_loopback_inventory(self) -> None:
        inv_path = self._write_inventory(
            """
            hosts:
              local:
                mode: local
                repo_path: /tmp
                use_sudo: false
            roles:
              initiator: local
              target: local
            nvmeof:
              trtype: TCP
              traddr: 127.0.0.1
              trsvcid: "4420"
              nqn: nqn.test
              hostnqn: nqn.host
            """
        )
        inv = load_inventory(inv_path)
        ctx = build_platform_context(inv, artifacts_root=None)
        self.assertFalse(ctx.split_mode)
        self.assertEqual(ctx.initiator.spec.mode, "local")
        self.assertEqual(ctx.target.spec.mode, "local")

    def test_split_inventory(self) -> None:
        inv_path = self._write_inventory(
            """
            hosts:
              initiator:
                mode: local
                repo_path: /tmp
                use_sudo: false
              target:
                mode: ssh
                ssh_host: 10.0.0.22
                ssh_user: user
                ssh_port: 22
                repo_path: /tmp
                use_sudo: false
            roles:
              initiator: initiator
              target: target
            nvmeof:
              trtype: TCP
              traddr: 10.0.0.22
              trsvcid: "4420"
              nqn: nqn.test
              hostnqn: nqn.host
            """
        )
        inv = load_inventory(inv_path)
        ctx = build_platform_context(inv, artifacts_root=None)
        self.assertTrue(ctx.split_mode)
        self.assertEqual(ctx.target.spec.mode, "ssh")


class CpcsExperimentsArgparseTests(unittest.TestCase):
    def test_scenario_args_without_delimiter(self) -> None:
        args = parse_args(
            [
                "--inventory",
                "/tmp/inventory.yaml",
                "vector-eval",
                "--query-limit",
                "7",
                "--backend",
                "vslm",
            ]
        )
        self.assertEqual(args.command, "vector-eval")
        self.assertEqual(args.scenario_args, ["--query-limit", "7", "--backend", "vslm"])

    def test_prepare_rejects_unknown_args(self) -> None:
        with self.assertRaises(SystemExit):
            parse_args(["--inventory", "/tmp/inventory.yaml", "prepare", "--bogus"])

    def test_ann_search_collects_scenario_args(self) -> None:
        args = parse_args(
            [
                "--inventory",
                "/tmp/inventory.yaml",
                "ann-search",
                "--index",
                "cpcs_realapps",
                "--queries-json",
                "/tmp/queries.json",
                "--top-k",
                "20",
            ]
        )
        self.assertEqual(args.command, "ann-search")
        self.assertEqual(
            args.scenario_args,
            ["--index", "cpcs_realapps", "--queries-json", "/tmp/queries.json", "--top-k", "20"],
        )

    def test_registry_has_ann_search(self) -> None:
        self.assertIn("ann-search", SCENARIO_REGISTRY)
        self.assertIn("realapp-b1b2", SCENARIO_REGISTRY)
        self.assertIn("realapp-r3", SCENARIO_REGISTRY)
        self.assertIn("realapp-r4", SCENARIO_REGISTRY)
        self.assertIn("locality", SCENARIO_REGISTRY)
        self.assertIn("lease-correctness", SCENARIO_REGISTRY)
        self.assertIn("lease-contention", SCENARIO_REGISTRY)

    def test_realapp_r4_collects_scenario_args(self) -> None:
        args = parse_args(
            [
                "--inventory",
                "/tmp/inventory.yaml",
                "realapp-r4",
                "--index",
                "cpcs_realapps",
                "--source-queries-json",
                "/tmp/queries.json",
                "--outer-count",
                "10",
                "--inner-rounds",
                "5",
            ]
        )
        self.assertEqual(args.command, "realapp-r4")
        self.assertEqual(
            args.scenario_args,
            [
                "--index",
                "cpcs_realapps",
                "--source-queries-json",
                "/tmp/queries.json",
                "--outer-count",
                "10",
                "--inner-rounds",
                "5",
            ],
        )

    def test_realapp_b1b2_collects_system_scenario_args(self) -> None:
        args = parse_args(
            [
                "--inventory",
                "/tmp/inventory.yaml",
                "realapp-b1b2",
                "--system",
                "qdrant",
                "--collection",
                "cpcs_realapps_qdrant",
                "--queries-json",
                "/tmp/queries.json",
                "--bandwidths-gbps",
                "25",
            ]
        )
        self.assertEqual(args.command, "realapp-b1b2")
        self.assertEqual(
            args.scenario_args,
            [
                "--system",
                "qdrant",
                "--collection",
                "cpcs_realapps_qdrant",
                "--queries-json",
                "/tmp/queries.json",
                "--bandwidths-gbps",
                "25",
            ],
        )

    def test_registered_scenario_handlers_accept_logs_dir(self) -> None:
        for name, spec in SCENARIO_REGISTRY.items():
            params = inspect.signature(spec.handler).parameters
            self.assertIn("logs_dir", params, msg=f"{name} handler missing logs_dir parameter")


if __name__ == "__main__":
    unittest.main()

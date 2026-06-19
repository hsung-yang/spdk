#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import builtins
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest import mock

from experiment_platform import load_inventory


class InventoryYamlFallbackTests(unittest.TestCase):
    def _write_inventory(self, root: Path, text: str) -> Path:
        path = root / "inventory.yaml"
        path.write_text(text, encoding="utf-8")
        return path

    def _import_without_yaml(self):
        original_import = builtins.__import__

        def fake_import(name, globals=None, locals=None, fromlist=(), level=0):  # type: ignore[no-untyped-def]
            if name == "yaml":
                raise ImportError("yaml unavailable for fallback test")
            return original_import(name, globals, locals, fromlist, level)

        return fake_import

    def test_load_inventory_fallback_minimal_yaml(self) -> None:
        content = """
hosts:
  local:
    mode: local
    repo_path: /tmp/spdk
roles:
  initiator: local
  target: local
nvmeof:
  trtype: TCP
  traddr: 127.0.0.1
  trsvcid: "4420"
  nqn: nqn.2026-03.io.spdk:cpcs-exp
  hostnqn: nqn.2026-03.io.spdk:cpcs-exp-host
"""
        with TemporaryDirectory() as td:
            inv_path = self._write_inventory(Path(td), content)
            with mock.patch("builtins.__import__", side_effect=self._import_without_yaml()):
                inventory = load_inventory(str(inv_path))

        self.assertEqual(inventory.initiator_role, "local")
        self.assertEqual(inventory.target_role, "local")
        self.assertEqual(inventory.nvmeof.trsvcid, "4420")
        self.assertEqual(inventory.hosts["local"].repo_path, "/tmp/spdk")

    def test_load_inventory_fallback_parses_ssh_options_list(self) -> None:
        content = """
hosts:
  initiator:
    mode: local
    repo_path: /tmp/spdk
  target:
    mode: ssh
    ssh_host: 10.0.0.22
    ssh_user: user
    ssh_port: 22
    ssh_options:
      - StrictHostKeyChecking=no
      - UserKnownHostsFile=/dev/null
    repo_path: /tmp/spdk
roles:
  initiator: initiator
  target: target
nvmeof:
  trtype: TCP
  traddr: 10.0.0.22
  trsvcid: "4420"
  nqn: nqn.2026-03.io.spdk:cpcs-exp
  hostnqn: nqn.2026-03.io.spdk:cpcs-exp-host
"""
        with TemporaryDirectory() as td:
            inv_path = self._write_inventory(Path(td), content)
            with mock.patch("builtins.__import__", side_effect=self._import_without_yaml()):
                inventory = load_inventory(str(inv_path))

        self.assertEqual(inventory.hosts["target"].mode, "ssh")
        self.assertEqual(
            inventory.hosts["target"].ssh_options,
            ["StrictHostKeyChecking=no", "UserKnownHostsFile=/dev/null"],
        )

    def test_load_inventory_fallback_parses_direct_probe_runtime_fields(self) -> None:
        content = """
hosts:
  local:
    mode: local
    repo_path: /tmp/spdk
roles:
  initiator: local
  target: local
runtime:
  direct_probe_nsid: 2
  direct_probe_lba_bytes: 512
nvmeof:
  trtype: TCP
  traddr: 127.0.0.1
  trsvcid: "4420"
  nqn: nqn.2026-03.io.spdk:cpcs-exp
  hostnqn: nqn.2026-03.io.spdk:cpcs-exp-host
"""
        with TemporaryDirectory() as td:
            inv_path = self._write_inventory(Path(td), content)
            with mock.patch("builtins.__import__", side_effect=self._import_without_yaml()):
                inventory = load_inventory(str(inv_path))

        self.assertEqual(int(inventory.runtime.direct_probe_nsid), 2)
        self.assertEqual(int(inventory.runtime.direct_probe_lba_bytes), 512)


if __name__ == "__main__":
    unittest.main()

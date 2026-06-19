#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.

import argparse
import pathlib
import subprocess
import sys


def run_command(cmd, cwd=None):
    print("+", " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    if proc.stdout:
        print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, end="", file=sys.stderr)
    return proc.returncode, proc.stdout + proc.stderr


def ensure_slm_ut(spdk_root: pathlib.Path, slm_ut: pathlib.Path) -> bool:
    if slm_ut.exists():
        return True

    slm_dir = spdk_root / "test/unit/lib/bdev/slm"
    if not slm_dir.exists():
        print(f"Missing unit test directory: {slm_dir}", file=sys.stderr)
        return False

    print(f"Missing unit binary: {slm_ut}", file=sys.stderr)
    print("Attempting to build slm_ut...", file=sys.stderr)

    rc, output = run_command(["make", "-C", str(slm_dir)])
    if rc != 0:
        # In constrained smoke environments, configure may enable ISAL/crypto
        # while the static libs are not built yet. Retry with local overrides.
        needs_isal_override = (
            "libisal.a" in output or "libisal_crypto.a" in output
        )
        if not needs_isal_override:
            print("Failed to build slm_ut via make -C test/unit/lib/bdev/slm", file=sys.stderr)
            return False
        print(
            "slm_ut build failed due missing ISAL libs; retrying with "
            "CONFIG_ISAL=n CONFIG_ISAL_CRYPTO=n",
            file=sys.stderr,
        )
        rc, _ = run_command(
            [
                "make",
                "-C",
                str(slm_dir),
                "CONFIG_ISAL=n",
                "CONFIG_ISAL_CRYPTO=n",
            ]
        )
        if rc != 0:
            print("Failed fallback build for slm_ut with CONFIG_ISAL=n", file=sys.stderr)
            return False

    if slm_ut.exists():
        return True

    # Some build environments need the explicit target after directory make.
    rc, _ = run_command(["make", "-C", str(slm_dir), "slm_ut"])
    if rc != 0:
        print("Failed to build explicit target slm_ut", file=sys.stderr)
        return False

    return slm_ut.exists()


def main():
    parser = argparse.ArgumentParser(
        description="Run pSLM lease-conflict scenario validation."
    )
    parser.add_argument(
        "--spdk-root",
        default=str(pathlib.Path(__file__).resolve().parents[2]),
        help="Path to SPDK repository root",
    )
    args = parser.parse_args()

    spdk_root = pathlib.Path(args.spdk_root).resolve()
    slm_ut = spdk_root / "test/unit/lib/bdev/slm/slm_ut"

    if not ensure_slm_ut(spdk_root, slm_ut):
        print(f"Missing unit binary after build attempt: {slm_ut}", file=sys.stderr)
        return 2

    rc, output = run_command([str(slm_ut)])
    if rc != 0:
        print("slm_ut failed; pSLM lease scenario not validated.", file=sys.stderr)
        return rc

    required_marker = "test_slm_bdev_lease_access ...passed"
    if required_marker not in output:
        print(
            f"Expected marker not found in output: '{required_marker}'",
            file=sys.stderr,
        )
        return 1

    print("pSLM lease scenario passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

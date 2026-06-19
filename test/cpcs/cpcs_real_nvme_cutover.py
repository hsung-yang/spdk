#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Generate a CPCS inventory for real-NVMe cutover.

This helper creates an inventory YAML wired to a physical NVMe controller
and validates that selected dataset/backing namespaces exist and are large
enough for CPCS experiments.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
from typing import Dict, List, Optional, Sequence, Tuple

SECTOR_BYTES = 512


@dataclass(frozen=True)
class NamespaceInfo:
    nsid: int
    block_dev: str
    size_bytes: int
    lba_bytes: int


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8").strip()


def _read_int(path: Path, default: Optional[int] = None) -> int:
    if not path.exists():
        if default is None:
            raise FileNotFoundError(str(path))
        return default
    return int(_read_text(path))


def _parse_nsid_from_name(name: str) -> Optional[int]:
    # Expected namespace names: nvme0n1, nvme1n2, ...
    m = re.match(r"^nvme\d+n(\d+)$", name)
    if not m:
        return None
    return int(m.group(1))


def _fmt_gib(size_bytes: int) -> float:
    return float(size_bytes) / float(1 << 30)


def _detect_controller_namespaces(pcie_bdf: str) -> Tuple[str, List[NamespaceInfo]]:
    dev_dir = Path("/sys/bus/pci/devices") / pcie_bdf
    if not dev_dir.exists():
        raise RuntimeError(f"PCIe device path not found: {dev_dir}")

    nvme_dir = dev_dir / "nvme"
    if not nvme_dir.exists():
        raise RuntimeError(f"No NVMe controller bound under: {nvme_dir}")

    controllers = sorted([p.name for p in nvme_dir.iterdir() if p.is_dir()])
    if not controllers:
        raise RuntimeError(f"No NVMe controllers found under: {nvme_dir}")

    controller = controllers[0]
    class_nvme = Path("/sys/class/nvme") / controller
    if not class_nvme.exists():
        raise RuntimeError(f"Controller class path missing: {class_nvme}")

    ns_nodes = sorted(class_nvme.glob(f"{controller}n*"), key=lambda p: p.name)
    infos: List[NamespaceInfo] = []
    for node in ns_nodes:
        ns_name = node.name
        nsid_file = node / "nsid"
        nsid = _read_int(nsid_file, default=_parse_nsid_from_name(ns_name) or -1)
        if nsid <= 0:
            continue

        blk = Path("/sys/class/block") / ns_name
        sectors = _read_int(blk / "size", default=0)
        lba_bytes = _read_int(blk / "queue" / "logical_block_size", default=SECTOR_BYTES)
        size_bytes = int(sectors) * SECTOR_BYTES
        infos.append(
            NamespaceInfo(
                nsid=int(nsid),
                block_dev=ns_name,
                size_bytes=size_bytes,
                lba_bytes=int(lba_bytes),
            )
        )

    infos.sort(key=lambda x: x.nsid)
    return controller, infos


def _inventory_yaml(
    *,
    repo_path: str,
    trtype: str,
    traddr: str,
    trsvcid: str,
    nqn: str,
    hostnqn: str,
    pcie_bdf: str,
    dataset_bdev: str,
    backing_bdev: str,
    passthru_lcores: str,
    direct_probe_nsid: int,
    direct_probe_lba_bytes: int,
) -> str:
    # Keep this in simple YAML subset compatible with experiment_platform fallback parser.
    return (
        "hosts:\n"
        "  local:\n"
        "    mode: local\n"
        f"    repo_path: {repo_path}\n"
        "    use_sudo: true\n"
        "\n"
        "roles:\n"
        "  initiator: local\n"
        "  target: local\n"
        "\n"
        "paths:\n"
        "  rpc_py: scripts/rpc.py\n"
        "  spdk_tgt: build/bin/spdk_tgt\n"
        "  spdk_nvme_passthru: build/bin/spdk_nvme_passthru\n"
        "\n"
        "runtime:\n"
        f"  initiator_passthru_lcores: \"{passthru_lcores}\"\n"
        f"  direct_probe_nsid: {int(direct_probe_nsid)}\n"
        f"  direct_probe_lba_bytes: {int(direct_probe_lba_bytes)}\n"
        "\n"
        "nvmeof:\n"
        f"  trtype: {trtype}\n"
        f"  traddr: {traddr}\n"
        f"  trsvcid: \"{trsvcid}\"\n"
        f"  nqn: {nqn}\n"
        f"  hostnqn: {hostnqn}\n"
        f"  pcie_bdf: {pcie_bdf}\n"
        f"  dataset_bdev: {dataset_bdev}\n"
        f"  backing_bdev: {backing_bdev}\n"
    )


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate inventory YAML for real NVMe CPCS cutover")
    p.add_argument("--repo-path", required=True, help="SPDK repo path on the run host")
    p.add_argument("--output", required=True, help="Output inventory YAML path")

    p.add_argument("--pcie-bdf", default="0000:11:00.0", help="PCIe BDF (default: 0000:11:00.0)")
    p.add_argument("--controller-name", default="Nvme0", help="SPDK NVMe controller alias")
    p.add_argument("--dataset-nsid", type=int, default=1, help="Namespace NSID for dataset/NVM namespace")
    p.add_argument("--backing-nsid", type=int, default=2, help="Namespace NSID for backing namespace")
    p.add_argument(
        "--min-namespace-gib",
        type=float,
        default=3.0,
        help="Minimum namespace size check in GiB (default: 3.0)",
    )
    p.add_argument(
        "--skip-sysfs-probe",
        action="store_true",
        help="Skip Linux sysfs validation and emit YAML directly",
    )

    p.add_argument("--trtype", default="TCP")
    p.add_argument("--traddr", default="127.0.0.1")
    p.add_argument("--trsvcid", default="4420")
    p.add_argument("--nqn", default="nqn.2026-03.io.spdk:cpcs-exp")
    p.add_argument("--hostnqn", default="nqn.2026-03.io.spdk:cpcs-exp-host")
    p.add_argument("--passthru-lcores", default="1")
    return p.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)

    if args.dataset_nsid <= 0 or args.backing_nsid <= 0:
        raise ValueError("--dataset-nsid and --backing-nsid must be > 0")
    if args.dataset_nsid == args.backing_nsid:
        raise ValueError("--dataset-nsid and --backing-nsid must be different")

    dataset_bdev = f"{args.controller_name}n{args.dataset_nsid}"
    backing_bdev = f"{args.controller_name}n{args.backing_nsid}"
    direct_probe_nsid = int(args.dataset_nsid)
    direct_probe_lba_bytes = 4096

    if not args.skip_sysfs_probe:
        controller, infos = _detect_controller_namespaces(str(args.pcie_bdf))
        info_by_nsid: Dict[int, NamespaceInfo] = {i.nsid: i for i in infos}

        if args.dataset_nsid not in info_by_nsid:
            available = ", ".join(str(i.nsid) for i in infos) or "none"
            raise RuntimeError(
                f"Dataset NSID {args.dataset_nsid} not found on {args.pcie_bdf}. "
                f"Available NSIDs: {available}"
            )
        if args.backing_nsid not in info_by_nsid:
            available = ", ".join(str(i.nsid) for i in infos) or "none"
            raise RuntimeError(
                f"Backing NSID {args.backing_nsid} not found on {args.pcie_bdf}. "
                f"Available NSIDs: {available}"
            )

        min_bytes = int(float(args.min_namespace_gib) * (1 << 30))
        dataset_info = info_by_nsid[args.dataset_nsid]
        backing_info = info_by_nsid[args.backing_nsid]
        direct_probe_lba_bytes = int(dataset_info.lba_bytes)

        if dataset_info.size_bytes < min_bytes:
            raise RuntimeError(
                f"Dataset NSID {dataset_info.nsid} size too small: "
                f"{_fmt_gib(dataset_info.size_bytes):.2f} GiB < {args.min_namespace_gib:.2f} GiB"
            )
        if backing_info.size_bytes < min_bytes:
            raise RuntimeError(
                f"Backing NSID {backing_info.nsid} size too small: "
                f"{_fmt_gib(backing_info.size_bytes):.2f} GiB < {args.min_namespace_gib:.2f} GiB"
            )

        print(
            "Detected controller/namespace mapping:\n"
            f"  PCIe BDF: {args.pcie_bdf}\n"
            f"  Linux controller: {controller}\n"
            f"  SPDK controller alias: {args.controller_name}\n"
            f"  Dataset NSID {dataset_info.nsid}: {dataset_info.block_dev}, "
            f"{_fmt_gib(dataset_info.size_bytes):.2f} GiB, LBA={dataset_info.lba_bytes}\n"
            f"  Backing NSID {backing_info.nsid}: {backing_info.block_dev}, "
            f"{_fmt_gib(backing_info.size_bytes):.2f} GiB, LBA={backing_info.lba_bytes}",
            flush=True,
        )

    text = _inventory_yaml(
        repo_path=str(args.repo_path),
        trtype=str(args.trtype),
        traddr=str(args.traddr),
        trsvcid=str(args.trsvcid),
        nqn=str(args.nqn),
        hostnqn=str(args.hostnqn),
        pcie_bdf=str(args.pcie_bdf),
        dataset_bdev=dataset_bdev,
        backing_bdev=backing_bdev,
        passthru_lcores=str(args.passthru_lcores),
        direct_probe_nsid=direct_probe_nsid,
        direct_probe_lba_bytes=direct_probe_lba_bytes,
    )

    out_path = Path(args.output).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(text, encoding="utf-8")

    print(f"Wrote inventory: {out_path}")
    print(f"  dataset_bdev: {dataset_bdev}")
    print(f"  backing_bdev: {backing_bdev}")
    print(f"  runtime.direct_probe_nsid: {direct_probe_nsid}")
    print(f"  runtime.direct_probe_lba_bytes: {direct_probe_lba_bytes}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

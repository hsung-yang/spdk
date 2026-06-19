#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any, Dict, List


def load_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def parse_bandwidths(text: str) -> List[float]:
    out = []
    for token in text.split(","):
        v = float(token.strip())
        if v <= 0.0:
            raise ValueError("bandwidth values must be > 0")
        out.append(v)
    return out


def gbps_to_bytes_per_sec(gbps: float) -> float:
    return (gbps * 1_000_000_000.0) / 8.0


def build_transport_rows(host: Dict[str, Any], cpcs: Dict[str, Any], bandwidths: List[float]) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for link in bandwidths:
        bw = gbps_to_bytes_per_sec(link)
        host_t = host["summary"]["avg_host_total_bytes"] / bw
        cpcs_t = cpcs["summary"]["avg_host_total_bytes"] / bw
        rows.append(
            {
                "family": "B2",
                "mode_pair": "H0_vs_C0",
                "link_gbps": link,
                "host_transport_seconds": host_t,
                "cpcs_transport_seconds": cpcs_t,
                "transport_speedup_x": (host_t / cpcs_t) if cpcs_t > 0 else 0.0,
                "host_avg_bytes": host["summary"]["avg_host_total_bytes"],
                "cpcs_avg_bytes": cpcs["summary"]["avg_host_total_bytes"],
            }
        )
    return rows


def build_loop_rows(host: Dict[str, Any], cpcs: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [
        {
            "family": "B3",
            "mode_pair": "H0_vs_C0",
            "host_outer_latency_p50_us": host["summary"].get("outer_latency_us_p50", 0.0),
            "cpcs_outer_latency_p50_us": cpcs["summary"].get("outer_latency_us_p50", 0.0),
            "host_outer_bytes_avg": host["summary"].get("outer_total_bytes_avg", 0.0),
            "cpcs_outer_bytes_avg": cpcs["summary"].get("outer_total_bytes_avg", 0.0),
        }
    ]


def build_capacity_rows(c0: Dict[str, Any], c1: Dict[str, Any], c2: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [
        {
            "family": "C1",
            "mode": "C0",
            "avg_latency_p50_us": c0["summary"]["latency_us"]["p50"],
            "avg_host_total_bytes": c0["summary"]["avg_host_total_bytes"],
            "avg_command_count": c0["summary"].get("avg_command_count", 1),
            "avg_vslm_fault_bytes": c0["summary"].get("avg_vslm_fault_read_bytes", 0.0)
            + c0["summary"].get("avg_vslm_fault_write_bytes", 0.0),
        },
        {
            "family": "C1",
            "mode": "C1",
            "avg_latency_p50_us": c1["summary"]["latency_us"]["p50"],
            "avg_host_total_bytes": c1["summary"]["avg_host_total_bytes"],
            "avg_command_count": c1["summary"].get("avg_command_count", 1),
            "avg_vslm_fault_bytes": c1["summary"].get("avg_vslm_fault_read_bytes", 0.0)
            + c1["summary"].get("avg_vslm_fault_write_bytes", 0.0),
        },
        {
            "family": "C1",
            "mode": "C2",
            "avg_latency_p50_us": c2["summary"]["latency_us"]["p50"],
            "avg_host_total_bytes": c2["summary"]["avg_host_total_bytes"],
            "avg_command_count": c2["summary"].get("avg_command_count", 1),
            "avg_vslm_fault_bytes": c2["summary"].get("avg_vslm_fault_read_bytes", 0.0)
            + c2["summary"].get("avg_vslm_fault_write_bytes", 0.0),
        },
    ]


def write_rows(path: str, rows: List[Dict[str, Any]]) -> None:
    if not rows:
        raise ValueError("no rows to write")
    fieldnames = sorted({k for row in rows for k in row.keys()})
    with open(path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compose CPCS vector experiment matrices and CSVs")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_transport = sub.add_parser("transport", help="Build transport matrix (H0 vs C0/C2)")
    p_transport.add_argument("--host-json", required=True)
    p_transport.add_argument("--cpcs-json", required=True)
    p_transport.add_argument("--bandwidth-gbps", default="1,10,25,100")
    p_transport.add_argument("--output-csv", required=True)

    p_loop = sub.add_parser("loop", help="Build multi-round loop comparison matrix")
    p_loop.add_argument("--host-json", required=True)
    p_loop.add_argument("--cpcs-json", required=True)
    p_loop.add_argument("--output-csv", required=True)

    p_capacity = sub.add_parser("capacity", help="Build C0/C1/C2 capacity matrix")
    p_capacity.add_argument("--c0-json", required=True)
    p_capacity.add_argument("--c1-json", required=True)
    p_capacity.add_argument("--c2-json", required=True)
    p_capacity.add_argument("--output-csv", required=True)

    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.cmd == "transport":
        host = load_json(args.host_json)
        cpcs = load_json(args.cpcs_json)
        rows = build_transport_rows(host, cpcs, parse_bandwidths(args.bandwidth_gbps))
        write_rows(args.output_csv, rows)
    elif args.cmd == "loop":
        host = load_json(args.host_json)
        cpcs = load_json(args.cpcs_json)
        rows = build_loop_rows(host, cpcs)
        write_rows(args.output_csv, rows)
    else:
        c0 = load_json(args.c0_json)
        c1 = load_json(args.c1_json)
        c2 = load_json(args.c2_json)
        rows = build_capacity_rows(c0, c1, c2)
        write_rows(args.output_csv, rows)

    print(f"Wrote CSV: {args.output_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

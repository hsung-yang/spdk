#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

from __future__ import annotations

import argparse
import json
import re
import struct
import tempfile
import time
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

from cpcs_vector_common import (
    FLAG_STABLE_TIE_DOCID,
    FILTER_CLAUSE_STRUCT,
    OP_FILTERED_TOPK_EXACT,
    TOPK_REC_STRUCT,
    encode_filtered_topk_request,
    summarize_latency_us,
)


RESULT_HDR_STRUCT = struct.Struct("<IHHIIII")
RESULT_STATS_STRUCT = struct.Struct("<10Q")
RESULT_TOPK_HDR_STRUCT = struct.Struct("<HHI")

PIND_FILTER_AGG = 5
PIND_FILTERED_TOPK_EXACT = 6


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run CPCS builtin_filtered_topk_exact queries via nvme passthru")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--queries", default=None)

    parser.add_argument("--spdk-nvme-passthru", required=True)
    parser.add_argument("--trtype", default="TCP")
    parser.add_argument("--traddr", default="127.0.0.1")
    parser.add_argument("--trsvcid", default="4420")
    parser.add_argument("--src-addr", default=None)
    parser.add_argument("--src-svcid", default=None)
    parser.add_argument("--subnqn", required=True)
    parser.add_argument("--hostnqn", required=True)
    parser.add_argument("--passthru-lcores", default="1")

    parser.add_argument("--cpcs-nsid", type=int, required=True)
    parser.add_argument("--slm-nsid", type=int, required=True)
    parser.add_argument("--rsid", type=int, required=True)
    parser.add_argument("--metadata-mr-id", type=int, default=1)
    parser.add_argument("--vector-mr-id", type=int, default=1)
    parser.add_argument("--output-mr-id", type=int, default=1)
    parser.add_argument("--metadata-offset", type=int, required=True)
    parser.add_argument("--vector-offset", type=int, required=True)
    parser.add_argument("--output-offset", type=int, required=True)
    parser.add_argument("--output-length", type=int, default=65536)
    parser.add_argument("--lba-size", type=int, default=4096)

    parser.add_argument("--query-limit", type=int, default=None)
    parser.add_argument(
        "--disable-stable-tie-docid",
        action="store_true",
        help="Disable stable doc_id tie-break flag for filtered top-k requests",
    )
    parser.add_argument("--output-json", default=None)
    return parser.parse_args()


def run_cmd(cmd: Sequence[str]) -> str:
    import subprocess

    cp = subprocess.run(list(cmd), text=True, capture_output=True)
    if cp.returncode != 0:
        raise RuntimeError(
            f"Command failed rc={cp.returncode}\ncmd={' '.join(cmd)}\nstdout={cp.stdout}\nstderr={cp.stderr}"
        )
    return (cp.stdout or "") + (cp.stderr or "")


def passthru_prefix(args: argparse.Namespace) -> List[str]:
    cmd = [
        args.spdk_nvme_passthru,
        "--lcores", args.passthru_lcores,
        "--disable-cpumask-locks",
        "--no-rpc-server",
        "--io-cmd",
        "--trtype", args.trtype,
        "--traddr", args.traddr,
        "--trsvcid", args.trsvcid,
        "--subnqn", args.subnqn,
        "--hostnqn", args.hostnqn,
    ]
    if args.src_addr:
        cmd.extend(["--src-addr", args.src_addr])
    if args.src_svcid:
        cmd.extend(["--src-svcid", args.src_svcid])
    return cmd


def parse_result_hex(text: str) -> int:
    m = re.search(r"result[:=]\s*0x([0-9a-fA-F]+)", text)
    if not m:
        return 0
    return int(m.group(1), 16)


def issue_execute(args: argparse.Namespace, pind: int, req_payload: bytes) -> int:
    with tempfile.NamedTemporaryFile(prefix="cpcs_vec_exec_", suffix=".bin", delete=False) as tf:
        tf.write(req_payload)
        tf.flush()
        path = tf.name
    cmd = passthru_prefix(args) + [
        "--opcode", "0x01",
        "--nsid", str(args.cpcs_nsid),
        "--cdw2", str((args.rsid << 16) | pind),
        "--cdw3", "0",
        "--cdw4", str(len(req_payload)),
        "--data-len", str(len(req_payload)),
        "--write",
        "--input-file", path,
    ]
    out = run_cmd(cmd)
    return parse_result_hex(out)


def read_slm_bytes(args: argparse.Namespace, offset: int, length: int) -> bytes:
    lba_size = int(args.lba_size)
    start = (offset // lba_size) * lba_size
    head = offset - start
    total = head + length
    padded = ((total + lba_size - 1) // lba_size) * lba_size

    with tempfile.NamedTemporaryFile(prefix="cpcs_vec_read_", suffix=".bin", delete=False) as tf:
        out_path = tf.name
    cmd = passthru_prefix(args) + [
        "--opcode", "0x02",
        "--nsid", str(args.slm_nsid),
        "--cdw10", str(start & 0xFFFFFFFF),
        "--cdw11", str((start >> 32) & 0xFFFFFFFF),
        "--cdw12", str(padded),
        "--data-len", str(padded),
        "--read",
        "--output-file", out_path,
    ]
    run_cmd(cmd)
    raw = Path(out_path).read_bytes()
    return raw[head: head + length]


def parse_topk_result(raw: bytes) -> Dict[str, Any]:
    if len(raw) < RESULT_HDR_STRUCT.size + RESULT_STATS_STRUCT.size + RESULT_TOPK_HDR_STRUCT.size:
        raise ValueError("result payload too small")

    version, opcode, status, returned_count, matched_count, scored_count, total_length = RESULT_HDR_STRUCT.unpack_from(raw, 0)
    stats = RESULT_STATS_STRUCT.unpack_from(raw, RESULT_HDR_STRUCT.size)
    topk_off = RESULT_HDR_STRUCT.size + RESULT_STATS_STRUCT.size
    metric, k, _rsvd = RESULT_TOPK_HDR_STRUCT.unpack_from(raw, topk_off)

    rec_off = topk_off + RESULT_TOPK_HDR_STRUCT.size
    records: List[Dict[str, Any]] = []
    for i in range(returned_count):
        doc_id, score, _ = TOPK_REC_STRUCT.unpack_from(raw, rec_off + i * TOPK_REC_STRUCT.size)
        records.append({"doc_id": int(doc_id), "score": float(score)})

    return {
        "version": version,
        "opcode": opcode,
        "status": status,
        "returned_count": returned_count,
        "matched_count": matched_count,
        "scored_count": scored_count,
        "total_length": total_length,
        "metric": metric,
        "k": k,
        "stats": {
            "host_req_bytes": stats[0],
            "host_resp_bytes": stats[1],
            "media_read_bytes": stats[2],
            "media_write_bytes": stats[3],
            "vslm_fault_read_bytes": stats[4],
            "vslm_fault_write_bytes": stats[5],
            "metadata_records_scanned": stats[6],
            "vectors_scored": stats[7],
            "command_count": stats[8],
            "latency_us": stats[9],
        },
        "records": records,
    }


def read_query_vector(path: Path, offset: int, dim: int) -> List[float]:
    with path.open("rb") as fh:
        fh.seek(offset)
        raw = fh.read(dim * 4)
    return list(struct.unpack("<" + "f" * dim, raw))


def load_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def main() -> int:
    args = parse_args()
    manifest = load_json(args.manifest)
    query_manifest = load_json(args.queries or manifest["paths"]["queries_json"])
    query_vec_path = Path(query_manifest["queries_bin"])

    vector_dim = int(manifest["vector_dim"])
    vector_stride = int(manifest["vector_stride"])
    metadata_stride = int(manifest["metadata_stride"])
    record_count = int(manifest["record_count"])

    queries = list(query_manifest["queries"])
    if args.query_limit is not None:
        queries = queries[: args.query_limit]

    latencies_us: List[float] = []
    rows: List[Dict[str, Any]] = []

    for q in queries:
        query_vec = read_query_vector(query_vec_path, int(q["query_vector_offset"]), vector_dim)
        from cpcs_vector_common import FilterClause

        clauses = [
            FilterClause(
                op=int(c["op"]),
                field_id=int(c["field_id"]),
                args=tuple(int(x) for x in c.get("args", [])),
                in_count=int(c.get("in_count", 0)),
            )
            for c in q["filters"]
        ]
        req = encode_filtered_topk_request(
            flags=(0 if args.disable_stable_tie_docid else FLAG_STABLE_TIE_DOCID),
            output_mr_id=int(args.output_mr_id),
            output_offset=int(args.output_offset),
            output_length=int(args.output_length),
            metadata_mr_id=int(args.metadata_mr_id),
            metadata_offset=int(args.metadata_offset),
            vector_mr_id=int(args.vector_mr_id),
            vector_offset=int(args.vector_offset),
            record_count=record_count,
            metadata_stride=metadata_stride,
            vector_dim=vector_dim,
            vector_stride=vector_stride,
            metric=int(q["metric"]),
            k=int(q["k"]),
            clauses=clauses,
            query_vector=query_vec,
        )

        t0 = time.perf_counter()
        returned = issue_execute(args, PIND_FILTERED_TOPK_EXACT, req)
        elapsed_us = (time.perf_counter() - t0) * 1_000_000.0
        raw_result = read_slm_bytes(args, int(args.output_offset), int(args.output_length))
        parsed = parse_topk_result(raw_result)

        rows.append(
            {
                "query_id": int(q["query_id"]),
                "outer_request_id": int(q.get("outer_request_id", q["query_id"])),
                "round": int(q.get("round", 0)),
                "returned_dw0": int(returned),
                "elapsed_us": elapsed_us,
                "matched_count": int(parsed["matched_count"]),
                "scored_count": int(parsed["scored_count"]),
                "returned_count": int(parsed["returned_count"]),
                "stats": parsed["stats"],
                "records": parsed["records"],
            }
        )
        latencies_us.append(elapsed_us)

    summary = {
        "query_count": len(rows),
        "latency_us": summarize_latency_us(latencies_us),
        "avg_host_req_bytes": sum(r["stats"]["host_req_bytes"] for r in rows) / max(1, len(rows)),
        "avg_host_resp_bytes": sum(r["stats"]["host_resp_bytes"] for r in rows) / max(1, len(rows)),
        "avg_host_total_bytes": sum((r["stats"]["host_req_bytes"] + r["stats"]["host_resp_bytes"]) for r in rows) / max(1, len(rows)),
        "avg_media_read_bytes": sum(r["stats"]["media_read_bytes"] for r in rows) / max(1, len(rows)),
        "avg_media_write_bytes": sum(r["stats"]["media_write_bytes"] for r in rows) / max(1, len(rows)),
        "avg_vslm_fault_read_bytes": sum(r["stats"]["vslm_fault_read_bytes"] for r in rows) / max(1, len(rows)),
        "avg_vslm_fault_write_bytes": sum(r["stats"]["vslm_fault_write_bytes"] for r in rows) / max(1, len(rows)),
        "avg_records_matched": sum(r["matched_count"] for r in rows) / max(1, len(rows)),
        "avg_vectors_scored": sum(r["scored_count"] for r in rows) / max(1, len(rows)),
        "avg_returned_count": sum(r["returned_count"] for r in rows) / max(1, len(rows)),
    }

    payload = {"summary": summary, "per_query": rows}
    print(json.dumps(summary, indent=2))
    if args.output_json:
        Path(args.output_json).write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"Wrote JSON: {args.output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

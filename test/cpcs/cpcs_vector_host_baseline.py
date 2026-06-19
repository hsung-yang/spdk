#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

from __future__ import annotations

import argparse
import csv
import json
import struct
import time
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

from cpcs_vector_common import (
    AGG_COUNT,
    FIELD_CATEGORY_ID,
    FILTER_EQ_U32,
    FILTER_CLAUSE_STRUCT,
    FILTER_RANGE_F32,
    FILTER_RANGE_U32,
    META_RECORD_STRUCT,
    METRIC_COSINE,
    MetaRecord,
    encode_filter_agg_request,
    encode_filtered_topk_request,
    match_all,
    parse_meta_record,
    score_vector,
    summarize_latency_us,
    topk_desc,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Host baseline driver for CPCS vector workloads")
    parser.add_argument("--manifest", required=True, help="Dataset manifest JSON")
    parser.add_argument("--queries", default=None, help="Query manifest JSON (default: from dataset manifest)")
    parser.add_argument("--kernel", choices=["K1", "K2", "K3"], default="K2")
    parser.add_argument("--query-limit", type=int, default=None, help="Limit number of queries")
    parser.add_argument("--k", type=int, default=None, help="Override top-k")
    parser.add_argument("--metric", type=str, default="cosine", choices=["cosine", "l2", "l2_squared"])
    parser.add_argument("--k3-rounds", type=int, default=5)
    parser.add_argument("--output-json", default=None)
    parser.add_argument("--output-csv", default=None)
    return parser.parse_args()


def load_json(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def read_query_vector(path: Path, offset: int, dim: int) -> List[float]:
    with path.open("rb") as fh:
        fh.seek(offset)
        raw = fh.read(dim * 4)
    return list(struct.unpack("<" + "f" * dim, raw))


def decode_filter_clauses(raw_clauses: Sequence[Dict[str, Any]]) -> List[Tuple[int, int, Tuple[int, ...], int]]:
    out = []
    for c in raw_clauses:
        args = tuple(int(x) for x in c.get("args", []))
        out.append((int(c["op"]), int(c["field_id"]), args, int(c.get("in_count", 0))))
    return out


def clauses_to_common(raw_clauses: Sequence[Dict[str, Any]]) -> List[Any]:
    from cpcs_vector_common import FilterClause  # lazy import to avoid circular hints

    return [
        FilterClause(
            op=int(c["op"]),
            field_id=int(c["field_id"]),
            args=tuple(int(x) for x in c.get("args", [])),
            in_count=int(c.get("in_count", 0)),
        )
        for c in raw_clauses
    ]


def metric_name_to_id(name: str) -> int:
    if name == "cosine":
        return METRIC_COSINE
    if name == "l2":
        from cpcs_vector_common import METRIC_L2

        return METRIC_L2
    from cpcs_vector_common import METRIC_L2_SQUARED

    return METRIC_L2_SQUARED


def load_meta_records(meta_path: Path) -> List[MetaRecord]:
    records: List[MetaRecord] = []
    with meta_path.open("rb") as fh:
        while True:
            raw = fh.read(META_RECORD_STRUCT.size)
            if not raw:
                break
            if len(raw) != META_RECORD_STRUCT.size:
                raise ValueError(f"truncated metadata record: got {len(raw)} bytes")
            records.append(parse_meta_record(raw))
    return records


def read_vector(vec_fh, vector_stride: int, vector_dim: int, vector_index: int) -> List[float]:
    vec_fh.seek(vector_index * vector_stride)
    raw = vec_fh.read(vector_dim * 4)
    return list(struct.unpack("<" + "f" * vector_dim, raw))


def run_k1(
    records: Sequence[MetaRecord],
    query: Dict[str, Any],
    metadata_stride: int,
) -> Dict[str, Any]:
    from cpcs_vector_common import FilterClause

    clauses = [FilterClause(op=o, field_id=f, args=args, in_count=in_count)
               for o, f, args, in_count in decode_filter_clauses(query["filters"])]
    start = time.perf_counter()
    matched = 0
    for rec in records:
        if match_all(rec, clauses):
            matched += 1
    elapsed = time.perf_counter() - start

    req_bytes = len(
        encode_filter_agg_request(
            output_mr_id=1,
            output_offset=0,
            output_length=128,
            metadata_mr_id=1,
            metadata_offset=0,
            record_count=len(records),
            metadata_stride=metadata_stride,
            agg_op=AGG_COUNT,
            agg_field_id=FIELD_CATEGORY_ID,
            clauses=clauses,
        )
    )

    return {
        "matched_count": matched,
        "scored_count": 0,
        "returned_count": 1,
        "elapsed_us": elapsed * 1_000_000.0,
        "host_req_bytes": req_bytes,
        "host_resp_bytes": 128,
        "media_read_bytes": len(records) * metadata_stride,
        "media_write_bytes": 128,
        "result": {"aggregate_count": matched},
    }


def run_k2(
    records: Sequence[MetaRecord],
    vec_path: Path,
    query_vec: Sequence[float],
    query: Dict[str, Any],
    metadata_stride: int,
    vector_stride: int,
    vector_dim: int,
    k: int,
    metric_id: int,
) -> Dict[str, Any]:
    clauses = clauses_to_common(query["filters"])
    start = time.perf_counter()
    candidates: List[Tuple[int, float]] = []
    matched = 0
    scored = 0

    with vec_path.open("rb") as vec_fh:
        for rec in records:
            if not match_all(rec, clauses):
                continue
            matched += 1
            vector = read_vector(vec_fh, vector_stride, vector_dim, rec.vector_index)
            score = score_vector(query_vec, vector, metric_id, query_normalized=False)
            candidates.append((rec.doc_id, score))
            scored += 1

    topk = topk_desc(candidates, k)
    elapsed = time.perf_counter() - start
    req_bytes = len(
        encode_filtered_topk_request(
            flags=0,
            output_mr_id=1,
            output_offset=0,
            output_length=112 + (k * 16),
            metadata_mr_id=1,
            metadata_offset=0,
            vector_mr_id=1,
            vector_offset=0,
            record_count=len(records),
            metadata_stride=metadata_stride,
            vector_dim=vector_dim,
            vector_stride=vector_stride,
            metric=metric_id,
            k=k,
            clauses=clauses,
            query_vector=query_vec,
        )
    )
    result_bytes = 112 + (len(topk) * 16)

    return {
        "matched_count": matched,
        "scored_count": scored,
        "returned_count": len(topk),
        "elapsed_us": elapsed * 1_000_000.0,
        "host_req_bytes": req_bytes + (len(records) * metadata_stride) + (scored * vector_dim * 4),
        "host_resp_bytes": result_bytes,
        "media_read_bytes": (len(records) * metadata_stride) + (scored * vector_dim * 4),
        "media_write_bytes": result_bytes,
        "result": {"topk": [{"doc_id": d, "score": s} for d, s in topk]},
    }


def main() -> int:
    args = parse_args()
    manifest = load_json(args.manifest)
    query_manifest_path = args.queries or manifest["paths"]["queries_json"]
    query_manifest = load_json(query_manifest_path)

    meta_path = Path(manifest["paths"]["meta_bin"])
    vec_path = Path(manifest["paths"]["vec_bin"])
    query_vec_path = Path(query_manifest["queries_bin"])

    records = load_meta_records(meta_path)
    metadata_stride = int(manifest["metadata_stride"])
    vector_stride = int(manifest["vector_stride"])
    vector_dim = int(manifest["vector_dim"])

    queries: List[Dict[str, Any]] = list(query_manifest["queries"])
    if args.query_limit is not None:
        queries = queries[: args.query_limit]

    metric_id = metric_name_to_id(args.metric)
    default_k = int(manifest.get("k_default", 100))
    topk_k = int(args.k or default_k)

    per_query: List[Dict[str, Any]] = []
    outer_acc: Dict[int, Dict[str, Any]] = {}
    for query in queries:
        query_vec = read_query_vector(query_vec_path, int(query["query_vector_offset"]), vector_dim)
        if args.kernel == "K1":
            res = run_k1(records, query, metadata_stride)
        else:
            res = run_k2(
                records=records,
                vec_path=vec_path,
                query_vec=query_vec,
                query=query,
                metadata_stride=metadata_stride,
                vector_stride=vector_stride,
                vector_dim=vector_dim,
                k=int(query.get("k", topk_k) if args.k is None else topk_k),
                metric_id=metric_id if "metric" not in query else int(query["metric"]),
            )

        row = {
            "query_id": int(query["query_id"]),
            "outer_request_id": int(query.get("outer_request_id", query["query_id"])),
            "round": int(query.get("round", 0)),
            **res,
        }
        per_query.append(row)

        outer_id = row["outer_request_id"]
        bucket = outer_acc.setdefault(
            outer_id,
            {"elapsed_us": 0.0, "host_total_bytes": 0, "query_count": 0},
        )
        bucket["elapsed_us"] += row["elapsed_us"]
        bucket["host_total_bytes"] += row["host_req_bytes"] + row["host_resp_bytes"]
        bucket["query_count"] += 1

    if args.kernel == "K3":
        # Keep only requested round depth for each outer request.
        trimmed_outer = []
        for outer_id, agg in sorted(outer_acc.items()):
            if agg["query_count"] < args.k3_rounds:
                continue
            trimmed_outer.append(agg)
        outer_lat_us = [x["elapsed_us"] for x in trimmed_outer]
        outer_bytes = [x["host_total_bytes"] for x in trimmed_outer]
    else:
        outer_lat_us = []
        outer_bytes = []

    latencies = [row["elapsed_us"] for row in per_query]
    total_elapsed_s = sum(latencies) / 1_000_000.0
    qps = (len(per_query) / total_elapsed_s) if total_elapsed_s > 0 else 0.0
    summary = {
        "kernel": args.kernel,
        "query_count": len(per_query),
        "latency_us": summarize_latency_us(latencies),
        "throughput_qps": qps,
        "avg_host_req_bytes": sum(row["host_req_bytes"] for row in per_query) / max(1, len(per_query)),
        "avg_host_resp_bytes": sum(row["host_resp_bytes"] for row in per_query) / max(1, len(per_query)),
        "avg_host_total_bytes": sum((row["host_req_bytes"] + row["host_resp_bytes"]) for row in per_query) / max(1, len(per_query)),
        "avg_media_read_bytes": sum(row["media_read_bytes"] for row in per_query) / max(1, len(per_query)),
        "avg_media_write_bytes": sum(row["media_write_bytes"] for row in per_query) / max(1, len(per_query)),
        "avg_records_matched": sum(row["matched_count"] for row in per_query) / max(1, len(per_query)),
        "avg_vectors_scored": sum(row["scored_count"] for row in per_query) / max(1, len(per_query)),
        "avg_returned_count": sum(row["returned_count"] for row in per_query) / max(1, len(per_query)),
        "outer_request_count": len(outer_lat_us),
        "outer_latency_us_p50": summarize_latency_us(outer_lat_us)["p50"] if outer_lat_us else 0.0,
        "outer_latency_us_p95": summarize_latency_us(outer_lat_us)["p95"] if outer_lat_us else 0.0,
        "outer_latency_us_p99": summarize_latency_us(outer_lat_us)["p99"] if outer_lat_us else 0.0,
        "outer_total_bytes_avg": (sum(outer_bytes) / len(outer_bytes)) if outer_bytes else 0.0,
    }

    payload = {
        "manifest": args.manifest,
        "queries": query_manifest_path,
        "summary": summary,
        "per_query": per_query,
    }

    print(json.dumps(summary, indent=2))
    if args.output_json:
        Path(args.output_json).write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"Wrote JSON: {args.output_json}")

    if args.output_csv:
        with open(args.output_csv, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(
                fh,
                fieldnames=[
                    "query_id",
                    "outer_request_id",
                    "round",
                    "matched_count",
                    "scored_count",
                    "returned_count",
                    "elapsed_us",
                    "host_req_bytes",
                    "host_resp_bytes",
                    "media_read_bytes",
                    "media_write_bytes",
                ],
            )
            writer.writeheader()
            for row in per_query:
                writer.writerow({k: row[k] for k in writer.fieldnames})
        print(f"Wrote CSV: {args.output_csv}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

from __future__ import annotations

import math
import struct
from dataclasses import dataclass
from typing import Dict, Iterable, List, Sequence, Tuple


ABI_VERSION = 1

OP_FILTER_AGG = 1
OP_FILTERED_TOPK_EXACT = 2

FLAG_QUERY_NORMALIZED = 1 << 0
FLAG_RETURN_SCORES = 1 << 1
FLAG_STABLE_TIE_DOCID = 1 << 2

FILTER_EQ_U32 = 1
FILTER_IN_SET_U32 = 2
FILTER_BITMASK_ANY = 3
FILTER_RANGE_F32 = 4
FILTER_RANGE_U32 = 5

FIELD_DOC_ID = 1
FIELD_CATEGORY_ID = 2
FIELD_FLAGS = 3
FIELD_REGION_ID = 4
FIELD_PRICE = 5
FIELD_TIMESTAMP_BUCKET = 6
FIELD_VECTOR_INDEX = 7

METRIC_COSINE = 1
METRIC_L2 = 2
METRIC_L2_SQUARED = 3

AGG_COUNT = 1
AGG_SUM = 2
AGG_MIN = 3
AGG_MAX = 4

META_RECORD_STRUCT = struct.Struct("<QIIHHfII")
FILTER_CLAUSE_STRUCT = struct.Struct("<BBBB6I")
REQ_HDR_STRUCT = struct.Struct("<IHHQQII")
FILTER_AGG_REQ_STRUCT = struct.Struct("<QQQIHBB")
TOPK_REQ_STRUCT = struct.Struct("<QQQQQIIIHHHHII")
TOPK_REC_STRUCT = struct.Struct("<QfI")


@dataclass(frozen=True)
class FilterClause:
    op: int
    field_id: int
    args: Tuple[int, ...]
    in_count: int = 0


@dataclass(frozen=True)
class MetaRecord:
    doc_id: int
    category_id: int
    flags: int
    region_id: int
    price: float
    timestamp_bucket: int
    vector_index: int


def parse_meta_record(raw: bytes) -> MetaRecord:
    doc_id, category_id, flags, region_id, _rsvd, price, ts_bucket, vector_index = META_RECORD_STRUCT.unpack(raw)
    return MetaRecord(
        doc_id=doc_id,
        category_id=category_id,
        flags=flags,
        region_id=region_id,
        price=price,
        timestamp_bucket=ts_bucket,
        vector_index=vector_index,
    )


def pack_meta_record(record: MetaRecord) -> bytes:
    return META_RECORD_STRUCT.pack(
        record.doc_id,
        record.category_id,
        record.flags,
        record.region_id,
        0,
        record.price,
        record.timestamp_bucket,
        record.vector_index,
    )


def pack_filter_clause(clause: FilterClause) -> bytes:
    args = list(clause.args[:6])
    args.extend([0] * (6 - len(args)))
    return FILTER_CLAUSE_STRUCT.pack(
        int(clause.op),
        int(clause.field_id),
        int(clause.in_count),
        0,
        *[int(x) & 0xFFFFFFFF for x in args],
    )


def field_u32(record: MetaRecord, field_id: int) -> int:
    if field_id == FIELD_CATEGORY_ID:
        return record.category_id
    if field_id == FIELD_FLAGS:
        return record.flags
    if field_id == FIELD_REGION_ID:
        return record.region_id
    if field_id == FIELD_TIMESTAMP_BUCKET:
        return record.timestamp_bucket
    if field_id == FIELD_VECTOR_INDEX:
        return record.vector_index
    raise ValueError(f"unsupported u32 field id: {field_id}")


def field_f32(record: MetaRecord, field_id: int) -> float:
    if field_id == FIELD_PRICE:
        return record.price
    raise ValueError(f"unsupported f32 field id: {field_id}")


def match_clause(record: MetaRecord, clause: FilterClause) -> bool:
    if clause.op == FILTER_RANGE_F32:
        if len(clause.args) < 2:
            return False
        low = struct.unpack("<f", struct.pack("<I", clause.args[0] & 0xFFFFFFFF))[0]
        high = struct.unpack("<f", struct.pack("<I", clause.args[1] & 0xFFFFFFFF))[0]
        return low <= field_f32(record, clause.field_id) <= high

    value = field_u32(record, clause.field_id)
    if clause.op == FILTER_EQ_U32:
        return len(clause.args) >= 1 and value == clause.args[0]
    if clause.op == FILTER_IN_SET_U32:
        limit = clause.in_count or len(clause.args)
        return value in tuple(clause.args[:limit])
    if clause.op == FILTER_BITMASK_ANY:
        return len(clause.args) >= 1 and (value & clause.args[0]) != 0
    if clause.op == FILTER_RANGE_U32:
        return len(clause.args) >= 2 and clause.args[0] <= value <= clause.args[1]
    return False


def match_all(record: MetaRecord, clauses: Sequence[FilterClause]) -> bool:
    return all(match_clause(record, c) for c in clauses)


def score_vector(query: Sequence[float], vector: Sequence[float], metric: int, query_normalized: bool = False) -> float:
    if metric == METRIC_COSINE:
        dot = 0.0
        qn = 0.0
        vn = 0.0
        for q, v in zip(query, vector):
            dot += q * v
            vn += v * v
            if not query_normalized:
                qn += q * q
        if query_normalized:
            qn = 1.0
        if qn <= 0.0 or vn <= 0.0:
            return 0.0
        return float(dot / (math.sqrt(qn) * math.sqrt(vn)))

    dist_sq = 0.0
    for q, v in zip(query, vector):
        d = q - v
        dist_sq += d * d
    if metric == METRIC_L2:
        return float(-math.sqrt(dist_sq))
    if metric == METRIC_L2_SQUARED:
        return float(-dist_sq)
    raise ValueError(f"unsupported metric: {metric}")


def topk_desc(items: Iterable[Tuple[int, float]], k: int) -> List[Tuple[int, float]]:
    selected: List[Tuple[int, float]] = []
    for doc_id, score in items:
        selected.append((doc_id, score))
        selected.sort(key=lambda x: (-x[1], x[0]))
        if len(selected) > k:
            selected.pop()
    return selected


def encode_filter_agg_request(
    *,
    output_mr_id: int,
    output_offset: int,
    output_length: int,
    metadata_mr_id: int,
    metadata_offset: int,
    record_count: int,
    metadata_stride: int,
    agg_op: int,
    agg_field_id: int,
    clauses: Sequence[FilterClause],
) -> bytes:
    hdr = REQ_HDR_STRUCT.pack(
        ABI_VERSION,
        OP_FILTER_AGG,
        0,
        output_mr_id,
        output_offset,
        output_length,
        0,
    )
    body = FILTER_AGG_REQ_STRUCT.pack(
        metadata_mr_id,
        metadata_offset,
        record_count,
        metadata_stride,
        len(clauses),
        agg_op,
        agg_field_id,
    )
    payload = b"".join(pack_filter_clause(c) for c in clauses)
    return hdr + body + payload


def encode_filtered_topk_request(
    *,
    flags: int,
    output_mr_id: int,
    output_offset: int,
    output_length: int,
    metadata_mr_id: int,
    metadata_offset: int,
    vector_mr_id: int,
    vector_offset: int,
    record_count: int,
    metadata_stride: int,
    vector_dim: int,
    vector_stride: int,
    metric: int,
    k: int,
    clauses: Sequence[FilterClause],
    query_vector: Sequence[float],
) -> bytes:
    query_raw = struct.pack("<" + "f" * len(query_vector), *query_vector)
    hdr = REQ_HDR_STRUCT.pack(
        ABI_VERSION,
        OP_FILTERED_TOPK_EXACT,
        flags,
        output_mr_id,
        output_offset,
        output_length,
        0,
    )
    body = TOPK_REQ_STRUCT.pack(
        metadata_mr_id,
        metadata_offset,
        vector_mr_id,
        vector_offset,
        record_count,
        metadata_stride,
        vector_dim,
        vector_stride,
        metric,
        k,
        len(clauses),
        0,
        len(query_raw),
        0,
    )
    payload = b"".join(pack_filter_clause(c) for c in clauses) + query_raw
    return hdr + body + payload


def percentile(values: Sequence[float], pct: float) -> float:
    if not values:
        return 0.0
    if pct <= 0:
        return float(min(values))
    if pct >= 100:
        return float(max(values))
    sorted_values = sorted(values)
    rank = (pct / 100.0) * (len(sorted_values) - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return float(sorted_values[lo])
    frac = rank - lo
    return float(sorted_values[lo] * (1.0 - frac) + sorted_values[hi] * frac)


def summarize_latency_us(latencies_us: Sequence[float]) -> Dict[str, float]:
    return {
        "p50": percentile(latencies_us, 50.0),
        "p95": percentile(latencies_us, 95.0),
        "p99": percentile(latencies_us, 99.0),
    }

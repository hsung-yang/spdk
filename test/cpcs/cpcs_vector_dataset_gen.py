#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 CPCS Implementation Team.
#  All rights reserved.

from __future__ import annotations

import argparse
import json
import random
import struct
from pathlib import Path
from typing import Dict, List

from cpcs_vector_common import (
    FILTER_BITMASK_ANY,
    FILTER_EQ_U32,
    FILTER_RANGE_F32,
    FIELD_CATEGORY_ID,
    FIELD_FLAGS,
    FIELD_PRICE,
    META_RECORD_STRUCT,
    METRIC_COSINE,
)

try:
    import numpy as np  # type: ignore
except Exception:
    np = None


PROFILE_DEFAULTS: Dict[str, Dict[str, int]] = {
    "mongo_like": {"dim": 2048, "k": 100, "canonical_count": 15_300_000},
    "pinecone_yfcc_like": {"dim": 192, "k": 10, "canonical_count": 10_000_000},
    "pinecone_customer_like": {"dim": 768, "k": 100, "canonical_count": 35_000_000},
    "elastic_agentic_like": {"dim": 128, "k": 100, "canonical_count": 20_000_000},
}

TIER_COUNTS: Dict[str, int] = {
    "T0": 100_000,
    "T1": 1_000_000,
    "T2": 5_000_000,
}


def _f32_to_u32(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def _progress_stride(total: int, slices: int = 20) -> int:
    if total <= 0:
        return 1
    return max(1, total // slices)


def ensure_vector_generation_supported(total_vectors: int, dim: int) -> None:
    total_values = total_vectors * dim
    # Pure-Python random generation is prohibitively slow at this scale.
    if np is None and total_values > 50_000_000:
        raise RuntimeError(
            "NumPy is required for large vector generation. "
            "Install python3-numpy (or pip install numpy), or reduce --count/--dim."
        )


def write_meta_records(path: Path, profile: str, count: int, seed: int) -> None:
    rng = random.Random(seed)
    rand = rng.random
    rand_uniform = rng.uniform
    progress_stride = _progress_stride(count)
    next_report = progress_stride
    category_hot = 1
    category_mid = 2

    with path.open("wb") as fh:
        for i in range(count):
            if profile == "mongo_like":
                category_id = category_hot if rand() < 0.03 else 10 + (i % 37)
            elif profile == "pinecone_yfcc_like":
                roll = rand()
                if roll < 0.0005:
                    category_id = category_hot
                elif roll < 0.2:
                    category_id = category_mid
                else:
                    category_id = 100 + (i % 400)
            elif profile == "pinecone_customer_like":
                category_id = category_mid if rand() < 0.178 else 50 + (i % 97)
            else:
                category_id = 200 + (i % 64)

            flags = 0
            if rand() < 0.85:
                flags |= 0x1  # in_stock
            if rand() < 0.92:
                flags |= 0x2  # valid_listing

            region_id = i % 16
            price = round(rand_uniform(1.0, 2000.0), 3)
            timestamp_bucket = 1_700_000_000 + (i % 100_000)
            fh.write(META_RECORD_STRUCT.pack(i, category_id, flags, region_id, 0, price, timestamp_bucket, i))

            done = i + 1
            if done >= next_report or done == count:
                pct = (100.0 * done) / count
                print(f"[dataset] metadata: {done}/{count} ({pct:.1f}%)", flush=True)
                while next_report <= done:
                    next_report += progress_stride


def build_query_specs(profile: str, query_count: int, k: int, dim: int, rng: random.Random) -> List[Dict[str, object]]:
    queries: List[Dict[str, object]] = []
    for qid in range(query_count):
        filters: List[Dict[str, object]] = []
        outer_id = qid
        round_index = 0

        if profile == "mongo_like":
            filters.append({"op": FILTER_EQ_U32, "field_id": FIELD_CATEGORY_ID, "args": [1], "in_count": 0})
        elif profile == "pinecone_yfcc_like":
            dense_mode = (qid % 4) in (2, 3)
            cat = 2 if dense_mode else 1
            filters.append({"op": FILTER_EQ_U32, "field_id": FIELD_CATEGORY_ID, "args": [cat], "in_count": 0})
            if dense_mode:
                filters.append({"op": FILTER_BITMASK_ANY, "field_id": FIELD_FLAGS, "args": [0x1], "in_count": 0})
        elif profile == "pinecone_customer_like":
            filters.append({"op": FILTER_EQ_U32, "field_id": FIELD_CATEGORY_ID, "args": [2], "in_count": 0})
            low = 100.0 + float((qid * 7) % 500)
            high = low + 300.0
            filters.append(
                {
                    "op": FILTER_RANGE_F32,
                    "field_id": FIELD_PRICE,
                    "args": [_f32_to_u32(low), _f32_to_u32(high)],
                    "in_count": 0,
                }
            )
        else:
            outer_id = qid // 5
            round_index = qid % 5
            filters.append({"op": FILTER_BITMASK_ANY, "field_id": FIELD_FLAGS, "args": [0x1], "in_count": 0})
            if round_index >= 1:
                filters.append({"op": FILTER_BITMASK_ANY, "field_id": FIELD_FLAGS, "args": [0x2], "in_count": 0})
            if round_index >= 2:
                low = 50.0 + float((outer_id * 13) % 300)
                high = low + 250.0
                filters.append(
                    {
                        "op": FILTER_RANGE_F32,
                        "field_id": FIELD_PRICE,
                        "args": [_f32_to_u32(low), _f32_to_u32(high)],
                        "in_count": 0,
                    }
                )

        queries.append(
            {
                "query_id": qid,
                "outer_request_id": outer_id,
                "round": round_index,
                "metric": METRIC_COSINE,
                "k": k,
                "vector_dim": dim,
                "filters": filters,
                "target_selectivity_hint": "profile-shaped",
            }
        )
    return queries


def write_vectors(path: Path, total_vectors: int, dim: int, seed: int) -> None:
    ensure_vector_generation_supported(total_vectors, dim)

    progress_stride = _progress_stride(total_vectors)
    next_report = progress_stride

    if np is not None:
        bytes_per_vec = max(1, dim * 4)
        chunk_vectors = max(1, (64 * 1024 * 1024) // bytes_per_vec)
        rng = np.random.default_rng(seed)
        done = 0
        with path.open("wb") as fh:
            while done < total_vectors:
                cur = min(chunk_vectors, total_vectors - done)
                batch = rng.uniform(-1.0, 1.0, size=(cur, dim)).astype("<f4", copy=False)
                fh.write(batch.tobytes(order="C"))
                done += cur
                if done >= next_report or done == total_vectors:
                    pct = (100.0 * done) / total_vectors
                    print(f"[dataset] vectors:  {done}/{total_vectors} ({pct:.1f}%)", flush=True)
                    while next_report <= done:
                        next_report += progress_stride
        return

    # Small-scale fallback when NumPy is unavailable.
    rng = random.Random(seed)
    fmt = struct.Struct("<" + "f" * dim)
    with path.open("wb") as fh:
        for i in range(total_vectors):
            vec = [rng.uniform(-1.0, 1.0) for _ in range(dim)]
            fh.write(fmt.pack(*vec))
            done = i + 1
            if done >= next_report or done == total_vectors:
                pct = (100.0 * done) / total_vectors
                print(f"[dataset] vectors:  {done}/{total_vectors} ({pct:.1f}%)", flush=True)
                while next_report <= done:
                    next_report += progress_stride


def write_query_vectors(path: Path, queries: List[Dict[str, object]], dim: int, seed: int) -> None:
    if np is not None:
        rng = np.random.default_rng(seed)
        qn = len(queries)
        vectors = rng.uniform(-1.0, 1.0, size=(qn, dim)).astype("<f4", copy=False)
        path.write_bytes(vectors.tobytes(order="C"))
        stride = dim * 4
        for i, q in enumerate(queries):
            q["query_vector_offset"] = i * stride
            q["query_vector_bytes"] = stride
        return

    rng = random.Random(seed)
    fmt = struct.Struct("<" + "f" * dim)
    with path.open("wb") as fh:
        offset = 0
        for q in queries:
            vec = [rng.uniform(-1.0, 1.0) for _ in range(dim)]
            fh.write(fmt.pack(*vec))
            q["query_vector_offset"] = offset
            q["query_vector_bytes"] = dim * 4
            offset += dim * 4


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate deterministic CPCS vector evaluation datasets")
    parser.add_argument("--profile", choices=sorted(PROFILE_DEFAULTS.keys()), required=True)
    parser.add_argument("--tier", choices=["T0", "T1", "T2", "T3"], default="T0")
    parser.add_argument("--count", type=int, default=None, help="Override vector count")
    parser.add_argument("--dim", type=int, default=None, help="Override vector dimension")
    parser.add_argument("--query-count", type=int, default=300)
    parser.add_argument("--seed", type=int, default=20260409)
    parser.add_argument("--output-dir", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    profile_cfg = PROFILE_DEFAULTS[args.profile]
    count = args.count if args.count is not None else (
        profile_cfg["canonical_count"] if args.tier == "T3" else TIER_COUNTS[args.tier]
    )
    dim = args.dim if args.dim is not None else profile_cfg["dim"]
    k = profile_cfg["k"]

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    meta_path = output_dir / "meta.bin"
    vec_path = output_dir / "vec.bin"
    query_vec_path = output_dir / "queries.bin"
    query_manifest_path = output_dir / "queries.json"
    manifest_path = output_dir / "manifest.json"

    meta_bytes = count * META_RECORD_STRUCT.size
    vec_bytes = count * dim * 4
    ensure_vector_generation_supported(count, dim)
    print(
        f"[dataset] generating profile={args.profile} tier={args.tier} "
        f"count={count} dim={dim} (meta={meta_bytes} bytes, vectors={vec_bytes} bytes)",
        flush=True,
    )
    write_meta_records(meta_path, args.profile, count, args.seed ^ 0xABCDEF)
    write_vectors(vec_path, count, dim, args.seed ^ 0x13579B)
    queries = build_query_specs(args.profile, args.query_count, k, dim, random.Random(args.seed ^ 0x2468AC))
    write_query_vectors(query_vec_path, queries, dim, args.seed ^ 0xDEADBEEF)

    query_manifest = {
        "version": 1,
        "profile": args.profile,
        "tier": args.tier,
        "query_count": len(queries),
        "queries_bin": str(query_vec_path),
        "queries": queries,
    }
    query_manifest_path.write_text(json.dumps(query_manifest, indent=2), encoding="utf-8")

    manifest = {
        "version": 1,
        "profile": args.profile,
        "tier": args.tier,
        "seed": args.seed,
        "record_count": count,
        "vector_dim": dim,
        "metadata_stride": 32,
        "vector_stride": dim * 4,
        "metric_default": "cosine",
        "k_default": k,
        "paths": {
            "meta_bin": str(meta_path),
            "vec_bin": str(vec_path),
            "queries_bin": str(query_vec_path),
            "queries_json": str(query_manifest_path),
        },
    }
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    print(f"Wrote dataset manifest: {manifest_path}")
    print(f"  profile={args.profile} tier={args.tier} count={count} dim={dim} k={k}")
    print(f"  meta={meta_path}")
    print(f"  vec={vec_path}")
    print(f"  queries={query_manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

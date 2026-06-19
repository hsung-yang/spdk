#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause

import struct
import unittest

from cpcs_vector_common import (
    ABI_VERSION,
    AGG_COUNT,
    FIELD_CATEGORY_ID,
    FIELD_FLAGS,
    FIELD_PRICE,
    FILTER_BITMASK_ANY,
    FILTER_EQ_U32,
    FILTER_RANGE_F32,
    FILTER_RANGE_U32,
    FLAG_QUERY_NORMALIZED,
    METRIC_COSINE,
    METRIC_L2,
    MetaRecord,
    REQ_HDR_STRUCT,
    TOPK_REC_STRUCT,
    encode_filter_agg_request,
    encode_filtered_topk_request,
    match_all,
    score_vector,
    topk_desc,
)


def _f32_bits(v: float) -> int:
    return struct.unpack("<I", struct.pack("<f", v))[0]


class VectorEvalCommonTests(unittest.TestCase):
    def test_filter_matching(self) -> None:
        from cpcs_vector_common import FilterClause

        rec = MetaRecord(
            doc_id=7,
            category_id=2,
            flags=0x3,
            region_id=1,
            price=123.5,
            timestamp_bucket=100,
            vector_index=7,
        )
        clauses = [
            FilterClause(op=FILTER_EQ_U32, field_id=FIELD_CATEGORY_ID, args=(2,)),
            FilterClause(op=FILTER_BITMASK_ANY, field_id=FIELD_FLAGS, args=(0x1,)),
            FilterClause(op=FILTER_RANGE_U32, field_id=FIELD_CATEGORY_ID, args=(1, 3)),
            FilterClause(op=FILTER_RANGE_F32, field_id=FIELD_PRICE, args=(_f32_bits(100.0), _f32_bits(200.0))),
        ]
        self.assertTrue(match_all(rec, clauses))

    def test_scoring(self) -> None:
        q = [1.0, 0.0, 0.0]
        v1 = [1.0, 0.0, 0.0]
        v2 = [0.0, 1.0, 0.0]
        self.assertGreater(score_vector(q, v1, METRIC_COSINE, query_normalized=True), 0.99)
        self.assertLess(score_vector(q, v2, METRIC_COSINE, query_normalized=True), 0.01)
        self.assertGreater(score_vector(q, v1, METRIC_L2), score_vector(q, v2, METRIC_L2))

    def test_topk_tie_break(self) -> None:
        items = [(9, 0.9), (2, 1.0), (3, 1.0), (1, 0.8)]
        top = topk_desc(items, 2)
        self.assertEqual(top[0][0], 2)
        self.assertEqual(top[1][0], 3)

    def test_request_encoding(self) -> None:
        from cpcs_vector_common import FilterClause

        req = encode_filter_agg_request(
            output_mr_id=1,
            output_offset=64,
            output_length=128,
            metadata_mr_id=1,
            metadata_offset=0,
            record_count=10,
            metadata_stride=32,
            agg_op=AGG_COUNT,
            agg_field_id=FIELD_CATEGORY_ID,
            clauses=[FilterClause(op=FILTER_EQ_U32, field_id=FIELD_CATEGORY_ID, args=(2,))],
        )
        version, opcode, _, _, _, _, _ = REQ_HDR_STRUCT.unpack_from(req, 0)
        self.assertEqual(version, ABI_VERSION)
        self.assertEqual(opcode, 1)

        req2 = encode_filtered_topk_request(
            flags=FLAG_QUERY_NORMALIZED,
            output_mr_id=1,
            output_offset=128,
            output_length=1024,
            metadata_mr_id=1,
            metadata_offset=0,
            vector_mr_id=1,
            vector_offset=0,
            record_count=10,
            metadata_stride=32,
            vector_dim=4,
            vector_stride=16,
            metric=METRIC_COSINE,
            k=3,
            clauses=[],
            query_vector=[0.1, 0.2, 0.3, 0.4],
        )
        self.assertGreaterEqual(len(req2), REQ_HDR_STRUCT.size + 56)

    def test_topk_record_size(self) -> None:
        self.assertEqual(TOPK_REC_STRUCT.size, 16)


if __name__ == "__main__":
    unittest.main()

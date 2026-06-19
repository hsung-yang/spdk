/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

/**
 * \file
 * CPCS built-in evaluation program request/response ABI.
 */

#ifndef SPDK_NVME_CPCS_BUILTIN_EVAL_H
#define SPDK_NVME_CPCS_BUILTIN_EVAL_H

#include "spdk/assert.h"
#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_CPCS_BUILTIN_EVAL_ABI_VERSION 1u

enum spdk_cpcs_builtin_eval_opcode {
	SPDK_CPCS_BUILTIN_OP_FILTER_AGG = 1,
	SPDK_CPCS_BUILTIN_OP_FILTERED_TOPK_EXACT = 2,
};

enum spdk_cpcs_builtin_eval_flags {
	SPDK_CPCS_BUILTIN_FLAG_QUERY_NORMALIZED = (1u << 0),
	SPDK_CPCS_BUILTIN_FLAG_RETURN_SCORES = (1u << 1),
	SPDK_CPCS_BUILTIN_FLAG_STABLE_TIE_DOCID = (1u << 2),
};

enum spdk_cpcs_builtin_eval_filter_op {
	SPDK_CPCS_BUILTIN_FILTER_EQ_U32 = 1,
	SPDK_CPCS_BUILTIN_FILTER_IN_SET_U32 = 2,
	SPDK_CPCS_BUILTIN_FILTER_BITMASK_ANY = 3,
	SPDK_CPCS_BUILTIN_FILTER_RANGE_F32 = 4,
	SPDK_CPCS_BUILTIN_FILTER_RANGE_U32 = 5,
};

enum spdk_cpcs_builtin_eval_field_id {
	SPDK_CPCS_BUILTIN_FIELD_DOC_ID = 1,
	SPDK_CPCS_BUILTIN_FIELD_CATEGORY_ID = 2,
	SPDK_CPCS_BUILTIN_FIELD_FLAGS = 3,
	SPDK_CPCS_BUILTIN_FIELD_REGION_ID = 4,
	SPDK_CPCS_BUILTIN_FIELD_PRICE = 5,
	SPDK_CPCS_BUILTIN_FIELD_TIMESTAMP_BUCKET = 6,
	SPDK_CPCS_BUILTIN_FIELD_VECTOR_INDEX = 7,
};

enum spdk_cpcs_builtin_eval_metric {
	SPDK_CPCS_BUILTIN_METRIC_COSINE = 1,
	SPDK_CPCS_BUILTIN_METRIC_L2 = 2,
	SPDK_CPCS_BUILTIN_METRIC_L2_SQUARED = 3,
};

enum spdk_cpcs_builtin_eval_agg_op {
	SPDK_CPCS_BUILTIN_AGG_COUNT = 1,
	SPDK_CPCS_BUILTIN_AGG_SUM = 2,
	SPDK_CPCS_BUILTIN_AGG_MIN = 3,
	SPDK_CPCS_BUILTIN_AGG_MAX = 4,
};

#pragma pack(push, 1)
struct spdk_cpcs_builtin_eval_stats {
	uint64_t host_req_bytes;
	uint64_t host_resp_bytes;
	uint64_t media_read_bytes;
	uint64_t media_write_bytes;
	uint64_t vslm_fault_read_bytes;
	uint64_t vslm_fault_write_bytes;
	uint64_t metadata_records_scanned;
	uint64_t vectors_scored;
	uint64_t command_count;
	uint64_t latency_us;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct spdk_cpcs_builtin_eval_filter_clause {
	uint8_t op;
	uint8_t field_id;
	uint8_t in_count;
	uint8_t reserved0;
	uint32_t args[6];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct spdk_cpcs_builtin_eval_req_header {
	uint32_t version;
	uint16_t opcode;
	uint16_t flags;
	uint64_t output_mr_id;
	uint64_t output_offset;
	uint32_t output_length;
	uint32_t reserved0;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct spdk_cpcs_builtin_filter_agg_req {
	struct spdk_cpcs_builtin_eval_req_header hdr;
	uint64_t metadata_mr_id;
	uint64_t metadata_offset;
	uint64_t record_count;
	uint32_t metadata_stride;
	uint16_t filter_count;
	uint8_t agg_op;
	uint8_t agg_field_id;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct spdk_cpcs_builtin_filtered_topk_exact_req {
	struct spdk_cpcs_builtin_eval_req_header hdr;
	uint64_t metadata_mr_id;
	uint64_t metadata_offset;
	uint64_t vector_mr_id;
	uint64_t vector_offset;
	uint64_t record_count;
	uint32_t metadata_stride;
	uint32_t vector_dim;
	uint32_t vector_stride;
	uint16_t metric;
	uint16_t k;
	uint16_t filter_count;
	uint16_t reserved0;
	uint32_t query_vector_bytes;
	uint32_t reserved1;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct spdk_cpcs_builtin_eval_result_header {
	uint32_t version;
	uint16_t opcode;
	uint16_t status;
	uint32_t returned_count;
	uint32_t matched_count;
	uint32_t scored_count;
	uint32_t total_length;
	struct spdk_cpcs_builtin_eval_stats stats;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct spdk_cpcs_builtin_filter_agg_result {
	struct spdk_cpcs_builtin_eval_result_header hdr;
	uint8_t agg_op;
	uint8_t agg_field_id;
	uint16_t reserved0;
	uint32_t reserved1;
	uint64_t aggregate_u64;
	double aggregate_f64;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct spdk_cpcs_builtin_topk_record {
	uint64_t doc_id;
	float score;
	uint32_t reserved0;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct spdk_cpcs_builtin_filtered_topk_exact_result {
	struct spdk_cpcs_builtin_eval_result_header hdr;
	uint16_t metric;
	uint16_t k;
	uint32_t reserved0;
	struct spdk_cpcs_builtin_topk_record records[];
};
#pragma pack(pop)

SPDK_STATIC_ASSERT(sizeof(struct spdk_cpcs_builtin_eval_filter_clause) == 28,
		   "Unexpected filter clause size");
SPDK_STATIC_ASSERT(sizeof(struct spdk_cpcs_builtin_eval_req_header) == 32,
		   "Unexpected request header size");
SPDK_STATIC_ASSERT(sizeof(struct spdk_cpcs_builtin_filter_agg_req) == 64,
		   "Unexpected filter agg request size");
SPDK_STATIC_ASSERT(sizeof(struct spdk_cpcs_builtin_filtered_topk_exact_req) == 100,
		   "Unexpected filtered top-k request size");
SPDK_STATIC_ASSERT(sizeof(struct spdk_cpcs_builtin_eval_stats) == 80,
		   "Unexpected stats size");
SPDK_STATIC_ASSERT(sizeof(struct spdk_cpcs_builtin_eval_result_header) == 104,
		   "Unexpected result header size");
SPDK_STATIC_ASSERT(sizeof(struct spdk_cpcs_builtin_filter_agg_result) == 128,
		   "Unexpected filter agg result size");
SPDK_STATIC_ASSERT(sizeof(struct spdk_cpcs_builtin_topk_record) == 16,
		   "Unexpected top-k record size");

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVME_CPCS_BUILTIN_EVAL_H */

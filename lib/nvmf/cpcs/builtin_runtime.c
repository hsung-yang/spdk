/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "builtin_runtime.h"
#include "builtin_programs.h"
#include "execute.h"
#include "memory_range_set.h"
#include "program.h"
#include "runtime.h"

#include "spdk/bdev.h"
#include "spdk/bdev_slm.h"
#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme_cpcs_builtin_eval.h"
#include "spdk/thread.h"

#include "spdk/stdinc.h"

#include <math.h>

struct cpcs_builtin_memcpy_desc {
	uint64_t src_mr_id;
	uint64_t src_off;
	uint64_t dst_mr_id;
	uint64_t dst_off;
	uint64_t len;
};

struct cpcs_builtin_memfill_desc {
	uint64_t mr_id;
	uint64_t off;
	uint64_t len;
	uint8_t pattern;
	uint8_t _rsvd[7];
};

struct cpcs_builtin_sum64_desc {
	uint64_t mr_id;
	uint64_t off;
	uint64_t len;
};

struct cpcs_builtin_multi_agg64_result {
	uint64_t count;
	double sum;
	double min;
	double max;
};

/*
 * Direct namespace aggregation descriptor (24 bytes).
 * Sent by the host in the data buffer when invoking DIRECT_NS_AGG.
 * Reads straight from a CSD-local NVMe namespace instead of an MRS,
 * eliminating host round-trips for the input data.
 */
struct cs_direct_ns_desc {
	uint32_t nsid;        /* NVMe namespace ID to read from */
	uint32_t n_uint64;     /* number of uint64 elements to process */
	uint64_t lba_offset;  /* starting byte offset into the namespace */
	uint8_t  workload;    /* 0=SUM, 1=MAX, 2=MIN, 3=FILTER_GT, 4=DOT_PRODUCT */
	uint8_t  pad[7];      /* reserved, must be zero */
};

/*
 * Direct namespace aggregation result (16 bytes).
 * Written back into the data buffer just past the descriptor.
 */
struct cs_direct_ns_result {
	uint64_t result; /* aggregation result (SUM/MAX/MIN value, FILTER_GT count, or DOT_PRODUCT double bit-pattern) */
	uint64_t count;  /* elements processed */
};

/* Workload codes for cs_direct_ns_desc.workload */
#define CS_DIRECT_NS_WORKLOAD_SUM         0
#define CS_DIRECT_NS_WORKLOAD_MAX         1
#define CS_DIRECT_NS_WORKLOAD_MIN         2
#define CS_DIRECT_NS_WORKLOAD_FILTER_GT   3
#define CS_DIRECT_NS_WORKLOAD_DOT_PRODUCT 4

struct cpcs_builtin_metadata_record {
	uint64_t doc_id;
	uint32_t category_id;
	uint32_t flags;
	uint16_t region_id;
	uint16_t reserved0;
	float price;
	uint32_t timestamp_bucket;
	uint32_t vector_index;
} __attribute__((packed));

struct cpcs_builtin_topk_state_entry {
	uint64_t doc_id;
	float score;
};

struct cpcs_builtin_deferred_done_ctx {
	cpcs_runtime_execute_done_cb done_cb;
	void *done_arg;
	int status;
	uint64_t return_value;
};

struct cpcs_builtin_sync_wait_ctx {
	bool done;
	int status;
};

struct cpcs_builtin_extended_exec_ctx {
	struct cpcs_program *prog;
	const struct cpcs_exec_context *exec_ctx;
	cpcs_runtime_execute_done_cb done_cb;
	void *done_arg;
	struct spdk_thread *submit_thread;
};

struct cpcs_builtin_filter_agg_async_ctx {
	struct cpcs_builtin_extended_exec_ctx *worker_ctx;
	const struct cpcs_exec_context *exec_ctx;
	const struct spdk_cpcs_builtin_filter_agg_req *req;
	const struct spdk_cpcs_builtin_eval_filter_clause *clauses;
	struct spdk_cpcs_builtin_filter_agg_result result;
	struct cpcs_builtin_metadata_record meta;
	uint64_t aggregate_u64;
	double aggregate_f64;
	bool aggregate_initialized;
	uint64_t record_idx;
	uint64_t start_ticks;
	uint64_t ticks_hz;
};

struct cpcs_builtin_topk_async_ctx {
	struct cpcs_builtin_extended_exec_ctx *worker_ctx;
	const struct cpcs_exec_context *exec_ctx;
	const struct spdk_cpcs_builtin_filtered_topk_exact_req *req;
	const struct spdk_cpcs_builtin_eval_filter_clause *clauses;
	const float *query_vec;
	struct cpcs_builtin_metadata_record meta;
	struct cpcs_builtin_topk_state_entry *entries;
	struct spdk_cpcs_builtin_filtered_topk_exact_result *out;
	uint8_t *out_buf;
	float *vector_buf;
	uint32_t entries_count;
	uint32_t dim;
	uint16_t filter_count;
	uint16_t k;
	uint64_t record_idx;
	uint64_t start_ticks;
	uint64_t ticks_hz;
	bool query_normalized;
};

struct cpcs_builtin_compute_thread {
	struct spdk_thread *thread;
	uint32_t lcore;
};

struct cpcs_builtin_async_exec_ctx {
	struct cpcs_program *prog;
	struct cpcs_exec_context *exec_ctx;
	cpcs_runtime_execute_done_cb done_cb;
	void *done_arg;

	uint16_t pind;
	uint64_t len;
	uint64_t processed;
	uint64_t chunk_len;
	uint64_t return_value;
	uint8_t *buf;
	bool initialized;

	uint64_t src_mr_id;
	uint64_t src_off;
	uint64_t dst_mr_id;
	uint64_t dst_off;
	uint64_t mr_id;
	uint64_t off;
	uint8_t pattern;
};

struct cpcs_builtin_kv_req_header {
	char magic[8];
	uint32_t version;
	uint32_t op;
	uint32_t mode;
	uint32_t flags;
	uint32_t rank;
	uint32_t dtype_len;
	uint32_t extra_len;
	uint64_t payload_len;
} __attribute__((packed));

struct cpcs_builtin_kv_lossless_header {
	char magic[4];
	uint8_t kind;
	uint8_t reserved0[3];
	uint32_t raw_len;
	uint32_t encoded_len;
} __attribute__((packed));

struct cpcs_builtin_kv_quant_header {
	char magic[4];
	uint8_t kind;
	uint8_t reserved0[3];
	uint32_t elem_count;
	uint32_t scale_bits;
} __attribute__((packed));

struct cpcs_builtin_kv_layout_header {
	char magic[4];
	uint8_t kind;
	uint8_t reserved0[3];
	uint32_t block_size;
	uint32_t raw_len;
} __attribute__((packed));

struct cpcs_builtin_kv_req_view {
	uint32_t op;
	uint32_t mode;
	uint32_t flags;
	uint32_t rank;
	const uint8_t *shape;
	const uint8_t *dtype;
	uint32_t dtype_len;
	const uint8_t *extra;
	uint32_t extra_len;
	const uint8_t *payload;
	uint64_t payload_len;
};

struct cpcs_builtin_kv_output_target {
	uint64_t mr_id;
	uint64_t off;
	uint32_t len;
};

struct cpcs_builtin_kv_async_ctx {
	struct cpcs_builtin_extended_exec_ctx *worker_ctx;
	struct cpcs_builtin_kv_output_target target;
	uint8_t *owned_buf;
	const uint8_t *result_buf;
	uint64_t result_len;
	uint64_t return_value;
};

#define CPCS_BUILTIN_IO_CHUNK (64 * 1024)
#define CPCS_BUILTIN_REDUCE_MAX_SG_ENTRIES 32
#define CPCS_BUILTIN_COPY_MAX_SG_ENTRIES 64
#define CPCS_BUILTIN_FILL_MAX_SG_ENTRIES 64
#define CPCS_BUILTIN_KV_ABI_VERSION 1u
#define CPCS_BUILTIN_KV_MAGIC "CPCSREQ1"
#define CPCS_BUILTIN_KV_MAGIC_LOSSLESS "KVL1"
#define CPCS_BUILTIN_KV_MAGIC_QUANT "KVQ1"
#define CPCS_BUILTIN_KV_MAGIC_LAYOUT "KVR1"
#define CPCS_BUILTIN_KV_KIND_LOSSLESS_RLE 1u
#define CPCS_BUILTIN_KV_KIND_QUANT_I8 2u
#define CPCS_BUILTIN_KV_KIND_LAYOUT_TRANSPOSE 3u
#define CPCS_BUILTIN_KV_MAX_JSON_NUMBER_LEN 63u
#define CPCS_BUILTIN_KV_DEFAULT_BLOCK_SIZE 4096u
#define CPCS_BUILTIN_KV_MAX_BLOCK_SIZE (1024u * 1024u)
#define CPCS_BUILTIN_KV_RETURN_COUNT_MASK 0xFFFFFFFFull

static struct spdk_cpuset g_compute_core_mask;
static bool g_compute_core_mask_configured;
static struct cpcs_builtin_compute_thread g_compute_threads[SPDK_CPUSET_SIZE];
static uint32_t g_compute_thread_count;
static uint32_t g_compute_thread_next;
static struct cpcs_builtin_runtime_stats g_cpcs_builtin_runtime_stats;

enum cpcs_builtin_kv_op {
	CPCS_BUILTIN_KV_OP_PACK_STORE = 1,
	CPCS_BUILTIN_KV_OP_UNPACK_LOAD = 2,
	CPCS_BUILTIN_KV_OP_LAYOUT_REPACK = 3,
	CPCS_BUILTIN_KV_OP_BLOCK_SELECT = 4,
	CPCS_BUILTIN_KV_OP_PREFIX_LOOKUP = 5,
	CPCS_BUILTIN_KV_OP_BATCH_READ = 6,
};

enum cpcs_builtin_kv_mode {
	CPCS_BUILTIN_KV_MODE_OFF = 0,
	CPCS_BUILTIN_KV_MODE_NOOP = 1,
	CPCS_BUILTIN_KV_MODE_LOSSLESS_COMPRESS = 2,
	CPCS_BUILTIN_KV_MODE_INT8_QUANTIZE = 3,
	CPCS_BUILTIN_KV_MODE_LAYOUT = 4,
	CPCS_BUILTIN_KV_MODE_BLOCK_SELECT = 5,
	CPCS_BUILTIN_KV_MODE_PREFIX_INDEX = 6,
};

SPDK_STATIC_ASSERT(sizeof(struct cpcs_builtin_metadata_record) == 32,
		   "Unexpected metadata record size");
SPDK_STATIC_ASSERT(sizeof(struct cpcs_builtin_kv_req_header) == 44,
		   "Unexpected KV request header size");
SPDK_STATIC_ASSERT(sizeof(struct cpcs_builtin_kv_lossless_header) == 16,
		   "Unexpected KV lossless header size");
SPDK_STATIC_ASSERT(sizeof(struct cpcs_builtin_kv_quant_header) == 16,
		   "Unexpected KV quant header size");
SPDK_STATIC_ASSERT(sizeof(struct cpcs_builtin_kv_layout_header) == 16,
		   "Unexpected KV layout header size");

static struct spdk_bdev *
_cpcs_find_bdev_by_nsid(uint32_t mnsid)
{
	struct spdk_bdev *bdev;

	for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
		if (spdk_bdev_get_nvme_nsid(bdev) == mnsid) {
			return bdev;
		}
	}

	return NULL;
}

static int
_cpcs_exec_resolve_range(const struct cpcs_exec_context *ctx,
			 uint64_t mr_id, uint64_t off, uint64_t len,
			 struct spdk_bdev **bdev_out, uint64_t *absolute_offset_out)
{
	const struct cpcs_memory_range *ranges = NULL;
	uint32_t range_count = 0;
	const struct cpcs_exec_resolved_range *mr;
	struct spdk_bdev *bdev = NULL;
	uint64_t absolute_offset;

	if (ctx == NULL || bdev_out == NULL || absolute_offset_out == NULL) {
		return -EINVAL;
	}

	if (mr_id == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	if (ctx->resolved_ranges != NULL && ctx->resolved_range_count != 0) {
		range_count = ctx->resolved_range_count;
		if (mr_id > range_count) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}
		mr = &ctx->resolved_ranges[mr_id - 1];
		bdev = mr->bdev;
		absolute_offset = mr->starting_byte;
		range_count = mr->length;
	} else {
		/*
		 * Direct in-process test harnesses can call cpcs_execute_run() without
		 * the full NVMf request path that populates resolved_ranges.
		 * Allow that only when ctx->req is absent.
		 */
		if (ctx->req != NULL) {
			return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}

		if (ctx->mrs != NULL) {
			ranges = ctx->mrs->ranges;
			range_count = ctx->mrs->range_count;
		} else {
			ranges = ctx->inline_ranges;
			range_count = ctx->inline_range_count;
		}

		if (ranges == NULL || range_count == 0) {
			return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
		if (mr_id > range_count) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		bdev = _cpcs_find_bdev_by_nsid(ranges[mr_id - 1].mnsid);
		if (bdev == NULL) {
			return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
		}

		absolute_offset = ranges[mr_id - 1].starting_byte;
		range_count = ranges[mr_id - 1].length;
	}

	if (bdev == NULL) {
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}

	if (off > UINT64_MAX - len) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (off + len > range_count) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (absolute_offset > UINT64_MAX - off) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	absolute_offset += off;
	*bdev_out = bdev;
	*absolute_offset_out = absolute_offset;
	return 0;
}

static int
cpcs_builtin_map_slm_status(int rc)
{
	if (rc == -ENOENT || rc == -ENOTSUP) {
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}
	if (rc == -EINVAL) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	return rc;
}

static void
_cpcs_builtin_sync_wait_done(void *cb_arg, int status)
{
	struct cpcs_builtin_sync_wait_ctx *wait_ctx = cb_arg;

	wait_ctx->status = cpcs_builtin_map_slm_status(status);
	wait_ctx->done = true;
}

static int
_cpcs_builtin_sync_wait(struct cpcs_builtin_sync_wait_ctx *wait_ctx)
{
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (thread == NULL) {
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	__atomic_add_fetch(&g_cpcs_builtin_runtime_stats.sync_wait_total, 1,
			   __ATOMIC_RELAXED);

	while (!wait_ctx->done) {
		spdk_thread_poll(thread, 0, 0);
	}

	return wait_ctx->status;
}

void
cpcs_builtin_runtime_get_stats(struct cpcs_builtin_runtime_stats *stats)
{
	if (stats == NULL) {
		return;
	}

	memset(stats, 0, sizeof(*stats));
	stats->sync_wait_total =
		__atomic_load_n(&g_cpcs_builtin_runtime_stats.sync_wait_total,
				__ATOMIC_RELAXED);
}

void
cpcs_builtin_runtime_reset_stats(void)
{
	__atomic_store_n(&g_cpcs_builtin_runtime_stats.sync_wait_total, 0,
			 __ATOMIC_RELAXED);
}

static int
_cpcs_exec_read_range_async(const struct cpcs_exec_context *ctx,
			    uint64_t mr_id, uint64_t off, uint64_t len, void *buf,
			    spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev;
	uint64_t absolute_offset;
	int rc;

	if (len != 0 && buf == NULL) {
		return -EINVAL;
	}

	rc = _cpcs_exec_resolve_range(ctx, mr_id, off, len, &bdev, &absolute_offset);
	if (rc != 0) {
		return rc;
	}

	rc = bdev_slm_exec_read_by_bdev_async(bdev, absolute_offset, len, buf, cb_fn, cb_arg);
	if (rc != 0) {
		return cpcs_builtin_map_slm_status(rc);
	}

	return 0;
}

static int
_cpcs_exec_write_range_async(const struct cpcs_exec_context *ctx,
			     uint64_t mr_id, uint64_t off, uint64_t len, const void *buf,
			     spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev;
	uint64_t absolute_offset;
	int rc;

	if (len != 0 && buf == NULL) {
		return -EINVAL;
	}

	rc = _cpcs_exec_resolve_range(ctx, mr_id, off, len, &bdev, &absolute_offset);
	if (rc != 0) {
		return rc;
	}

	rc = bdev_slm_exec_write_by_bdev_async(bdev, absolute_offset, len, buf, cb_fn, cb_arg);
	if (rc != 0) {
		return cpcs_builtin_map_slm_status(rc);
	}

	return 0;
}

static int
_cpcs_exec_read_range_sync(const struct cpcs_exec_context *ctx,
			   uint64_t mr_id, uint64_t off, uint64_t len, void *buf)
{
	struct cpcs_builtin_sync_wait_ctx wait_ctx = {};
	struct spdk_bdev *bdev;
	uint64_t absolute_offset;
	int rc;

	if (len != 0 && buf == NULL) {
		return -EINVAL;
	}

	rc = _cpcs_exec_resolve_range(ctx, mr_id, off, len, &bdev, &absolute_offset);
	if (rc != 0) {
		return rc;
	}

	if (spdk_get_thread() != NULL) {
		rc = bdev_slm_exec_read_by_bdev_async(bdev, absolute_offset, len, buf,
						      _cpcs_builtin_sync_wait_done, &wait_ctx);
		if (rc != 0) {
			return cpcs_builtin_map_slm_status(rc);
		}
		return _cpcs_builtin_sync_wait(&wait_ctx);
	}

	rc = bdev_slm_exec_read_by_bdev(bdev, absolute_offset, len, buf);
	if (rc != 0) {
		return cpcs_builtin_map_slm_status(rc);
	}

	return 0;
}

static int
__attribute__((unused))
_cpcs_exec_write_range_sync(const struct cpcs_exec_context *ctx,
			    uint64_t mr_id, uint64_t off, uint64_t len, const void *buf)
{
	struct cpcs_builtin_sync_wait_ctx wait_ctx = {};
	struct spdk_bdev *bdev;
	uint64_t absolute_offset;
	int rc;

	if (len != 0 && buf == NULL) {
		return -EINVAL;
	}

	rc = _cpcs_exec_resolve_range(ctx, mr_id, off, len, &bdev, &absolute_offset);
	if (rc != 0) {
		return rc;
	}

	if (spdk_get_thread() != NULL) {
		rc = bdev_slm_exec_write_by_bdev_async(bdev, absolute_offset, len, buf,
						       _cpcs_builtin_sync_wait_done, &wait_ctx);
		if (rc != 0) {
			return cpcs_builtin_map_slm_status(rc);
		}
		return _cpcs_builtin_sync_wait(&wait_ctx);
	}

	rc = bdev_slm_exec_write_by_bdev(bdev, absolute_offset, len, buf);
	if (rc != 0) {
		return cpcs_builtin_map_slm_status(rc);
	}

	return 0;
}

static int
_cpcs_exec_pin_range_raw(const struct cpcs_exec_context *ctx,
			 uint64_t mr_id, uint64_t off, uint64_t len, bool for_write,
			 struct spdk_bdev **bdev_out,
			 struct spdk_bdev_slm_sg_entry *entries, uint32_t max_entries,
			 uint32_t *entry_count)
{
	struct spdk_bdev *bdev;
	uint64_t absolute_offset;
	int rc;

	if (bdev_out == NULL || entries == NULL || entry_count == NULL || max_entries == 0) {
		return -EINVAL;
	}

	rc = _cpcs_exec_resolve_range(ctx, mr_id, off, len, &bdev, &absolute_offset);
	if (rc != 0) {
		return rc;
	}

	rc = bdev_slm_try_pin_range_by_bdev(bdev, absolute_offset, len, for_write,
					    entries, max_entries, entry_count);
	if (rc != 0) {
		return rc;
	}

	*bdev_out = bdev;
	return 0;
}

static int
_cpcs_exec_unpin_range_raw(struct spdk_bdev *bdev,
			   const struct spdk_bdev_slm_sg_entry *entries,
			   uint32_t entry_count, bool dirtied)
{
	if (bdev == NULL) {
		return -EINVAL;
	}

	return bdev_slm_unpin_range_by_bdev(bdev, entries, entry_count, dirtied);
}

static float
_cpcs_f32_from_u32(uint32_t value)
{
	union {
		uint32_t u32;
		float f32;
	} u = {};

	u.u32 = value;
	return u.f32;
}

static bool
_cpcs_builtin_field_is_float(uint8_t field_id)
{
	return field_id == SPDK_CPCS_BUILTIN_FIELD_PRICE;
}

static int
_cpcs_builtin_meta_get_u32(const struct cpcs_builtin_metadata_record *meta,
			   uint8_t field_id, uint32_t *value_out)
{
	if (meta == NULL || value_out == NULL) {
		return -EINVAL;
	}

	switch (field_id) {
	case SPDK_CPCS_BUILTIN_FIELD_CATEGORY_ID:
		*value_out = from_le32(&meta->category_id);
		return 0;
	case SPDK_CPCS_BUILTIN_FIELD_FLAGS:
		*value_out = from_le32(&meta->flags);
		return 0;
	case SPDK_CPCS_BUILTIN_FIELD_REGION_ID:
		*value_out = (uint32_t)from_le16(&meta->region_id);
		return 0;
	case SPDK_CPCS_BUILTIN_FIELD_TIMESTAMP_BUCKET:
		*value_out = from_le32(&meta->timestamp_bucket);
		return 0;
	case SPDK_CPCS_BUILTIN_FIELD_VECTOR_INDEX:
		*value_out = from_le32(&meta->vector_index);
		return 0;
	default:
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
}

static int
_cpcs_builtin_meta_get_f32(const struct cpcs_builtin_metadata_record *meta,
			   uint8_t field_id, float *value_out)
{
	uint32_t raw_le;
	uint32_t raw;

	if (meta == NULL || value_out == NULL) {
		return -EINVAL;
	}

	if (field_id != SPDK_CPCS_BUILTIN_FIELD_PRICE) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	memcpy(&raw_le, &meta->price, sizeof(raw_le));
	raw = from_le32(&raw_le);
	*value_out = _cpcs_f32_from_u32(raw);
	return 0;
}

static bool
_cpcs_builtin_filter_u32_match(uint8_t op, uint32_t value,
			       const struct spdk_cpcs_builtin_eval_filter_clause *clause)
{
	uint32_t i;

	switch (op) {
	case SPDK_CPCS_BUILTIN_FILTER_EQ_U32:
		return value == from_le32(&clause->args[0]);
	case SPDK_CPCS_BUILTIN_FILTER_IN_SET_U32:
		if (clause->in_count == 0 || clause->in_count > SPDK_COUNTOF(clause->args)) {
			return false;
		}
		for (i = 0; i < clause->in_count; i++) {
			if (value == from_le32(&clause->args[i])) {
				return true;
			}
		}
		return false;
	case SPDK_CPCS_BUILTIN_FILTER_BITMASK_ANY:
		return (value & from_le32(&clause->args[0])) != 0;
	case SPDK_CPCS_BUILTIN_FILTER_RANGE_U32: {
		uint32_t min_v = from_le32(&clause->args[0]);
		uint32_t max_v = from_le32(&clause->args[1]);
		return value >= min_v && value <= max_v;
	}
	default:
		return false;
	}
}

static bool
_cpcs_builtin_filter_f32_match(uint8_t op, float value,
			       const struct spdk_cpcs_builtin_eval_filter_clause *clause)
{
	float min_v, max_v;

	if (op != SPDK_CPCS_BUILTIN_FILTER_RANGE_F32) {
		return false;
	}

	min_v = _cpcs_f32_from_u32(from_le32(&clause->args[0]));
	max_v = _cpcs_f32_from_u32(from_le32(&clause->args[1]));
	return value >= min_v && value <= max_v;
}

static int
_cpcs_builtin_filter_match_one(const struct cpcs_builtin_metadata_record *meta,
			       const struct spdk_cpcs_builtin_eval_filter_clause *clause,
			       bool *match_out)
{
	uint32_t u32_value;
	float f32_value;
	int rc;

	if (meta == NULL || clause == NULL || match_out == NULL) {
		return -EINVAL;
	}

	if (_cpcs_builtin_field_is_float(clause->field_id)) {
		rc = _cpcs_builtin_meta_get_f32(meta, clause->field_id, &f32_value);
		if (rc != 0) {
			return rc;
		}
		*match_out = _cpcs_builtin_filter_f32_match(clause->op, f32_value, clause);
		return 0;
	}

	rc = _cpcs_builtin_meta_get_u32(meta, clause->field_id, &u32_value);
	if (rc != 0) {
		return rc;
	}
	*match_out = _cpcs_builtin_filter_u32_match(clause->op, u32_value, clause);
	return 0;
}

static int
_cpcs_builtin_filter_match_all(const struct cpcs_builtin_metadata_record *meta,
			       const struct spdk_cpcs_builtin_eval_filter_clause *clauses,
			       uint16_t filter_count, bool *match_out)
{
	bool one_match;
	uint16_t i;
	int rc;

	if (match_out == NULL) {
		return -EINVAL;
	}

	*match_out = true;
	if (filter_count == 0) {
		return 0;
	}
	if (meta == NULL || clauses == NULL) {
		return -EINVAL;
	}

	for (i = 0; i < filter_count; i++) {
		rc = _cpcs_builtin_filter_match_one(meta, &clauses[i], &one_match);
		if (rc != 0) {
			return rc;
		}
		if (!one_match) {
			*match_out = false;
			return 0;
		}
	}

	return 0;
}

static float
_cpcs_builtin_score_cosine(const float *query, const float *vector, uint32_t dim,
			   bool query_normalized)
{
	double dot = 0.0;
	double qnorm = 0.0;
	double vnorm = 0.0;
	uint32_t i;

	for (i = 0; i < dim; i++) {
		double q = query[i];
		double v = vector[i];
		dot += q * v;
		vnorm += v * v;
		if (!query_normalized) {
			qnorm += q * q;
		}
	}

	if (query_normalized) {
		qnorm = 1.0;
	}
	if (qnorm <= 0.0 || vnorm <= 0.0) {
		return 0.0f;
	}

	return (float)(dot / (sqrt(qnorm) * sqrt(vnorm)));
}

static float
_cpcs_builtin_score_l2(const float *query, const float *vector, uint32_t dim, bool squared)
{
	double dist_sq = 0.0;
	uint32_t i;

	for (i = 0; i < dim; i++) {
		double d = (double)query[i] - (double)vector[i];
		dist_sq += d * d;
	}

	if (squared) {
		return (float)(-dist_sq);
	}
	return (float)(-sqrt(dist_sq));
}

static bool
_cpcs_builtin_topk_better(float score_a, uint64_t doc_id_a, float score_b, uint64_t doc_id_b)
{
	if (score_a > score_b) {
		return true;
	}
	if (score_a < score_b) {
		return false;
	}
	return doc_id_a < doc_id_b;
}

static uint32_t
_cpcs_builtin_topk_find_worst(const struct cpcs_builtin_topk_state_entry *entries, uint32_t count)
{
	uint32_t worst = 0;
	uint32_t i;

	for (i = 1; i < count; i++) {
		if (_cpcs_builtin_topk_better(entries[worst].score, entries[worst].doc_id,
					      entries[i].score, entries[i].doc_id)) {
			worst = i;
		}
	}

	return worst;
}

static int
_cpcs_builtin_topk_cmp_desc(const void *a, const void *b)
{
	const struct cpcs_builtin_topk_state_entry *ra = a;
	const struct cpcs_builtin_topk_state_entry *rb = b;

	if (_cpcs_builtin_topk_better(ra->score, ra->doc_id, rb->score, rb->doc_id)) {
		return -1;
	}
	if (_cpcs_builtin_topk_better(rb->score, rb->doc_id, ra->score, ra->doc_id)) {
		return 1;
	}
	return 0;
}

static int
_cpcs_builtin_write_output_async(const struct cpcs_exec_context *ctx,
				 const struct spdk_cpcs_builtin_eval_req_header *hdr,
				 const void *buf, uint32_t len,
				 spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	if (len > from_le32(&hdr->output_length)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	return _cpcs_exec_write_range_async(ctx, from_le64(&hdr->output_mr_id),
					    from_le64(&hdr->output_offset), len, buf,
					    cb_fn, cb_arg);
}

static void
_cpcs_builtin_fill_common_stats(const struct cpcs_exec_context *ctx,
				struct spdk_cpcs_builtin_eval_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->host_req_bytes = ctx->data_len;
	stats->host_resp_bytes = sizeof(uint64_t);
	stats->command_count = 1;
}

static void
_cpcs_builtin_deferred_done_msg(void *arg)
{
	struct cpcs_builtin_deferred_done_ctx *ctx = arg;
	cpcs_runtime_execute_done_cb cb;
	void *cb_arg;
	int status;
	uint64_t return_value;

	cb = ctx->done_cb;
	cb_arg = ctx->done_arg;
	status = ctx->status;
	return_value = ctx->return_value;
	free(ctx);
	cb(cb_arg, status, return_value);
}

static int
_cpcs_builtin_schedule_done_on_thread(struct spdk_thread *thread,
				      cpcs_runtime_execute_done_cb done_cb,
				      void *done_arg, int status, uint64_t return_value)
{
	struct cpcs_builtin_deferred_done_ctx *done_ctx;
	int rc;

	if (thread == NULL) {
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	done_ctx = calloc(1, sizeof(*done_ctx));
	if (done_ctx == NULL) {
		return -ENOMEM;
	}

	done_ctx->done_cb = done_cb;
	done_ctx->done_arg = done_arg;
	done_ctx->status = status;
	done_ctx->return_value = return_value;

	rc = spdk_thread_send_msg(thread, _cpcs_builtin_deferred_done_msg, done_ctx);
	if (rc != 0) {
		free(done_ctx);
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	return 0;
}

static void
_cpcs_builtin_extended_complete(struct cpcs_builtin_extended_exec_ctx *ctx,
				int status, uint64_t return_value)
{
	cpcs_runtime_execute_done_cb done_cb;
	void *done_arg;
	struct spdk_thread *submit_thread;
	int rc;

	done_cb = ctx->done_cb;
	done_arg = ctx->done_arg;
	submit_thread = ctx->submit_thread;
	free(ctx);

	if (submit_thread == spdk_get_thread()) {
		done_cb(done_arg, status, return_value);
		return;
	}

	rc = _cpcs_builtin_schedule_done_on_thread(submit_thread, done_cb, done_arg,
			status, return_value);
	if (rc != 0) {
		done_cb(done_arg, -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 0);
	}
}

static struct spdk_thread *
_cpcs_builtin_select_compute_thread(void)
{
	uint32_t count;
	uint32_t idx;

	count = __atomic_load_n(&g_compute_thread_count, __ATOMIC_ACQUIRE);
	if (count == 0) {
		return NULL;
	}

	idx = __atomic_fetch_add(&g_compute_thread_next, 1, __ATOMIC_RELAXED) % count;
	return g_compute_threads[idx].thread;
}

static void
_cpcs_builtin_compute_thread_exit_msg(void *arg)
{
	(void)arg;

	spdk_thread_exit(spdk_get_thread());
}

static void
_cpcs_builtin_stop_compute_threads(uint32_t count)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		if (g_compute_threads[i].thread != NULL) {
			spdk_thread_send_msg(g_compute_threads[i].thread,
					     _cpcs_builtin_compute_thread_exit_msg, NULL);
			g_compute_threads[i].thread = NULL;
		}
	}
}

int
cpcs_builtin_runtime_set_compute_core_mask(const struct spdk_cpuset *mask)
{
	if (mask == NULL || spdk_cpuset_count(mask) == 0) {
		return -EINVAL;
	}

	if (__atomic_load_n(&g_compute_thread_count, __ATOMIC_ACQUIRE) != 0) {
		return -EBUSY;
	}

	spdk_cpuset_copy(&g_compute_core_mask, mask);
	g_compute_core_mask_configured = true;
	g_compute_thread_next = 0;

	return 0;
}

const struct spdk_cpuset *
cpcs_builtin_runtime_get_compute_core_mask(void)
{
	return g_compute_core_mask_configured ? &g_compute_core_mask : NULL;
}

bool
cpcs_builtin_runtime_is_compute_core(uint32_t lcore)
{
	if (!g_compute_core_mask_configured) {
		return false;
	}

	return spdk_cpuset_get_cpu(&g_compute_core_mask, lcore);
}

uint32_t
cpcs_builtin_runtime_get_compute_core_count(void)
{
	return __atomic_load_n(&g_compute_thread_count, __ATOMIC_ACQUIRE);
}

int
cpcs_builtin_runtime_start_compute_threads(void)
{
	struct spdk_cpuset thread_mask = {};
	struct spdk_thread *thread;
	char thread_name[32];
	uint32_t lcore;
	uint32_t count = 0;
	uint32_t configured_count;

	if (!g_compute_core_mask_configured) {
		return 0;
	}

	if (__atomic_load_n(&g_compute_thread_count, __ATOMIC_ACQUIRE) != 0) {
		return 0;
	}

	configured_count = spdk_cpuset_count(&g_compute_core_mask);
	SPDK_ENV_FOREACH_CORE(lcore) {
		if (!spdk_cpuset_get_cpu(&g_compute_core_mask, lcore)) {
			continue;
		}

		spdk_cpuset_zero(&thread_mask);
		spdk_cpuset_set_cpu(&thread_mask, lcore, true);
		snprintf(thread_name, sizeof(thread_name), "cpcs_compute_%03u", lcore);
		thread = spdk_thread_create(thread_name, &thread_mask);
		if (thread == NULL) {
			_cpcs_builtin_stop_compute_threads(count);
			return -ENOMEM;
		}

		g_compute_threads[count].thread = thread;
		g_compute_threads[count].lcore = lcore;
		count++;
	}

	if (count != configured_count) {
		_cpcs_builtin_stop_compute_threads(count);
		return -EINVAL;
	}

	__atomic_store_n(&g_compute_thread_count, count, __ATOMIC_RELEASE);
	SPDK_NOTICELOG("CPCS builtin compute threads created on core mask 0x%s\n",
		       spdk_cpuset_fmt(&g_compute_core_mask));

	return 0;
}

void
cpcs_builtin_runtime_stop_compute_threads(void)
{
	uint32_t count;

	count = __atomic_exchange_n(&g_compute_thread_count, 0, __ATOMIC_ACQ_REL);
	_cpcs_builtin_stop_compute_threads(count);
	g_compute_thread_next = 0;
}

static int _cpcs_builtin_exec_filter_agg_async(struct cpcs_builtin_extended_exec_ctx *worker_ctx);
static int _cpcs_builtin_exec_filtered_topk_exact_async(
	struct cpcs_builtin_extended_exec_ctx *worker_ctx);

static uint32_t
_cpcs_builtin_kv_expected_op(uint16_t pind)
{
	switch (pind) {
	case CPCS_BUILTIN_PIND_KV_PACK_STORE:
		return CPCS_BUILTIN_KV_OP_PACK_STORE;
	case CPCS_BUILTIN_PIND_KV_UNPACK_LOAD:
		return CPCS_BUILTIN_KV_OP_UNPACK_LOAD;
	case CPCS_BUILTIN_PIND_KV_LAYOUT_REPACK:
		return CPCS_BUILTIN_KV_OP_LAYOUT_REPACK;
	case CPCS_BUILTIN_PIND_KV_BLOCK_SELECT:
		return CPCS_BUILTIN_KV_OP_BLOCK_SELECT;
	case CPCS_BUILTIN_PIND_KV_PREFIX_LOOKUP:
		return CPCS_BUILTIN_KV_OP_PREFIX_LOOKUP;
	case CPCS_BUILTIN_PIND_KV_BATCH_READ:
		return CPCS_BUILTIN_KV_OP_BATCH_READ;
	default:
		return 0;
	}
}

static int
_cpcs_builtin_kv_safe_u64_to_u32(uint64_t value, uint32_t *out)
{
	if (out == NULL || value > UINT32_MAX) {
		return -EINVAL;
	}

	*out = (uint32_t)value;
	return 0;
}

static int
_cpcs_builtin_kv_copy_payload(const uint8_t *src, uint64_t src_len, uint8_t **dst,
			      uint64_t *dst_len)
{
	uint8_t *out;

	if (src_len > 0 && src == NULL) {
		return -EINVAL;
	}
	if (dst == NULL || dst_len == NULL) {
		return -EINVAL;
	}
	if (src_len > SIZE_MAX) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	out = NULL;
	if (src_len > 0) {
		out = malloc((size_t)src_len);
		if (out == NULL) {
			return -ENOMEM;
		}
		memcpy(out, src, (size_t)src_len);
	}

	*dst = out;
	*dst_len = src_len;
	return 0;
}

static uint64_t
_cpcs_builtin_kv_pack_return(uint32_t value, uint32_t crc32)
{
	return ((uint64_t)crc32 << 32) | (uint64_t)value;
}

static int
_cpcs_builtin_kv_parse_request(const struct cpcs_exec_context *ctx, uint16_t pind,
			       struct cpcs_builtin_kv_req_view *view)
{
	struct cpcs_builtin_kv_req_header hdr = {};
	uint32_t version;
	uint32_t op;
	uint64_t shape_bytes;
	uint64_t remaining;
	uint32_t expected_op;
	const uint8_t *cursor;
	uint32_t rank;
	uint32_t dtype_len;
	uint32_t extra_len;
	uint64_t payload_len;

	if (ctx == NULL || view == NULL || ctx->data_buffer == NULL || ctx->data_len < sizeof(hdr)) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}

	memcpy(&hdr, ctx->data_buffer, sizeof(hdr));
	if (memcmp(hdr.magic, CPCS_BUILTIN_KV_MAGIC, sizeof(hdr.magic)) != 0) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}

	version = from_le32(&hdr.version);
	if (version != CPCS_BUILTIN_KV_ABI_VERSION) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}

	op = from_le32(&hdr.op);
	expected_op = _cpcs_builtin_kv_expected_op(pind);
	if (expected_op == 0 || op != expected_op) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	payload_len = from_le64(&hdr.payload_len);
	rank = from_le32(&hdr.rank);
	shape_bytes = (uint64_t)rank * sizeof(uint64_t);
	dtype_len = from_le32(&hdr.dtype_len);
	extra_len = from_le32(&hdr.extra_len);

	/*
	 * payload_len is fully host-controlled and shape_bytes can be as large
	 * as UINT32_MAX * 8, so a single additive "required" expression can wrap
	 * uint64 and slip past the bounds check. Validate each variable-length
	 * section by subtracting it from the bytes remaining after the header;
	 * ctx->data_len is uint32_t so remaining never overflows.
	 */
	remaining = (uint64_t)ctx->data_len - sizeof(hdr);
	if (shape_bytes > remaining) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}
	remaining -= shape_bytes;
	if ((uint64_t)dtype_len > remaining) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}
	remaining -= dtype_len;
	if ((uint64_t)extra_len > remaining) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}
	remaining -= extra_len;
	if (payload_len > remaining) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}

	cursor = (const uint8_t *)ctx->data_buffer + sizeof(hdr);
	view->op = op;
	view->mode = from_le32(&hdr.mode);
	view->flags = from_le32(&hdr.flags);
	view->rank = rank;
	view->shape = cursor;
	cursor += shape_bytes;
	view->dtype = cursor;
	view->dtype_len = dtype_len;
	cursor += dtype_len;
	view->extra = cursor;
	view->extra_len = extra_len;
	cursor += extra_len;
	view->payload = cursor;
	view->payload_len = payload_len;

	return 0;
}

static const uint8_t *
_cpcs_builtin_kv_json_find_value(const uint8_t *json, uint32_t json_len, const char *key)
{
	char pattern[96];
	size_t key_len;
	size_t pattern_len;
	uint32_t i;
	uint32_t j;

	if (json == NULL || key == NULL) {
		return NULL;
	}

	key_len = strlen(key);
	if (key_len == 0 || key_len > (sizeof(pattern) - 4)) {
		return NULL;
	}

	pattern[0] = '\"';
	memcpy(&pattern[1], key, key_len);
	pattern[1 + key_len] = '\"';
	pattern[2 + key_len] = '\0';
	pattern_len = key_len + 2;

	for (i = 0; i + pattern_len < json_len; i++) {
		if (memcmp(&json[i], pattern, pattern_len) != 0) {
			continue;
		}

		j = i + (uint32_t)pattern_len;
		while (j < json_len &&
		       (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n')) {
			j++;
		}
		if (j >= json_len || json[j] != ':') {
			continue;
		}
		j++;
		while (j < json_len &&
		       (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n')) {
			j++;
		}
		if (j >= json_len) {
			return NULL;
		}
		return &json[j];
	}

	return NULL;
}

static int
_cpcs_builtin_kv_json_parse_u64(const uint8_t *json, uint32_t json_len,
				const char *key, uint64_t *value_out)
{
	const uint8_t *value;
	char tmp[CPCS_BUILTIN_KV_MAX_JSON_NUMBER_LEN + 1];
	size_t n = 0;
	size_t remain = 0;
	char *end = NULL;
	unsigned long long parsed;
	bool quoted = false;

	if (value_out == NULL) {
		return -EINVAL;
	}

	value = _cpcs_builtin_kv_json_find_value(json, json_len, key);
	if (value == NULL) {
		return -ENOENT;
	}
	remain = (size_t)(json_len - (uint32_t)(value - json));

	if (*value == '\"') {
		quoted = true;
		value++;
		remain--;
	}

	while (n < CPCS_BUILTIN_KV_MAX_JSON_NUMBER_LEN && n < remain) {
		uint8_t c = value[n];

		if (quoted) {
			if (c == '\"') {
				break;
			}
		} else if (c == ',' || c == '}' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			break;
		}

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
		      c == 'x' || c == 'X' || c == '+')) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}
		tmp[n] = (char)c;
		n++;
	}

	if (n == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	tmp[n] = '\0';
	parsed = strtoull(tmp, &end, 0);
	if (end == tmp || *end != '\0') {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	*value_out = (uint64_t)parsed;
	return 0;
}

static int
_cpcs_builtin_kv_json_parse_string(const uint8_t *json, uint32_t json_len,
				   const char *key, char *out, size_t out_len)
{
	const uint8_t *value;
	size_t n = 0;
	size_t i = 0;
	size_t remain = 0;
	bool escaped = false;

	if (out == NULL || out_len == 0) {
		return -EINVAL;
	}

	value = _cpcs_builtin_kv_json_find_value(json, json_len, key);
	if (value == NULL) {
		return -ENOENT;
	}
	if (*value != '\"') {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	value++;
	remain = (size_t)(json_len - (uint32_t)(value - json));

	while (i < remain && n + 1 < out_len) {
		uint8_t c = value[i++];

		if (!escaped && c == '\"') {
			out[n] = '\0';
			return 0;
		}
		if (!escaped && c == '\\') {
			escaped = true;
			continue;
		}

		out[n] = (char)c;
		escaped = false;
		n++;
	}

	out[out_len - 1] = '\0';
	return -SPDK_NVME_SC_INVALID_FIELD;
}

static int
_cpcs_builtin_kv_parse_output_target(const struct cpcs_builtin_kv_req_view *view,
				     struct cpcs_builtin_kv_output_target *target)
{
	uint64_t tmp;
	int rc;

	if (view == NULL || target == NULL) {
		return -EINVAL;
	}

	memset(target, 0, sizeof(*target));
	if (view->extra == NULL || view->extra_len == 0) {
		return 0;
	}

	rc = _cpcs_builtin_kv_json_parse_u64(view->extra, view->extra_len, "output_mr_id", &tmp);
	if (rc == -ENOENT) {
		return 0;
	}
	if (rc != 0) {
		return rc;
	}
	target->mr_id = tmp;
	if (target->mr_id == 0) {
		return 0;
	}

	rc = _cpcs_builtin_kv_json_parse_u64(view->extra, view->extra_len, "output_offset", &tmp);
	if (rc == 0) {
		target->off = tmp;
	} else if (rc != -ENOENT) {
		return rc;
	}

	rc = _cpcs_builtin_kv_json_parse_u64(view->extra, view->extra_len, "output_length", &tmp);
	if (rc == 0) {
		if (tmp > UINT32_MAX) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}
		target->len = (uint32_t)tmp;
	} else if (rc != -ENOENT) {
		return rc;
	}

	return 0;
}

static float
_cpcs_builtin_kv_load_le_f32(const uint8_t *buf)
{
	union {
		uint32_t u32;
		float f32;
	} u = {};
	uint32_t le_u32;

	memcpy(&le_u32, buf, sizeof(le_u32));
	u.u32 = from_le32(&le_u32);
	return u.f32;
}

static void
_cpcs_builtin_kv_store_le_f32(uint8_t *buf, float value)
{
	union {
		uint32_t u32;
		float f32;
	} u = {};
	uint32_t le_u32;

	u.f32 = value;
	to_le32(&le_u32, u.u32);
	memcpy(buf, &le_u32, sizeof(le_u32));
}

static int
_cpcs_builtin_kv_encode_lossless(const uint8_t *in, uint64_t in_len,
				 uint8_t **out, uint64_t *out_len)
{
	struct cpcs_builtin_kv_lossless_header hdr = {};
	uint64_t max_runs;
	uint64_t max_len;
	uint8_t *buf;
	uint64_t i = 0;
	uint64_t pos;
	uint64_t body_len;

	if (in_len > UINT32_MAX) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (out == NULL || out_len == NULL) {
		return -EINVAL;
	}

	max_runs = (in_len / UINT16_MAX) + 1;
	if (max_runs > ((UINT64_MAX - sizeof(hdr)) / 3)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	max_len = sizeof(hdr) + in_len + (max_runs * 3);
	if (max_len > SIZE_MAX) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	buf = malloc((size_t)max_len);
	if (buf == NULL) {
		return -ENOMEM;
	}

	memcpy(hdr.magic, CPCS_BUILTIN_KV_MAGIC_LOSSLESS, sizeof(hdr.magic));
	hdr.kind = CPCS_BUILTIN_KV_KIND_LOSSLESS_RLE;
	to_le32(&hdr.raw_len, (uint32_t)in_len);
	pos = sizeof(hdr);

	while (i < in_len) {
		uint64_t run = 1;

		if (in[i] == 0) {
			while (i + run < in_len && run < UINT16_MAX && in[i + run] == 0) {
				run++;
			}
			buf[pos++] = 1u;
			buf[pos++] = (uint8_t)(run & 0xFFu);
			buf[pos++] = (uint8_t)((run >> 8) & 0xFFu);
			i += run;
			continue;
		}

		while (i + run < in_len && run < UINT16_MAX && in[i + run] != 0) {
			run++;
		}
		buf[pos++] = 0u;
		buf[pos++] = (uint8_t)(run & 0xFFu);
		buf[pos++] = (uint8_t)((run >> 8) & 0xFFu);
		memcpy(&buf[pos], &in[i], (size_t)run);
		pos += run;
		i += run;
	}

	body_len = pos - sizeof(hdr);
	to_le32(&hdr.encoded_len, (uint32_t)body_len);
	memcpy(buf, &hdr, sizeof(hdr));

	if (pos >= in_len) {
		free(buf);
		return _cpcs_builtin_kv_copy_payload(in, in_len, out, out_len);
	}

	*out = buf;
	*out_len = pos;
	return 0;
}

static int
_cpcs_builtin_kv_decode_lossless(const uint8_t *in, uint64_t in_len,
				 uint8_t **out, uint64_t *out_len)
{
	struct cpcs_builtin_kv_lossless_header hdr = {};
	uint32_t raw_len;
	uint32_t encoded_len;
	uint64_t pos;
	uint64_t produced = 0;
	uint8_t *buf;

	if (in_len < sizeof(hdr)) {
		return -ENOENT;
	}
	memcpy(&hdr, in, sizeof(hdr));
	if (memcmp(hdr.magic, CPCS_BUILTIN_KV_MAGIC_LOSSLESS, sizeof(hdr.magic)) != 0 ||
	    hdr.kind != CPCS_BUILTIN_KV_KIND_LOSSLESS_RLE) {
		return -ENOENT;
	}

	raw_len = from_le32(&hdr.raw_len);
	encoded_len = from_le32(&hdr.encoded_len);
	if ((uint64_t)encoded_len > in_len - sizeof(hdr)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	buf = NULL;
	if (raw_len > 0) {
		buf = malloc(raw_len);
		if (buf == NULL) {
			return -ENOMEM;
		}
	}

	pos = sizeof(hdr);
	while (pos < sizeof(hdr) + encoded_len) {
		uint8_t tag;
		uint16_t run;

		if (pos + 3 > sizeof(hdr) + encoded_len) {
			free(buf);
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		tag = in[pos++];
		run = (uint16_t)in[pos] | ((uint16_t)in[pos + 1] << 8);
		pos += 2;
		if (run == 0 || produced + run > raw_len) {
			free(buf);
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		if (tag == 1u) {
			memset(&buf[produced], 0, run);
			produced += run;
			continue;
		}
		if (tag != 0u || pos + run > sizeof(hdr) + encoded_len) {
			free(buf);
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		memcpy(&buf[produced], &in[pos], run);
		pos += run;
		produced += run;
	}

	if (produced != raw_len) {
		free(buf);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	*out = buf;
	*out_len = raw_len;
	return 0;
}

static int
_cpcs_builtin_kv_encode_quant_i8(const uint8_t *in, uint64_t in_len,
				 uint8_t **out, uint64_t *out_len)
{
	struct cpcs_builtin_kv_quant_header hdr = {};
	uint32_t elem_count;
	uint8_t *buf;
	float max_abs = 0.0f;
	float scale;
	union {
		float f32;
		uint32_t u32;
	} scale_u = {};
	uint64_t i;
	uint64_t total_len;

	if (in_len == 0 || (in_len % sizeof(float)) != 0) {
		return _cpcs_builtin_kv_copy_payload(in, in_len, out, out_len);
	}

	if (_cpcs_builtin_kv_safe_u64_to_u32(in_len / sizeof(float), &elem_count) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	total_len = sizeof(hdr) + elem_count;
	if (total_len > SIZE_MAX) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	buf = malloc((size_t)total_len);
	if (buf == NULL) {
		return -ENOMEM;
	}

	for (i = 0; i < elem_count; i++) {
		float v = _cpcs_builtin_kv_load_le_f32(&in[i * sizeof(float)]);
		float abs_v = fabsf(v);

		if (abs_v > max_abs) {
			max_abs = abs_v;
		}
	}

	scale = (max_abs > 0.0f) ? (max_abs / 127.0f) : 1.0f;
	scale_u.f32 = scale;
	memcpy(hdr.magic, CPCS_BUILTIN_KV_MAGIC_QUANT, sizeof(hdr.magic));
	hdr.kind = CPCS_BUILTIN_KV_KIND_QUANT_I8;
	to_le32(&hdr.elem_count, elem_count);
	to_le32(&hdr.scale_bits, scale_u.u32);
	memcpy(buf, &hdr, sizeof(hdr));

	for (i = 0; i < elem_count; i++) {
		float v = _cpcs_builtin_kv_load_le_f32(&in[i * sizeof(float)]);
		int q = (int)lroundf(v / scale);

		if (q > 127) {
			q = 127;
		} else if (q < -127) {
			q = -127;
		}
		buf[sizeof(hdr) + i] = (uint8_t)(int8_t)q;
	}

	*out = buf;
	*out_len = total_len;
	return 0;
}

static int
_cpcs_builtin_kv_decode_quant_i8(const uint8_t *in, uint64_t in_len,
				 uint8_t **out, uint64_t *out_len)
{
	struct cpcs_builtin_kv_quant_header hdr = {};
	uint32_t elem_count;
	union {
		float f32;
		uint32_t u32;
	} scale_u = {};
	float scale;
	uint8_t *buf;
	uint64_t raw_len;
	uint64_t i;

	if (in_len < sizeof(hdr)) {
		return -ENOENT;
	}
	memcpy(&hdr, in, sizeof(hdr));
	if (memcmp(hdr.magic, CPCS_BUILTIN_KV_MAGIC_QUANT, sizeof(hdr.magic)) != 0 ||
	    hdr.kind != CPCS_BUILTIN_KV_KIND_QUANT_I8) {
		return -ENOENT;
	}

	elem_count = from_le32(&hdr.elem_count);
	if ((uint64_t)elem_count > (in_len - sizeof(hdr))) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	raw_len = (uint64_t)elem_count * sizeof(float);
	if (raw_len > SIZE_MAX) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	buf = malloc((size_t)raw_len);
	if (buf == NULL && raw_len != 0) {
		return -ENOMEM;
	}

	scale_u.u32 = from_le32(&hdr.scale_bits);
	scale = scale_u.f32;
	if (!(scale > 0.0f) || !isfinite(scale)) {
		free(buf);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	for (i = 0; i < elem_count; i++) {
		int8_t q = (int8_t)in[sizeof(hdr) + i];
		float v = (float)q * scale;

		_cpcs_builtin_kv_store_le_f32(&buf[i * sizeof(float)], v);
	}

	*out = buf;
	*out_len = raw_len;
	return 0;
}

static int
_cpcs_builtin_kv_encode_layout(const uint8_t *in, uint64_t in_len, uint32_t block_size,
			       uint8_t **out, uint64_t *out_len)
{
	struct cpcs_builtin_kv_layout_header hdr = {};
	uint8_t *buf;
	uint64_t nblocks;
	uint64_t pos;
	uint64_t i;
	uint64_t b;

	if (in_len > UINT32_MAX) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (block_size == 0 || block_size > CPCS_BUILTIN_KV_MAX_BLOCK_SIZE) {
		block_size = CPCS_BUILTIN_KV_DEFAULT_BLOCK_SIZE;
	}
	if (in_len == 0 || in_len <= block_size) {
		return _cpcs_builtin_kv_copy_payload(in, in_len, out, out_len);
	}
	if (sizeof(hdr) + in_len > SIZE_MAX) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	buf = malloc((size_t)(sizeof(hdr) + in_len));
	if (buf == NULL) {
		return -ENOMEM;
	}

	memcpy(hdr.magic, CPCS_BUILTIN_KV_MAGIC_LAYOUT, sizeof(hdr.magic));
	hdr.kind = CPCS_BUILTIN_KV_KIND_LAYOUT_TRANSPOSE;
	to_le32(&hdr.block_size, block_size);
	to_le32(&hdr.raw_len, (uint32_t)in_len);
	memcpy(buf, &hdr, sizeof(hdr));

	nblocks = (in_len + block_size - 1) / block_size;
	pos = sizeof(hdr);
	for (i = 0; i < block_size; i++) {
		for (b = 0; b < nblocks; b++) {
			uint64_t src = b * block_size + i;

			if (src >= in_len) {
				continue;
			}
			buf[pos++] = in[src];
		}
	}

	*out = buf;
	*out_len = pos;
	return 0;
}

static int
_cpcs_builtin_kv_decode_layout(const uint8_t *in, uint64_t in_len,
			       uint8_t **out, uint64_t *out_len)
{
	struct cpcs_builtin_kv_layout_header hdr = {};
	uint32_t block_size;
	uint32_t raw_len;
	uint8_t *buf;
	uint64_t nblocks;
	uint64_t pos;
	uint64_t i;
	uint64_t b;

	if (in_len < sizeof(hdr)) {
		return -ENOENT;
	}
	memcpy(&hdr, in, sizeof(hdr));
	if (memcmp(hdr.magic, CPCS_BUILTIN_KV_MAGIC_LAYOUT, sizeof(hdr.magic)) != 0 ||
	    hdr.kind != CPCS_BUILTIN_KV_KIND_LAYOUT_TRANSPOSE) {
		return -ENOENT;
	}

	block_size = from_le32(&hdr.block_size);
	raw_len = from_le32(&hdr.raw_len);
	if (block_size == 0 || block_size > CPCS_BUILTIN_KV_MAX_BLOCK_SIZE) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (raw_len > in_len - sizeof(hdr)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	buf = malloc(raw_len);
	if (buf == NULL && raw_len != 0) {
		return -ENOMEM;
	}

	nblocks = ((uint64_t)raw_len + block_size - 1) / block_size;
	pos = sizeof(hdr);
	for (i = 0; i < block_size; i++) {
		for (b = 0; b < nblocks; b++) {
			uint64_t dst = b * block_size + i;

			if (dst >= raw_len) {
				continue;
			}
			if (pos >= in_len) {
				free(buf);
				return -SPDK_NVME_SC_INVALID_FIELD;
			}
			buf[dst] = in[pos++];
		}
	}

	if (pos != sizeof(hdr) + raw_len) {
		free(buf);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	*out = buf;
	*out_len = raw_len;
	return 0;
}

static int
_cpcs_builtin_kv_select_blocks(const uint8_t *in, uint64_t in_len,
			       uint32_t block_size, uint32_t selected_blocks,
			       uint8_t **out, uint64_t *out_len)
{
	uint64_t total_blocks;
	uint64_t selected_bytes;
	uint64_t max_selected;

	if (block_size == 0 || block_size > CPCS_BUILTIN_KV_MAX_BLOCK_SIZE) {
		block_size = CPCS_BUILTIN_KV_DEFAULT_BLOCK_SIZE;
	}
	if (selected_blocks == 0) {
		selected_blocks = 1;
	}
	if (in_len == 0) {
		return _cpcs_builtin_kv_copy_payload(in, in_len, out, out_len);
	}

	total_blocks = (in_len + block_size - 1) / block_size;
	max_selected = spdk_min((uint64_t)selected_blocks, total_blocks);
	selected_bytes = spdk_min(in_len, max_selected * block_size);
	return _cpcs_builtin_kv_copy_payload(in, selected_bytes, out, out_len);
}

static uint32_t
_cpcs_builtin_kv_count_token(const uint8_t *buf, uint64_t buf_len,
			     const uint8_t *token, size_t token_len)
{
	uint64_t i;
	uint32_t count = 0;

	if (buf == NULL || token == NULL || token_len == 0 || buf_len < token_len) {
		return 0;
	}

	for (i = 0; i + token_len <= buf_len; i++) {
		if (memcmp(&buf[i], token, token_len) == 0) {
			count++;
		}
	}

	return count;
}

static uint32_t
_cpcs_builtin_kv_batch_entry_count(const uint8_t *payload, uint64_t payload_len)
{
	static const char key_token[] = "\"key\":";

	return _cpcs_builtin_kv_count_token(payload, payload_len,
					    (const uint8_t *)key_token, sizeof(key_token) - 1);
}

static int
_cpcs_builtin_kv_prepare_result(const struct cpcs_exec_context *ctx, uint16_t pind,
				struct cpcs_builtin_kv_async_ctx *kv_ctx)
{
	struct cpcs_builtin_kv_req_view view = {};
	uint8_t *owned_buf = NULL;
	const uint8_t *result_buf = NULL;
	uint64_t result_len = 0;
	uint32_t result_value = 0;
	uint32_t result_crc = 0;
	uint32_t block_size = CPCS_BUILTIN_KV_DEFAULT_BLOCK_SIZE;
	uint32_t selected_blocks = 1;
	uint64_t tmp_u64;
	int rc;
	char phase[16] = "";
	char prefix_token[128] = "";

	if (ctx == NULL || kv_ctx == NULL) {
		return -EINVAL;
	}

	rc = _cpcs_builtin_kv_parse_request(ctx, pind, &view);
	if (rc != 0) {
		return rc;
	}

	result_buf = view.payload;
	result_len = view.payload_len;

	rc = _cpcs_builtin_kv_parse_output_target(&view, &kv_ctx->target);
	if (rc != 0) {
		return rc;
	}

	switch (view.op) {
	case CPCS_BUILTIN_KV_OP_PACK_STORE:
		if (view.mode == CPCS_BUILTIN_KV_MODE_LOSSLESS_COMPRESS) {
			rc = _cpcs_builtin_kv_encode_lossless(view.payload, view.payload_len,
							      &owned_buf, &result_len);
			if (rc != 0) {
				return rc;
			}
			result_buf = owned_buf;
		} else if (view.mode == CPCS_BUILTIN_KV_MODE_INT8_QUANTIZE) {
			rc = _cpcs_builtin_kv_encode_quant_i8(view.payload, view.payload_len,
							      &owned_buf, &result_len);
			if (rc != 0) {
				return rc;
			}
			result_buf = owned_buf;
		}
		break;

	case CPCS_BUILTIN_KV_OP_UNPACK_LOAD:
		rc = _cpcs_builtin_kv_decode_quant_i8(view.payload, view.payload_len,
						      &owned_buf, &result_len);
		if (rc == 0) {
			result_buf = owned_buf;
			break;
		}
		if (rc != -ENOENT) {
			return rc;
		}

		rc = _cpcs_builtin_kv_decode_lossless(view.payload, view.payload_len,
						      &owned_buf, &result_len);
		if (rc == 0) {
			result_buf = owned_buf;
			break;
		}
		if (rc != -ENOENT) {
			return rc;
		}

		rc = _cpcs_builtin_kv_decode_layout(view.payload, view.payload_len,
						    &owned_buf, &result_len);
		if (rc == 0) {
			result_buf = owned_buf;
			break;
		}
		if (rc != -ENOENT) {
			return rc;
		}
		break;

	case CPCS_BUILTIN_KV_OP_LAYOUT_REPACK:
		if (view.extra != NULL && view.extra_len > 0) {
			rc = _cpcs_builtin_kv_json_parse_u64(view.extra, view.extra_len,
							     "layout_block_size_bytes", &tmp_u64);
			if (rc == 0 && tmp_u64 > 0 && tmp_u64 <= UINT32_MAX) {
				block_size = (uint32_t)tmp_u64;
			} else if (rc != 0 && rc != -ENOENT) {
				return rc;
			}
			rc = _cpcs_builtin_kv_json_parse_string(view.extra, view.extra_len,
								"phase", phase, sizeof(phase));
			if (rc != 0 && rc != -ENOENT) {
				return rc;
			}
		}

		if (strcmp(phase, "read") == 0) {
			rc = _cpcs_builtin_kv_decode_layout(view.payload, view.payload_len,
							    &owned_buf, &result_len);
			if (rc == 0) {
				result_buf = owned_buf;
				break;
			}
			if (rc != -ENOENT) {
				return rc;
			}
		} else {
			rc = _cpcs_builtin_kv_encode_layout(view.payload, view.payload_len, block_size,
							    &owned_buf, &result_len);
			if (rc != 0) {
				return rc;
			}
			result_buf = owned_buf;
		}
		break;

	case CPCS_BUILTIN_KV_OP_BLOCK_SELECT:
		if (view.extra != NULL && view.extra_len > 0) {
			rc = _cpcs_builtin_kv_json_parse_u64(view.extra, view.extra_len,
							     "selector_block_size_bytes", &tmp_u64);
			if (rc == 0 && tmp_u64 > 0 && tmp_u64 <= UINT32_MAX) {
				block_size = (uint32_t)tmp_u64;
			} else if (rc != 0 && rc != -ENOENT) {
				return rc;
			}

			rc = _cpcs_builtin_kv_json_parse_u64(view.extra, view.extra_len,
							     "selector_selected_blocks", &tmp_u64);
			if (rc == 0 && tmp_u64 > 0 && tmp_u64 <= UINT32_MAX) {
				selected_blocks = (uint32_t)tmp_u64;
			} else if (rc != 0 && rc != -ENOENT) {
				return rc;
			}
		}

		rc = _cpcs_builtin_kv_select_blocks(view.payload, view.payload_len,
						    block_size, selected_blocks,
						    &owned_buf, &result_len);
		if (rc != 0) {
			return rc;
		}
		result_buf = owned_buf;
		break;

	case CPCS_BUILTIN_KV_OP_PREFIX_LOOKUP:
		if (view.extra != NULL && view.extra_len > 0) {
			rc = _cpcs_builtin_kv_json_parse_string(view.extra, view.extra_len,
								"prefix_token", prefix_token,
								sizeof(prefix_token));
			if (rc != 0 && rc != -ENOENT) {
				return rc;
			}
		}
		result_value = _cpcs_builtin_kv_count_token(view.payload, view.payload_len,
				(const uint8_t *)prefix_token,
				strlen(prefix_token));
		break;

	case CPCS_BUILTIN_KV_OP_BATCH_READ:
		result_value = _cpcs_builtin_kv_batch_entry_count(view.payload, view.payload_len);
		break;

	default:
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	if (kv_ctx->target.mr_id != 0 && result_buf != NULL) {
		/*
		 * When the caller bounds the output via target.len, the full
		 * result must fit. Silently writing a prefix and reporting OK
		 * (with a CRC computed over the truncated buffer) would
		 * misreport the result, so reject instead of truncating.
		 */
		if (kv_ctx->target.len != 0 && result_len > (uint64_t)kv_ctx->target.len) {
			free(owned_buf);
			return -SPDK_NVME_SC_INVALID_FIELD;
		}
		if (result_len > UINT32_MAX) {
			free(owned_buf);
			return -SPDK_NVME_SC_INVALID_FIELD;
		}
	}

	if (result_value == 0 && view.op != CPCS_BUILTIN_KV_OP_PREFIX_LOOKUP &&
	    view.op != CPCS_BUILTIN_KV_OP_BATCH_READ) {
		if (result_len > CPCS_BUILTIN_KV_RETURN_COUNT_MASK) {
			result_value = (uint32_t)CPCS_BUILTIN_KV_RETURN_COUNT_MASK;
		} else {
			result_value = (uint32_t)result_len;
		}
	}

	if (result_len == 0 || result_buf == NULL) {
		result_crc = 0;
	} else {
		result_crc = spdk_crc32_ieee_update(result_buf,
						    (size_t)spdk_min(result_len, (uint64_t)SIZE_MAX), 0);
	}
	kv_ctx->owned_buf = owned_buf;
	kv_ctx->result_buf = result_buf;
	kv_ctx->result_len = result_len;
	kv_ctx->return_value = _cpcs_builtin_kv_pack_return(result_value, result_crc);
	return 0;
}

static void
_cpcs_builtin_kv_async_finish(struct cpcs_builtin_kv_async_ctx *kv_ctx,
			      int status, uint64_t return_value)
{
	struct cpcs_builtin_extended_exec_ctx *worker_ctx;

	worker_ctx = kv_ctx->worker_ctx;
	free(kv_ctx->owned_buf);
	free(kv_ctx);
	_cpcs_builtin_extended_complete(worker_ctx, status, return_value);
}

static void
_cpcs_builtin_kv_write_done(void *cb_arg, int status)
{
	struct cpcs_builtin_kv_async_ctx *kv_ctx = cb_arg;

	if (status != 0) {
		_cpcs_builtin_kv_async_finish(kv_ctx, cpcs_builtin_map_slm_status(status), 0);
		return;
	}

	_cpcs_builtin_kv_async_finish(kv_ctx, 0, kv_ctx->return_value);
}

static int
_cpcs_builtin_kv_exec_async(struct cpcs_builtin_extended_exec_ctx *worker_ctx)
{
	struct cpcs_builtin_kv_async_ctx *kv_ctx;
	uint32_t expected_op;
	int rc;

	if (worker_ctx == NULL || worker_ctx->prog == NULL) {
		return -EINVAL;
	}

	expected_op = _cpcs_builtin_kv_expected_op(worker_ctx->prog->pind);
	if (expected_op == 0) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}

	kv_ctx = calloc(1, sizeof(*kv_ctx));
	if (kv_ctx == NULL) {
		return -ENOMEM;
	}

	kv_ctx->worker_ctx = worker_ctx;
	rc = _cpcs_builtin_kv_prepare_result(worker_ctx->exec_ctx, worker_ctx->prog->pind,
					     kv_ctx);
	if (rc != 0) {
		free(kv_ctx);
		return rc;
	}

	if (kv_ctx->target.mr_id == 0 || kv_ctx->result_buf == NULL) {
		_cpcs_builtin_kv_async_finish(kv_ctx, 0, kv_ctx->return_value);
		return 0;
	}

	rc = _cpcs_exec_write_range_async(worker_ctx->exec_ctx, kv_ctx->target.mr_id,
					  kv_ctx->target.off, kv_ctx->result_len,
					  kv_ctx->result_buf, _cpcs_builtin_kv_write_done,
					  kv_ctx);
	if (rc != 0) {
		_cpcs_builtin_kv_async_finish(kv_ctx, rc, 0);
	}

	return 0;
}

/*
 * Ported from github/e2e_benchmark (originally PIND 5-8, inline-data-only
 * builtins). Renumbered to PIND 13-16 to avoid colliding with this branch's
 * FILTER_AGG/FILTERED_TOPK_EXACT/KV_* builtins. These are synchronous,
 * data_buffer-only computations, so they are dispatched through the
 * "extended" compute-thread path below rather than the SG/SLM async state
 * machine used by MEMCPY/MEMFILL/SUM64/MAX64/MIN64.
 */
/*
 * DOT_PRODUCT descriptor (same layout as sum64):
 *   [0:7]   mr_id  - Memory Range ID (1-based)
 *   [8:15]  offset - byte offset within MRS
 *   [16:23] length - total bytes (must be divisible by 2*sizeof(float))
 *
 * The first half of the data is vector A, the second half is vector B.
 * Result = A[0]*B[0] + A[1]*B[1] + ... + A[n-1]*B[n-1], returned as
 * a float bit-pattern in the lower 32 bits of return_value.
 */
static int
_builtin_execute_dot_product(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_sum64_desc *desc;
	const float *p;
	uint8_t *buf;
	uint64_t mr_id;
	uint64_t off;
	uint64_t processed = 0;
	uint64_t chunk;
	uint64_t len;
	uint64_t half_len;
	float sum = 0.0f;
	uint32_t sum_bits = 0;
	uint8_t *buf_a = NULL;
	size_t i;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0 || (len % (2 * sizeof(float))) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);
	half_len = len / 2;

	/* Read vector A (first half) into buf_a */
	buf_a = malloc(half_len);
	if (buf_a == NULL) {
		return -ENOMEM;
	}

	processed = 0;
	while (processed < half_len) {
		chunk = half_len - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
			chunk -= chunk % sizeof(float);
		}

		rc = _cpcs_exec_read_range_sync(ctx, mr_id, off + processed, chunk,
						buf_a + processed);
		if (rc != 0) {
			free(buf_a);
			return rc;
		}
		processed += chunk;
	}

	/* Stream vector B (second half) and accumulate dot product */
	buf = malloc(CPCS_BUILTIN_IO_CHUNK);
	if (buf == NULL) {
		free(buf_a);
		return -ENOMEM;
	}

	processed = 0;
	while (processed < half_len) {
		chunk = half_len - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
			chunk -= chunk % sizeof(float);
		}

		rc = _cpcs_exec_read_range_sync(ctx, mr_id, off + half_len + processed,
						chunk, buf);
		if (rc != 0) {
			free(buf);
			free(buf_a);
			return rc;
		}

		p = (const float *)buf;
		for (i = 0; i < (chunk / sizeof(float)); i++) {
			sum += ((const float *)buf_a)[processed / sizeof(float) + i] * p[i];
		}
		processed += chunk;
	}

	free(buf);
	free(buf_a);

	memcpy(&sum_bits, &sum, sizeof(sum_bits));
	*return_value = (uint64_t)sum_bits;
	return 0;
}

/*
 * Ported from github/e2e_benchmark 861c5a92b (originally PIND 9-11, direct-
 * data-only builtins). Renumbered to PIND 17-19 to avoid colliding with this
 * branch's KV_LAYOUT_REPACK/KV_BLOCK_SELECT/KV_PREFIX_LOOKUP. At this point in
 * the ported history these three ops only support the inline "direct data"
 * calling convention (RSID=0, NUMR=0, no MRS at all) -- MRS/SLM-backed input
 * is added later by e3d6b3848. Unlike DOT_PRODUCT, which b187526dc already
 * rewrote to be MRS-only, these three have no MRS path yet, so the
 * direct-data check is not optional here.
 */
static bool
_builtin_has_direct_data(const struct cpcs_exec_context *ctx)
{
	return ctx != NULL && ctx->resolved_range_count == 0;
}

static int
_builtin_execute_vector_float_direct(const struct cpcs_exec_context *ctx, uint16_t pind, uint64_t *return_value)
{
	const float *vals;
	size_t count;
	size_t half;
	float result = 0.0f;
	uint32_t bits = 0;
	size_t i;

	if (ctx == NULL || ctx->data_buffer == NULL || return_value == NULL) {
		return -EINVAL;
	}
	if (ctx->data_len == 0 || (ctx->data_len % (2 * sizeof(float))) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	vals = (const float *)ctx->data_buffer;
	count = ctx->data_len / sizeof(float);
	half = count / 2;

	if (pind == CPCS_BUILTIN_PIND_L2_DISTANCE_SQ) {
		for (i = 0; i < half; i++) {
			float diff = vals[i] - vals[i + half];
			result += diff * diff;
		}
	} else if (pind == CPCS_BUILTIN_PIND_COSINE_SIMILARITY) {
		float dot = 0.0f;
		float lhs_norm = 0.0f;
		float rhs_norm = 0.0f;

		for (i = 0; i < half; i++) {
			float lhs = vals[i];
			float rhs = vals[i + half];
			dot += lhs * rhs;
			lhs_norm += lhs * lhs;
			rhs_norm += rhs * rhs;
		}

		/*
		 * Guard against NaN: sqrtf(0) denominator with a zero dot product
		 * is 0/0, which bypasses the clamp below and returns NaN.
		 */
		float denom = sqrtf(lhs_norm * rhs_norm);
		if (denom == 0.0f) {
			result = 0.0f;
		} else {
			result = dot / denom;
			if (result < -1.0f) {
				result = -1.0f;
			} else if (result > 1.0f) {
				result = 1.0f;
			}
		}
	} else {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}

	memcpy(&bits, &result, sizeof(bits));
	*return_value = (uint64_t)bits;
	return 0;
}

static int
_builtin_execute_multi_agg64_direct(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	struct cpcs_builtin_multi_agg64_result result;
	const double *vals;
	uint8_t *out;
	size_t input_len;
	size_t count;
	size_t i;

	if (ctx == NULL || ctx->data_buffer == NULL || return_value == NULL) {
		return -EINVAL;
	}
	if (ctx->data_len <= sizeof(result)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	input_len = ctx->data_len - sizeof(result);
	if (input_len == 0 || (input_len % sizeof(double)) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	vals = (const double *)ctx->data_buffer;
	count = input_len / sizeof(double);
	result.count = count;
	result.sum = 0.0;
	result.min = vals[0];
	result.max = vals[0];

	for (i = 0; i < count; i++) {
		double value = vals[i];
		result.sum += value;
		if (value < result.min) {
			result.min = value;
		}
		if (value > result.max) {
			result.max = value;
		}
	}

	out = (uint8_t *)ctx->data_buffer + input_len;
	memcpy(out, &result, sizeof(result));
	*return_value = sizeof(result);
	return 0;
}

static int
_builtin_execute_multi_agg64(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	if (!_builtin_has_direct_data(ctx)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	return _builtin_execute_multi_agg64_direct(ctx, return_value);
}

static int
_builtin_execute_l2_distance_sq(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	if (!_builtin_has_direct_data(ctx)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	return _builtin_execute_vector_float_direct(ctx, CPCS_BUILTIN_PIND_L2_DISTANCE_SQ, return_value);
}

static int
_builtin_execute_cosine_similarity(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	if (!_builtin_has_direct_data(ctx)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	return _builtin_execute_vector_float_direct(ctx, CPCS_BUILTIN_PIND_COSINE_SIMILARITY, return_value);
}

/*
 * MRS-based FILTER_GT descriptor (32 bytes).
 * Sent inline by the host when data has been pre-staged into SLM.
 *   - mr_id/off/len identify the input range in SLM
 *   - threshold is the float comparison value (stored as raw bits)
 * The result (count of elements > threshold) is returned via cdw0.
 */
struct cpcs_builtin_filter_gt_desc {
	uint64_t mr_id;
	uint64_t off;
	uint64_t len;
	uint32_t threshold_bits;
	uint32_t _pad;
};

static int
_builtin_execute_filter_gt(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const uint8_t *buf;
	const float *in;
	float *out;
	float threshold = 0.0f;
	size_t n;
	size_t i;
	size_t out_count = 0;
	size_t bytes_after_header;

	/*
	 * MRS path: data resides in SLM, descriptor is inline.
	 * Stream the input from SLM in chunks and count elements > threshold.
	 * Returns count via cdw0; no filtered data is written back.
	 */
	if (!_builtin_has_direct_data(ctx)) {
		const struct cpcs_builtin_filter_gt_desc *desc;
		uint64_t mr_id;
		uint64_t off;
		uint64_t len;
		uint64_t processed = 0;
		uint64_t chunk;
		uint8_t *chunk_buf = NULL;
		uint32_t tbits;
		float thr;
		int rc;

		if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		desc = (const struct cpcs_builtin_filter_gt_desc *)ctx->data_buffer;
		len = from_le64(&desc->len);
		if (len == 0 || (len % sizeof(float)) != 0) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		mr_id = from_le64(&desc->mr_id);
		off = from_le64(&desc->off);
		tbits = from_le32(&desc->threshold_bits);
		memcpy(&thr, &tbits, sizeof(thr));

		chunk_buf = malloc(CPCS_BUILTIN_IO_CHUNK);
		if (chunk_buf == NULL) {
			return -ENOMEM;
		}

		while (processed < len) {
			chunk = len - processed;
			if (chunk > CPCS_BUILTIN_IO_CHUNK) {
				chunk = CPCS_BUILTIN_IO_CHUNK;
				chunk -= chunk % sizeof(float);
			}

			rc = _cpcs_exec_read_range_sync(ctx, mr_id, off + processed, chunk,
							chunk_buf);
			if (rc != 0) {
				free(chunk_buf);
				return rc;
			}

			{
				const float *p = (const float *)chunk_buf;
				size_t nf = chunk / sizeof(float);
				size_t j;

				for (j = 0; j < nf; j++) {
					if (p[j] > thr) {
						out_count++;
					}
				}
			}
			processed += chunk;
		}

		free(chunk_buf);
		*return_value = out_count;
		return 0;
	}

	if (ctx->data_buffer == NULL || ctx->data_len < (4 + 2 * sizeof(float))) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	buf = (const uint8_t *)ctx->data_buffer;
	memcpy(&threshold, buf, sizeof(threshold));

	bytes_after_header = ctx->data_len - 4;
	if ((bytes_after_header % (2 * sizeof(float))) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	n = bytes_after_header / (2 * sizeof(float));
	in = (const float *)(buf + 4);
	out = (float *)((uint8_t *)ctx->data_buffer + 4 + n * sizeof(float));

	for (i = 0; i < n; i++) {
		if (in[i] > threshold) {
			out[out_count++] = in[i];
		}
	}

	*return_value = out_count;
	return 0;
}

static int
_builtin_execute_memcpy_inline(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	uint8_t *buf = (uint8_t *)ctx->data_buffer;
	size_t n;

	if (buf == NULL || ctx->data_len < 2 || (ctx->data_len % 2) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	n = ctx->data_len / 2;
	memcpy(buf + n, buf, n);
	*return_value = n;
	return 0;
}

static int
_builtin_execute_rle_compress(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	uint8_t *buf = (uint8_t *)ctx->data_buffer;
	uint8_t *src;
	uint8_t *dst;
	uint64_t input_size = 0;
	size_t out_capacity;
	size_t out_pos = 0;
	size_t i = 0;

	if (buf == NULL || ctx->data_len < (8 + 1 + 1)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	memcpy(&input_size, buf, sizeof(input_size));
	if (input_size == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (ctx->data_len < 8 + input_size + 1) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	src = buf + 8;
	dst = buf + 8 + input_size;
	out_capacity = ctx->data_len - 8 - input_size;

	while (i < input_size) {
		uint8_t value = src[i];
		uint8_t run = 1;

		while ((i + run) < input_size && src[i + run] == value && run < 255) {
			run++;
		}

		if (out_pos + 2 > out_capacity) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		dst[out_pos++] = run;
		dst[out_pos++] = value;
		i += run;
	}

	*return_value = out_pos;
	return 0;
}

/*
 * DIRECT_NS_AGG reads straight from a CSD-local NVMe namespace (identified
 * by nsid in the descriptor) rather than through an MRS/SLM range, so it
 * bypasses _cpcs_exec_read_range_sync entirely and calls bdev_slm_read_by_bdev
 * directly against the resolved bdev. Reuses the existing nsid->bdev lookup
 * helper (_cpcs_find_bdev_by_nsid) rather than duplicating it.
 */
static int
_builtin_execute_direct_ns_agg(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cs_direct_ns_desc *desc;
	struct cs_direct_ns_result result;
	struct spdk_bdev *bdev;
	const uint64_t *p;
	uint8_t *buf;
	uint64_t offset;
	uint64_t total_bytes;
	uint64_t processed = 0;
	uint64_t chunk;
	uint64_t agg = 0;
	uint64_t count = 0;
	bool initialized = false;
	size_t i;
	size_t n;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc) + sizeof(result)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cs_direct_ns_desc *)ctx->data_buffer;

	SPDK_NOTICELOG("DIRECT_NS_AGG entry: nsid=%u n_uint64=%u lba_offset=%" PRIu64 " workload=%u data_len=%u\n",
		       desc->nsid, desc->n_uint64, desc->lba_offset, desc->workload, ctx->data_len);

	if (desc->n_uint64 == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	bdev = _cpcs_find_bdev_by_nsid(desc->nsid);
	if (bdev == NULL) {
		SPDK_ERRLOG("DIRECT_NS_AGG: bdev not found for nsid=%u\n", desc->nsid);
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}

	SPDK_NOTICELOG("DIRECT_NS_AGG: using bdev=%s for nsid=%u\n",
		       spdk_bdev_get_name(bdev), desc->nsid);

	total_bytes = (uint64_t)desc->n_uint64 * sizeof(uint64_t);
	offset = desc->lba_offset;

	/*
	 * DOT_PRODUCT needs both vector halves in memory at once, so read
	 * the entire buffer up-front. For other workloads the chunked
	 * streaming path below is still used (lower peak memory).
	 */
	if (desc->workload == CS_DIRECT_NS_WORKLOAD_DOT_PRODUCT) {
		uint8_t *full_buf;
		const float *fvals;
		double dp = 0.0;
		size_t half;

		full_buf = spdk_dma_malloc(total_bytes, 4096, NULL);
		if (full_buf == NULL) {
			return -ENOMEM;
		}

		processed = 0;
		while (processed < total_bytes) {
			chunk = total_bytes - processed;
			if (chunk > CPCS_BUILTIN_IO_CHUNK) {
				chunk = CPCS_BUILTIN_IO_CHUNK;
			}
			rc = bdev_slm_read_by_bdev(bdev, offset + processed, chunk, full_buf + processed);
			if (rc != 0) {
				SPDK_ERRLOG("DIRECT_NS_AGG: bdev_slm_read_by_bdev failed nsid=%u offset=%" PRIu64 " chunk=%" PRIu64 " rc=%d\n",
					    desc->nsid, offset + processed, chunk, rc);
				spdk_dma_free(full_buf);
				if (rc == -ENOENT || rc == -ENOTSUP) {
					return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
				}
				return rc;
			}
			processed += chunk;
		}

		/* Layout: [a0..a_{dim-1}, b0..b_{dim-1}] as float32 */
		fvals = (const float *)full_buf;
		half = total_bytes / (2 * sizeof(float));
		for (i = 0; i < half; i++) {
			dp += (double)fvals[i] * (double)fvals[i + half];
		}

		spdk_dma_free(full_buf);

		/* Pack the full double bit-pattern into the uint64_t result field. */
		memcpy(&agg, &dp, sizeof(double));
		count = half;

		goto write_result;
	}

	/* DMA-safe: bdev_slm_read_by_bdev() may issue real backing-device I/O. */
	buf = spdk_dma_malloc(CPCS_BUILTIN_IO_CHUNK, 4096, NULL);
	if (buf == NULL) {
		return -ENOMEM;
	}

	while (processed < total_bytes) {
		chunk = total_bytes - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
		}
		chunk -= chunk % sizeof(uint64_t);

		rc = bdev_slm_read_by_bdev(bdev, offset + processed, chunk, buf);
		if (rc != 0) {
			SPDK_ERRLOG("DIRECT_NS_AGG: bdev_slm_read_by_bdev failed nsid=%u offset=%" PRIu64 " chunk=%" PRIu64 " rc=%d\n",
				    desc->nsid, offset + processed, chunk, rc);
			spdk_dma_free(buf);
			if (rc == -ENOENT || rc == -ENOTSUP) {
				return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
			}
			if (rc == -EINVAL) {
				return -SPDK_NVME_SC_INVALID_FIELD;
			}
			return rc;
		}

		p = (const uint64_t *)buf;
		n = chunk / sizeof(uint64_t);

		switch (desc->workload) {
		case CS_DIRECT_NS_WORKLOAD_SUM:
			for (i = 0; i < n; i++) {
				agg += p[i];
			}
			break;
		case CS_DIRECT_NS_WORKLOAD_MAX:
			for (i = 0; i < n; i++) {
				if (!initialized || p[i] > agg) {
					agg = p[i];
					initialized = true;
				}
			}
			break;
		case CS_DIRECT_NS_WORKLOAD_MIN:
			for (i = 0; i < n; i++) {
				if (!initialized || p[i] < agg) {
					agg = p[i];
					initialized = true;
				}
			}
			break;
		case CS_DIRECT_NS_WORKLOAD_FILTER_GT:
			/* Threshold = 0: count all non-zero elements */
			for (i = 0; i < n; i++) {
				if (p[i] > 0) {
					agg++;
				}
			}
			break;
		default:
			spdk_dma_free(buf);
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		count += n;
		processed += chunk;
	}

	spdk_dma_free(buf);

write_result:
	result.result = agg;
	result.count  = count;

	SPDK_NOTICELOG("DIRECT_NS_AGG complete: result=%" PRIu64 " count=%" PRIu64 "\n",
		       result.result, result.count);

	memcpy((uint8_t *)ctx->data_buffer + sizeof(*desc), &result, sizeof(result));
	*return_value = sizeof(result);
	return 0;
}

static void
_cpcs_builtin_extended_msg(void *arg)
{
	struct cpcs_builtin_extended_exec_ctx *ctx = arg;
	uint64_t return_value = 0;
	int status;

	if (_cpcs_builtin_kv_expected_op(ctx->prog->pind) != 0) {
		status = _cpcs_builtin_kv_exec_async(ctx);
		if (status == 0) {
			return;
		}
		_cpcs_builtin_extended_complete(ctx, status, 0);
		return;
	} else {
		switch (ctx->prog->pind) {
		case CPCS_BUILTIN_PIND_FILTER_AGG:
			status = _cpcs_builtin_exec_filter_agg_async(ctx);
			if (status == 0) {
				return;
			}
			_cpcs_builtin_extended_complete(ctx, status, 0);
			return;
		case CPCS_BUILTIN_PIND_FILTERED_TOPK_EXACT:
			status = _cpcs_builtin_exec_filtered_topk_exact_async(ctx);
			if (status == 0) {
				return;
			}
			_cpcs_builtin_extended_complete(ctx, status, 0);
			return;
		case CPCS_BUILTIN_PIND_DOT_PRODUCT:
			status = _builtin_execute_dot_product(ctx->exec_ctx, &return_value);
			break;
		case CPCS_BUILTIN_PIND_FILTER_GT:
			status = _builtin_execute_filter_gt(ctx->exec_ctx, &return_value);
			break;
		case CPCS_BUILTIN_PIND_MEMCPY_INLINE:
			status = _builtin_execute_memcpy_inline(ctx->exec_ctx, &return_value);
			break;
		case CPCS_BUILTIN_PIND_RLE_COMPRESS:
			status = _builtin_execute_rle_compress(ctx->exec_ctx, &return_value);
			break;
		case CPCS_BUILTIN_PIND_MULTI_AGG64:
			status = _builtin_execute_multi_agg64(ctx->exec_ctx, &return_value);
			break;
		case CPCS_BUILTIN_PIND_L2_DISTANCE_SQ:
			status = _builtin_execute_l2_distance_sq(ctx->exec_ctx, &return_value);
			break;
		case CPCS_BUILTIN_PIND_COSINE_SIMILARITY:
			status = _builtin_execute_cosine_similarity(ctx->exec_ctx, &return_value);
			break;
		case CPCS_BUILTIN_PIND_DIRECT_NS_AGG:
			status = _builtin_execute_direct_ns_agg(ctx->exec_ctx, &return_value);
			break;
		default:
			status = -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
			break;
		}
	}

	_cpcs_builtin_extended_complete(ctx, status, return_value);
}

static int
_cpcs_builtin_filter_agg_apply_record(const struct spdk_cpcs_builtin_filter_agg_req *req,
				      const struct spdk_cpcs_builtin_eval_filter_clause *clauses,
				      const struct cpcs_builtin_metadata_record *meta,
				      struct spdk_cpcs_builtin_filter_agg_result *result,
				      uint64_t *aggregate_u64, double *aggregate_f64,
				      bool *aggregate_initialized)
{
	uint32_t field_u32;
	float field_f32;
	bool matched;
	int rc;

	rc = _cpcs_builtin_filter_match_all(meta, clauses, from_le16(&req->filter_count), &matched);
	if (rc != 0) {
		return rc;
	}
	if (!matched) {
		return 0;
	}

	result->hdr.matched_count++;
	switch (req->agg_op) {
	case SPDK_CPCS_BUILTIN_AGG_COUNT:
		*aggregate_u64 = result->hdr.matched_count;
		break;
	case SPDK_CPCS_BUILTIN_AGG_SUM:
		if (_cpcs_builtin_field_is_float(req->agg_field_id)) {
			rc = _cpcs_builtin_meta_get_f32(meta, req->agg_field_id, &field_f32);
			if (rc != 0) {
				return rc;
			}
			*aggregate_f64 += field_f32;
		} else {
			rc = _cpcs_builtin_meta_get_u32(meta, req->agg_field_id, &field_u32);
			if (rc != 0) {
				return rc;
			}
			*aggregate_u64 += field_u32;
		}
		break;
	case SPDK_CPCS_BUILTIN_AGG_MIN:
	case SPDK_CPCS_BUILTIN_AGG_MAX:
		if (_cpcs_builtin_field_is_float(req->agg_field_id)) {
			rc = _cpcs_builtin_meta_get_f32(meta, req->agg_field_id, &field_f32);
			if (rc != 0) {
				return rc;
			}
			if (!*aggregate_initialized ||
			    (req->agg_op == SPDK_CPCS_BUILTIN_AGG_MIN && field_f32 < *aggregate_f64) ||
			    (req->agg_op == SPDK_CPCS_BUILTIN_AGG_MAX && field_f32 > *aggregate_f64)) {
				*aggregate_f64 = field_f32;
				*aggregate_initialized = true;
			}
		} else {
			rc = _cpcs_builtin_meta_get_u32(meta, req->agg_field_id, &field_u32);
			if (rc != 0) {
				return rc;
			}
			if (!*aggregate_initialized ||
			    (req->agg_op == SPDK_CPCS_BUILTIN_AGG_MIN && field_u32 < *aggregate_u64) ||
			    (req->agg_op == SPDK_CPCS_BUILTIN_AGG_MAX && field_u32 > *aggregate_u64)) {
				*aggregate_u64 = field_u32;
				*aggregate_initialized = true;
			}
		}
		break;
	default:
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	return 0;
}

static void _cpcs_builtin_filter_agg_async_step(struct cpcs_builtin_filter_agg_async_ctx
		*async_ctx);

static void
_cpcs_builtin_filter_agg_async_finish(struct cpcs_builtin_filter_agg_async_ctx *async_ctx,
				      int status, uint64_t return_value)
{
	struct cpcs_builtin_extended_exec_ctx *worker_ctx;

	worker_ctx = async_ctx->worker_ctx;
	free(async_ctx);
	_cpcs_builtin_extended_complete(worker_ctx, status, return_value);
}

static void
_cpcs_builtin_filter_agg_write_done(void *cb_arg, int status)
{
	struct cpcs_builtin_filter_agg_async_ctx *async_ctx = cb_arg;

	if (status != 0) {
		_cpcs_builtin_filter_agg_async_finish(async_ctx,
						      cpcs_builtin_map_slm_status(status), 0);
		return;
	}

	_cpcs_builtin_filter_agg_async_finish(async_ctx, 0,
					      async_ctx->result.hdr.matched_count);
}

static void
_cpcs_builtin_filter_agg_read_done(void *cb_arg, int status)
{
	struct cpcs_builtin_filter_agg_async_ctx *async_ctx = cb_arg;
	int rc;

	if (status != 0) {
		_cpcs_builtin_filter_agg_async_finish(async_ctx,
						      cpcs_builtin_map_slm_status(status), 0);
		return;
	}

	async_ctx->result.hdr.stats.media_read_bytes += sizeof(async_ctx->meta);
	async_ctx->result.hdr.stats.metadata_records_scanned++;

	rc = _cpcs_builtin_filter_agg_apply_record(async_ctx->req, async_ctx->clauses,
			&async_ctx->meta, &async_ctx->result, &async_ctx->aggregate_u64,
			&async_ctx->aggregate_f64, &async_ctx->aggregate_initialized);
	if (rc != 0) {
		_cpcs_builtin_filter_agg_async_finish(async_ctx, rc, 0);
		return;
	}

	async_ctx->record_idx++;
	_cpcs_builtin_filter_agg_async_step(async_ctx);
}

static void
_cpcs_builtin_filter_agg_async_step(struct cpcs_builtin_filter_agg_async_ctx *async_ctx)
{
	uint64_t record_count;
	uint64_t metadata_off;
	int rc;

	record_count = from_le64(&async_ctx->req->record_count);
	if (async_ctx->record_idx < record_count) {
		metadata_off = from_le64(&async_ctx->req->metadata_offset) +
			       (async_ctx->record_idx * from_le32(&async_ctx->req->metadata_stride));
		rc = _cpcs_exec_read_range_async(async_ctx->exec_ctx,
						 from_le64(&async_ctx->req->metadata_mr_id),
						 metadata_off, sizeof(async_ctx->meta),
						 &async_ctx->meta,
						 _cpcs_builtin_filter_agg_read_done, async_ctx);
		if (rc != 0) {
			_cpcs_builtin_filter_agg_async_finish(async_ctx, rc, 0);
		}
		return;
	}

	async_ctx->result.aggregate_u64 = async_ctx->aggregate_u64;
	async_ctx->result.aggregate_f64 = async_ctx->aggregate_f64;
	async_ctx->result.hdr.returned_count = 1;
	async_ctx->result.hdr.total_length = sizeof(async_ctx->result);
	if (async_ctx->ticks_hz != 0) {
		async_ctx->result.hdr.stats.latency_us =
			((spdk_get_ticks() - async_ctx->start_ticks) * 1000000ULL) /
			async_ctx->ticks_hz;
	}
	async_ctx->result.hdr.stats.media_write_bytes += sizeof(async_ctx->result);

	rc = _cpcs_builtin_write_output_async(async_ctx->exec_ctx, &async_ctx->req->hdr,
					      &async_ctx->result, sizeof(async_ctx->result),
					      _cpcs_builtin_filter_agg_write_done, async_ctx);
	if (rc != 0) {
		_cpcs_builtin_filter_agg_async_finish(async_ctx, rc, 0);
	}
}

static int
_cpcs_builtin_exec_filter_agg_async(struct cpcs_builtin_extended_exec_ctx *worker_ctx)
{
	struct cpcs_builtin_filter_agg_async_ctx *async_ctx;
	const struct cpcs_exec_context *ctx;
	const struct spdk_cpcs_builtin_filter_agg_req *req;
	const struct spdk_cpcs_builtin_eval_filter_clause *clauses;
	size_t required;

	if (worker_ctx == NULL || worker_ctx->exec_ctx == NULL ||
	    worker_ctx->exec_ctx->data_buffer == NULL) {
		return -EINVAL;
	}

	ctx = worker_ctx->exec_ctx;
	if (ctx->data_len < sizeof(*req)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	req = (const struct spdk_cpcs_builtin_filter_agg_req *)ctx->data_buffer;
	if (from_le32(&req->hdr.version) != SPDK_CPCS_BUILTIN_EVAL_ABI_VERSION ||
	    from_le16(&req->hdr.opcode) != SPDK_CPCS_BUILTIN_OP_FILTER_AGG) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (from_le32(&req->metadata_stride) < sizeof(struct cpcs_builtin_metadata_record)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	required = sizeof(*req) + ((size_t)from_le16(&req->filter_count) * sizeof(*clauses));
	if (ctx->data_len < required) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	clauses = (const struct spdk_cpcs_builtin_eval_filter_clause *)(req + 1);
	async_ctx = calloc(1, sizeof(*async_ctx));
	if (async_ctx == NULL) {
		return -ENOMEM;
	}

	async_ctx->worker_ctx = worker_ctx;
	async_ctx->exec_ctx = ctx;
	async_ctx->req = req;
	async_ctx->clauses = clauses;
	async_ctx->start_ticks = spdk_get_ticks();
	async_ctx->ticks_hz = spdk_get_ticks_hz();
	_cpcs_builtin_fill_common_stats(ctx, &async_ctx->result.hdr.stats);
	async_ctx->result.hdr.version = SPDK_CPCS_BUILTIN_EVAL_ABI_VERSION;
	async_ctx->result.hdr.opcode = SPDK_CPCS_BUILTIN_OP_FILTER_AGG;
	async_ctx->result.hdr.status = SPDK_NVME_SC_SUCCESS;
	async_ctx->result.agg_op = req->agg_op;
	async_ctx->result.agg_field_id = req->agg_field_id;

	_cpcs_builtin_filter_agg_async_step(async_ctx);
	return 0;
}

static void _cpcs_builtin_topk_async_step(struct cpcs_builtin_topk_async_ctx *async_ctx);

static void
_cpcs_builtin_topk_async_finish(struct cpcs_builtin_topk_async_ctx *async_ctx,
				int status, uint64_t return_value)
{
	struct cpcs_builtin_extended_exec_ctx *worker_ctx;

	worker_ctx = async_ctx->worker_ctx;
	free(async_ctx->entries);
	free(async_ctx->vector_buf);
	free(async_ctx->out_buf);
	free(async_ctx);
	_cpcs_builtin_extended_complete(worker_ctx, status, return_value);
}

static int
_cpcs_builtin_topk_score_current(struct cpcs_builtin_topk_async_ctx *async_ctx)
{
	uint32_t worst_idx;
	uint64_t doc_id;
	float score;

	switch (from_le16(&async_ctx->req->metric)) {
	case SPDK_CPCS_BUILTIN_METRIC_COSINE:
		score = _cpcs_builtin_score_cosine(async_ctx->query_vec, async_ctx->vector_buf,
						   async_ctx->dim, async_ctx->query_normalized);
		break;
	case SPDK_CPCS_BUILTIN_METRIC_L2:
		score = _cpcs_builtin_score_l2(async_ctx->query_vec, async_ctx->vector_buf,
					       async_ctx->dim, false);
		break;
	case SPDK_CPCS_BUILTIN_METRIC_L2_SQUARED:
		score = _cpcs_builtin_score_l2(async_ctx->query_vec, async_ctx->vector_buf,
					       async_ctx->dim, true);
		break;
	default:
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	doc_id = from_le64(&async_ctx->meta.doc_id);
	if (async_ctx->entries_count < async_ctx->k) {
		async_ctx->entries[async_ctx->entries_count].doc_id = doc_id;
		async_ctx->entries[async_ctx->entries_count].score = score;
		async_ctx->entries_count++;
		return 0;
	}

	worst_idx = _cpcs_builtin_topk_find_worst(async_ctx->entries, async_ctx->entries_count);
	if (_cpcs_builtin_topk_better(score, doc_id, async_ctx->entries[worst_idx].score,
				      async_ctx->entries[worst_idx].doc_id)) {
		async_ctx->entries[worst_idx].doc_id = doc_id;
		async_ctx->entries[worst_idx].score = score;
	}

	return 0;
}

static void
_cpcs_builtin_topk_write_done(void *cb_arg, int status)
{
	struct cpcs_builtin_topk_async_ctx *async_ctx = cb_arg;

	if (status != 0) {
		_cpcs_builtin_topk_async_finish(async_ctx, cpcs_builtin_map_slm_status(status), 0);
		return;
	}

	_cpcs_builtin_topk_async_finish(async_ctx, 0, async_ctx->entries_count);
}

static void
_cpcs_builtin_topk_vector_read_done(void *cb_arg, int status)
{
	struct cpcs_builtin_topk_async_ctx *async_ctx = cb_arg;
	int rc;

	if (status != 0) {
		_cpcs_builtin_topk_async_finish(async_ctx, cpcs_builtin_map_slm_status(status), 0);
		return;
	}

	async_ctx->out->hdr.stats.media_read_bytes += async_ctx->dim * sizeof(float);
	async_ctx->out->hdr.stats.vectors_scored++;
	async_ctx->out->hdr.scored_count++;

	rc = _cpcs_builtin_topk_score_current(async_ctx);
	if (rc != 0) {
		_cpcs_builtin_topk_async_finish(async_ctx, rc, 0);
		return;
	}

	async_ctx->record_idx++;
	_cpcs_builtin_topk_async_step(async_ctx);
}

static void
_cpcs_builtin_topk_metadata_read_done(void *cb_arg, int status)
{
	struct cpcs_builtin_topk_async_ctx *async_ctx = cb_arg;
	uint64_t vector_index;
	uint64_t vector_off;
	bool matched;
	int rc;

	if (status != 0) {
		_cpcs_builtin_topk_async_finish(async_ctx, cpcs_builtin_map_slm_status(status), 0);
		return;
	}

	async_ctx->out->hdr.stats.media_read_bytes += sizeof(async_ctx->meta);
	async_ctx->out->hdr.stats.metadata_records_scanned++;

	rc = _cpcs_builtin_filter_match_all(&async_ctx->meta, async_ctx->clauses,
					    async_ctx->filter_count, &matched);
	if (rc != 0) {
		_cpcs_builtin_topk_async_finish(async_ctx, rc, 0);
		return;
	}
	if (!matched) {
		async_ctx->record_idx++;
		_cpcs_builtin_topk_async_step(async_ctx);
		return;
	}

	async_ctx->out->hdr.matched_count++;
	vector_index = from_le32(&async_ctx->meta.vector_index);
	vector_off = from_le64(&async_ctx->req->vector_offset) +
		     (vector_index * from_le32(&async_ctx->req->vector_stride));
	rc = _cpcs_exec_read_range_async(async_ctx->exec_ctx,
					 from_le64(&async_ctx->req->vector_mr_id),
					 vector_off, async_ctx->dim * sizeof(float),
					 async_ctx->vector_buf,
					 _cpcs_builtin_topk_vector_read_done, async_ctx);
	if (rc != 0) {
		_cpcs_builtin_topk_async_finish(async_ctx, rc, 0);
	}
}

static void
_cpcs_builtin_topk_async_step(struct cpcs_builtin_topk_async_ctx *async_ctx)
{
	uint64_t record_count;
	uint64_t metadata_off;
	uint64_t i;
	int rc;

	record_count = from_le64(&async_ctx->req->record_count);
	if (async_ctx->record_idx < record_count) {
		metadata_off = from_le64(&async_ctx->req->metadata_offset) +
			       (async_ctx->record_idx *
				from_le32(&async_ctx->req->metadata_stride));
		rc = _cpcs_exec_read_range_async(async_ctx->exec_ctx,
						 from_le64(&async_ctx->req->metadata_mr_id),
						 metadata_off, sizeof(async_ctx->meta),
						 &async_ctx->meta,
						 _cpcs_builtin_topk_metadata_read_done,
						 async_ctx);
		if (rc != 0) {
			_cpcs_builtin_topk_async_finish(async_ctx, rc, 0);
		}
		return;
	}

	qsort(async_ctx->entries, async_ctx->entries_count, sizeof(*async_ctx->entries),
	      _cpcs_builtin_topk_cmp_desc);
	for (i = 0; i < async_ctx->entries_count; i++) {
		async_ctx->out->records[i].doc_id = async_ctx->entries[i].doc_id;
		async_ctx->out->records[i].score = async_ctx->entries[i].score;
	}
	async_ctx->out->hdr.returned_count = async_ctx->entries_count;
	async_ctx->out->hdr.total_length = sizeof(*async_ctx->out) +
					   (async_ctx->entries_count *
					    sizeof(struct spdk_cpcs_builtin_topk_record));
	if (async_ctx->ticks_hz != 0) {
		async_ctx->out->hdr.stats.latency_us =
			((spdk_get_ticks() - async_ctx->start_ticks) * 1000000ULL) /
			async_ctx->ticks_hz;
	}
	async_ctx->out->hdr.stats.media_write_bytes += async_ctx->out->hdr.total_length;

	rc = _cpcs_builtin_write_output_async(async_ctx->exec_ctx, &async_ctx->req->hdr,
					      async_ctx->out_buf,
					      async_ctx->out->hdr.total_length,
					      _cpcs_builtin_topk_write_done, async_ctx);
	if (rc != 0) {
		_cpcs_builtin_topk_async_finish(async_ctx, rc, 0);
	}
}

static int
_cpcs_builtin_exec_filtered_topk_exact_async(struct cpcs_builtin_extended_exec_ctx *worker_ctx)
{
	struct cpcs_builtin_topk_async_ctx *async_ctx;
	const struct cpcs_exec_context *ctx;
	const struct spdk_cpcs_builtin_filtered_topk_exact_req *req;
	const uint8_t *payload;
	uint32_t output_len;
	uint32_t dim;
	uint16_t filter_count;
	uint16_t k;
	size_t required;

	if (worker_ctx == NULL || worker_ctx->exec_ctx == NULL ||
	    worker_ctx->exec_ctx->data_buffer == NULL) {
		return -EINVAL;
	}

	ctx = worker_ctx->exec_ctx;
	if (ctx->data_len < sizeof(*req)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	req = (const struct spdk_cpcs_builtin_filtered_topk_exact_req *)ctx->data_buffer;
	if (from_le32(&req->hdr.version) != SPDK_CPCS_BUILTIN_EVAL_ABI_VERSION ||
	    from_le16(&req->hdr.opcode) != SPDK_CPCS_BUILTIN_OP_FILTERED_TOPK_EXACT) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (from_le32(&req->metadata_stride) < sizeof(struct cpcs_builtin_metadata_record)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	dim = from_le32(&req->vector_dim);
	if (dim == 0 || dim > (1u << 20)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (from_le32(&req->vector_stride) < dim * sizeof(float)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	k = from_le16(&req->k);
	if (k == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	filter_count = from_le16(&req->filter_count);
	required = sizeof(*req) +
		   ((size_t)filter_count * sizeof(struct spdk_cpcs_builtin_eval_filter_clause));
	required += from_le32(&req->query_vector_bytes);
	if (ctx->data_len < required ||
	    from_le32(&req->query_vector_bytes) != dim * sizeof(float)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	async_ctx = calloc(1, sizeof(*async_ctx));
	if (async_ctx == NULL) {
		return -ENOMEM;
	}
	async_ctx->worker_ctx = worker_ctx;

	async_ctx->entries = calloc(k, sizeof(*async_ctx->entries));
	async_ctx->vector_buf = malloc(dim * sizeof(float));
	output_len = sizeof(*async_ctx->out) +
		     ((uint32_t)k * sizeof(struct spdk_cpcs_builtin_topk_record));
	async_ctx->out_buf = calloc(1, output_len);
	if (async_ctx->entries == NULL || async_ctx->vector_buf == NULL ||
	    async_ctx->out_buf == NULL) {
		free(async_ctx->entries);
		free(async_ctx->vector_buf);
		free(async_ctx->out_buf);
		free(async_ctx);
		return -ENOMEM;
	}

	payload = (const uint8_t *)(req + 1);
	async_ctx->exec_ctx = ctx;
	async_ctx->req = req;
	async_ctx->clauses = (const struct spdk_cpcs_builtin_eval_filter_clause *)payload;
	async_ctx->query_vec = (const float *)(payload + ((size_t)filter_count *
					       sizeof(struct spdk_cpcs_builtin_eval_filter_clause)));
	async_ctx->dim = dim;
	async_ctx->filter_count = filter_count;
	async_ctx->k = k;
	async_ctx->start_ticks = spdk_get_ticks();
	async_ctx->ticks_hz = spdk_get_ticks_hz();
	async_ctx->query_normalized =
		(from_le16(&req->hdr.flags) & SPDK_CPCS_BUILTIN_FLAG_QUERY_NORMALIZED) != 0;
	async_ctx->out = (struct spdk_cpcs_builtin_filtered_topk_exact_result *)
			 async_ctx->out_buf;
	_cpcs_builtin_fill_common_stats(ctx, &async_ctx->out->hdr.stats);
	async_ctx->out->hdr.version = SPDK_CPCS_BUILTIN_EVAL_ABI_VERSION;
	async_ctx->out->hdr.opcode = SPDK_CPCS_BUILTIN_OP_FILTERED_TOPK_EXACT;
	async_ctx->out->metric = from_le16(&req->metric);
	async_ctx->out->k = k;
	async_ctx->out->hdr.status = SPDK_NVME_SC_SUCCESS;

	_cpcs_builtin_topk_async_step(async_ctx);
	return 0;
}

static int
_cpcs_builtin_execute_extended(struct cpcs_program *prog, struct cpcs_exec_context *ctx,
			       cpcs_runtime_execute_done_cb done_cb, void *cb_arg)
{
	struct cpcs_builtin_extended_exec_ctx *worker_ctx;
	struct spdk_thread *submit_thread;
	struct spdk_thread *thread;
	int rc;

	worker_ctx = calloc(1, sizeof(*worker_ctx));
	if (worker_ctx == NULL) {
		return -ENOMEM;
	}

	worker_ctx->prog = prog;
	worker_ctx->exec_ctx = ctx;
	worker_ctx->done_cb = done_cb;
	worker_ctx->done_arg = cb_arg;
	submit_thread = spdk_get_thread();
	if (submit_thread == NULL) {
		free(worker_ctx);
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}
	worker_ctx->submit_thread = submit_thread;

	thread = _cpcs_builtin_select_compute_thread();
	if (thread == NULL) {
		thread = submit_thread;
	}

	rc = spdk_thread_send_msg(thread, _cpcs_builtin_extended_msg, worker_ctx);
	if (rc != 0) {
		free(worker_ctx);
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	return 0;
}

static void builtin_async_finish(struct cpcs_builtin_async_exec_ctx *ctx, int status);
static int builtin_async_step(struct cpcs_builtin_async_exec_ctx *ctx);

static void
builtin_async_continue_msg(void *arg)
{
	struct cpcs_builtin_async_exec_ctx *ctx = arg;
	int rc;

	rc = builtin_async_step(ctx);
	if (rc != 0) {
		builtin_async_finish(ctx, rc);
	}
}

static void
builtin_async_schedule_continue(struct cpcs_builtin_async_exec_ctx *ctx)
{
	struct spdk_thread *thread;
	int rc;

	thread = spdk_get_thread();
	if (thread == NULL) {
		rc = builtin_async_step(ctx);
		if (rc != 0) {
			builtin_async_finish(ctx, rc);
		}
		return;
	}

	rc = spdk_thread_send_msg(thread, builtin_async_continue_msg, ctx);
	if (rc != 0) {
		builtin_async_finish(ctx, rc);
	}
}

static void
builtin_async_finish(struct cpcs_builtin_async_exec_ctx *ctx, int status)
{
	cpcs_runtime_execute_done_cb done_cb = ctx->done_cb;
	void *done_arg = ctx->done_arg;
	uint64_t return_value = ctx->return_value;

	free(ctx->buf);
	free(ctx);
	done_cb(done_arg, status, return_value);
}

static void
builtin_memcpy_write_done(void *cb_arg, int status)
{
	struct cpcs_builtin_async_exec_ctx *ctx = cb_arg;

	status = cpcs_builtin_map_slm_status(status);
	if (status != 0) {
		builtin_async_finish(ctx, status);
		return;
	}

	ctx->processed += ctx->chunk_len;
	builtin_async_schedule_continue(ctx);
}

static void
builtin_memcpy_read_done(void *cb_arg, int status)
{
	struct cpcs_builtin_async_exec_ctx *ctx = cb_arg;
	int rc;

	status = cpcs_builtin_map_slm_status(status);
	if (status != 0) {
		builtin_async_finish(ctx, status);
		return;
	}

	rc = _cpcs_exec_write_range_async(ctx->exec_ctx, ctx->dst_mr_id,
					  ctx->dst_off + ctx->processed,
					  ctx->chunk_len, ctx->buf,
					  builtin_memcpy_write_done, ctx);
	if (rc != 0) {
		builtin_async_finish(ctx, rc);
	}
}

static void
builtin_memfill_write_done(void *cb_arg, int status)
{
	struct cpcs_builtin_async_exec_ctx *ctx = cb_arg;

	status = cpcs_builtin_map_slm_status(status);
	if (status != 0) {
		builtin_async_finish(ctx, status);
		return;
	}

	ctx->processed += ctx->chunk_len;
	builtin_async_schedule_continue(ctx);
}

static int
_cpcs_builtin_reduce_apply_sg(struct cpcs_builtin_async_exec_ctx *ctx,
			      const struct spdk_bdev_slm_sg_entry *entries,
			      uint32_t entry_count)
{
	const uint64_t *p;
	uint32_t i;
	size_t j;

	if (ctx == NULL || entries == NULL) {
		return -EINVAL;
	}

	for (i = 0; i < entry_count; i++) {
		if ((entries[i].len % sizeof(uint64_t)) != 0) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}
		p = (const uint64_t *)entries[i].addr;

		switch (ctx->pind) {
		case CPCS_BUILTIN_PIND_SUM64:
			for (j = 0; j < (entries[i].len / sizeof(uint64_t)); j++) {
				ctx->return_value += p[j];
			}
			break;
		case CPCS_BUILTIN_PIND_MAX64:
			for (j = 0; j < (entries[i].len / sizeof(uint64_t)); j++) {
				if (!ctx->initialized || p[j] > ctx->return_value) {
					ctx->return_value = p[j];
					ctx->initialized = true;
				}
			}
			break;
		case CPCS_BUILTIN_PIND_MIN64:
			for (j = 0; j < (entries[i].len / sizeof(uint64_t)); j++) {
				if (!ctx->initialized || p[j] < ctx->return_value) {
					ctx->return_value = p[j];
					ctx->initialized = true;
				}
			}
			break;
		default:
			return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
		}
	}

	return 0;
}

static bool
_cpcs_range_overlap_u64(uint64_t offset_a, uint64_t length_a,
			uint64_t offset_b, uint64_t length_b)
{
	uint64_t end_a;
	uint64_t end_b;

	if (length_a == 0 || length_b == 0) {
		return false;
	}
	if (offset_a > UINT64_MAX - length_a || offset_b > UINT64_MAX - length_b) {
		return true;
	}

	end_a = offset_a + length_a;
	end_b = offset_b + length_b;
	return !(end_a <= offset_b || end_b <= offset_a);
}

static int
_cpcs_builtin_memfill_apply_sg(const struct spdk_bdev_slm_sg_entry *entries,
			       uint32_t entry_count, uint8_t pattern)
{
	uint32_t i;

	if (entries == NULL) {
		return -EINVAL;
	}

	for (i = 0; i < entry_count; i++) {
		memset(entries[i].addr, pattern, entries[i].len);
	}

	return 0;
}

static int
_cpcs_builtin_memcpy_apply_sg(const struct spdk_bdev_slm_sg_entry *src_entries,
			      uint32_t src_entry_count,
			      const struct spdk_bdev_slm_sg_entry *dst_entries,
			      uint32_t dst_entry_count,
			      uint64_t total_len)
{
	uint32_t src_i = 0;
	uint32_t dst_i = 0;
	uint32_t src_off = 0;
	uint32_t dst_off = 0;
	uint64_t copied = 0;
	uint32_t chunk;
	uint32_t src_avail;
	uint32_t dst_avail;
	uint8_t *src_ptr;
	uint8_t *dst_ptr;

	if (src_entries == NULL || dst_entries == NULL) {
		return -EINVAL;
	}

	while (copied < total_len) {
		if (src_i >= src_entry_count || dst_i >= dst_entry_count) {
			return -EIO;
		}

		src_avail = src_entries[src_i].len - src_off;
		dst_avail = dst_entries[dst_i].len - dst_off;
		chunk = spdk_min((uint32_t)(total_len - copied), spdk_min(src_avail, dst_avail));
		if (chunk == 0) {
			return -EIO;
		}

		src_ptr = (uint8_t *)src_entries[src_i].addr + src_off;
		dst_ptr = (uint8_t *)dst_entries[dst_i].addr + dst_off;
		memcpy(dst_ptr, src_ptr, chunk);

		copied += chunk;
		src_off += chunk;
		dst_off += chunk;
		if (src_off == src_entries[src_i].len) {
			src_i++;
			src_off = 0;
		}
		if (dst_off == dst_entries[dst_i].len) {
			dst_i++;
			dst_off = 0;
		}
	}

	return 0;
}

static void
builtin_reduce_read_done(void *cb_arg, int status)
{
	struct cpcs_builtin_async_exec_ctx *ctx = cb_arg;
	const uint64_t *p;
	size_t i;

	status = cpcs_builtin_map_slm_status(status);
	if (status != 0) {
		builtin_async_finish(ctx, status);
		return;
	}

	p = (const uint64_t *)ctx->buf;
	switch (ctx->pind) {
	case CPCS_BUILTIN_PIND_SUM64:
		for (i = 0; i < (ctx->chunk_len / sizeof(uint64_t)); i++) {
			ctx->return_value += p[i];
		}
		break;
	case CPCS_BUILTIN_PIND_MAX64:
		for (i = 0; i < (ctx->chunk_len / sizeof(uint64_t)); i++) {
			if (!ctx->initialized || p[i] > ctx->return_value) {
				ctx->return_value = p[i];
				ctx->initialized = true;
			}
		}
		break;
	case CPCS_BUILTIN_PIND_MIN64:
		for (i = 0; i < (ctx->chunk_len / sizeof(uint64_t)); i++) {
			if (!ctx->initialized || p[i] < ctx->return_value) {
				ctx->return_value = p[i];
				ctx->initialized = true;
			}
		}
		break;
	default:
		builtin_async_finish(ctx, -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX);
		return;
	}

	ctx->processed += ctx->chunk_len;
	builtin_async_schedule_continue(ctx);
}

static int
builtin_async_step(struct cpcs_builtin_async_exec_ctx *ctx)
{
	switch (ctx->pind) {
	case CPCS_BUILTIN_PIND_MEMCPY: {
		struct spdk_bdev_slm_sg_entry src_entries[CPCS_BUILTIN_COPY_MAX_SG_ENTRIES];
		struct spdk_bdev_slm_sg_entry dst_entries[CPCS_BUILTIN_COPY_MAX_SG_ENTRIES];
		struct spdk_bdev *src_bdev = NULL;
		struct spdk_bdev *dst_bdev = NULL;
		uint32_t src_entry_count = 0;
		uint32_t dst_entry_count = 0;
		uint64_t src_absolute;
		uint64_t dst_absolute;
		bool fallback_to_async_rw = false;
		int src_pin_rc;
		int dst_pin_rc;
		int copy_rc = 0;
		int unpin_src_rc = 0;
		int unpin_dst_rc = 0;

		if (ctx->processed >= ctx->len) {
			ctx->return_value = ctx->len;
			builtin_async_finish(ctx, 0);
			return 0;
		}

		ctx->chunk_len = spdk_min(ctx->len - ctx->processed, (uint64_t)CPCS_BUILTIN_IO_CHUNK);
		src_absolute = ctx->src_off + ctx->processed;
		dst_absolute = ctx->dst_off + ctx->processed;

		src_pin_rc = _cpcs_exec_pin_range_raw(ctx->exec_ctx, ctx->src_mr_id, src_absolute,
						      ctx->chunk_len, false, &src_bdev,
						      src_entries, SPDK_COUNTOF(src_entries),
						      &src_entry_count);
		if (src_pin_rc == 0) {
			dst_pin_rc = _cpcs_exec_pin_range_raw(ctx->exec_ctx, ctx->dst_mr_id, dst_absolute,
							      ctx->chunk_len, true, &dst_bdev,
							      dst_entries, SPDK_COUNTOF(dst_entries),
							      &dst_entry_count);
			if (dst_pin_rc == 0) {
				if (src_bdev == dst_bdev &&
				    _cpcs_range_overlap_u64(src_absolute, ctx->chunk_len,
							    dst_absolute, ctx->chunk_len)) {
					fallback_to_async_rw = true;
				} else {
					copy_rc = _cpcs_builtin_memcpy_apply_sg(src_entries, src_entry_count,
										dst_entries, dst_entry_count,
										ctx->chunk_len);
				}

				unpin_src_rc = _cpcs_exec_unpin_range_raw(src_bdev, src_entries,
						src_entry_count, false);
				unpin_dst_rc = _cpcs_exec_unpin_range_raw(dst_bdev, dst_entries,
						dst_entry_count, true);

				if (!fallback_to_async_rw && copy_rc == 0 &&
				    unpin_src_rc == 0 && unpin_dst_rc == 0) {
					ctx->processed += ctx->chunk_len;
					builtin_async_schedule_continue(ctx);
					return 0;
				}

				if (unpin_src_rc != 0) {
					return cpcs_builtin_map_slm_status(unpin_src_rc);
				}
				if (unpin_dst_rc != 0) {
					return cpcs_builtin_map_slm_status(unpin_dst_rc);
				}
				if (!fallback_to_async_rw) {
					return cpcs_builtin_map_slm_status(copy_rc);
				}
			} else {
				unpin_src_rc = _cpcs_exec_unpin_range_raw(src_bdev, src_entries,
						src_entry_count, false);
				if (unpin_src_rc != 0) {
					return cpcs_builtin_map_slm_status(unpin_src_rc);
				}
				if (dst_pin_rc != -ENOTSUP && dst_pin_rc != -EAGAIN &&
				    dst_pin_rc != -ENOSPC) {
					return cpcs_builtin_map_slm_status(dst_pin_rc);
				}
				fallback_to_async_rw = true;
			}
		} else if (src_pin_rc != -ENOTSUP && src_pin_rc != -EAGAIN &&
			   src_pin_rc != -ENOSPC) {
			return cpcs_builtin_map_slm_status(src_pin_rc);
		} else {
			fallback_to_async_rw = true;
		}

		return _cpcs_exec_read_range_async(ctx->exec_ctx, ctx->src_mr_id,
						   ctx->src_off + ctx->processed,
						   ctx->chunk_len, ctx->buf,
						   builtin_memcpy_read_done, ctx);
	}

	case CPCS_BUILTIN_PIND_MEMFILL: {
		struct spdk_bdev_slm_sg_entry entries[CPCS_BUILTIN_FILL_MAX_SG_ENTRIES];
		struct spdk_bdev *sg_bdev = NULL;
		uint32_t entry_count = 0;
		int pin_rc;
		int fill_rc;
		int unpin_rc;

		if (ctx->processed >= ctx->len) {
			ctx->return_value = ctx->len;
			builtin_async_finish(ctx, 0);
			return 0;
		}

		ctx->chunk_len = spdk_min(ctx->len - ctx->processed, (uint64_t)CPCS_BUILTIN_IO_CHUNK);
		pin_rc = _cpcs_exec_pin_range_raw(ctx->exec_ctx, ctx->mr_id,
						  ctx->off + ctx->processed,
						  ctx->chunk_len, true, &sg_bdev,
						  entries, SPDK_COUNTOF(entries),
						  &entry_count);
		if (pin_rc == 0) {
			fill_rc = _cpcs_builtin_memfill_apply_sg(entries, entry_count, ctx->pattern);
			unpin_rc = _cpcs_exec_unpin_range_raw(sg_bdev, entries, entry_count, true);
			if (fill_rc == 0 && unpin_rc == 0) {
				ctx->processed += ctx->chunk_len;
				builtin_async_schedule_continue(ctx);
				return 0;
			}
			if (fill_rc != 0) {
				return cpcs_builtin_map_slm_status(fill_rc);
			}
			return cpcs_builtin_map_slm_status(unpin_rc);
		}
		if (pin_rc != -ENOTSUP && pin_rc != -EAGAIN && pin_rc != -ENOSPC) {
			return cpcs_builtin_map_slm_status(pin_rc);
		}

		return _cpcs_exec_write_range_async(ctx->exec_ctx, ctx->mr_id,
						    ctx->off + ctx->processed,
						    ctx->chunk_len, ctx->buf,
						    builtin_memfill_write_done, ctx);
	}

	case CPCS_BUILTIN_PIND_SUM64:
	case CPCS_BUILTIN_PIND_MAX64:
	case CPCS_BUILTIN_PIND_MIN64: {
		struct spdk_bdev_slm_sg_entry sg_entries[CPCS_BUILTIN_REDUCE_MAX_SG_ENTRIES];
		struct spdk_bdev *sg_bdev = NULL;
		uint32_t sg_entry_count = 0;
		int pin_rc;
		int reduce_rc;
		int unpin_rc;

		if (ctx->processed >= ctx->len) {
			builtin_async_finish(ctx, 0);
			return 0;
		}

		ctx->chunk_len = spdk_min(ctx->len - ctx->processed, (uint64_t)CPCS_BUILTIN_IO_CHUNK);
		ctx->chunk_len -= ctx->chunk_len % sizeof(uint64_t);
		if (ctx->chunk_len == 0) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		pin_rc = _cpcs_exec_pin_range_raw(ctx->exec_ctx, ctx->mr_id,
						  ctx->off + ctx->processed,
						  ctx->chunk_len, false, &sg_bdev,
						  sg_entries, SPDK_COUNTOF(sg_entries),
						  &sg_entry_count);
		if (pin_rc == 0) {
			reduce_rc = _cpcs_builtin_reduce_apply_sg(ctx, sg_entries, sg_entry_count);
			unpin_rc = _cpcs_exec_unpin_range_raw(sg_bdev, sg_entries, sg_entry_count, false);
			if (reduce_rc == 0 && unpin_rc == 0) {
				ctx->processed += ctx->chunk_len;
				builtin_async_schedule_continue(ctx);
				return 0;
			}
			if (reduce_rc != 0) {
				return cpcs_builtin_map_slm_status(reduce_rc);
			}
			return cpcs_builtin_map_slm_status(unpin_rc);
		}
		if (pin_rc != -ENOTSUP && pin_rc != -EAGAIN && pin_rc != -ENOSPC) {
			return cpcs_builtin_map_slm_status(pin_rc);
		}

		return _cpcs_exec_read_range_async(ctx->exec_ctx, ctx->mr_id,
						   ctx->off + ctx->processed,
						   ctx->chunk_len, ctx->buf,
						   builtin_reduce_read_done, ctx);
	}

	default:
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}
}

static int
builtin_async_ctx_init(struct cpcs_builtin_async_exec_ctx *ctx)
{
	const struct cpcs_builtin_memcpy_desc *memcpy_desc;
	const struct cpcs_builtin_memfill_desc *memfill_desc;
	const struct cpcs_builtin_sum64_desc *sum_desc;
	uint64_t chunk;

	ctx->pind = ctx->prog->pind;
	switch (ctx->pind) {
	case CPCS_BUILTIN_PIND_MEMCPY:
		if (ctx->exec_ctx->data_buffer == NULL ||
		    ctx->exec_ctx->data_len < sizeof(*memcpy_desc)) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		memcpy_desc = (const struct cpcs_builtin_memcpy_desc *)ctx->exec_ctx->data_buffer;
		ctx->len = from_le64(&memcpy_desc->len);
		if (ctx->len == 0) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		ctx->src_mr_id = from_le64(&memcpy_desc->src_mr_id);
		ctx->src_off = from_le64(&memcpy_desc->src_off);
		ctx->dst_mr_id = from_le64(&memcpy_desc->dst_mr_id);
		ctx->dst_off = from_le64(&memcpy_desc->dst_off);
		chunk = spdk_min(ctx->len, (uint64_t)CPCS_BUILTIN_IO_CHUNK);
		ctx->buf = malloc(chunk);
		if (ctx->buf == NULL) {
			return -ENOMEM;
		}
		return 0;

	case CPCS_BUILTIN_PIND_MEMFILL:
		if (ctx->exec_ctx->data_buffer == NULL ||
		    ctx->exec_ctx->data_len < sizeof(*memfill_desc)) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		memfill_desc = (const struct cpcs_builtin_memfill_desc *)ctx->exec_ctx->data_buffer;
		ctx->len = from_le64(&memfill_desc->len);
		if (ctx->len == 0) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		ctx->mr_id = from_le64(&memfill_desc->mr_id);
		ctx->off = from_le64(&memfill_desc->off);
		ctx->pattern = memfill_desc->pattern;
		chunk = spdk_min(ctx->len, (uint64_t)CPCS_BUILTIN_IO_CHUNK);
		ctx->buf = malloc(chunk);
		if (ctx->buf == NULL) {
			return -ENOMEM;
		}
		memset(ctx->buf, ctx->pattern, chunk);
		return 0;

	case CPCS_BUILTIN_PIND_SUM64:
	case CPCS_BUILTIN_PIND_MAX64:
	case CPCS_BUILTIN_PIND_MIN64:
		if (ctx->exec_ctx->data_buffer == NULL ||
		    ctx->exec_ctx->data_len < sizeof(*sum_desc)) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		sum_desc = (const struct cpcs_builtin_sum64_desc *)ctx->exec_ctx->data_buffer;
		ctx->len = from_le64(&sum_desc->len);
		if (ctx->len == 0 || (ctx->len % sizeof(uint64_t)) != 0) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		ctx->mr_id = from_le64(&sum_desc->mr_id);
		ctx->off = from_le64(&sum_desc->off);
		ctx->buf = malloc(CPCS_BUILTIN_IO_CHUNK);
		if (ctx->buf == NULL) {
			return -ENOMEM;
		}
		ctx->return_value = 0;
		ctx->initialized = false;
		return 0;

	default:
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}
}

static int
builtin_execute_async(struct cpcs_program *prog,
		      struct cpcs_exec_context *ctx,
		      cpcs_runtime_execute_done_cb done_cb,
		      void *cb_arg)
{
	struct cpcs_builtin_async_exec_ctx *exec_ctx;
	int rc;

	if (prog == NULL || ctx == NULL || done_cb == NULL) {
		return -EINVAL;
	}

	if (prog->pind == CPCS_BUILTIN_PIND_FILTER_AGG ||
	    prog->pind == CPCS_BUILTIN_PIND_FILTERED_TOPK_EXACT) {
		return _cpcs_builtin_execute_extended(prog, ctx, done_cb, cb_arg);
	}

	if (_cpcs_builtin_kv_expected_op(prog->pind) != 0) {
		return _cpcs_builtin_execute_extended(prog, ctx, done_cb, cb_arg);
	}

	if (prog->pind == CPCS_BUILTIN_PIND_DOT_PRODUCT ||
	    prog->pind == CPCS_BUILTIN_PIND_FILTER_GT ||
	    prog->pind == CPCS_BUILTIN_PIND_MEMCPY_INLINE ||
	    prog->pind == CPCS_BUILTIN_PIND_RLE_COMPRESS ||
	    prog->pind == CPCS_BUILTIN_PIND_MULTI_AGG64 ||
	    prog->pind == CPCS_BUILTIN_PIND_L2_DISTANCE_SQ ||
	    prog->pind == CPCS_BUILTIN_PIND_COSINE_SIMILARITY ||
	    prog->pind == CPCS_BUILTIN_PIND_DIRECT_NS_AGG) {
		return _cpcs_builtin_execute_extended(prog, ctx, done_cb, cb_arg);
	}

	exec_ctx = calloc(1, sizeof(*exec_ctx));
	if (exec_ctx == NULL) {
		return -ENOMEM;
	}

	exec_ctx->prog = prog;
	exec_ctx->exec_ctx = ctx;
	exec_ctx->done_cb = done_cb;
	exec_ctx->done_arg = cb_arg;

	rc = builtin_async_ctx_init(exec_ctx);
	if (rc != 0) {
		free(exec_ctx);
		return rc;
	}

	rc = builtin_async_step(exec_ctx);
	if (rc != 0) {
		free(exec_ctx->buf);
		free(exec_ctx);
		return rc;
	}

	return 0;
}

static const struct cpcs_runtime_ops g_builtin_runtime = {
	.name = "builtin",
	.ptype = SPDK_NVME_CPCS_PTYPE_DEVICE_DEFINED,
	.init = NULL,
	.validate = NULL,
	.activate = NULL,
	.execute_async = builtin_execute_async,
	.deactivate = NULL,
	.fini = NULL,
};

int
cpcs_builtin_runtime_register(void)
{
	return cpcs_runtime_register(&g_builtin_runtime);
}

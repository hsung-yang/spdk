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

#include "spdk/bdev_slm.h"
#include "spdk/endian.h"
#include "spdk/log.h"

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

static int
_cpcs_exec_get_buffer(const struct cpcs_exec_context *ctx,
		      uint64_t mr_id, uint64_t off, uint64_t len, void **ptr_out)
{
	struct cpcs_memory_range inline_mr;
	int rc;

	if (ctx == NULL || ptr_out == NULL) {
		return -EINVAL;
	}

	if (mr_id == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	if (ctx->mrs != NULL) {
		return cpcs_mrs_get_buffer(ctx->mrs, mr_id, off, len, ptr_out);
	}

	/* Inline ranges: treat Memory Range ID as 1-based index. */
	if (ctx->inline_ranges == NULL || ctx->inline_range_count == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	if (mr_id > ctx->inline_range_count) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	inline_mr = ctx->inline_ranges[mr_id - 1];
	if (off + len > inline_mr.length) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	rc = bdev_slm_get_buffer_ptr_by_nsid(inline_mr.mnsid, inline_mr.starting_byte + off, len, ptr_out);
	if (rc == -ENOENT) {
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}
	return rc;
}

static int
_builtin_execute_memcpy(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_memcpy_desc *desc;
	void *src;
	void *dst;
	uint64_t len;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_memcpy_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	rc = _cpcs_exec_get_buffer(ctx, from_le64(&desc->src_mr_id), from_le64(&desc->src_off), len, &src);
	if (rc != 0) {
		return rc;
	}

	rc = _cpcs_exec_get_buffer(ctx, from_le64(&desc->dst_mr_id), from_le64(&desc->dst_off), len, &dst);
	if (rc != 0) {
		return rc;
	}

	memcpy(dst, src, len);
	*return_value = len;
	return 0;
}

static int
_builtin_execute_memfill(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_memfill_desc *desc;
	void *buf;
	uint64_t len;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_memfill_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	rc = _cpcs_exec_get_buffer(ctx, from_le64(&desc->mr_id), from_le64(&desc->off), len, &buf);
	if (rc != 0) {
		return rc;
	}

	memset(buf, desc->pattern, len);
	*return_value = len;
	return 0;
}

static int
_builtin_execute_sum64(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_sum64_desc *desc;
	const uint64_t *p;
	void *buf;
	uint64_t len;
	uint64_t sum = 0;
	size_t i;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0 || (len % sizeof(uint64_t)) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	rc = _cpcs_exec_get_buffer(ctx, from_le64(&desc->mr_id), from_le64(&desc->off), len, &buf);
	if (rc != 0) {
		return rc;
	}

	p = (const uint64_t *)buf;
	for (i = 0; i < (len / sizeof(uint64_t)); i++) {
		sum += p[i];
	}

	*return_value = sum;
	return 0;
}

static int
_builtin_execute_max64(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_sum64_desc *desc;
	const uint64_t *p;
	void *buf;
	uint64_t len;
	uint64_t max;
	size_t i;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0 || (len % sizeof(uint64_t)) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	rc = _cpcs_exec_get_buffer(ctx, from_le64(&desc->mr_id), from_le64(&desc->off), len, &buf);
	if (rc != 0) {
		return rc;
	}

	p = (const uint64_t *)buf;
	max = p[0];
	for (i = 1; i < (len / sizeof(uint64_t)); i++) {
		if (p[i] > max) {
			max = p[i];
		}
	}

	*return_value = max;
	return 0;
}

static int
_builtin_execute_min64(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_sum64_desc *desc;
	const uint64_t *p;
	void *buf;
	uint64_t len;
	uint64_t min;
	size_t i;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0 || (len % sizeof(uint64_t)) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	rc = _cpcs_exec_get_buffer(ctx, from_le64(&desc->mr_id), from_le64(&desc->off), len, &buf);
	if (rc != 0) {
		return rc;
	}

	p = (const uint64_t *)buf;
	min = p[0];
	for (i = 1; i < (len / sizeof(uint64_t)); i++) {
		if (p[i] < min) {
			min = p[i];
		}
	}

	*return_value = min;
	return 0;
}

static int
builtin_execute(struct cpcs_program *prog, struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	if (prog == NULL || ctx == NULL || return_value == NULL) {
		return -EINVAL;
	}

	switch (prog->pind) {
	case CPCS_BUILTIN_PIND_MEMCPY:
		return _builtin_execute_memcpy(ctx, return_value);
	case CPCS_BUILTIN_PIND_MEMFILL:
		return _builtin_execute_memfill(ctx, return_value);
	case CPCS_BUILTIN_PIND_SUM64:
		return _builtin_execute_sum64(ctx, return_value);
	case CPCS_BUILTIN_PIND_MAX64:
		return _builtin_execute_max64(ctx, return_value);
	case CPCS_BUILTIN_PIND_MIN64:
		return _builtin_execute_min64(ctx, return_value);
	default:
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}
}

static const struct cpcs_runtime_ops g_builtin_runtime = {
	.name = "builtin",
	.ptype = SPDK_NVME_CPCS_PTYPE_DEVICE_DEFINED,
	.init = NULL,
	.validate = NULL,
	.activate = NULL,
	.execute = builtin_execute,
	.deactivate = NULL,
	.fini = NULL,
};

int
cpcs_builtin_runtime_register(void)
{
	return cpcs_runtime_register(&g_builtin_runtime);
}

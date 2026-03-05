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

#define CPCS_BUILTIN_IO_CHUNK (64 * 1024)

static int
_cpcs_exec_resolve_range(const struct cpcs_exec_context *ctx,
			 uint64_t mr_id, uint64_t off, uint64_t len,
			 struct spdk_bdev **bdev_out, uint64_t *absolute_offset_out)
{
	const struct cpcs_exec_resolved_range *mr;
	uint64_t absolute_offset;

	if (ctx == NULL || bdev_out == NULL || absolute_offset_out == NULL) {
		return -EINVAL;
	}

	if (mr_id == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	if (ctx->resolved_ranges == NULL || ctx->resolved_range_count == 0) {
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	if (mr_id > ctx->resolved_range_count) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	mr = &ctx->resolved_ranges[mr_id - 1];

	if (mr->bdev == NULL) {
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}

	if (off > UINT64_MAX - len) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (off + len > mr->length) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (mr->starting_byte > UINT64_MAX - off) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	absolute_offset = mr->starting_byte + off;
	*bdev_out = mr->bdev;
	*absolute_offset_out = absolute_offset;
	return 0;
}

static int
_cpcs_exec_read_range(const struct cpcs_exec_context *ctx,
		      uint64_t mr_id, uint64_t off, uint64_t len, void *buf)
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

	rc = bdev_slm_read_by_bdev(bdev, absolute_offset, len, buf);
	if (rc == -ENOENT || rc == -ENOTSUP) {
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}
	if (rc == -EINVAL) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	return rc;
}

static int
_cpcs_exec_write_range(const struct cpcs_exec_context *ctx,
		       uint64_t mr_id, uint64_t off, uint64_t len, const void *buf)
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

	rc = bdev_slm_write_by_bdev(bdev, absolute_offset, len, buf);
	if (rc == -ENOENT || rc == -ENOTSUP) {
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}
	if (rc == -EINVAL) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	return rc;
}

static int
_builtin_execute_memcpy(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_memcpy_desc *desc;
	uint8_t *tmp;
	uint64_t src_mr_id;
	uint64_t src_off;
	uint64_t dst_mr_id;
	uint64_t dst_off;
	uint64_t copied = 0;
	uint64_t chunk;
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

	src_mr_id = from_le64(&desc->src_mr_id);
	src_off = from_le64(&desc->src_off);
	dst_mr_id = from_le64(&desc->dst_mr_id);
	dst_off = from_le64(&desc->dst_off);

	chunk = len < CPCS_BUILTIN_IO_CHUNK ? len : CPCS_BUILTIN_IO_CHUNK;
	tmp = malloc(chunk);
	if (tmp == NULL) {
		return -ENOMEM;
	}

	while (copied < len) {
		chunk = len - copied;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
		}

		rc = _cpcs_exec_read_range(ctx, src_mr_id, src_off + copied, chunk, tmp);
		if (rc != 0) {
			free(tmp);
			return rc;
		}

		rc = _cpcs_exec_write_range(ctx, dst_mr_id, dst_off + copied, chunk, tmp);
		if (rc != 0) {
			free(tmp);
			return rc;
		}

		copied += chunk;
	}

	free(tmp);
	*return_value = len;
	return 0;
}

static int
_builtin_execute_memfill(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_memfill_desc *desc;
	uint64_t mr_id;
	uint64_t off;
	uint8_t *tmp;
	uint64_t written = 0;
	uint64_t chunk;
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

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);

	chunk = len < CPCS_BUILTIN_IO_CHUNK ? len : CPCS_BUILTIN_IO_CHUNK;
	tmp = malloc(chunk);
	if (tmp == NULL) {
		return -ENOMEM;
	}
	memset(tmp, desc->pattern, chunk);

	while (written < len) {
		chunk = len - written;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
		}

		rc = _cpcs_exec_write_range(ctx, mr_id, off + written, chunk, tmp);
		if (rc != 0) {
			free(tmp);
			return rc;
		}
		written += chunk;
	}

	free(tmp);
	*return_value = len;
	return 0;
}

static int
_builtin_execute_sum64(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_sum64_desc *desc;
	const uint64_t *p;
	uint8_t *buf;
	uint64_t mr_id;
	uint64_t off;
	uint64_t processed = 0;
	uint64_t chunk;
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

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);

	buf = malloc(CPCS_BUILTIN_IO_CHUNK);
	if (buf == NULL) {
		return -ENOMEM;
	}

	while (processed < len) {
		chunk = len - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
			chunk -= chunk % sizeof(uint64_t);
		}

		rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk, buf);
		if (rc != 0) {
			free(buf);
			return rc;
		}

		p = (const uint64_t *)buf;
		for (i = 0; i < (chunk / sizeof(uint64_t)); i++) {
			sum += p[i];
		}
		processed += chunk;
	}

	free(buf);
	*return_value = sum;
	return 0;
}

static int
_builtin_execute_max64(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_sum64_desc *desc;
	const uint64_t *p;
	uint8_t *buf;
	uint64_t mr_id;
	uint64_t off;
	uint64_t processed = 0;
	uint64_t chunk;
	uint64_t len;
	uint64_t max = 0;
	bool initialized = false;
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

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);

	buf = malloc(CPCS_BUILTIN_IO_CHUNK);
	if (buf == NULL) {
		return -ENOMEM;
	}

	while (processed < len) {
		chunk = len - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
			chunk -= chunk % sizeof(uint64_t);
		}

		rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk, buf);
		if (rc != 0) {
			free(buf);
			return rc;
		}

		p = (const uint64_t *)buf;
		for (i = 0; i < (chunk / sizeof(uint64_t)); i++) {
			if (!initialized || p[i] > max) {
				max = p[i];
				initialized = true;
			}
		}
		processed += chunk;
	}

	free(buf);
	*return_value = max;
	return 0;
}

static int
_builtin_execute_min64(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_sum64_desc *desc;
	const uint64_t *p;
	uint8_t *buf;
	uint64_t mr_id;
	uint64_t off;
	uint64_t processed = 0;
	uint64_t chunk;
	uint64_t len;
	uint64_t min = 0;
	bool initialized = false;
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

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);

	buf = malloc(CPCS_BUILTIN_IO_CHUNK);
	if (buf == NULL) {
		return -ENOMEM;
	}

	while (processed < len) {
		chunk = len - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
			chunk -= chunk % sizeof(uint64_t);
		}

		rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk, buf);
		if (rc != 0) {
			free(buf);
			return rc;
		}

		p = (const uint64_t *)buf;
		for (i = 0; i < (chunk / sizeof(uint64_t)); i++) {
			if (!initialized || p[i] < min) {
				min = p[i];
				initialized = true;
			}
		}
		processed += chunk;
	}

	free(buf);
	*return_value = min;
	return 0;
}

static int
_builtin_execute_dot_product(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const float *data;
	uint32_t sum_bits = 0;
	float sum = 0.0f;
	size_t total_floats;
	size_t n;
	size_t i;

	if (ctx->data_buffer == NULL || ctx->data_len < (2 * sizeof(float))) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if ((ctx->data_len % (2 * sizeof(float))) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	data = (const float *)ctx->data_buffer;
	total_floats = ctx->data_len / sizeof(float);
	n = total_floats / 2;

	for (i = 0; i < n; i++) {
		sum += data[i] * data[i + n];
	}

	memcpy(&sum_bits, &sum, sizeof(sum_bits));
	*return_value = (uint64_t)sum_bits;
	return 0;
}

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

	if (ctx->data_buffer == NULL || ctx->data_len < (4 + 2 * sizeof(float))) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	buf = (const uint8_t *)ctx->data_buffer;
	memcpy(&threshold, buf, sizeof(threshold));

	if (ctx->data_len < 4) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
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
	case CPCS_BUILTIN_PIND_DOT_PRODUCT:
		return _builtin_execute_dot_product(ctx, return_value);
	case CPCS_BUILTIN_PIND_FILTER_GT:
		return _builtin_execute_filter_gt(ctx, return_value);
	case CPCS_BUILTIN_PIND_MEMCPY_INLINE:
		return _builtin_execute_memcpy_inline(ctx, return_value);
	case CPCS_BUILTIN_PIND_RLE_COMPRESS:
		return _builtin_execute_rle_compress(ctx, return_value);
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

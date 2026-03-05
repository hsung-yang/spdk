/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

/**
 * \file
 * CPCS built-in predefined program smoke test
 *
 * Exercises the predefined device-defined CPCS programs using SLM buffers.
 */

#include "cpcs_common.h"

#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/nvme_spec.h"

#include "builtin_programs.h"
#include "execute.h"
#include "nvmf_cpcs.h"
#include "program.h"
#include "runtime.h"

static struct spdk_nvmf_cpcs_ns *
_cpcs_smoke_ns_create(void)
{
	struct spdk_nvmf_cpcs_ns *ns;

	ns = calloc(1, sizeof(*ns));
	if (ns == NULL) {
		return NULL;
	}

	ns->max_programs = CPCS_MAX_PROGRAMS_PER_NS;
	ns->next_rsid = 1;
	TAILQ_INIT(&ns->mrs_list);
	if (pthread_mutex_init(&ns->lock, NULL) != 0) {
		free(ns);
		return NULL;
	}

	return ns;
}

static int
_run_exec_with_setup(struct cpcs_exec_context *ctx)
{
	struct cpcs_exec_resolved_range *resolved = NULL;
	struct spdk_bdev *bdev = NULL;
	uint32_t range_count = 0;
	int rc;
	uint32_t i;

	if (ctx == NULL || ctx->program == NULL) {
		return -EINVAL;
	}

	if (ctx->ns == NULL) {
		ctx->ns = ctx->program->ns;
	}
	if (ctx->ns == NULL) {
		return -EINVAL;
	}

	/* Resolve test-provided inline ranges directly (no request-path / MRS mutation). */
	if (ctx->inline_range_count == 0 || ctx->inline_ranges == NULL) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	range_count = ctx->inline_range_count;
	resolved = calloc(range_count, sizeof(*resolved));
	if (resolved == NULL) {
		return -ENOMEM;
	}

	for (i = 0; i < range_count; i++) {
		bdev = NULL;
		for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
			if (spdk_bdev_get_nvme_nsid(bdev) == ctx->inline_ranges[i].mnsid) {
				break;
			}
		}

		if (bdev == NULL) {
			rc = -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
			goto out;
		}

		resolved[i].bdev = bdev;
		resolved[i].mnsid = ctx->inline_ranges[i].mnsid;
		resolved[i].starting_byte = ctx->inline_ranges[i].starting_byte;
		resolved[i].length = ctx->inline_ranges[i].length;
	}

	ctx->resolved_ranges = resolved;
	ctx->resolved_range_count = range_count;
	resolved = NULL;

	rc = cpcs_execute_run(ctx);

out:
	free(resolved);

	free(ctx->resolved_ranges);
	ctx->resolved_ranges = NULL;
	ctx->resolved_range_count = 0;

	return rc;
}

static void
_cpcs_smoke_ns_destroy(struct spdk_nvmf_cpcs_ns *ns)
{
	if (ns == NULL) {
		return;
	}

	cpcs_program_unload_all(ns);
	pthread_mutex_destroy(&ns->lock);
	free(ns);
}

static int
_run_memfill(struct cpcs_test_ctx *tctx, struct spdk_nvmf_cpcs_ns *ns,
	     const char *slm_name, uint32_t slm_nsid)
{
	struct cpcs_program *prog;
	struct cpcs_exec_context ctx = {};
	struct cpcs_memory_range range = {};
	struct cpcs_builtin_memfill_desc {
		uint64_t mr_id;
		uint64_t off;
		uint64_t len;
		uint8_t pattern;
		uint8_t _rsvd[7];
	} desc;
	uint8_t expected[4096];
	int rc;

	SPDK_NOTICELOG("CPCS builtin: memfill\n");

	prog = cpcs_program_get(ns, CPCS_BUILTIN_PIND_MEMFILL);
	if (prog == NULL) {
		return -EINVAL;
	}

	range.mnsid = slm_nsid;
	range.starting_byte = 0;
	range.length = 64 * 1024;

	memset(&desc, 0, sizeof(desc));
	to_le64(&desc.mr_id, 1);
	to_le64(&desc.off, 0);
	to_le64(&desc.len, sizeof(expected));
	desc.pattern = 0xA5;

	ctx.program = prog;
	ctx.inline_ranges = &range;
	ctx.inline_range_count = 1;
	ctx.data_buffer = &desc;
	ctx.data_len = sizeof(desc);

	rc = _run_exec_with_setup(&ctx);
	if (rc != 0) {
		return rc;
	}
	if (ctx.return_value != sizeof(expected)) {
		return -EINVAL;
	}

	memset(expected, desc.pattern, sizeof(expected));
	return cpcs_test_verify_buffer(tctx, slm_name, 0, expected, sizeof(expected));
}

static int
_run_memcpy(struct cpcs_test_ctx *tctx, struct spdk_nvmf_cpcs_ns *ns,
	    const char *slm_name, uint32_t slm_nsid)
{
	struct cpcs_program *prog;
	struct cpcs_exec_context ctx = {};
	struct cpcs_memory_range range = {};
	struct cpcs_builtin_memcpy_desc {
		uint64_t src_mr_id;
		uint64_t src_off;
		uint64_t dst_mr_id;
		uint64_t dst_off;
		uint64_t len;
	} desc;
	uint8_t expected[4096];
	int rc;
	uint64_t src_off = 0;
	uint64_t dst_off = 8192;

	SPDK_NOTICELOG("CPCS builtin: memcpy\n");

	prog = cpcs_program_get(ns, CPCS_BUILTIN_PIND_MEMCPY);
	if (prog == NULL) {
		return -EINVAL;
	}

	range.mnsid = slm_nsid;
	range.starting_byte = 0;
	range.length = 64 * 1024;

	rc = cpcs_test_write_pattern(tctx, slm_name, src_off, sizeof(expected), 0x11);
	if (rc != 0) {
		return rc;
	}
	rc = cpcs_test_write_pattern(tctx, slm_name, dst_off, sizeof(expected), 0xEE);
	if (rc != 0) {
		return rc;
	}

	memset(&desc, 0, sizeof(desc));
	to_le64(&desc.src_mr_id, 1);
	to_le64(&desc.src_off, src_off);
	to_le64(&desc.dst_mr_id, 1);
	to_le64(&desc.dst_off, dst_off);
	to_le64(&desc.len, sizeof(expected));

	ctx.program = prog;
	ctx.inline_ranges = &range;
	ctx.inline_range_count = 1;
	ctx.data_buffer = &desc;
	ctx.data_len = sizeof(desc);

	rc = _run_exec_with_setup(&ctx);
	if (rc != 0) {
		return rc;
	}
	if (ctx.return_value != sizeof(expected)) {
		return -EINVAL;
	}

	memset(expected, 0x11, sizeof(expected));
	return cpcs_test_verify_buffer(tctx, slm_name, dst_off, expected, sizeof(expected));
}

static int
_run_sum64(struct cpcs_test_ctx *tctx, struct spdk_nvmf_cpcs_ns *ns,
	   const char *slm_name, uint32_t slm_nsid)
{
	struct cpcs_program *prog;
	struct cpcs_exec_context ctx = {};
	struct cpcs_memory_range range = {};
	struct cpcs_builtin_sum64_desc {
		uint64_t mr_id;
		uint64_t off;
		uint64_t len;
	} desc;
	void *buf;
	uint64_t expected_sum = 0;
	uint64_t *p;
	size_t i;
	int rc;
	const uint64_t off = 16384;
	const size_t n = 64;

	SPDK_NOTICELOG("CPCS builtin: sum64\n");

	prog = cpcs_program_get(ns, CPCS_BUILTIN_PIND_SUM64);
	if (prog == NULL) {
		return -EINVAL;
	}

	range.mnsid = slm_nsid;
	range.starting_byte = 0;
	range.length = 64 * 1024;

	rc = bdev_slm_get_buffer_ptr(slm_name, off, n * sizeof(uint64_t), &buf);
	if (rc != 0) {
		tctx->error_count++;
		return rc;
	}

	p = buf;
	for (i = 0; i < n; i++) {
		p[i] = (uint64_t)(i + 1);
		expected_sum += p[i];
	}

	memset(&desc, 0, sizeof(desc));
	to_le64(&desc.mr_id, 1);
	to_le64(&desc.off, off);
	to_le64(&desc.len, n * sizeof(uint64_t));

	ctx.program = prog;
	ctx.inline_ranges = &range;
	ctx.inline_range_count = 1;
	ctx.data_buffer = &desc;
	ctx.data_len = sizeof(desc);

	rc = _run_exec_with_setup(&ctx);
	if (rc != 0) {
		return rc;
	}
	if (ctx.return_value != expected_sum) {
		SPDK_ERRLOG("sum64 mismatch: expected=%lu got=%lu\n",
			    expected_sum, ctx.return_value);
		return -EINVAL;
	}

	return 0;
}

static int
_run_dot_product(struct cpcs_test_ctx *tctx, struct spdk_nvmf_cpcs_ns *ns,
		 const char *slm_name, uint32_t slm_nsid)
{
	struct cpcs_program *prog;
	struct cpcs_exec_context ctx = {};
	struct cpcs_memory_range range = {};
	struct cpcs_builtin_sum64_desc {
		uint64_t mr_id;
		uint64_t off;
		uint64_t len;
	} desc;
	void *buf;
	/* A = [1, 2, 3, 4], B = [1, 2, 3, 4] => dot = 1+4+9+16 = 30 */
	float data[8] = { 1.0f, 2.0f, 3.0f, 4.0f, 1.0f, 2.0f, 3.0f, 4.0f };
	float expected = 30.0f;
	uint32_t expected_bits = 0;
	uint32_t got_bits = 0;
	const uint64_t off = 32768;
	int rc;

	SPDK_NOTICELOG("CPCS builtin: dot_product (MRS-based)\n");

	prog = cpcs_program_get(ns, CPCS_BUILTIN_PIND_DOT_PRODUCT);
	if (prog == NULL) {
		return -EINVAL;
	}

	/* Write float data into SLM at offset */
	rc = bdev_slm_get_buffer_ptr(slm_name, off, sizeof(data), &buf);
	if (rc != 0) {
		tctx->error_count++;
		return rc;
	}
	memcpy(buf, data, sizeof(data));

	/* Set up MRS range pointing to SLM */
	range.mnsid = slm_nsid;
	range.starting_byte = 0;
	range.length = 64 * 1024;

	/* Build descriptor: mr_id=1, off=offset_in_slm, len=sizeof(data) */
	memset(&desc, 0, sizeof(desc));
	to_le64(&desc.mr_id, 1);
	to_le64(&desc.off, off);
	to_le64(&desc.len, sizeof(data));

	ctx.program = prog;
	ctx.inline_ranges = &range;
	ctx.inline_range_count = 1;
	ctx.data_buffer = &desc;
	ctx.data_len = sizeof(desc);

	rc = _run_exec_with_setup(&ctx);
	if (rc != 0) {
		return rc;
	}

	memcpy(&expected_bits, &expected, sizeof(expected_bits));
	got_bits = (uint32_t)ctx.return_value;
	if (got_bits != expected_bits) {
		SPDK_ERRLOG("dot_product mismatch: expected_bits=0x%x got_bits=0x%x\n",
			    expected_bits, got_bits);
		return -EINVAL;
	}

	return 0;
}

static int
_run_filter_gt(struct spdk_nvmf_cpcs_ns *ns)
{
	struct cpcs_program *prog;
	struct cpcs_exec_context ctx = {};
	uint8_t buf[4 + (8 * sizeof(float)) + (8 * sizeof(float))] = { 0 };
	const float input[8] = { 1.0f, 5.0f, 2.0f, 8.0f, 3.0f, 7.0f, 4.0f, 6.0f };
	const float expected[4] = { 5.0f, 8.0f, 7.0f, 6.0f };
	float threshold = 4.0f;
	float *in;
	float *out;
	int rc;
	size_t i;

	SPDK_NOTICELOG("CPCS builtin: filter_gt\n");

	prog = cpcs_program_get(ns, CPCS_BUILTIN_PIND_FILTER_GT);
	if (prog == NULL) {
		return -EINVAL;
	}

	memcpy(buf, &threshold, sizeof(threshold));
	in = (float *)(buf + 4);
	out = (float *)(buf + 4 + sizeof(input));
	memcpy(in, input, sizeof(input));
	memset(out, 0, sizeof(input));

	ctx.program = prog;
	ctx.data_buffer = buf;
	ctx.data_len = sizeof(buf);

	rc = cpcs_execute_run(&ctx);
	if (rc != 0) {
		return rc;
	}
	if (ctx.return_value != 4) {
		SPDK_ERRLOG("filter_gt count mismatch: expected=4 got=%lu\n", ctx.return_value);
		return -EINVAL;
	}
	for (i = 0; i < 4; i++) {
		if (out[i] != expected[i]) {
			SPDK_ERRLOG("filter_gt output mismatch at %zu: expected=%f got=%f\n",
				    i, expected[i], out[i]);
			return -EINVAL;
		}
	}

	return 0;
}

static int
_run_memcpy_inline(struct spdk_nvmf_cpcs_ns *ns)
{
	struct cpcs_program *prog;
	struct cpcs_exec_context ctx = {};
	uint8_t buf[128] = { 0 };
	size_t i;
	int rc;

	SPDK_NOTICELOG("CPCS builtin: memcpy_inline\n");

	prog = cpcs_program_get(ns, CPCS_BUILTIN_PIND_MEMCPY_INLINE);
	if (prog == NULL) {
		return -EINVAL;
	}

	for (i = 0; i < 64; i++) {
		buf[i] = (uint8_t)(0x10 + i);
	}

	ctx.program = prog;
	ctx.data_buffer = buf;
	ctx.data_len = sizeof(buf);

	rc = cpcs_execute_run(&ctx);
	if (rc != 0) {
		return rc;
	}
	if (ctx.return_value != 64) {
		return -EINVAL;
	}
	if (memcmp(buf, buf + 64, 64) != 0) {
		SPDK_ERRLOG("memcpy_inline mismatch\n");
		return -EINVAL;
	}

	return 0;
}

static int
_run_rle_compress(struct spdk_nvmf_cpcs_ns *ns)
{
	struct cpcs_program *prog;
	struct cpcs_exec_context ctx = {};
	uint8_t buf[8 + 8 + 8] = { 0 };
	const uint8_t input[8] = { 0xAA, 0xAA, 0xAA, 0xBB, 0xBB, 0xCC, 0xCC, 0xCC };
	const uint8_t expected[6] = { 3, 0xAA, 2, 0xBB, 3, 0xCC };
	uint64_t n = 8;
	int rc;

	SPDK_NOTICELOG("CPCS builtin: rle_compress\n");

	prog = cpcs_program_get(ns, CPCS_BUILTIN_PIND_RLE_COMPRESS);
	if (prog == NULL) {
		return -EINVAL;
	}

	memcpy(buf, &n, sizeof(n));
	memcpy(buf + 8, input, sizeof(input));
	memset(buf + 16, 0, 8);

	ctx.program = prog;
	ctx.data_buffer = buf;
	ctx.data_len = sizeof(buf);

	rc = cpcs_execute_run(&ctx);
	if (rc != 0) {
		return rc;
	}
	if (ctx.return_value != 6) {
		SPDK_ERRLOG("rle_compress size mismatch: expected=6 got=%lu\n", ctx.return_value);
		return -EINVAL;
	}
	if (memcmp(buf + 16, expected, sizeof(expected)) != 0) {
		SPDK_ERRLOG("rle_compress output mismatch\n");
		return -EINVAL;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct cpcs_test_ctx tctx;
	struct spdk_nvmf_cpcs_ns *ns = NULL;
	const char *slm_name = "slm_builtin";
	const uint32_t slm_nsid = 300;
	int failures = 0;
	int rc;

	(void)argc;
	(void)argv;

	SPDK_NOTICELOG("Starting CPCS predefined program smoke tests\n");

	rc = cpcs_test_init(&tctx);
	if (rc != 0) {
		return 1;
	}

	rc = cpcs_test_create_slm(&tctx, slm_name, slm_nsid, 64);
	if (rc != 0) {
		failures++;
		goto out;
	}

	ns = _cpcs_smoke_ns_create();
	if (ns == NULL) {
		failures++;
		goto out;
	}

	rc = cpcs_runtime_init_all();
	if (rc != 0) {
		failures++;
		goto out;
	}

	rc = cpcs_program_install_builtins(ns);
	if (rc != 0) {
		failures++;
		goto out;
	}

	rc = _run_memfill(&tctx, ns, slm_name, slm_nsid);
	if (rc != 0) {
		SPDK_ERRLOG("memfill FAILED: %d\n", rc);
		failures++;
	}

	rc = _run_memcpy(&tctx, ns, slm_name, slm_nsid);
	if (rc != 0) {
		SPDK_ERRLOG("memcpy FAILED: %d\n", rc);
		failures++;
	}

	rc = _run_sum64(&tctx, ns, slm_name, slm_nsid);
	if (rc != 0) {
		SPDK_ERRLOG("sum64 FAILED: %d\n", rc);
		failures++;
	}

	rc = _run_dot_product(&tctx, ns, slm_name, slm_nsid);
	if (rc != 0) {
		SPDK_ERRLOG("dot_product FAILED: %d\n", rc);
		failures++;
	}

	rc = _run_filter_gt(ns);
	if (rc != 0) {
		SPDK_ERRLOG("filter_gt FAILED: %d\n", rc);
		failures++;
	}

	rc = _run_memcpy_inline(ns);
	if (rc != 0) {
		SPDK_ERRLOG("memcpy_inline FAILED: %d\n", rc);
		failures++;
	}

	rc = _run_rle_compress(ns);
	if (rc != 0) {
		SPDK_ERRLOG("rle_compress FAILED: %d\n", rc);
		failures++;
	}

out:
	if (failures == 0) {
		SPDK_NOTICELOG("CPCS predefined program smoke tests PASSED\n");
	} else {
		SPDK_ERRLOG("CPCS predefined program smoke tests FAILED (%d errors)\n", failures);
	}

	_cpcs_smoke_ns_destroy(ns);
	cpcs_test_delete_slm(&tctx, slm_name);
	cpcs_test_cleanup(&tctx);

	return failures == 0 ? 0 : 1;
}

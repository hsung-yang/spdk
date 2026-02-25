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

#include "spdk/endian.h"
#include "spdk/log.h"

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
	if (pthread_mutex_init(&ns->lock, NULL) != 0) {
		free(ns);
		return NULL;
	}

	return ns;
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

	rc = cpcs_execute_run(&ctx);
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

	rc = cpcs_execute_run(&ctx);
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

	rc = cpcs_execute_run(&ctx);
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

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
#include "spdk/bdev_vslm.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"

#include "builtin_programs.h"
#include "execute.h"
#include "nvmf_cpcs.h"
#include "program.h"
#include "runtime.h"

#include "bdev/malloc/bdev_malloc.h"

struct cpcs_smoke_io_wait {
	bool done;
	int status;
};

static bool g_vslm_delete_done;
static int g_vslm_delete_rc;
static bool g_malloc_delete_done;
static int g_malloc_delete_rc;

static void
_cpcs_smoke_poll(void)
{
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (thread == NULL) {
		return;
	}

	while (spdk_thread_poll(thread, 0, 0) > 0) {
	}
}

static int
_cpcs_smoke_wait_for_io(struct cpcs_smoke_io_wait *wait)
{
	while (!wait->done) {
		_cpcs_smoke_poll();
	}

	return wait->status;
}

static void
_cpcs_smoke_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			  void *event_ctx)
{
	(void)type;
	(void)bdev;
	(void)event_ctx;
}

static void
_cpcs_smoke_bdev_io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct cpcs_smoke_io_wait *wait = cb_arg;

	wait->status = success ? 0 : -EIO;
	wait->done = true;
	spdk_bdev_free_io(bdev_io);
}

static int
_cpcs_smoke_write_bdev_blocks(const char *bdev_name, uint64_t offset_bytes,
			      const void *buf, uint64_t len)
{
	struct cpcs_smoke_io_wait wait = {};
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ch = NULL;
	struct spdk_bdev *bdev;
	uint32_t block_size;
	uint64_t offset_blocks;
	uint64_t num_blocks;
	void *dma_buf = NULL;
	int rc;

	if (bdev_name == NULL || (len != 0 && buf == NULL)) {
		return -EINVAL;
	}

	rc = spdk_bdev_open_ext(bdev_name, true, _cpcs_smoke_bdev_event_cb,
				NULL, &desc);
	if (rc != 0) {
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);
	block_size = spdk_bdev_get_block_size(bdev);
	if (block_size == 0 ||
	    (offset_bytes % block_size) != 0 ||
	    (len % block_size) != 0) {
		rc = -EINVAL;
		goto out;
	}

	dma_buf = spdk_dma_malloc(len, block_size, NULL);
	if (dma_buf == NULL && len != 0) {
		rc = -ENOMEM;
		goto out;
	}
	if (len != 0) {
		memcpy(dma_buf, buf, len);
	}

	ch = spdk_bdev_get_io_channel(desc);
	if (ch == NULL) {
		rc = -ENOMEM;
		goto out;
	}

	offset_blocks = offset_bytes / block_size;
	num_blocks = len / block_size;
	rc = spdk_bdev_write_blocks(desc, ch, dma_buf, offset_blocks, num_blocks,
				    _cpcs_smoke_bdev_io_done, &wait);
	if (rc == 0) {
		rc = _cpcs_smoke_wait_for_io(&wait);
	}

out:
	if (ch != NULL) {
		spdk_put_io_channel(ch);
	}
	if (dma_buf != NULL) {
		spdk_dma_free(dma_buf);
	}
	if (desc != NULL) {
		spdk_bdev_close(desc);
	}
	return rc;
}

static void
_cpcs_smoke_vslm_delete_done(void *cb_arg, int rc)
{
	(void)cb_arg;

	g_vslm_delete_rc = rc;
	g_vslm_delete_done = true;
}

static void
_cpcs_smoke_malloc_delete_done(void *cb_arg, int rc)
{
	(void)cb_arg;

	g_malloc_delete_rc = rc;
	g_malloc_delete_done = true;
}

static int
_cpcs_smoke_delete_vslm(const char *name)
{
	g_vslm_delete_done = false;
	g_vslm_delete_rc = 0;
	bdev_vslm_delete(name, _cpcs_smoke_vslm_delete_done, NULL);
	while (!g_vslm_delete_done) {
		_cpcs_smoke_poll();
	}

	return g_vslm_delete_rc;
}

static int
_cpcs_smoke_delete_malloc(const char *name)
{
	g_malloc_delete_done = false;
	g_malloc_delete_rc = 0;
	delete_malloc_disk(name, _cpcs_smoke_malloc_delete_done, NULL);
	while (!g_malloc_delete_done) {
		_cpcs_smoke_poll();
	}

	return g_malloc_delete_rc;
}

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
		SPDK_ERRLOG("sum64 mismatch: expected=%llu got=%llu\n",
			    (unsigned long long)expected_sum,
			    (unsigned long long)ctx.return_value);
		return -EINVAL;
	}

	return 0;
}

static int
_run_vslm_cold_sum64(struct spdk_nvmf_cpcs_ns *ns)
{
	const char *base_name = "MallocCpcsVslm0";
	const char *vslm_name = "vslm_cpcs_builtin";
	const uint32_t vslm_nsid = 301;
	const uint64_t off = 0;
	const size_t n = 64;
	struct malloc_bdev_opts malloc_opts = {};
	struct spdk_bdev_vslm_opts vslm_opts = {};
	struct spdk_bdev_vslm_stats vslm_before = {};
	struct spdk_bdev_vslm_stats vslm_after = {};
	struct cpcs_builtin_runtime_stats builtin_before = {};
	struct cpcs_builtin_runtime_stats builtin_after = {};
	struct cpcs_program *prog;
	struct cpcs_exec_context ctx = {};
	struct cpcs_memory_range range = {};
	struct spdk_bdev *base_bdev = NULL;
	struct cpcs_builtin_sum64_desc {
		uint64_t mr_id;
		uint64_t off;
		uint64_t len;
	} desc;
	uint64_t values[64];
	uint64_t expected_sum = 0;
	bool base_created = false;
	bool vslm_created = false;
	size_t i;
	int cleanup_rc;
	int rc;

	SPDK_NOTICELOG("CPCS builtin: vSLM cold sum64\n");

	prog = cpcs_program_get(ns, CPCS_BUILTIN_PIND_SUM64);
	if (prog == NULL) {
		return -EINVAL;
	}

	for (i = 0; i < n; i++) {
		values[i] = i + 1;
		expected_sum += values[i];
	}

	malloc_opts.name = (char *)base_name;
	malloc_opts.num_blocks = 128;
	malloc_opts.block_size = 512;
	malloc_opts.physical_block_size = 512;
	malloc_opts.md_size = 0;
	malloc_opts.md_interleave = false;
	malloc_opts.dif_type = SPDK_DIF_DISABLE;
	malloc_opts.dif_is_head_of_md = false;
	malloc_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = create_malloc_disk(&base_bdev, &malloc_opts);
	if (rc != 0) {
		SPDK_ERRLOG("create_malloc_disk failed: %d\n", rc);
		return rc;
	}
	base_created = true;

	rc = _cpcs_smoke_write_bdev_blocks(base_name, off, values, sizeof(values));
	if (rc != 0) {
		SPDK_ERRLOG("Failed to seed vSLM backing bdev: %d\n", rc);
		goto out;
	}

	bdev_vslm_opts_init(&vslm_opts);
	vslm_opts.nsid = vslm_nsid;
	rc = bdev_vslm_create_with_opts(vslm_name, base_name, 8 * 1024, &vslm_opts);
	if (rc != 0) {
		SPDK_ERRLOG("bdev_vslm_create_with_opts failed: %d\n", rc);
		goto out;
	}
	vslm_created = true;

	rc = bdev_vslm_reset_stats(vslm_name);
	if (rc != 0) {
		SPDK_ERRLOG("bdev_vslm_reset_stats failed: %d\n", rc);
		goto out;
	}

	range.mnsid = vslm_nsid;
	range.starting_byte = 0;
	range.length = 4096;

	memset(&desc, 0, sizeof(desc));
	to_le64(&desc.mr_id, 1);
	to_le64(&desc.off, off);
	to_le64(&desc.len, sizeof(values));

	ctx.program = prog;
	ctx.inline_ranges = &range;
	ctx.inline_range_count = 1;
	ctx.data_buffer = &desc;
	ctx.data_len = sizeof(desc);

	cpcs_builtin_runtime_reset_stats();
	cpcs_builtin_runtime_get_stats(&builtin_before);
	rc = bdev_vslm_get_stats(vslm_name, &vslm_before);
	if (rc != 0) {
		SPDK_ERRLOG("bdev_vslm_get_stats before execute failed: %d\n", rc);
		goto out;
	}

	rc = cpcs_execute_run(&ctx);
	if (rc != 0) {
		SPDK_ERRLOG("vSLM cold sum64 execute failed: %d\n", rc);
		goto out;
	}
	if (ctx.return_value != expected_sum) {
		rc = -EINVAL;
		SPDK_ERRLOG("vSLM cold sum64 mismatch: expected=%llu got=%llu\n",
			    (unsigned long long)expected_sum,
			    (unsigned long long)ctx.return_value);
		goto out;
	}

	cpcs_builtin_runtime_get_stats(&builtin_after);
	rc = bdev_vslm_get_stats(vslm_name, &vslm_after);
	if (rc != 0) {
		SPDK_ERRLOG("bdev_vslm_get_stats after execute failed: %d\n", rc);
		goto out;
	}

	if (builtin_after.sync_wait_total != builtin_before.sync_wait_total) {
		rc = -EINVAL;
		SPDK_ERRLOG("Builtin sync wait counter advanced: before=%llu after=%llu\n",
			    (unsigned long long)builtin_before.sync_wait_total,
			    (unsigned long long)builtin_after.sync_wait_total);
		goto out;
	}
	if (vslm_after.perf_sync_base_io_total != vslm_before.perf_sync_base_io_total ||
	    vslm_after.perf_sync_read_desc_io_total != vslm_before.perf_sync_read_desc_io_total) {
		rc = -EINVAL;
		SPDK_ERRLOG("vSLM sync I/O counters advanced: base %llu->%llu desc %llu->%llu\n",
			    (unsigned long long)vslm_before.perf_sync_base_io_total,
			    (unsigned long long)vslm_after.perf_sync_base_io_total,
			    (unsigned long long)vslm_before.perf_sync_read_desc_io_total,
			    (unsigned long long)vslm_after.perf_sync_read_desc_io_total);
		goto out;
	}
	if (vslm_after.page_faults <= vslm_before.page_faults) {
		rc = -EINVAL;
		SPDK_ERRLOG("vSLM cold execute did not fault a page: before=%llu after=%llu\n",
			    (unsigned long long)vslm_before.page_faults,
			    (unsigned long long)vslm_after.page_faults);
		goto out;
	}

out:
	if (vslm_created) {
		cleanup_rc = _cpcs_smoke_delete_vslm(vslm_name);
		if (cleanup_rc != 0 && rc == 0) {
			rc = cleanup_rc;
		}
	}
	if (base_created) {
		cleanup_rc = _cpcs_smoke_delete_malloc(base_name);
		if (cleanup_rc != 0 && rc == 0) {
			rc = cleanup_rc;
		}
	}

	return rc;
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

	rc = _run_vslm_cold_sum64(ns);
	if (rc != 0) {
		SPDK_ERRLOG("vSLM cold sum64 FAILED: %d\n", rc);
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

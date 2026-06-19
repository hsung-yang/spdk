/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2026 Samsung Electronics Co., Ltd.
 * All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk/log.h"
#include "spdk/endian.h"

#include "nvmf/cpcs/builtin_runtime.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf_cpcs_builtin_runtime_ut)

enum ut_pin_payload {
	UT_PIN_PAYLOAD_NONE = 0,
	UT_PIN_PAYLOAD_SRC,
	UT_PIN_PAYLOAD_DST,
	UT_PIN_PAYLOAD_FILL,
	UT_PIN_PAYLOAD_REDUCE,
};

static struct spdk_thread *g_ut_thread = (struct spdk_thread *)0x1;
static struct spdk_bdev g_src_bdev;
static struct spdk_bdev g_dst_bdev;
static struct spdk_bdev g_shared_bdev;

static uint8_t g_src_bytes[4096];
static uint8_t g_dst_bytes[4096];
static uint8_t g_fill_bytes[4096];
static uint64_t g_reduce_words[256];

static uint64_t g_ticks;
static int g_thread_send_msg_rc;
static uint32_t g_thread_send_msg_calls;
static spdk_msg_fn g_last_send_msg_fn;
static void *g_last_send_msg_ctx;

static int g_exec_read_sync_rc;
static int g_exec_write_sync_rc;
static int g_exec_read_async_rc;
static int g_exec_write_async_rc;
static bool g_exec_read_async_invoke_cb;
static bool g_exec_write_async_invoke_cb;
static uint32_t g_exec_read_sync_calls;
static uint32_t g_exec_write_sync_calls;
static uint32_t g_exec_read_async_calls;
static uint32_t g_exec_write_async_calls;
static uint64_t g_last_exec_read_offset;
static uint64_t g_last_exec_read_length;
static uint64_t g_last_exec_write_offset;
static uint64_t g_last_exec_write_length;
static const uint8_t *g_exec_read_async_payload;
static uint64_t g_exec_read_async_payload_base_offset;
static uint64_t g_exec_read_async_payload_len;
static uint8_t g_last_exec_write_buf[512];
static uint64_t g_last_exec_write_buf_len;

static int g_pin_rc_seq[8];
static enum ut_pin_payload g_pin_payload_seq[8];
static uint32_t g_pin_seq_len;
static uint32_t g_pin_seq_idx;
static uint32_t g_pin_calls;
static uint32_t g_pin_last_max_entries;
static bool g_pin_last_for_write;
static uint64_t g_pin_last_offset;
static uint64_t g_pin_last_length;
static struct spdk_bdev *g_pin_last_bdev;

static int g_unpin_rc;
static uint32_t g_unpin_calls;
static bool g_last_unpin_dirtied;
static uint32_t g_last_unpin_entry_count;
static struct spdk_bdev *g_last_unpin_bdev;

static int
ut_next_pin_rc(enum ut_pin_payload *payload_out)
{
	int rc = 0;

	if (payload_out != NULL) {
		*payload_out = UT_PIN_PAYLOAD_NONE;
	}

	if (g_pin_seq_idx < g_pin_seq_len) {
		rc = g_pin_rc_seq[g_pin_seq_idx];
		if (payload_out != NULL) {
			*payload_out = g_pin_payload_seq[g_pin_seq_idx];
		}
		g_pin_seq_idx++;
	}

	return rc;
}

static void
ut_reset_stubs(void)
{
	g_ticks = 0;
	g_thread_send_msg_rc = 0;
	g_thread_send_msg_calls = 0;
	g_last_send_msg_fn = NULL;
	g_last_send_msg_ctx = NULL;

	g_exec_read_sync_rc = 0;
	g_exec_write_sync_rc = 0;
	g_exec_read_async_rc = 0;
	g_exec_write_async_rc = 0;
	g_exec_read_async_invoke_cb = false;
	g_exec_write_async_invoke_cb = false;
	g_exec_read_sync_calls = 0;
	g_exec_write_sync_calls = 0;
	g_exec_read_async_calls = 0;
	g_exec_write_async_calls = 0;
	g_last_exec_read_offset = 0;
	g_last_exec_read_length = 0;
	g_last_exec_write_offset = 0;
	g_last_exec_write_length = 0;
	g_exec_read_async_payload = NULL;
	g_exec_read_async_payload_base_offset = 0;
	g_exec_read_async_payload_len = 0;
	memset(g_last_exec_write_buf, 0, sizeof(g_last_exec_write_buf));
	g_last_exec_write_buf_len = 0;

	memset(g_pin_rc_seq, 0, sizeof(g_pin_rc_seq));
	memset(g_pin_payload_seq, 0, sizeof(g_pin_payload_seq));
	g_pin_seq_len = 0;
	g_pin_seq_idx = 0;
	g_pin_calls = 0;
	g_pin_last_max_entries = 0;
	g_pin_last_for_write = false;
	g_pin_last_offset = 0;
	g_pin_last_length = 0;
	g_pin_last_bdev = NULL;

	g_unpin_rc = 0;
	g_unpin_calls = 0;
	g_last_unpin_dirtied = false;
	g_last_unpin_entry_count = 0;
	g_last_unpin_bdev = NULL;

	memset(g_src_bytes, 0, sizeof(g_src_bytes));
	memset(g_dst_bytes, 0, sizeof(g_dst_bytes));
	memset(g_fill_bytes, 0, sizeof(g_fill_bytes));
	memset(g_reduce_words, 0, sizeof(g_reduce_words));
}

static void
ut_set_pin_sequence(int rc0, enum ut_pin_payload payload0,
		    int rc1, enum ut_pin_payload payload1)
{
	g_pin_seq_len = 2;
	g_pin_seq_idx = 0;
	g_pin_rc_seq[0] = rc0;
	g_pin_payload_seq[0] = payload0;
	g_pin_rc_seq[1] = rc1;
	g_pin_payload_seq[1] = payload1;
}

static void
ut_set_single_pin_sequence(int rc, enum ut_pin_payload payload)
{
	g_pin_seq_len = 1;
	g_pin_seq_idx = 0;
	g_pin_rc_seq[0] = rc;
	g_pin_payload_seq[0] = payload;
}

static void
ut_init_common_exec_ctx(struct cpcs_exec_context *exec_ctx,
			struct cpcs_exec_resolved_range *ranges,
			struct spdk_bdev *src_bdev, struct spdk_bdev *dst_bdev,
			uint32_t src_len, uint32_t dst_len)
{
	memset(exec_ctx, 0, sizeof(*exec_ctx));
	memset(ranges, 0, 2 * sizeof(*ranges));

	ranges[0].bdev = src_bdev;
	ranges[0].mnsid = 1;
	ranges[0].starting_byte = 0;
	ranges[0].length = src_len;

	ranges[1].bdev = dst_bdev;
	ranges[1].mnsid = 2;
	ranges[1].starting_byte = 0;
	ranges[1].length = dst_len;

	exec_ctx->resolved_ranges = ranges;
	exec_ctx->resolved_range_count = 2;
}

struct spdk_thread *
spdk_get_thread(void)
{
	return g_ut_thread;
}

int
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx)
{
	(void)thread;

	g_thread_send_msg_calls++;
	g_last_send_msg_fn = fn;
	g_last_send_msg_ctx = ctx;

	return g_thread_send_msg_rc;
}

struct spdk_thread *
spdk_thread_create(const char *name, const struct spdk_cpuset *cpumask)
{
	(void)name;
	(void)cpumask;
	return g_ut_thread;
}

int
spdk_thread_exit(struct spdk_thread *thread)
{
	(void)thread;
	return 0;
}

int
spdk_thread_poll(struct spdk_thread *thread, uint32_t max_msgs, uint64_t now)
{
	(void)thread;
	(void)max_msgs;
	(void)now;
	return 0;
}

uint64_t
spdk_get_ticks(void)
{
	return ++g_ticks;
}

uint64_t
spdk_get_ticks_hz(void)
{
	return 1000000ULL;
}

struct spdk_bdev *
spdk_bdev_first(void)
{
	return NULL;
}

struct spdk_bdev *
spdk_bdev_next(struct spdk_bdev *prev)
{
	(void)prev;
	return NULL;
}

uint32_t
spdk_bdev_get_nvme_nsid(struct spdk_bdev *bdev)
{
	(void)bdev;
	return 0;
}

int
cpcs_runtime_register(const struct cpcs_runtime_ops *ops)
{
	(void)ops;
	return 0;
}

int
bdev_slm_exec_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			   uint64_t length, void *buf)
{
	(void)bdev;
	(void)offset;
	(void)length;
	(void)buf;

	g_exec_read_sync_calls++;
	return g_exec_read_sync_rc;
}

int
bdev_slm_exec_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			    uint64_t length, const void *buf)
{
	(void)bdev;
	(void)offset;
	(void)length;
	(void)buf;

	g_exec_write_sync_calls++;
	return g_exec_write_sync_rc;
}

int
bdev_slm_exec_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
				 uint64_t length, void *buf,
				 spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	uint64_t payload_off;

	(void)bdev;

	g_exec_read_async_calls++;
	g_last_exec_read_offset = offset;
	g_last_exec_read_length = length;

	if (g_exec_read_async_rc == 0 && buf != NULL && length != 0) {
		if (g_exec_read_async_payload != NULL &&
		    offset >= g_exec_read_async_payload_base_offset) {
			payload_off = offset - g_exec_read_async_payload_base_offset;
			if (payload_off <= g_exec_read_async_payload_len &&
			    length <= g_exec_read_async_payload_len - payload_off) {
				memcpy(buf, g_exec_read_async_payload + payload_off, length);
			} else {
				memset(buf, 0xA5, length);
			}
		} else {
			memset(buf, 0xA5, length);
		}
	}

	if (g_exec_read_async_invoke_cb && cb_fn != NULL) {
		cb_fn(cb_arg, g_exec_read_async_rc);
	}

	return g_exec_read_async_rc;
}

int
bdev_slm_exec_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
				  uint64_t length, const void *buf,
				  spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	(void)bdev;
	(void)buf;

	g_exec_write_async_calls++;
	g_last_exec_write_offset = offset;
	g_last_exec_write_length = length;
	g_last_exec_write_buf_len = spdk_min(length, (uint64_t)sizeof(g_last_exec_write_buf));
	if (buf != NULL && g_last_exec_write_buf_len != 0) {
		memcpy(g_last_exec_write_buf, buf, g_last_exec_write_buf_len);
	}

	if (g_exec_write_async_invoke_cb && cb_fn != NULL) {
		cb_fn(cb_arg, g_exec_write_async_rc);
	}

	return g_exec_write_async_rc;
}

static int
ut_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
		     uint64_t length, bool for_write,
		     struct spdk_bdev_slm_sg_entry *entries,
		     uint32_t max_entries, uint32_t *entry_count)
{
	enum ut_pin_payload payload = UT_PIN_PAYLOAD_NONE;
	void *addr = NULL;
	int rc;

	g_pin_calls++;
	g_pin_last_bdev = bdev;
	g_pin_last_offset = offset;
	g_pin_last_length = length;
	g_pin_last_for_write = for_write;
	g_pin_last_max_entries = max_entries;

	if (entries == NULL || entry_count == NULL || max_entries == 0) {
		return -EINVAL;
	}

	rc = ut_next_pin_rc(&payload);
	if (rc != 0) {
		return rc;
	}

	switch (payload) {
	case UT_PIN_PAYLOAD_SRC:
		addr = g_src_bytes;
		break;
	case UT_PIN_PAYLOAD_DST:
		addr = g_dst_bytes;
		break;
	case UT_PIN_PAYLOAD_FILL:
		addr = g_fill_bytes;
		break;
	case UT_PIN_PAYLOAD_REDUCE:
		addr = g_reduce_words;
		break;
	default:
		addr = for_write ? g_dst_bytes : g_src_bytes;
		break;
	}

	entries[0].addr = addr;
	entries[0].logical_offset = offset;
	entries[0].len = (uint32_t)length;
	entries[0].vpn = 0;
	entries[0].ppn = 0;
	entries[0].dirtied = false;
	*entry_count = 1;

	return 0;
}

int
bdev_slm_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			   uint64_t length, bool for_write,
			   struct spdk_bdev_slm_sg_entry *entries,
			   uint32_t max_entries, uint32_t *entry_count)
{
	return ut_pin_range_by_bdev(bdev, offset, length, for_write,
				    entries, max_entries, entry_count);
}

int
bdev_slm_try_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			       uint64_t length, bool for_write,
			       struct spdk_bdev_slm_sg_entry *entries,
			       uint32_t max_entries, uint32_t *entry_count)
{
	return ut_pin_range_by_bdev(bdev, offset, length, for_write,
				    entries, max_entries, entry_count);
}

int
bdev_slm_unpin_range_by_bdev(struct spdk_bdev *bdev,
			     const struct spdk_bdev_slm_sg_entry *entries,
			     uint32_t entry_count, bool dirtied)
{
	(void)entries;

	g_unpin_calls++;
	g_last_unpin_bdev = bdev;
	g_last_unpin_entry_count = entry_count;
	g_last_unpin_dirtied = dirtied;

	return g_unpin_rc;
}

static void
ut_done_cb(void *cb_arg, int status, uint64_t return_value)
{
	(void)cb_arg;
	(void)status;
	(void)return_value;
}

struct ut_done_capture {
	bool done;
	int status;
	uint64_t return_value;
};

static void
ut_capture_done_cb(void *cb_arg, int status, uint64_t return_value)
{
	struct ut_done_capture *capture = cb_arg;

	capture->done = true;
	capture->status = status;
	capture->return_value = return_value;
}

static void
test_builtin_memcpy_sg_fast_path(void)
{
	struct cpcs_exec_resolved_range ranges[2];
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_program prog = {};
	struct cpcs_builtin_async_exec_ctx ctx = {};
	struct cpcs_builtin_memcpy_desc desc = {};
	uint64_t i;
	int rc;

	ut_reset_stubs();

	for (i = 0; i < 64; i++) {
		g_src_bytes[i] = (uint8_t)(0x30 + i);
	}
	memset(g_dst_bytes, 0, 64);

	ut_init_common_exec_ctx(&exec_ctx, ranges, &g_src_bdev, &g_dst_bdev, 512, 512);

	to_le64(&desc.src_mr_id, 1);
	to_le64(&desc.src_off, 0);
	to_le64(&desc.dst_mr_id, 2);
	to_le64(&desc.dst_off, 0);
	to_le64(&desc.len, 64);

	exec_ctx.data_buffer = &desc;
	exec_ctx.data_len = sizeof(desc);

	memset(&prog, 0, sizeof(prog));
	prog.pind = CPCS_BUILTIN_PIND_MEMCPY;

	ctx.prog = &prog;
	ctx.exec_ctx = &exec_ctx;
	ctx.done_cb = ut_done_cb;
	ctx.done_arg = NULL;

	rc = builtin_async_ctx_init(&ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	ut_set_pin_sequence(0, UT_PIN_PAYLOAD_SRC, 0, UT_PIN_PAYLOAD_DST);
	rc = builtin_async_step(&ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.processed == 64);
	CU_ASSERT(ctx.chunk_len == 64);
	CU_ASSERT(g_pin_calls == 2);
	CU_ASSERT(g_unpin_calls == 2);
	CU_ASSERT(g_exec_read_async_calls == 0);
	CU_ASSERT(g_exec_write_async_calls == 0);
	CU_ASSERT(g_thread_send_msg_calls == 1);
	CU_ASSERT(memcmp(g_src_bytes, g_dst_bytes, 64) == 0);

	free(ctx.buf);
}

static void
test_builtin_memcpy_fallback_on_pin_unsupported(void)
{
	struct cpcs_exec_resolved_range ranges[2];
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_program prog = {};
	struct cpcs_builtin_async_exec_ctx ctx = {};
	struct cpcs_builtin_memcpy_desc desc = {};
	int rc;

	ut_reset_stubs();

	ut_init_common_exec_ctx(&exec_ctx, ranges, &g_src_bdev, &g_dst_bdev, 512, 512);

	to_le64(&desc.src_mr_id, 1);
	to_le64(&desc.src_off, 4);
	to_le64(&desc.dst_mr_id, 2);
	to_le64(&desc.dst_off, 8);
	to_le64(&desc.len, 48);

	exec_ctx.data_buffer = &desc;
	exec_ctx.data_len = sizeof(desc);

	memset(&prog, 0, sizeof(prog));
	prog.pind = CPCS_BUILTIN_PIND_MEMCPY;

	ctx.prog = &prog;
	ctx.exec_ctx = &exec_ctx;
	ctx.done_cb = ut_done_cb;
	ctx.done_arg = NULL;

	rc = builtin_async_ctx_init(&ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	ut_set_single_pin_sequence(-ENOTSUP, UT_PIN_PAYLOAD_NONE);
	rc = builtin_async_step(&ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.processed == 0);
	CU_ASSERT(g_pin_calls == 1);
	CU_ASSERT(g_unpin_calls == 0);
	CU_ASSERT(g_exec_read_async_calls == 1);
	CU_ASSERT(g_exec_write_async_calls == 0);
	CU_ASSERT(g_last_exec_read_offset == 4);
	CU_ASSERT(g_last_exec_read_length == 48);

	free(ctx.buf);
}

static void
test_builtin_memcpy_fallback_on_try_pin_would_fault(void)
{
	struct cpcs_exec_resolved_range ranges[2];
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_program prog = {};
	struct cpcs_builtin_async_exec_ctx ctx = {};
	struct cpcs_builtin_memcpy_desc desc = {};
	int rc;

	ut_reset_stubs();

	ut_init_common_exec_ctx(&exec_ctx, ranges, &g_src_bdev, &g_dst_bdev, 512, 512);

	to_le64(&desc.src_mr_id, 1);
	to_le64(&desc.src_off, 12);
	to_le64(&desc.dst_mr_id, 2);
	to_le64(&desc.dst_off, 16);
	to_le64(&desc.len, 40);

	exec_ctx.data_buffer = &desc;
	exec_ctx.data_len = sizeof(desc);

	memset(&prog, 0, sizeof(prog));
	prog.pind = CPCS_BUILTIN_PIND_MEMCPY;

	ctx.prog = &prog;
	ctx.exec_ctx = &exec_ctx;
	ctx.done_cb = ut_done_cb;
	ctx.done_arg = NULL;

	rc = builtin_async_ctx_init(&ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	ut_set_single_pin_sequence(-EAGAIN, UT_PIN_PAYLOAD_NONE);
	rc = builtin_async_step(&ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.processed == 0);
	CU_ASSERT(g_pin_calls == 1);
	CU_ASSERT(g_unpin_calls == 0);
	CU_ASSERT(g_exec_read_async_calls == 1);
	CU_ASSERT(g_exec_write_async_calls == 0);
	CU_ASSERT(g_last_exec_read_offset == 12);
	CU_ASSERT(g_last_exec_read_length == 40);

	free(ctx.buf);
}

static void
test_builtin_memcpy_overlap_forces_fallback(void)
{
	struct cpcs_exec_resolved_range ranges[2];
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_program prog = {};
	struct cpcs_builtin_async_exec_ctx ctx = {};
	struct cpcs_builtin_memcpy_desc desc = {};
	int rc;

	ut_reset_stubs();

	ut_init_common_exec_ctx(&exec_ctx, ranges, &g_shared_bdev, &g_shared_bdev, 1024, 1024);

	to_le64(&desc.src_mr_id, 1);
	to_le64(&desc.src_off, 8);
	to_le64(&desc.dst_mr_id, 2);
	to_le64(&desc.dst_off, 16);
	to_le64(&desc.len, 32);

	exec_ctx.data_buffer = &desc;
	exec_ctx.data_len = sizeof(desc);

	memset(&prog, 0, sizeof(prog));
	prog.pind = CPCS_BUILTIN_PIND_MEMCPY;

	ctx.prog = &prog;
	ctx.exec_ctx = &exec_ctx;
	ctx.done_cb = ut_done_cb;
	ctx.done_arg = NULL;

	rc = builtin_async_ctx_init(&ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	ut_set_pin_sequence(0, UT_PIN_PAYLOAD_SRC, 0, UT_PIN_PAYLOAD_DST);
	rc = builtin_async_step(&ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.processed == 0);
	CU_ASSERT(g_pin_calls == 2);
	CU_ASSERT(g_unpin_calls == 2);
	CU_ASSERT(g_exec_read_async_calls == 1);
	CU_ASSERT(g_exec_write_async_calls == 0);
	CU_ASSERT(g_last_exec_read_offset == 8);
	CU_ASSERT(g_last_exec_read_length == 32);

	free(ctx.buf);
}

static void
test_builtin_memfill_sg_fast_path(void)
{
	struct cpcs_exec_resolved_range ranges[2];
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_program prog = {};
	struct cpcs_builtin_async_exec_ctx ctx = {};
	struct cpcs_builtin_memfill_desc desc = {};
	uint64_t i;
	int rc;

	ut_reset_stubs();

	memset(g_fill_bytes, 0x11, 96);
	ut_init_common_exec_ctx(&exec_ctx, ranges, &g_src_bdev, &g_dst_bdev, 1024, 1024);

	to_le64(&desc.mr_id, 2);
	to_le64(&desc.off, 64);
	to_le64(&desc.len, 96);
	desc.pattern = 0xCC;

	exec_ctx.data_buffer = &desc;
	exec_ctx.data_len = sizeof(desc);

	memset(&prog, 0, sizeof(prog));
	prog.pind = CPCS_BUILTIN_PIND_MEMFILL;

	ctx.prog = &prog;
	ctx.exec_ctx = &exec_ctx;
	ctx.done_cb = ut_done_cb;
	ctx.done_arg = NULL;

	rc = builtin_async_ctx_init(&ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	ut_set_single_pin_sequence(0, UT_PIN_PAYLOAD_FILL);
	rc = builtin_async_step(&ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.processed == 96);
	CU_ASSERT(g_pin_calls == 1);
	CU_ASSERT(g_unpin_calls == 1);
	CU_ASSERT(g_last_unpin_dirtied == true);
	CU_ASSERT(g_exec_write_async_calls == 0);
	for (i = 0; i < 96; i++) {
		CU_ASSERT(g_fill_bytes[i] == 0xCC);
	}

	free(ctx.buf);
}

static void
test_builtin_memfill_fallback_on_try_pin_would_fault(void)
{
	struct cpcs_exec_resolved_range ranges[2];
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_program prog = {};
	struct cpcs_builtin_async_exec_ctx ctx = {};
	struct cpcs_builtin_memfill_desc desc = {};
	int rc;

	ut_reset_stubs();

	ut_init_common_exec_ctx(&exec_ctx, ranges, &g_src_bdev, &g_dst_bdev, 1024, 1024);

	to_le64(&desc.mr_id, 2);
	to_le64(&desc.off, 64);
	to_le64(&desc.len, 96);
	desc.pattern = 0xCC;

	exec_ctx.data_buffer = &desc;
	exec_ctx.data_len = sizeof(desc);

	memset(&prog, 0, sizeof(prog));
	prog.pind = CPCS_BUILTIN_PIND_MEMFILL;

	ctx.prog = &prog;
	ctx.exec_ctx = &exec_ctx;
	ctx.done_cb = ut_done_cb;
	ctx.done_arg = NULL;

	rc = builtin_async_ctx_init(&ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	ut_set_single_pin_sequence(-EAGAIN, UT_PIN_PAYLOAD_NONE);
	rc = builtin_async_step(&ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.processed == 0);
	CU_ASSERT(g_pin_calls == 1);
	CU_ASSERT(g_unpin_calls == 0);
	CU_ASSERT(g_exec_write_async_calls == 1);
	CU_ASSERT(g_last_exec_write_offset == 64);
	CU_ASSERT(g_last_exec_write_length == 96);

	free(ctx.buf);
}

static void
test_builtin_sum64_sg_fast_path(void)
{
	struct cpcs_exec_resolved_range ranges[2];
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_program prog = {};
	struct cpcs_builtin_async_exec_ctx ctx = {};
	struct cpcs_builtin_sum64_desc desc = {};
	int rc;

	ut_reset_stubs();

	g_reduce_words[0] = 10;
	g_reduce_words[1] = 20;
	g_reduce_words[2] = 30;
	g_reduce_words[3] = 40;

	ut_init_common_exec_ctx(&exec_ctx, ranges, &g_src_bdev, &g_dst_bdev, 1024, 1024);

	to_le64(&desc.mr_id, 1);
	to_le64(&desc.off, 0);
	to_le64(&desc.len, 4 * sizeof(uint64_t));

	exec_ctx.data_buffer = &desc;
	exec_ctx.data_len = sizeof(desc);

	memset(&prog, 0, sizeof(prog));
	prog.pind = CPCS_BUILTIN_PIND_SUM64;

	ctx.prog = &prog;
	ctx.exec_ctx = &exec_ctx;
	ctx.done_cb = ut_done_cb;
	ctx.done_arg = NULL;

	rc = builtin_async_ctx_init(&ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	ut_set_single_pin_sequence(0, UT_PIN_PAYLOAD_REDUCE);
	rc = builtin_async_step(&ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.processed == 4 * sizeof(uint64_t));
	CU_ASSERT(ctx.return_value == 100);
	CU_ASSERT(g_pin_calls == 1);
	CU_ASSERT(g_unpin_calls == 1);
	CU_ASSERT(g_last_unpin_dirtied == false);
	CU_ASSERT(g_exec_read_async_calls == 0);

	free(ctx.buf);
}

static void
test_builtin_sum64_fallback_on_try_pin_would_fault(void)
{
	struct cpcs_exec_resolved_range ranges[2];
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_program prog = {};
	struct cpcs_builtin_async_exec_ctx ctx = {};
	struct cpcs_builtin_sum64_desc desc = {};
	int rc;

	ut_reset_stubs();

	ut_init_common_exec_ctx(&exec_ctx, ranges, &g_src_bdev, &g_dst_bdev, 1024, 1024);

	to_le64(&desc.mr_id, 1);
	to_le64(&desc.off, 32);
	to_le64(&desc.len, 4 * sizeof(uint64_t));

	exec_ctx.data_buffer = &desc;
	exec_ctx.data_len = sizeof(desc);

	memset(&prog, 0, sizeof(prog));
	prog.pind = CPCS_BUILTIN_PIND_SUM64;

	ctx.prog = &prog;
	ctx.exec_ctx = &exec_ctx;
	ctx.done_cb = ut_done_cb;
	ctx.done_arg = NULL;

	rc = builtin_async_ctx_init(&ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	ut_set_single_pin_sequence(-EAGAIN, UT_PIN_PAYLOAD_NONE);
	rc = builtin_async_step(&ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.processed == 0);
	CU_ASSERT(g_pin_calls == 1);
	CU_ASSERT(g_unpin_calls == 0);
	CU_ASSERT(g_exec_read_async_calls == 1);
	CU_ASSERT(g_last_exec_read_offset == 32);
	CU_ASSERT(g_last_exec_read_length == 4 * sizeof(uint64_t));

	free(ctx.buf);
}

static void
test_builtin_filter_agg_uses_async_io(void)
{
	struct cpcs_exec_resolved_range ranges[2];
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_program prog = {};
	struct cpcs_builtin_extended_exec_ctx *worker_ctx;
	struct ut_done_capture done = {};
	struct cpcs_builtin_metadata_record records[2] = {};
	struct spdk_cpcs_builtin_filter_agg_result *result;
	struct {
		struct spdk_cpcs_builtin_filter_agg_req req;
		struct spdk_cpcs_builtin_eval_filter_clause clause;
	} req_buf = {};
	int rc;

	ut_reset_stubs();

	to_le32(&records[0].category_id, 7);
	to_le32(&records[1].category_id, 8);

	ut_init_common_exec_ctx(&exec_ctx, ranges, &g_src_bdev, &g_dst_bdev,
				sizeof(records), sizeof(g_last_exec_write_buf));
	to_le32(&req_buf.req.hdr.version, SPDK_CPCS_BUILTIN_EVAL_ABI_VERSION);
	to_le16(&req_buf.req.hdr.opcode, SPDK_CPCS_BUILTIN_OP_FILTER_AGG);
	to_le64(&req_buf.req.hdr.output_mr_id, 2);
	to_le64(&req_buf.req.hdr.output_offset, 128);
	to_le32(&req_buf.req.hdr.output_length,
		sizeof(struct spdk_cpcs_builtin_filter_agg_result));
	to_le64(&req_buf.req.metadata_mr_id, 1);
	to_le64(&req_buf.req.metadata_offset, 0);
	to_le64(&req_buf.req.record_count, 2);
	to_le32(&req_buf.req.metadata_stride, sizeof(struct cpcs_builtin_metadata_record));
	to_le16(&req_buf.req.filter_count, 1);
	req_buf.req.agg_op = SPDK_CPCS_BUILTIN_AGG_COUNT;
	req_buf.req.agg_field_id = SPDK_CPCS_BUILTIN_FIELD_CATEGORY_ID;
	req_buf.clause.op = SPDK_CPCS_BUILTIN_FILTER_EQ_U32;
	req_buf.clause.field_id = SPDK_CPCS_BUILTIN_FIELD_CATEGORY_ID;
	to_le32(&req_buf.clause.args[0], 7);

	exec_ctx.data_buffer = &req_buf;
	exec_ctx.data_len = sizeof(req_buf);
	g_exec_read_async_payload = (const uint8_t *)records;
	g_exec_read_async_payload_len = sizeof(records);
	g_exec_read_async_invoke_cb = true;
	g_exec_write_async_invoke_cb = true;

	memset(&prog, 0, sizeof(prog));
	prog.pind = CPCS_BUILTIN_PIND_FILTER_AGG;

	worker_ctx = calloc(1, sizeof(*worker_ctx));
	SPDK_CU_ASSERT_FATAL(worker_ctx != NULL);
	worker_ctx->prog = &prog;
	worker_ctx->exec_ctx = &exec_ctx;
	worker_ctx->done_cb = ut_capture_done_cb;
	worker_ctx->done_arg = &done;
	worker_ctx->submit_thread = g_ut_thread;

	rc = _cpcs_builtin_exec_filter_agg_async(worker_ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(done.done);
	CU_ASSERT(done.status == 0);
	CU_ASSERT(done.return_value == 1);
	CU_ASSERT(g_exec_read_sync_calls == 0);
	CU_ASSERT(g_exec_write_sync_calls == 0);
	CU_ASSERT(g_exec_read_async_calls == 2);
	CU_ASSERT(g_exec_write_async_calls == 1);
	CU_ASSERT(g_last_exec_read_offset == sizeof(struct cpcs_builtin_metadata_record));
	CU_ASSERT(g_last_exec_read_length == sizeof(struct cpcs_builtin_metadata_record));
	CU_ASSERT(g_last_exec_write_offset == 128);
	CU_ASSERT(g_last_exec_write_length == sizeof(struct spdk_cpcs_builtin_filter_agg_result));
	CU_ASSERT(g_last_exec_write_buf_len == sizeof(struct spdk_cpcs_builtin_filter_agg_result));

	result = (struct spdk_cpcs_builtin_filter_agg_result *)g_last_exec_write_buf;
	CU_ASSERT(result->hdr.opcode == SPDK_CPCS_BUILTIN_OP_FILTER_AGG);
	CU_ASSERT(result->hdr.matched_count == 1);
	CU_ASSERT(result->hdr.returned_count == 1);
	CU_ASSERT(result->hdr.stats.metadata_records_scanned == 2);
	CU_ASSERT(result->aggregate_u64 == 1);
}

static void
test_builtin_filtered_topk_exact_uses_async_io(void)
{
	struct cpcs_exec_resolved_range ranges[3] = {};
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_program prog = {};
	struct cpcs_builtin_extended_exec_ctx *worker_ctx;
	struct ut_done_capture done = {};
	struct cpcs_builtin_metadata_record *records;
	struct spdk_cpcs_builtin_filtered_topk_exact_result *result;
	uint8_t media[2048] = {};
	float *vectors;
	struct {
		struct spdk_cpcs_builtin_filtered_topk_exact_req req;
		float query[2];
	} req_buf = {};
	int rc;

	ut_reset_stubs();

	records = (struct cpcs_builtin_metadata_record *)media;
	to_le64(&records[0].doc_id, 10);
	to_le32(&records[0].vector_index, 0);
	to_le64(&records[1].doc_id, 11);
	to_le32(&records[1].vector_index, 1);
	vectors = (float *)(media + 1024);
	vectors[0] = 3.0f;
	vectors[1] = 4.0f;
	vectors[2] = 1.0f;
	vectors[3] = 0.0f;

	ranges[0].bdev = &g_src_bdev;
	ranges[0].mnsid = 1;
	ranges[0].starting_byte = 0;
	ranges[0].length = sizeof(struct cpcs_builtin_metadata_record) * 2;
	ranges[1].bdev = &g_src_bdev;
	ranges[1].mnsid = 2;
	ranges[1].starting_byte = 1024;
	ranges[1].length = 4 * sizeof(float);
	ranges[2].bdev = &g_dst_bdev;
	ranges[2].mnsid = 3;
	ranges[2].starting_byte = 4096;
	ranges[2].length = sizeof(g_last_exec_write_buf);
	exec_ctx.resolved_ranges = ranges;
	exec_ctx.resolved_range_count = SPDK_COUNTOF(ranges);

	to_le32(&req_buf.req.hdr.version, SPDK_CPCS_BUILTIN_EVAL_ABI_VERSION);
	to_le16(&req_buf.req.hdr.opcode, SPDK_CPCS_BUILTIN_OP_FILTERED_TOPK_EXACT);
	to_le64(&req_buf.req.hdr.output_mr_id, 3);
	to_le64(&req_buf.req.hdr.output_offset, 64);
	to_le32(&req_buf.req.hdr.output_length,
		sizeof(struct spdk_cpcs_builtin_filtered_topk_exact_result) +
		sizeof(struct spdk_cpcs_builtin_topk_record));
	to_le64(&req_buf.req.metadata_mr_id, 1);
	to_le64(&req_buf.req.metadata_offset, 0);
	to_le64(&req_buf.req.vector_mr_id, 2);
	to_le64(&req_buf.req.vector_offset, 0);
	to_le64(&req_buf.req.record_count, 2);
	to_le32(&req_buf.req.metadata_stride, sizeof(struct cpcs_builtin_metadata_record));
	to_le32(&req_buf.req.vector_dim, 2);
	to_le32(&req_buf.req.vector_stride, 2 * sizeof(float));
	to_le16(&req_buf.req.metric, SPDK_CPCS_BUILTIN_METRIC_L2);
	to_le16(&req_buf.req.k, 1);
	to_le32(&req_buf.req.query_vector_bytes, 2 * sizeof(float));
	req_buf.query[0] = 0.0f;
	req_buf.query[1] = 0.0f;

	exec_ctx.data_buffer = &req_buf;
	exec_ctx.data_len = sizeof(req_buf);
	g_exec_read_async_payload = media;
	g_exec_read_async_payload_len = sizeof(media);
	g_exec_read_async_invoke_cb = true;
	g_exec_write_async_invoke_cb = true;

	memset(&prog, 0, sizeof(prog));
	prog.pind = CPCS_BUILTIN_PIND_FILTERED_TOPK_EXACT;

	worker_ctx = calloc(1, sizeof(*worker_ctx));
	SPDK_CU_ASSERT_FATAL(worker_ctx != NULL);
	worker_ctx->prog = &prog;
	worker_ctx->exec_ctx = &exec_ctx;
	worker_ctx->done_cb = ut_capture_done_cb;
	worker_ctx->done_arg = &done;
	worker_ctx->submit_thread = g_ut_thread;

	rc = _cpcs_builtin_exec_filtered_topk_exact_async(worker_ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(done.done);
	CU_ASSERT(done.status == 0);
	CU_ASSERT(done.return_value == 1);
	CU_ASSERT(g_exec_read_sync_calls == 0);
	CU_ASSERT(g_exec_write_sync_calls == 0);
	CU_ASSERT(g_exec_read_async_calls == 4);
	CU_ASSERT(g_exec_write_async_calls == 1);
	CU_ASSERT(g_last_exec_read_offset == 1024 + (2 * sizeof(float)));
	CU_ASSERT(g_last_exec_read_length == 2 * sizeof(float));
	CU_ASSERT(g_last_exec_write_offset == 4096 + 64);
	CU_ASSERT(g_last_exec_write_length ==
		  sizeof(struct spdk_cpcs_builtin_filtered_topk_exact_result) +
		  sizeof(struct spdk_cpcs_builtin_topk_record));

	result = (struct spdk_cpcs_builtin_filtered_topk_exact_result *)g_last_exec_write_buf;
	CU_ASSERT(result->hdr.opcode == SPDK_CPCS_BUILTIN_OP_FILTERED_TOPK_EXACT);
	CU_ASSERT(result->hdr.matched_count == 2);
	CU_ASSERT(result->hdr.scored_count == 2);
	CU_ASSERT(result->hdr.returned_count == 1);
	CU_ASSERT(result->hdr.stats.metadata_records_scanned == 2);
	CU_ASSERT(result->hdr.stats.vectors_scored == 2);
	CU_ASSERT(result->records[0].doc_id == 11);
}

static void
test_builtin_kv_output_uses_async_write(void)
{
	static const char extra[] = "{\"output_mr_id\":2,\"output_offset\":96,\"output_length\":3}";
	static const char payload[] = "abcdef";
	struct cpcs_exec_resolved_range ranges[2];
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_program prog = {};
	struct cpcs_builtin_extended_exec_ctx *worker_ctx;
	struct ut_done_capture done = {};
	uint8_t req_buf[sizeof(struct cpcs_builtin_kv_req_header) +
				      sizeof(extra) - 1 + sizeof(payload) - 1] = {};
	struct cpcs_builtin_kv_req_header *hdr;
	uint8_t *cursor;
	int rc;

	ut_reset_stubs();

	ut_init_common_exec_ctx(&exec_ctx, ranges, &g_src_bdev, &g_dst_bdev,
				1024, sizeof(g_last_exec_write_buf));

	hdr = (struct cpcs_builtin_kv_req_header *)req_buf;
	memcpy(hdr->magic, CPCS_BUILTIN_KV_MAGIC, sizeof(hdr->magic));
	to_le32(&hdr->version, CPCS_BUILTIN_KV_ABI_VERSION);
	to_le32(&hdr->op, CPCS_BUILTIN_KV_OP_PACK_STORE);
	to_le32(&hdr->mode, CPCS_BUILTIN_KV_MODE_NOOP);
	to_le32(&hdr->rank, 0);
	to_le32(&hdr->dtype_len, 0);
	to_le32(&hdr->extra_len, sizeof(extra) - 1);
	to_le64(&hdr->payload_len, sizeof(payload) - 1);
	cursor = req_buf + sizeof(*hdr);
	memcpy(cursor, extra, sizeof(extra) - 1);
	cursor += sizeof(extra) - 1;
	memcpy(cursor, payload, sizeof(payload) - 1);

	exec_ctx.data_buffer = req_buf;
	exec_ctx.data_len = sizeof(req_buf);
	g_exec_write_async_invoke_cb = true;

	memset(&prog, 0, sizeof(prog));
	prog.pind = CPCS_BUILTIN_PIND_KV_PACK_STORE;

	worker_ctx = calloc(1, sizeof(*worker_ctx));
	SPDK_CU_ASSERT_FATAL(worker_ctx != NULL);
	worker_ctx->prog = &prog;
	worker_ctx->exec_ctx = &exec_ctx;
	worker_ctx->done_cb = ut_capture_done_cb;
	worker_ctx->done_arg = &done;
	worker_ctx->submit_thread = g_ut_thread;

	rc = _cpcs_builtin_kv_exec_async(worker_ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(done.done);
	CU_ASSERT(done.status == 0);
	CU_ASSERT((uint32_t)done.return_value == 3);
	CU_ASSERT(g_exec_read_sync_calls == 0);
	CU_ASSERT(g_exec_write_sync_calls == 0);
	CU_ASSERT(g_exec_read_async_calls == 0);
	CU_ASSERT(g_exec_write_async_calls == 1);
	CU_ASSERT(g_last_exec_write_offset == 96);
	CU_ASSERT(g_last_exec_write_length == 3);
	CU_ASSERT(g_last_exec_write_buf_len == 3);
	CU_ASSERT(memcmp(g_last_exec_write_buf, payload, 3) == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("cpcs_builtin_runtime", NULL, NULL);
	CU_ADD_TEST(suite, test_builtin_memcpy_sg_fast_path);
	CU_ADD_TEST(suite, test_builtin_memcpy_fallback_on_pin_unsupported);
	CU_ADD_TEST(suite, test_builtin_memcpy_fallback_on_try_pin_would_fault);
	CU_ADD_TEST(suite, test_builtin_memcpy_overlap_forces_fallback);
	CU_ADD_TEST(suite, test_builtin_memfill_sg_fast_path);
	CU_ADD_TEST(suite, test_builtin_memfill_fallback_on_try_pin_would_fault);
	CU_ADD_TEST(suite, test_builtin_sum64_sg_fast_path);
	CU_ADD_TEST(suite, test_builtin_sum64_fallback_on_try_pin_would_fault);
	CU_ADD_TEST(suite, test_builtin_filter_agg_uses_async_io);
	CU_ADD_TEST(suite, test_builtin_filtered_topk_exact_uses_async_io);
	CU_ADD_TEST(suite, test_builtin_kv_output_uses_async_write);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}

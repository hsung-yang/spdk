/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2025 Samsung Electronics Co., Ltd. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk/log.h"

#include "nvmf/cpcs/program.c"
#include "nvmf/cpcs/builtin_programs.c"
#include "nvmf/cpcs/program_activation.c"
#include "nvmf/cpcs/runtime_stub.c"
#include "nvmf/cpcs/ebpf_runtime.c"
#include "nvmf/cpcs/memory_range_set.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf_cpcs)

static int g_direct_read_rc = -ENOENT;
static int g_direct_write_rc = -ENOENT;
static int g_exec_read_rc;
static int g_exec_write_rc;
static uint32_t g_direct_read_calls;
static uint32_t g_direct_write_calls;
static uint32_t g_exec_read_calls;
static uint32_t g_exec_write_calls;
static struct spdk_bdev *g_last_exec_read_bdev;
static struct spdk_bdev *g_last_exec_write_bdev;
static uint64_t g_last_exec_read_offset;
static uint64_t g_last_exec_write_offset;
static uint64_t g_last_exec_read_len;
static uint64_t g_last_exec_write_len;
static uint8_t g_exec_read_fill = 0x9A;

int
cpcs_builtin_runtime_register(void)
{
	return 0;
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
bdev_slm_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				uint64_t length, void **ptr)
{
	(void)bdev;
	(void)offset;
	(void)length;
	(void)ptr;

	return -ENOENT;
}

int
bdev_slm_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	g_direct_read_calls++;
	(void)bdev;
	(void)offset;
	(void)length;
	(void)buf;

	return g_direct_read_rc;
}

int
bdev_slm_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, const void *buf)
{
	g_direct_write_calls++;
	(void)bdev;
	(void)offset;
	(void)length;
	(void)buf;

	return g_direct_write_rc;
}

int
bdev_slm_exec_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	g_exec_read_calls++;
	g_last_exec_read_bdev = bdev;
	g_last_exec_read_offset = offset;
	g_last_exec_read_len = length;

	if (g_exec_read_rc == 0 && buf != NULL && length != 0) {
		memset(buf, g_exec_read_fill, length);
	}

	return g_exec_read_rc;
}

int
bdev_slm_exec_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
			    const void *buf)
{
	(void)buf;
	g_exec_write_calls++;
	g_last_exec_write_bdev = bdev;
	g_last_exec_write_offset = offset;
	g_last_exec_write_len = length;
	return g_exec_write_rc;
}

static void
reset_exec_io_stubs(void)
{
	g_direct_read_rc = -ENOENT;
	g_direct_write_rc = -ENOENT;
	g_exec_read_rc = 0;
	g_exec_write_rc = 0;
	g_direct_read_calls = 0;
	g_direct_write_calls = 0;
	g_exec_read_calls = 0;
	g_exec_write_calls = 0;
	g_last_exec_read_bdev = NULL;
	g_last_exec_write_bdev = NULL;
	g_last_exec_read_offset = 0;
	g_last_exec_write_offset = 0;
	g_last_exec_read_len = 0;
	g_last_exec_write_len = 0;
}

static void
_ut_ns_init(struct spdk_nvmf_cpcs_ns *ns)
{
	memset(ns, 0, sizeof(*ns));
	pthread_mutex_init(&ns->lock, NULL);
	TAILQ_INIT(&ns->mrs_list);
	ns->next_rsid = 1;
	ns->max_programs = CPCS_MAX_PROGRAMS_PER_NS;
	ns->max_activated = 4;
	ns->load_program_gran = 4; /* 2^4 = 16 bytes */
}

static void
_ut_ns_fini(struct spdk_nvmf_cpcs_ns *ns)
{
	pthread_mutex_destroy(&ns->lock);
}

static void
test_ebpf_user_program(void)
{
	struct spdk_nvmf_cpcs_ns ns;
	struct cpcs_program *prog;
	struct cpcs_exec_context exec_ctx = {};
	const struct cpcs_runtime_ops *ops;
	struct cpcs_ebpf_ctx *rt_ctx;
	uint64_t ret = 0;
	uint16_t pind = 16;
	int rc;

	static const uint8_t prog_bytes[] = {
		0xb7, 0x00, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00, /* r0 = 0x1234 */
		0x95, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  /* exit */
	};

	_ut_ns_init(&ns);
	cpcs_runtime_fini_all();

	rc = cpcs_ebpf_runtime_register();
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = cpcs_program_load(&ns, pind, CPCS_PTYPE_EBPF, SPDK_NVME_CPCS_PIT_PUID,
			       0x1234, sizeof(prog_bytes), 0, sizeof(prog_bytes), prog_bytes);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = cpcs_program_activate(&ns, pind);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	prog = cpcs_program_get(&ns, pind);
	CU_ASSERT_PTR_NOT_NULL_FATAL(prog);

	rt_ctx = (struct cpcs_ebpf_ctx *)prog->runtime;
	CU_ASSERT_PTR_NOT_NULL_FATAL(rt_ctx);
	CU_ASSERT_PTR_NOT_NULL_FATAL(rt_ctx->vm);

	ops = cpcs_runtime_get(CPCS_PTYPE_EBPF);
	CU_ASSERT_PTR_NOT_NULL_FATAL(ops);

	exec_ctx.program = prog;
	rc = ebpf_execute_sync(prog, &exec_ctx, &ret);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ret == 0x1234);

	rc = cpcs_program_deactivate(&ns, pind);
	CU_ASSERT(rc == 0);

	rc = cpcs_program_unload(&ns, pind);
	CU_ASSERT(rc == 0);

	_ut_ns_fini(&ns);
}

static void
test_ebpf_helpers_use_exec_view_access(void)
{
	struct cpcs_exec_context exec_ctx = {};
	struct cpcs_exec_resolved_range range = {};
	struct spdk_bdev bdev = {};
	uint8_t src_data[16];
	uint8_t read_buf[8];
	uint8_t write_buf[8];
	uint64_t rc;
	size_t i;

	reset_exec_io_stubs();

	range.bdev = &bdev;
	range.mnsid = 10;
	range.starting_byte = 256;
	range.length = 64;
	exec_ctx.resolved_ranges = &range;
	exec_ctx.resolved_range_count = 1;

	memset(read_buf, 0, sizeof(read_buf));
	rc = helper_slm_read(&exec_ctx, 1, 8, sizeof(read_buf), (uint64_t)(uintptr_t)read_buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_exec_read_calls == 1);
	CU_ASSERT(g_direct_read_calls == 0);
	CU_ASSERT(g_last_exec_read_bdev == &bdev);
	CU_ASSERT(g_last_exec_read_offset == 264);
	CU_ASSERT(g_last_exec_read_len == sizeof(read_buf));
	CU_ASSERT(read_buf[0] == g_exec_read_fill);

	memset(write_buf, 0x5A, sizeof(write_buf));
	rc = helper_slm_write(&exec_ctx, 1, 4, sizeof(write_buf), (uint64_t)(uintptr_t)write_buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_exec_write_calls == 1);
	CU_ASSERT(g_direct_write_calls == 0);
	CU_ASSERT(g_last_exec_write_bdev == &bdev);
	CU_ASSERT(g_last_exec_write_offset == 260);
	CU_ASSERT(g_last_exec_write_len == sizeof(write_buf));

	for (i = 0; i < sizeof(src_data); i++) {
		src_data[i] = (uint8_t)(0x20 + i);
	}
	exec_ctx.data_buffer = src_data;
	exec_ctx.data_len = sizeof(src_data);

	memset(read_buf, 0, sizeof(read_buf));
	rc = helper_slm_read(&exec_ctx, 0, 3, sizeof(read_buf), (uint64_t)(uintptr_t)read_buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(read_buf[0] == src_data[3]);
	CU_ASSERT(g_exec_read_calls == 1);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("cpcs_ebpf", NULL, NULL);
	CU_ADD_TEST(suite, test_ebpf_user_program);
	CU_ADD_TEST(suite, test_ebpf_helpers_use_exec_view_access);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}

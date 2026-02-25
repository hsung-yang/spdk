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
	(void)bdev;
	(void)offset;
	(void)length;
	(void)buf;

	return -ENOENT;
}

int
bdev_slm_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, const void *buf)
{
	(void)bdev;
	(void)offset;
	(void)length;
	(void)buf;

	return -ENOENT;
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
	CU_ASSERT_FATAL(rc == 0);

	rc = cpcs_program_load(&ns, pind, CPCS_PTYPE_EBPF, SPDK_NVME_CPCS_PIT_PUID,
			       0x1234, sizeof(prog_bytes), 0, sizeof(prog_bytes), prog_bytes);
	CU_ASSERT_FATAL(rc == 0);

	rc = cpcs_program_activate(&ns, pind);
	CU_ASSERT_FATAL(rc == 0);

	prog = cpcs_program_get(&ns, pind);
	CU_ASSERT_PTR_NOT_NULL_FATAL(prog);

	rt_ctx = (struct cpcs_ebpf_ctx *)prog->runtime;
	CU_ASSERT_PTR_NOT_NULL_FATAL(rt_ctx);
	CU_ASSERT_PTR_NOT_NULL_FATAL(rt_ctx->vm);

	ops = cpcs_runtime_get(CPCS_PTYPE_EBPF);
	CU_ASSERT_PTR_NOT_NULL_FATAL(ops);

	exec_ctx.program = prog;
	rc = ops->execute(prog, &exec_ctx, &ret);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ret == 0x1234);

	rc = cpcs_program_deactivate(&ns, pind);
	CU_ASSERT(rc == 0);

	rc = cpcs_program_unload(&ns, pind);
	CU_ASSERT(rc == 0);

	_ut_ns_fini(&ns);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("cpcs_ebpf", NULL, NULL);
	CU_ADD_TEST(suite, test_ebpf_user_program);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}

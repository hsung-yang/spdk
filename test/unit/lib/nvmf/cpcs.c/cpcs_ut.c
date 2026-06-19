/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2025 Samsung Electronics Co., Ltd. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk/log.h"

#include "nvmf/cpcs/program.c"
#include "nvmf/cpcs/builtin_programs.c"
#include "nvmf/cpcs/memory_range_set.c"
#include "nvmf/cpcs/execute.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf_cpcs)

static int g_lease_publish_rc;
static int g_lease_discard_rc;
static int g_lease_release_rc;
static uint64_t g_last_lease_id;
static uint32_t g_publish_calls;
static uint32_t g_discard_calls;
static uint32_t g_release_calls;

const struct cpcs_runtime_ops *
cpcs_runtime_get(uint8_t ptype)
{
	(void)ptype;
	return NULL;
}

bool
cpcs_reachability_is_memory_ns_reachable(struct cpcs_reachability_manager *mgr,
		struct spdk_nvmf_cpcs_ns *compute_ns,
		uint32_t mnsid)
{
	(void)mgr;
	(void)compute_ns;
	(void)mnsid;

	return true;
}

int
spdk_nvmf_request_get_bdev(uint32_t nsid, struct spdk_nvmf_request *req,
			   struct spdk_bdev **bdev, struct spdk_bdev_desc **desc,
			   struct spdk_io_channel **ch)
{
	(void)nsid;
	(void)req;
	(void)bdev;
	(void)desc;
	(void)ch;
	return -EINVAL;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_request_get_subsystem(struct spdk_nvmf_request *req)
{
	(void)req;
	return NULL;
}

size_t
spdk_nvmf_request_copy_to_buf(struct spdk_nvmf_request *req, void *buf, size_t buflen)
{
	(void)req;
	(void)buf;
	(void)buflen;
	return 0;
}

struct spdk_nvme_cmd *
spdk_nvmf_request_get_cmd(struct spdk_nvmf_request *req)
{
	if (req == NULL || req->cmd == NULL) {
		return NULL;
	}

	return &req->cmd->nvme_cmd;
}

struct spdk_nvme_cpl *
spdk_nvmf_request_get_response(struct spdk_nvmf_request *req)
{
	if (req == NULL || req->rsp == NULL) {
		return NULL;
	}

	return &req->rsp->nvme_cpl;
}

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	(void)req;
	return 0;
}

struct spdk_nvmf_cpcs_ns *
spdk_nvmf_cpcs_ns_get_by_nsid(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	(void)subsystem;
	(void)nsid;
	return NULL;
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
bdev_slm_lease_acquire_by_bdev(uint64_t lease_id, struct spdk_bdev *bdev,
			       uint64_t offset, uint64_t length)
{
	(void)lease_id;
	(void)bdev;
	(void)offset;
	(void)length;
	return 0;
}

int
bdev_slm_exec_publish_lease(uint64_t lease_id)
{
	g_publish_calls++;
	g_last_lease_id = lease_id;
	return g_lease_publish_rc;
}

int
bdev_slm_exec_discard_lease(uint64_t lease_id)
{
	g_discard_calls++;
	g_last_lease_id = lease_id;
	return g_lease_discard_rc;
}

int
bdev_slm_lease_release(uint64_t lease_id)
{
	g_release_calls++;
	g_last_lease_id = lease_id;
	return g_lease_release_rc;
}

static void
reset_execute_lease_stubs(void)
{
	g_lease_publish_rc = 0;
	g_lease_discard_rc = 0;
	g_lease_release_rc = 0;
	g_last_lease_id = 0;
	g_publish_calls = 0;
	g_discard_calls = 0;
	g_release_calls = 0;
}

static void
_ut_ns_init(struct spdk_nvmf_cpcs_ns *ns)
{
	memset(ns, 0, sizeof(*ns));
	pthread_mutex_init(&ns->lock, NULL);
	TAILQ_INIT(&ns->mrs_list);
	ns->next_rsid = 1;
}

static void
_ut_ns_fini(struct spdk_nvmf_cpcs_ns *ns)
{
	pthread_mutex_destroy(&ns->lock);
}

static void
test_mrs_validate_overlap(void)
{
	struct spdk_nvmf_cpcs_ns ns;
	struct spdk_nvme_cpcs_memory_range_descriptor ranges[2] = {};
	int rc;

	_ut_ns_init(&ns);
	ns.mrs_granularity = 2;

	ranges[0].mnsid = 10;
	ranges[0].starting_byte = 0;
	ranges[0].length = 8;

	ranges[1].mnsid = 10;
	ranges[1].starting_byte = 4;
	ranges[1].length = 8;

	rc = cpcs_mrs_validate(&ns, ranges, 2);
	CU_ASSERT(rc == -SPDK_NVME_CPCS_SC_OVERLAPPING_MEMORY_RANGES);

	_ut_ns_fini(&ns);
}

static void
test_mrs_validate_alignment(void)
{
	struct spdk_nvmf_cpcs_ns ns;
	struct spdk_nvme_cpcs_memory_range_descriptor ranges[1] = {};
	int rc;

	_ut_ns_init(&ns);
	ns.mrs_granularity = 3;

	ranges[0].mnsid = 11;
	ranges[0].starting_byte = 3;
	ranges[0].length = 8;

	rc = cpcs_mrs_validate(&ns, ranges, 1);
	CU_ASSERT(rc == -SPDK_NVME_CPCS_SC_INVALID_MEMORY_RANGE_SET);

	ranges[0].starting_byte = 8;
	ranges[0].length = 6;

	rc = cpcs_mrs_validate(&ns, ranges, 1);
	CU_ASSERT(rc == -SPDK_NVME_CPCS_SC_INVALID_MEMORY_RANGE_SET);

	_ut_ns_fini(&ns);
}

static void
test_mrs_create_delete(void)
{
	struct spdk_nvmf_cpcs_ns ns;
	struct spdk_nvme_cpcs_memory_range_descriptor ranges[2] = {};
	uint16_t rsid = 0;
	int rc;

	_ut_ns_init(&ns);
	ns.max_mrs = 4;
	ns.max_ranges_per_mrs = 4;
	ns.mrs_granularity = 2;

	ranges[0].mnsid = 12;
	ranges[0].starting_byte = 0;
	ranges[0].length = 8;

	ranges[1].mnsid = 12;
	ranges[1].starting_byte = 16;
	ranges[1].length = 8;

	rc = cpcs_mrs_create(&ns, ranges, 2, &rsid);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rsid == 1);
	CU_ASSERT(ns.mrs_count == 1);

	rc = cpcs_mrs_delete(&ns, rsid);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ns.mrs_count == 0);

	_ut_ns_fini(&ns);
}

static void
test_mrs_create_allows_inter_mrs_overlap(void)
{
	struct spdk_nvmf_cpcs_ns ns;
	struct spdk_nvme_cpcs_memory_range_descriptor range1[1] = {};
	struct spdk_nvme_cpcs_memory_range_descriptor range2[1] = {};
	uint16_t rsid1 = 0, rsid2 = 0;
	int rc;

	_ut_ns_init(&ns);
	ns.max_mrs = 4;
	ns.max_ranges_per_mrs = 4;
	ns.mrs_granularity = 2;

	range1[0].mnsid = 13;
	range1[0].starting_byte = 0;
	range1[0].length = 16;

	range2[0].mnsid = 13;
	range2[0].starting_byte = 8;
	range2[0].length = 16;

	rc = cpcs_mrs_create(&ns, range1, 1, &rsid1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rsid1 == 1);

	rc = cpcs_mrs_create(&ns, range2, 1, &rsid2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rsid2 == 2);
	CU_ASSERT(ns.mrs_count == 2);

	rc = cpcs_mrs_delete_all(&ns);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ns.mrs_count == 0);

	_ut_ns_fini(&ns);
}

static void
test_builtin_program_install(void)
{
	struct spdk_nvmf_cpcs_ns ns;
	int rc;

	_ut_ns_init(&ns);
	ns.max_programs = CPCS_MAX_PROGRAMS_PER_NS;

	rc = cpcs_program_install_builtins(&ns);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ns.programs[CPCS_BUILTIN_PIND_MEMCPY] != NULL);
	CU_ASSERT(ns.programs[CPCS_BUILTIN_PIND_MEMFILL] != NULL);
	CU_ASSERT(ns.programs[CPCS_BUILTIN_PIND_SUM64] != NULL);
	CU_ASSERT(ns.programs[CPCS_BUILTIN_PIND_MEMCPY]->peocc == SPDK_NVME_CPCS_PEOCC_DEVICE_DEFINED);

	CU_ASSERT(cpcs_program_index_downloadable(&ns, CPCS_BUILTIN_PIND_MEMCPY) == false);
	CU_ASSERT(cpcs_program_index_downloadable(&ns, CPCS_BUILTIN_PIND_MEMFILL) == false);
	CU_ASSERT(cpcs_program_index_downloadable(&ns, CPCS_BUILTIN_PIND_SUM64) == false);

	rc = cpcs_program_unload_all(&ns);
	CU_ASSERT(rc == 0);

	_ut_ns_fini(&ns);
}

static void
test_execute_finalize_slm_lease_boundary(void)
{
	struct cpcs_exec_context ctx = {};
	int status;

	reset_execute_lease_stubs();
	ctx.slm_lease_id = 0xA1;
	ctx.slm_lease_acquired = true;

	status = cpcs_execute_finalize_slm_lease(&ctx, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(status == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(g_publish_calls == 1);
	CU_ASSERT(g_discard_calls == 0);
	CU_ASSERT(g_release_calls == 1);
	CU_ASSERT(g_last_lease_id == 0xA1);
	CU_ASSERT(ctx.slm_lease_acquired == false);

	reset_execute_lease_stubs();
	ctx.slm_lease_id = 0xA2;
	ctx.slm_lease_acquired = true;

	status = cpcs_execute_finalize_slm_lease(&ctx, -SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT(status == -SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT(g_publish_calls == 0);
	CU_ASSERT(g_discard_calls == 1);
	CU_ASSERT(g_release_calls == 1);
	CU_ASSERT(g_last_lease_id == 0xA2);
	CU_ASSERT(ctx.slm_lease_acquired == false);

	reset_execute_lease_stubs();
	ctx.slm_lease_id = 0xA3;
	ctx.slm_lease_acquired = true;
	g_lease_publish_rc = -EIO;

	status = cpcs_execute_finalize_slm_lease(&ctx, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(status == -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
	CU_ASSERT(g_publish_calls == 1);
	CU_ASSERT(g_discard_calls == 0);
	CU_ASSERT(g_release_calls == 1);
	CU_ASSERT(ctx.slm_lease_acquired == false);

	reset_execute_lease_stubs();
	ctx.slm_lease_id = 0xA4;
	ctx.slm_lease_acquired = true;
	g_lease_discard_rc = -EIO;

	status = cpcs_execute_finalize_slm_lease(&ctx, -SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT(status == -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
	CU_ASSERT(g_publish_calls == 0);
	CU_ASSERT(g_discard_calls == 1);
	CU_ASSERT(g_release_calls == 1);
	CU_ASSERT(ctx.slm_lease_acquired == false);

	reset_execute_lease_stubs();
	ctx.slm_lease_id = 0xA5;
	ctx.slm_lease_acquired = true;
	g_lease_release_rc = -EIO;

	status = cpcs_execute_finalize_slm_lease(&ctx, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(status == -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
	CU_ASSERT(g_publish_calls == 1);
	CU_ASSERT(g_discard_calls == 0);
	CU_ASSERT(g_release_calls == 1);
	CU_ASSERT(ctx.slm_lease_acquired == false);

	reset_execute_lease_stubs();
	ctx.slm_lease_id = 0xA6;
	ctx.slm_lease_acquired = false;

	status = cpcs_execute_finalize_slm_lease(&ctx, -SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT(status == -SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT(g_publish_calls == 0);
	CU_ASSERT(g_discard_calls == 0);
	CU_ASSERT(g_release_calls == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("cpcs", NULL, NULL);

	CU_ADD_TEST(suite, test_mrs_validate_overlap);
	CU_ADD_TEST(suite, test_mrs_validate_alignment);
	CU_ADD_TEST(suite, test_mrs_create_delete);
	CU_ADD_TEST(suite, test_mrs_create_allows_inter_mrs_overlap);
	CU_ADD_TEST(suite, test_builtin_program_install);
	CU_ADD_TEST(suite, test_execute_finalize_slm_lease_boundary);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}

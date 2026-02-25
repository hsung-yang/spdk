/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2025 Samsung Electronics Co., Ltd. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk/log.h"

#include "nvmf/cpcs/program.c"
#include "nvmf/cpcs/builtin_programs.c"
#include "nvmf/cpcs/memory_range_set.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf_cpcs)

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
	CU_ADD_TEST(suite, test_builtin_program_install);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}

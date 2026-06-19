/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * Common test utilities for CPCS integration tests
 */

#ifndef SPDK_TEST_CPCS_COMMON_H
#define SPDK_TEST_CPCS_COMMON_H

#include "spdk/stdinc.h"
#include "spdk/bdev_slm.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvme_cpcs_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Test context for CPCS tests
 */
struct cpcs_test_ctx {
	/* SLM bdevs */
	const char *slm_name;
	uint32_t slm_nsid;
	uint64_t slm_size_mb;

	/* Test state */
	bool is_initialized;
	int error_count;
};

/**
 * Initialize CPCS test context
 */
int cpcs_test_init(struct cpcs_test_ctx *ctx);

/**
 * Cleanup CPCS test context
 */
void cpcs_test_cleanup(struct cpcs_test_ctx *ctx);

/**
 * Create a test SLM bdev
 */
int cpcs_test_create_slm(struct cpcs_test_ctx *ctx, const char *name,
			 uint32_t nsid, uint64_t size_mb);

/**
 * Delete a test SLM bdev
 */
void cpcs_test_delete_slm(struct cpcs_test_ctx *ctx, const char *name);

/**
 * Verify SLM buffer contents
 */
int cpcs_test_verify_buffer(struct cpcs_test_ctx *ctx, const char *name,
			    uint64_t offset, const void *expected_data, uint64_t len);

/**
 * Write test pattern to SLM buffer
 */
int cpcs_test_write_pattern(struct cpcs_test_ctx *ctx, const char *name,
			    uint64_t offset, uint64_t len, uint8_t pattern);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_TEST_CPCS_COMMON_H */

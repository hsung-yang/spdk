/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * CPCS Execute Program Command
 */

#ifndef SPDK_NVMF_CPCS_EXECUTE_H
#define SPDK_NVMF_CPCS_EXECUTE_H

#include "spdk/stdinc.h"
#include "spdk/nvmf.h"
#include "nvmf_cpcs.h"
#include "memory_range_set.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_bdev;

struct cpcs_exec_resolved_range {
	struct spdk_bdev	*bdev;
	uint32_t		mnsid;
	uint64_t		starting_byte;
	uint32_t		length;
};

/**
 * Execute Program context
 */
struct cpcs_exec_context {
	/* Namespace and program */
	struct spdk_nvmf_cpcs_ns       *ns;
	struct cpcs_program            *program;

	/* Memory access */
	uint16_t                        rsid;
	struct cpcs_memory_range_set   *mrs;

	/* Inline Memory Ranges (when RSID=0, NUMR>0) */
	struct cpcs_memory_range       *inline_ranges;
	uint32_t                        inline_range_count;
	struct cpcs_exec_resolved_range *resolved_ranges;
	uint32_t                        resolved_range_count;

	/* Parameters from command */
	uint64_t                        cparam1;    /* CDW10-11 */
	uint64_t                        cparam2;    /* CDW12-13 */

	/* Data buffer (from DPTR) */
	void                           *data_buffer;
	uint32_t                        data_len;   /* DLEN */
	bool                            data_buffer_owned;

	/* Output */
	uint64_t                        return_value;

	/* Request tracking */
	struct spdk_nvmf_request       *req;

	/* SLM lease tracking */
	uint64_t                        slm_lease_id;
	bool                            slm_lease_acquired;

	/* Async execution tracking */
	bool                            completion_done;
	bool                            runtime_pending;
};

/**
 * Handle Execute Program I/O command
 */
int cpcs_execute_program_cmd(struct spdk_nvmf_request *req);

/**
 * Parse and validate Execute Program command
 */
int cpcs_execute_parse_cmd(struct spdk_nvmf_request *req,
			   struct cpcs_exec_context *ctx);

/**
 * Setup memory access for execution
 */
int cpcs_execute_setup_memory(struct cpcs_exec_context *ctx);

/**
 * Run the program
 */
int cpcs_execute_run(struct cpcs_exec_context *ctx);

/**
 * Complete the Execute Program command
 */
void cpcs_execute_complete(struct cpcs_exec_context *ctx, int status);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_EXECUTE_H */

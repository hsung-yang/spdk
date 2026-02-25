/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * CPCS Program Runtime Interface
 */

#ifndef SPDK_NVMF_CPCS_RUNTIME_H
#define SPDK_NVMF_CPCS_RUNTIME_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct cpcs_program;
struct cpcs_exec_context;

/**
 * Runtime context (opaque to program management)
 */
struct cpcs_runtime_ctx;

/**
 * Runtime operations interface
 */
struct cpcs_runtime_ops {
	const char *name;
	uint8_t ptype;      /* Program type this runtime handles */

	/**
	 * Initialize runtime for a program
	 */
	int (*init)(struct cpcs_program *prog);

	/**
	 * Validate program data
	 */
	int (*validate)(struct cpcs_program *prog);

	/**
	 * Activate program (JIT compile, etc.)
	 */
	int (*activate)(struct cpcs_program *prog);

	/**
	 * Execute program
	 */
	int (*execute)(struct cpcs_program *prog,
		       struct cpcs_exec_context *ctx,
		       uint64_t *return_value);

	/**
	 * Deactivate program
	 */
	void (*deactivate)(struct cpcs_program *prog);

	/**
	 * Cleanup runtime
	 */
	void (*fini)(struct cpcs_program *prog);
};

/**
 * Register a runtime
 */
int cpcs_runtime_register(const struct cpcs_runtime_ops *ops);

/**
 * Get runtime for program type
 */
const struct cpcs_runtime_ops *cpcs_runtime_get(uint8_t ptype);

/**
 * Initialize all runtimes
 */
int cpcs_runtime_init_all(void);

/**
 * Cleanup all runtimes
 */
void cpcs_runtime_fini_all(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_RUNTIME_H */

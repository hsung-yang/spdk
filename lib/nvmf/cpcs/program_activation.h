/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * CPCS Program Activation Management
 */

#ifndef SPDK_NVMF_CPCS_PROGRAM_ACTIVATION_H
#define SPDK_NVMF_CPCS_PROGRAM_ACTIVATION_H

#include "program.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Activate a program
 *
 * - Validates program data (if not already validated)
 * - Initializes runtime (JIT compile for eBPF, etc.)
 * - Reserves compute resources
 *
 * \param ns Compute namespace
 * \param pind Program Index
 * \return 0 on success, negative errno on failure
 */
int cpcs_program_activate(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind);

/**
 * Deactivate a program
 *
 * Fails if program is currently executing
 *
 * \param ns Compute namespace
 * \param pind Program Index
 * \return 0 on success, negative errno on failure
 */
int cpcs_program_deactivate(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind);

/**
 * Deactivate all programs
 *
 * \param ns Compute namespace
 * \return 0 on success, negative errno on failure
 */
int cpcs_program_deactivate_all(struct spdk_nvmf_cpcs_ns *ns);

/**
 * Check if program is ready for execution
 *
 * \param prog Program to check
 * \return true if ready, false otherwise
 */
bool cpcs_program_is_ready(struct cpcs_program *prog);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_PROGRAM_ACTIVATION_H */

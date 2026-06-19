/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * CPCS NVMe-oF Command Handler Integration
 */

#ifndef SPDK_NVMF_CPCS_CMD_H
#define SPDK_NVMF_CPCS_CMD_H

#include "spdk/stdinc.h"
#include "spdk/nvmf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Handle CPCS admin commands
 *
 * \param req NVMe-oF request
 * \return 0 if handled, negative errno if not a CPCS command
 */
int cpcs_handle_admin_cmd(struct spdk_nvmf_request *req);

/**
 * Handle CPCS I/O commands
 *
 * \param req NVMe-oF request
 * \return 0 if handled, negative errno if not a CPCS command
 */
int cpcs_handle_io_cmd(struct spdk_nvmf_request *req);

/**
 * Initialize CPCS command handling
 *
 * \return 0 on success, negative errno on failure
 */
int cpcs_cmd_init(void);

/**
 * Cleanup CPCS command handling
 */
void cpcs_cmd_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_CMD_H */

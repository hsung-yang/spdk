/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * CPCS Admin Command Handlers
 */

#ifndef SPDK_NVMF_CPCS_ADMIN_CMD_H
#define SPDK_NVMF_CPCS_ADMIN_CMD_H

#include "spdk/stdinc.h"
#include "spdk/nvmf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Handle Load Program admin command
 *
 * \param req NVMe-oF request
 * \return 0 on success, negative errno on failure
 */
int cpcs_admin_load_program(struct spdk_nvmf_request *req);

/**
 * Handle Program Activation admin command
 *
 * \param req NVMe-oF request
 * \return 0 on success, negative errno on failure
 */
int cpcs_admin_program_activation(struct spdk_nvmf_request *req);

/**
 * Handle MRS Management admin command
 *
 * \param req NVMe-oF request
 * \return 0 on success, negative errno on failure
 */
int cpcs_admin_mrs_management(struct spdk_nvmf_request *req);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_ADMIN_CMD_H */

/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * CPCS NVMe-oF Command Handler Integration
 *
 * Integrates CPCS commands with NVMe-oF transport layer
 */

#include "cpcs_cmd.h"
#include "admin_cmd.h"
#include "execute.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvme_cpcs_spec.h"
#include "spdk/log.h"
#include "spdk/nvmf_cmd.h"

int
cpcs_handle_admin_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
	uint8_t opc = cmd->opc;

	/* Check if this is a CPCS admin command */
	switch (opc) {
	case SPDK_NVME_OPC_CPCS_LOAD_PROGRAM:
		SPDK_DEBUGLOG(nvmf_cpcs, "Load Program command received\n");
		return cpcs_admin_load_program(req);

	case SPDK_NVME_OPC_CPCS_PROGRAM_ACTIVATION:
		SPDK_DEBUGLOG(nvmf_cpcs, "Program Activation command received\n");
		return cpcs_admin_program_activation(req);

	case SPDK_NVME_OPC_CPCS_MRS_MANAGEMENT:
		SPDK_DEBUGLOG(nvmf_cpcs, "MRS Management command received\n");
		return cpcs_admin_mrs_management(req);

	default:
		/* Not a CPCS command */
		return -ENOTSUP;
	}
}

int
cpcs_handle_io_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
	uint8_t opc = cmd->opc;

	/* Check if this is a CPCS I/O command */
	switch (opc) {
	case SPDK_NVME_OPC_CPCS_EXECUTE_PROGRAM:
		SPDK_DEBUGLOG(nvmf_cpcs, "Execute Program command received\n");
		return cpcs_execute_program_cmd(req);

	default:
		/* Not a CPCS command */
		return -ENOTSUP;
	}
}

int
cpcs_cmd_init(void)
{
	SPDK_NOTICELOG("CPCS command handler initialized\n");
	return 0;
}

void
cpcs_cmd_fini(void)
{
	SPDK_NOTICELOG("CPCS command handler finalized\n");
}

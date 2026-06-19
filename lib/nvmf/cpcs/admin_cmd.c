/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * CPCS Admin Command Handlers
 */

#include "admin_cmd.h"
#include "nvmf_cpcs.h"
#include "program.h"
#include "program_activation.h"
#include "memory_range_set.h"
#include "spdk/nvme_cpcs_spec.h"
#include "spdk/log.h"
#include "spdk/nvmf_transport.h"
#include "spdk/nvmf_cmd.h"

/* Helper function to get compute namespace from request */
static struct spdk_nvmf_cpcs_ns *
get_cpcs_ns_from_req(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_subsystem *subsystem = spdk_nvmf_request_get_subsystem(req);

	return spdk_nvmf_cpcs_ns_get_by_nsid(subsystem, cmd->nsid);
}

int
cpcs_admin_load_program(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_cpcs_ns *ns;
	uint16_t pind;
	uint8_t ptype, pit;
	uint64_t puid;
	uint32_t psize, loff, numb;
	uint8_t sel;
	void *data = NULL;
	size_t copied;
	bool data_owned = false;
	int rc;

	/* Parse command fields (based on spdk_nvme_cpcs_cmd_load_program) */
	pind = cmd->cdw10_bits.cpcs_load_program.pind;
	ptype = cmd->cdw10_bits.cpcs_load_program.ptype;
	sel = cmd->cdw10_bits.cpcs_load_program.sel;
	pit = cmd->cdw10_bits.cpcs_load_program.pit;
	psize = cmd->cdw11_bits.cpcs_load_program.psize;
	puid = ((uint64_t)cmd->cdw13 << 32) | cmd->cdw12;
	numb = cmd->cdw14;
	loff = cmd->cdw15;

	SPDK_DEBUGLOG(nvmf_cpcs,
		      "Load Program: PIND=%u PTYPE=%u PIT=%u SEL=%u PUID=0x%lx PSIZE=%u LOFF=%u NUMB=%u\n",
		      pind, ptype, pit, sel, puid, psize, loff, numb);

	/* Get namespace */
	ns = get_cpcs_ns_from_req(req);
	if (!ns) {
		SPDK_ERRLOG("Compute namespace not found for NSID %u\n", cmd->nsid);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		spdk_nvmf_request_complete(req);
		return -EINVAL;
	}

	switch (sel) {
	case SPDK_NVME_CPCS_LOAD_OP_UNLOAD:
		rc = cpcs_program_unload(ns, pind);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to unload program %u: %d\n", pind, rc);
			req->rsp->nvme_cpl.status.sc = -rc;
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			spdk_nvmf_request_complete(req);
			return rc;
		}

		SPDK_DEBUGLOG(nvmf_cpcs, "Unload Program %u completed successfully\n", pind);
		spdk_nvmf_request_complete(req);
		return 0;

	case SPDK_NVME_CPCS_LOAD_OP_LOAD:
		/* Get data from DPTR if NUMB > 0 */
		if (numb > 0) {
			if (req->length < numb) {
				SPDK_ERRLOG("Load Program data length too small: %u < %u\n",
					    req->length, numb);
				req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
				spdk_nvmf_request_complete(req);
				return -EINVAL;
			}

			if (req->iovcnt == 1 && req->iov[0].iov_len >= numb) {
				data = req->iov[0].iov_base;
				data_owned = false;
			} else {
				data = malloc(numb);
				if (!data) {
					req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
					spdk_nvmf_request_complete(req);
					return -ENOMEM;
				}
				data_owned = true;

				copied = spdk_nvmf_request_copy_to_buf(req, data, numb);
				if (copied != numb) {
					SPDK_ERRLOG("Failed to copy Load Program data: %zu != %u\n", copied, numb);
					free(data);
					req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
					spdk_nvmf_request_complete(req);
					return -EINVAL;
				}
			}
		}

		/* Call program load function */
		rc = cpcs_program_load(ns, pind, ptype, pit, puid, psize, loff, numb, data);
		if (data_owned) {
			free(data);
		}
		if (rc != 0) {
			SPDK_ERRLOG("Failed to load program %u: %d\n", pind, rc);
			req->rsp->nvme_cpl.status.sc = -rc;
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			spdk_nvmf_request_complete(req);
			return rc;
		}

		/* Success */
		SPDK_DEBUGLOG(nvmf_cpcs, "Load Program %u completed successfully\n", pind);
		spdk_nvmf_request_complete(req);
		return 0;

	default:
		SPDK_ERRLOG("Invalid load program selection: %u\n", sel);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
		spdk_nvmf_request_complete(req);
		return -EINVAL;
	}
}

int
cpcs_admin_program_activation(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_cpcs_ns *ns;
	uint16_t pind;
	uint8_t sel;  /* Activation selection */
	int rc;

	/* Parse command fields */
	pind = cmd->cdw10_bits.cpcs_program_activation.pind;
	sel = cmd->cdw10_bits.cpcs_program_activation.sel;

	SPDK_DEBUGLOG(nvmf_cpcs, "Program Activation: PIND=%u SEL=%u\n", pind, sel);

	/* Get namespace */
	ns = get_cpcs_ns_from_req(req);
	if (!ns) {
		SPDK_ERRLOG("Compute namespace not found for NSID %u\n", cmd->nsid);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		spdk_nvmf_request_complete(req);
		return -EINVAL;
	}

	/* Handle activation action */
	switch (sel) {
	case SPDK_NVME_CPCS_ACT_OP_ACTIVATE:
		rc = cpcs_program_activate(ns, pind);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to activate program %u: %d\n", pind, rc);
			req->rsp->nvme_cpl.status.sc = -rc;
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		} else {
			SPDK_DEBUGLOG(nvmf_cpcs, "Program %u activated successfully\n", pind);
		}
		break;

	case SPDK_NVME_CPCS_ACT_OP_DEACTIVATE:
		rc = cpcs_program_deactivate(ns, pind);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to deactivate program %u: %d\n", pind, rc);
			req->rsp->nvme_cpl.status.sc = -rc;
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		} else {
			SPDK_DEBUGLOG(nvmf_cpcs, "Program %u deactivated successfully\n", pind);
		}
		break;

	default:
		SPDK_ERRLOG("Invalid activation selection: %u\n", sel);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
		rc = -EINVAL;
		break;
	}

	spdk_nvmf_request_complete(req);
	return rc;
}

int
cpcs_admin_mrs_management(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_cpcs_ns *ns;
	uint16_t rsid;
	uint8_t sel;  /* MRS action */
	uint8_t numr;
	struct spdk_nvme_cpcs_memory_range_descriptor *ranges = NULL;
	size_t expected_len;
	size_t copied;
	bool ranges_owned = false;
	int rc;

	/* Parse command fields */
	rsid = cmd->cdw10_bits.cpcs_mrs_management.rsid;
	sel = cmd->cdw10_bits.cpcs_mrs_management.sel;
	numr = cmd->cdw11_bits.cpcs_mrs_management.numr;

	SPDK_DEBUGLOG(nvmf_cpcs, "MRS Management: RSID=%u SEL=%u NUMR=%u\n", rsid, sel, numr);

	/* Get namespace */
	ns = get_cpcs_ns_from_req(req);
	if (!ns) {
		SPDK_ERRLOG("Compute namespace not found for NSID %u\n", cmd->nsid);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		spdk_nvmf_request_complete(req);
		return -EINVAL;
	}

	/* Handle MRS action */
	switch (sel) {
	case SPDK_NVME_CPCS_MRS_OP_CREATE:
		if (numr == 0 || (ns->max_ranges_per_mrs > 0 && numr > ns->max_ranges_per_mrs)) {
			SPDK_ERRLOG("Invalid NUMR: %u (max %u)\n", numr, ns->max_ranges_per_mrs);
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_CPCS_SC_MAX_MEMORY_RANGES_EXCEEDED;
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			rc = -EINVAL;
			break;
		}

		if (numr > 0) {
			expected_len = numr * sizeof(*ranges);

			if (req->length < expected_len) {
				SPDK_ERRLOG("MRS data length too small: %u < %zu\n",
					    req->length, expected_len);
				req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
				rc = -EINVAL;
				break;
			}

			if (req->iovcnt == 1 && req->iov[0].iov_len >= expected_len) {
				ranges = req->iov[0].iov_base;
				ranges_owned = false;
			} else {
				ranges = calloc(numr, sizeof(*ranges));
				if (!ranges) {
					req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
					rc = -ENOMEM;
					break;
				}
				ranges_owned = true;

				copied = spdk_nvmf_request_copy_to_buf(req, ranges, expected_len);
				if (copied != expected_len) {
					SPDK_ERRLOG("Failed to copy MRS data: %zu != %zu\n",
						    copied, expected_len);
					free(ranges);
					req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
					rc = -EINVAL;
					break;
				}
			}
		}

		/* Create MRS */
		rc = cpcs_mrs_create(ns, ranges, numr, &rsid);
		if (ranges_owned) {
			free(ranges);
		}
		if (rc != 0) {
			SPDK_ERRLOG("Failed to create MRS: %d\n", rc);
			req->rsp->nvme_cpl.status.sc = -rc;
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		} else {
			/* Return RSID in DW0 */
			req->rsp->nvme_cpl.cdw0 = rsid;
			SPDK_DEBUGLOG(nvmf_cpcs, "MRS %u created successfully\n", rsid);
		}
		break;

	case SPDK_NVME_CPCS_MRS_OP_DELETE:
		rc = cpcs_mrs_delete(ns, rsid);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to delete MRS %u: %d\n", rsid, rc);
			req->rsp->nvme_cpl.status.sc = -rc;
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		} else {
			SPDK_DEBUGLOG(nvmf_cpcs, "MRS %u deleted successfully\n", rsid);
		}
		break;

	default:
		SPDK_ERRLOG("Invalid MRS action: %u\n", sel);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
		rc = -EINVAL;
		break;
	}

	spdk_nvmf_request_complete(req);
	return rc;
}

/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * NVMe-oF Compute Namespace structures and APIs
 */

#ifndef SPDK_NVMF_CPCS_H
#define SPDK_NVMF_CPCS_H

#include "spdk/stdinc.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvme_cpcs_spec.h"
#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Map an internal return code to an NVMe completion status.
 *
 * CPCS handlers return either a negated CPCS command-specific status code
 * (0x80..0xFF, e.g. -SPDK_NVME_CPCS_SC_*) or a raw negative errno
 * (-EINVAL/-ENOMEM/...). This helper writes the correct (sct, sc, dnr) so
 * that raw errnos do not leak out as bogus command-specific NVMe statuses.
 *
 * \param rc Return code (0 on success, negative on failure)
 * \param status NVMe completion status to populate
 */
static inline void
cpcs_status_from_rc(int rc, struct spdk_nvme_status *status)
{
	if (rc == 0) {
		status->sct = SPDK_NVME_SCT_GENERIC;
		status->sc = SPDK_NVME_SC_SUCCESS;
		return;
	}

	if (rc < 0 && -rc >= 0x80) {
		/* CPCS (or other) command-specific status code. */
		status->sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		status->sc = (uint16_t)(-rc);
		status->dnr = 1;
		return;
	}

	/* Map raw errnos to generic NVMe status codes. */
	status->sct = SPDK_NVME_SCT_GENERIC;
	switch (rc) {
	case -EINVAL:
		status->sc = SPDK_NVME_SC_INVALID_FIELD;
		break;
	case -ENOENT:
		status->sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		break;
	case -ENOMEM:
	case -EIO:
	default:
		status->sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		break;
	}
	status->dnr = 1;
}

/* Forward declarations */
struct spdk_nvmf_subsystem;
struct spdk_nvmf_ns;
struct cpcs_program;
struct cpcs_memory_range_set;
struct cpcs_reachability_group;
struct cpcs_reachability_manager;

/* Maximum limits */
#define CPCS_MAX_PROGRAMS_PER_NS        256
#define CPCS_MAX_MRS_PER_NS             1024
#define CPCS_MAX_RANGES_PER_MRS         128

/**
 * Compute Namespace
 */
struct spdk_nvmf_cpcs_ns {
	/* Basic identification */
	uint32_t nsid;
	char *name;
	struct spdk_nvmf_ns *ns;

	/* Program Management */
	struct cpcs_program *programs[CPCS_MAX_PROGRAMS_PER_NS];
	uint16_t num_programs;
	uint16_t num_activated;
	uint16_t max_programs;       /* From ns_data */
	uint16_t max_activated;      /* MAXACT */

	/* Memory Range Sets */
	TAILQ_HEAD(, cpcs_memory_range_set) mrs_list;
	uint16_t mrs_count;
	uint16_t next_rsid;          /* Next available RSID */
	uint16_t max_mrs;            /* MAXMEMRS */
	uint8_t max_ranges_per_mrs; /* MAXMEMR */
	uint8_t mrs_granularity;    /* MRSG: 2^MRSG bytes */

	/* Reachability */
	struct cpcs_reachability_group *reach_group;
	uint16_t reach_group_id;
	struct cpcs_reachability_manager *reach_mgr;

	/* Downloadable program limits */
	uint64_t max_program_bytes;  /* MAXPB (MiB) */
	uint64_t used_program_bytes;
	uint8_t load_program_gran;  /* LPG: 2^LPG bytes */

	/* Associated NVMf subsystem */
	struct spdk_nvmf_subsystem *subsystem;
	pthread_mutex_t lock;
};

/**
 * Compute Namespace configuration options
 */
struct spdk_nvmf_cpcs_ns_opts {
	uint32_t nsid;
	uint16_t max_activated;      /* MAXACT: 0 = no limit */
	uint16_t max_mrs;            /* MAXMEMRS: 0 = no limit */
	uint8_t max_ranges_per_mrs; /* MAXMEMR: 0 = no limit */
	uint8_t mrs_granularity;    /* MRSG: 2^MRSG bytes */
	uint64_t max_program_bytes;  /* MAXPB in MiB: 0 = no limit */
	uint8_t load_program_gran;  /* LPG: 2^LPG bytes */
	uint16_t reach_group_id;     /* Reachability group */
};

/**
 * Initialize CPCS namespace options with defaults
 */
void spdk_nvmf_cpcs_ns_opts_init(struct spdk_nvmf_cpcs_ns_opts *opts);

/**
 * Create a compute namespace
 *
 * \param subsystem NVMf subsystem to attach the namespace to
 * \param opts Namespace configuration options
 * \param ns_out Output pointer for created namespace
 * \return 0 on success, negative errno on failure
 */
int spdk_nvmf_cpcs_ns_create(struct spdk_nvmf_subsystem *subsystem,
			     const struct spdk_nvmf_cpcs_ns_opts *opts,
			     struct spdk_nvmf_cpcs_ns **ns_out);

/**
 * Destroy a compute namespace
 *
 * \param ns Namespace to destroy
 */
void spdk_nvmf_cpcs_ns_destroy(struct spdk_nvmf_cpcs_ns *ns);

/**
 * Get compute namespace by NSID
 *
 * \param subsystem NVMf subsystem
 * \param nsid Namespace ID
 * \return Pointer to namespace or NULL if not found
 */
struct spdk_nvmf_cpcs_ns *spdk_nvmf_cpcs_ns_get_by_nsid(
	struct spdk_nvmf_subsystem *subsystem, uint32_t nsid);

/**
 * Get Identify Namespace data for CPCS
 *
 * \param ns Compute namespace
 * \param ns_data Output buffer for CPCS-specific Identify data
 * \return 0 on success, negative errno on failure
 */
int spdk_nvmf_cpcs_ns_identify(struct spdk_nvmf_cpcs_ns *ns,
			       struct spdk_nvme_cpcs_ns_data *ns_data);

/**
 * Release CPCS-specific resources for a namespace context.
 *
 * This does not remove the base namespace from the subsystem.
 */
void spdk_nvmf_cpcs_ns_fini(struct spdk_nvmf_cpcs_ns *ns);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_H */

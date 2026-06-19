/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

#include "nvmf_cpcs.h"
#include "nvmf_internal.h"
#include "reachability.h"
#include "memory_range_set.h"
#include "program.h"
#include "builtin_programs.h"
#include "runtime.h"

#include "spdk/log.h"
#include "spdk/string.h"

SPDK_LOG_REGISTER_COMPONENT(nvmf_cpcs);

/* Global runtime initialization flag */
static bool g_runtime_initialized = false;
static pthread_mutex_t g_runtime_init_lock = PTHREAD_MUTEX_INITIALIZER;

static void
cpcs_ns_changed(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_ctrlr *ctrlr;

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		if (nvmf_ctrlr_ns_is_visible(ctrlr, nsid)) {
			nvmf_ctrlr_ns_changed(ctrlr, nsid);
		}
	}
}

void
spdk_nvmf_cpcs_ns_opts_init(struct spdk_nvmf_cpcs_ns_opts *opts)
{
	memset(opts, 0, sizeof(*opts));

	/* Set defaults */
	opts->max_activated = 16;       /* Default: 16 activated programs */
	opts->max_mrs = 256;            /* Default: 256 MRS */
	opts->max_ranges_per_mrs = 32;  /* Default: 32 ranges per MRS */
	opts->mrs_granularity = 2;      /* Default: 2^2 = 4 bytes */
	opts->max_program_bytes = 1024; /* Default: 1 GiB */
	opts->load_program_gran = 12;   /* Default: 2^12 = 4 KiB */
	opts->reach_group_id = 0;       /* Default: group 0 */
}

int
spdk_nvmf_cpcs_ns_create(struct spdk_nvmf_subsystem *subsystem,
			 const struct spdk_nvmf_cpcs_ns_opts *opts,
			 struct spdk_nvmf_cpcs_ns **ns_out)
{
	struct spdk_nvmf_cpcs_ns *ns;
	struct spdk_nvmf_ns *base_ns;
	struct spdk_nvmf_ctrlr *ctrlr;
	uint32_t nsid;
	uint32_t anagrpid;
	int rc;

	if (!subsystem || !opts || !ns_out) {
		return -EINVAL;
	}

	if (opts->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		return -EINVAL;
	}

	nsid = opts->nsid;
	if (nsid == 0) {
		for (nsid = 1; nsid <= subsystem->max_nsid; nsid++) {
			if (subsystem->ns[nsid - 1] == NULL) {
				break;
			}
		}
		if (nsid > subsystem->max_nsid) {
			return -ENOSPC;
		}
	}

	if (nsid > subsystem->max_nsid) {
		return -EINVAL;
	}

	if (subsystem->ns[nsid - 1] != NULL) {
		return -EEXIST;
	}

	anagrpid = nsid;
	if (anagrpid == 0 || anagrpid > subsystem->max_nsid) {
		return -EINVAL;
	}

	/* Initialize runtime system (once globally) */
	pthread_mutex_lock(&g_runtime_init_lock);
	if (!g_runtime_initialized) {
		rc = cpcs_runtime_init_all();
		if (rc != 0) {
			pthread_mutex_unlock(&g_runtime_init_lock);
			SPDK_ERRLOG("Failed to initialize CPCS runtime system\n");
			return rc;
		}
		g_runtime_initialized = true;
	}
	pthread_mutex_unlock(&g_runtime_init_lock);

	ns = calloc(1, sizeof(*ns));
	if (!ns) {
		SPDK_ERRLOG("Failed to allocate compute namespace\n");
		return -ENOMEM;
	}

	/* Initialize basic fields */
	ns->nsid = opts->nsid;
	ns->name = NULL; /* Will be set by subsystem */
	ns->subsystem = subsystem;
	ns->reach_mgr = cpcs_reachability_mgr_get(subsystem);
	if (!ns->reach_mgr) {
		free(ns);
		return -ENOMEM;
	}

	/* Initialize program management */
	ns->max_programs = CPCS_MAX_PROGRAMS_PER_NS;
	ns->max_activated = opts->max_activated;
	ns->num_programs = 0;
	ns->num_activated = 0;

	/* Initialize Memory Range Sets */
	TAILQ_INIT(&ns->mrs_list);
	ns->mrs_count = 0;
	ns->next_rsid = 1;  /* RSID 0 is reserved */
	ns->max_mrs = opts->max_mrs;
	ns->max_ranges_per_mrs = opts->max_ranges_per_mrs;
	ns->mrs_granularity = opts->mrs_granularity;

	/* Initialize reachability */
	ns->reach_group_id = opts->reach_group_id;
	ns->reach_group = NULL; /* Will be set by reachability manager */

	/* Initialize downloadable program limits */
	ns->max_program_bytes = opts->max_program_bytes * 1024 * 1024; /* MiB to bytes */
	ns->used_program_bytes = 0;
	ns->load_program_gran = opts->load_program_gran;

	rc = pthread_mutex_init(&ns->lock, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize namespace mutex\n");
		cpcs_reachability_mgr_put(subsystem);
		free(ns);
		return -rc;
	}

	base_ns = calloc(1, sizeof(*base_ns));
	if (base_ns == NULL) {
		pthread_mutex_destroy(&ns->lock);
		cpcs_reachability_mgr_put(subsystem);
		free(ns);
		return -ENOMEM;
	}

	TAILQ_INIT(&base_ns->hosts);
	TAILQ_INIT(&base_ns->registrants);
	STAILQ_INIT(&base_ns->reservations);

	spdk_nvmf_ns_opts_get_defaults(&base_ns->opts, sizeof(base_ns->opts));
	base_ns->opts.nsid = nsid;
	base_ns->opts.anagrpid = anagrpid;
	base_ns->nsid = nsid;
	base_ns->anagrpid = anagrpid;
	base_ns->subsystem = subsystem;
	base_ns->always_visible = true;
	base_ns->csi = SPDK_NVME_CSI_CPCS;
	base_ns->cpcs_ns = ns;

	subsystem->ns[nsid - 1] = base_ns;
	subsystem->ana_group[anagrpid - 1]++;

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		nvmf_ctrlr_ns_set_visible(ctrlr, nsid, true);
	}

	cpcs_ns_changed(subsystem, nsid);

	ns->nsid = nsid;
	ns->subsystem = subsystem;
	ns->ns = base_ns;
	*ns_out = ns;

	SPDK_NOTICELOG("Created compute namespace NSID=%u\n", ns->nsid);
	return 0;
}

void
spdk_nvmf_cpcs_ns_fini(struct spdk_nvmf_cpcs_ns *ns)
{
	if (!ns) {
		return;
	}

	/* Delete all Memory Range Sets */
	cpcs_mrs_delete_all(ns);

	/* Delete all programs */
	cpcs_program_unload_all(ns);

	pthread_mutex_destroy(&ns->lock);
	cpcs_reachability_mgr_put(ns->subsystem);
	free(ns->name);
	free(ns);
}

void
spdk_nvmf_cpcs_ns_destroy(struct spdk_nvmf_cpcs_ns *ns)
{
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_ns *base_ns;
	struct spdk_nvmf_ctrlr *ctrlr;
	uint32_t nsid;

	if (!ns || ns->subsystem == NULL) {
		return;
	}

	subsystem = ns->subsystem;
	base_ns = ns->ns;
	nsid = ns->nsid;

	if (spdk_nvmf_subsystem_remove_ns(subsystem, nsid) == 0) {
		SPDK_NOTICELOG("Destroyed compute namespace NSID=%u\n", nsid);
		return;
	}

	/* Fallback for callers that delete CPCS namespaces without pausing subsystem first. */
	if (base_ns != NULL && nsid > 0 && nsid <= subsystem->max_nsid &&
	    subsystem->ns[nsid - 1] == base_ns) {
		subsystem->ns[nsid - 1] = NULL;
		if (base_ns->anagrpid > 0 && base_ns->anagrpid <= subsystem->max_nsid &&
		    subsystem->ana_group[base_ns->anagrpid - 1] > 0) {
			subsystem->ana_group[base_ns->anagrpid - 1]--;
		}

		cpcs_ns_changed(subsystem, nsid);
		TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
			nvmf_ctrlr_ns_set_visible(ctrlr, nsid, false);
		}
	}

	spdk_nvmf_cpcs_ns_fini(ns);
	free(base_ns);

	SPDK_NOTICELOG("Destroyed compute namespace NSID=%u (forced)\n", nsid);
}

struct spdk_nvmf_cpcs_ns *
spdk_nvmf_cpcs_ns_get_by_nsid(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_ns *ns;

	if (subsystem == NULL || nsid == 0 || nsid > subsystem->max_nsid) {
		return NULL;
	}

	ns = subsystem->ns[nsid - 1];
	if (ns == NULL || ns->csi != SPDK_NVME_CSI_CPCS || ns->cpcs_ns == NULL) {
		return NULL;
	}

	return ns->cpcs_ns;
}

int
spdk_nvmf_cpcs_ns_identify(struct spdk_nvmf_cpcs_ns *ns,
			   struct spdk_nvme_cpcs_ns_data *ns_data)
{
	if (!ns || !ns_data) {
		return -EINVAL;
	}

	memset(ns_data, 0, sizeof(*ns_data));

	/* Fill in CPCS-specific Identify data */
	ns_data->maxact = ns->max_activated;
	ns_data->maxmemrs = ns->max_mrs;
	ns_data->mrsg = ns->mrs_granularity;
	ns_data->maxmemr = ns->max_ranges_per_mrs;
	ns_data->maxpb = ns->max_program_bytes / (1024 * 1024); /* bytes to MiB */
	ns_data->lpg = ns->load_program_gran;

	return 0;
}

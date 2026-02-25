/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

#include "nvmf_cpcs.h"
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

/* Global namespace list for lookups */
static TAILQ_HEAD(, spdk_nvmf_cpcs_ns) g_cpcs_ns_list = TAILQ_HEAD_INITIALIZER(g_cpcs_ns_list);
static pthread_mutex_t g_cpcs_ns_list_lock = PTHREAD_MUTEX_INITIALIZER;

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
	opts->load_program_gran = 12;   /* Default: 2^12 = 4 KiB */}

int
spdk_nvmf_cpcs_ns_create(struct spdk_nvmf_subsystem *subsystem,
			 const struct spdk_nvmf_cpcs_ns_opts *opts,
			 struct spdk_nvmf_cpcs_ns **ns_out)
{
	struct spdk_nvmf_cpcs_ns *ns;
	int rc;

	if (!subsystem || !opts || !ns_out) {
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

	/* Initialize downloadable program limits */
	ns->max_program_bytes = opts->max_program_bytes * 1024 * 1024; /* MiB to bytes */
	ns->used_program_bytes = 0;
	ns->load_program_gran = opts->load_program_gran;

	rc = pthread_mutex_init(&ns->lock, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize namespace mutex\n");		free(ns);
		return -rc;
	}

	/* Add to global namespace list */
	pthread_mutex_lock(&g_cpcs_ns_list_lock);
	TAILQ_INSERT_TAIL(&g_cpcs_ns_list, ns, link);
	pthread_mutex_unlock(&g_cpcs_ns_list_lock);

	*ns_out = ns;

	SPDK_NOTICELOG("Created compute namespace NSID=%u\n", ns->nsid);
	return 0;
}

void
spdk_nvmf_cpcs_ns_destroy(struct spdk_nvmf_cpcs_ns *ns)
{
	if (!ns) {
		return;
	}

	/* Remove from global namespace list */
	pthread_mutex_lock(&g_cpcs_ns_list_lock);
	TAILQ_REMOVE(&g_cpcs_ns_list, ns, link);
	pthread_mutex_unlock(&g_cpcs_ns_list_lock);

	/* Delete all Memory Range Sets */
	cpcs_mrs_delete_all(ns);

	/* Delete all programs */
	cpcs_program_unload_all(ns);

	pthread_mutex_destroy(&ns->lock);	free(ns->name);
	free(ns);

	SPDK_NOTICELOG("Destroyed compute namespace\n");
}

struct spdk_nvmf_cpcs_ns *
spdk_nvmf_cpcs_ns_get_by_nsid(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_cpcs_ns *ns;

	pthread_mutex_lock(&g_cpcs_ns_list_lock);

	TAILQ_FOREACH(ns, &g_cpcs_ns_list, link) {
		if (ns->nsid == nsid && ns->subsystem == subsystem) {
			pthread_mutex_unlock(&g_cpcs_ns_list_lock);
			return ns;
		}
	}

	pthread_mutex_unlock(&g_cpcs_ns_list_lock);
	return NULL;
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

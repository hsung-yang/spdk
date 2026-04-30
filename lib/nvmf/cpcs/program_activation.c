/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

#include "program_activation.h"
#include "nvmf_cpcs.h"
#include "runtime.h"
#include "spdk/log.h"

static uint16_t
cpcs_program_count_activated(struct spdk_nvmf_cpcs_ns *ns)
{
	uint16_t count = 0;
	uint16_t i;

	for (i = 0; i < ns->max_programs; i++) {
		if (ns->programs[i] && ns->programs[i]->activated) {
			count++;
		}
	}

	return count;
}

int
cpcs_program_activate(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind)
{
	struct cpcs_program *prog;
	const struct cpcs_runtime_ops *ops;
	int rc;

	if (!ns) {
		return -EINVAL;
	}

	if (pind >= ns->max_programs) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}

	pthread_mutex_lock(&ns->lock);

	prog = ns->programs[pind];
	if (!prog || prog->peocc == SPDK_NVME_CPCS_PEOCC_EMPTY) {
		pthread_mutex_unlock(&ns->lock);
		return -SPDK_NVME_CPCS_SC_NO_PROGRAM;
	}

	/* Check if already activated */
	if (prog->activated) {
		if (ns->num_activated == 0) {
			ns->num_activated = cpcs_program_count_activated(ns);
		}
		pthread_mutex_unlock(&ns->lock);
		return 0; /* Already activated, success */
	}

	/* Check if program is fully loaded */
	if (prog->state != CPCS_PROGRAM_STATE_LOADED &&
	    prog->state != CPCS_PROGRAM_STATE_ACTIVATED) {
		pthread_mutex_unlock(&ns->lock);
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}

	/* Check activation limit */
	if (ns->max_activated > 0 && ns->num_activated >= ns->max_activated) {
		pthread_mutex_unlock(&ns->lock);
		return -SPDK_NVME_CPCS_SC_MAX_PROGRAMS_ACTIVATED;
	}

	/* Validate program data */
	rc = cpcs_program_validate(prog);
	if (rc != 0) {
		pthread_mutex_unlock(&ns->lock);
		return rc;
	}

	/* Set state to activating */
	prog->state = CPCS_PROGRAM_STATE_ACTIVATING;
	pthread_mutex_unlock(&ns->lock);

	/* Initialize runtime if not already done */
	if (!prog->runtime) {
		ops = cpcs_runtime_get(prog->ptype);
		if (!ops) {
			pthread_mutex_lock(&ns->lock);
			prog->state = CPCS_PROGRAM_STATE_LOADED;
			pthread_mutex_unlock(&ns->lock);
			return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
		}

		rc = ops->init(prog);
		if (rc != 0) {
			pthread_mutex_lock(&ns->lock);
			prog->state = CPCS_PROGRAM_STATE_LOADED;
			pthread_mutex_unlock(&ns->lock);
			return rc;
		}
	}

	/* Activate runtime (JIT compile, etc.) */
	ops = cpcs_runtime_get(prog->ptype);
	if (ops && ops->activate) {
		rc = ops->activate(prog);
		if (rc != 0) {
			if (ops->fini) {
				ops->fini(prog);
			}
			pthread_mutex_lock(&ns->lock);
			prog->state = CPCS_PROGRAM_STATE_LOADED;
			pthread_mutex_unlock(&ns->lock);
			return rc;
		}
	}

	pthread_mutex_lock(&ns->lock);
	prog->state = CPCS_PROGRAM_STATE_ACTIVATED;
	prog->activated = true;
	ns->num_activated++;
	pthread_mutex_unlock(&ns->lock);

	SPDK_NOTICELOG("Activated program %u (total activated: %u)\n",
		       pind, ns->num_activated);
	return 0;
}

int
cpcs_program_deactivate(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind)
{
	struct cpcs_program *prog;
	const struct cpcs_runtime_ops *ops;

	if (!ns) {
		return -EINVAL;
	}

	if (pind >= ns->max_programs) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}

	pthread_mutex_lock(&ns->lock);

	prog = ns->programs[pind];
	if (!prog || prog->peocc == SPDK_NVME_CPCS_PEOCC_EMPTY) {
		pthread_mutex_unlock(&ns->lock);
		return -SPDK_NVME_CPCS_SC_NO_PROGRAM;
	}

	/* Check if already deactivated */
	if (!prog->activated) {
		pthread_mutex_unlock(&ns->lock);
		return 0; /* Already deactivated, success */
	}

	/* Check if program is executing */
	if (prog->exec_count > 0) {
		pthread_mutex_unlock(&ns->lock);
		return -SPDK_NVME_CPCS_SC_PROGRAM_IN_USE;
	}

	pthread_mutex_unlock(&ns->lock);

	/* Deactivate runtime */
	ops = cpcs_runtime_get(prog->ptype);
	if (ops && ops->deactivate) {
		ops->deactivate(prog);
	}

	pthread_mutex_lock(&ns->lock);
	prog->state = CPCS_PROGRAM_STATE_LOADED;
	if (ns->num_activated == 0) {
		/* Counter desync: recompute while prog is still marked activated so
		 * the post-decrement below produces the correct count. */
		ns->num_activated = cpcs_program_count_activated(ns);
	}
	prog->activated = false;
	if (ns->num_activated > 0) {
		ns->num_activated--;
	}

	pthread_mutex_unlock(&ns->lock);

	SPDK_NOTICELOG("Deactivated program %u (total activated: %u)\n",
		       pind, ns->num_activated);
	return 0;
}

int
cpcs_program_deactivate_all(struct spdk_nvmf_cpcs_ns *ns)
{
	uint16_t i;
	int rc;

	if (!ns) {
		return -EINVAL;
	}

	for (i = 0; i < ns->max_programs; i++) {
		if (ns->programs[i] != NULL && ns->programs[i]->activated) {
			rc = cpcs_program_deactivate(ns, i);
			if (rc != 0 && rc != -SPDK_NVME_CPCS_SC_NO_PROGRAM) {
				SPDK_WARNLOG("Failed to deactivate program %u: %d\n", i, rc);
			}
		}
	}

	return 0;
}

bool
cpcs_program_is_ready(struct cpcs_program *prog)
{
	if (!prog) {
		return false;
	}

	return (prog->activated &&
		prog->state == CPCS_PROGRAM_STATE_ACTIVATED &&
		prog->exec_count == 0);
}

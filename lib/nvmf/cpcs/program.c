/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

#include "program.h"
#include "builtin_programs.h"
#include "nvmf_cpcs.h"
#include "runtime.h"
#include "spdk/log.h"
#include "spdk/string.h"

static int _cpcs_program_unload_locked(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind,
				       struct cpcs_program *prog, bool allow_device_defined);

int
cpcs_program_load(struct spdk_nvmf_cpcs_ns *ns,
		  uint16_t pind,
		  uint8_t ptype,
		  uint8_t pit,
		  uint64_t puid,
		  uint32_t psize,
		  uint32_t loff,
		  uint32_t numb,
		  const void *data)
{
	struct cpcs_program *prog;
	uint32_t load_gran;
	int rc;

	if (!ns || !data || numb == 0) {
		return -EINVAL;
	}

	/* Check if program index is valid */
	if (pind >= ns->max_programs) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}

	if (!cpcs_program_index_downloadable(ns, pind)) {
		return -SPDK_NVME_CPCS_SC_PROGRAM_INDEX_NOT_DOWNLOADABLE;
	}

	pthread_mutex_lock(&ns->lock);

	prog = ns->programs[pind];

	/* Initial load (LOFF = 0) */
	if (loff == 0) {
		/* Check if program slot is already occupied */
		if (prog != NULL && prog->peocc != SPDK_NVME_CPCS_PEOCC_EMPTY) {
			pthread_mutex_unlock(&ns->lock);
			return -SPDK_NVME_CPCS_SC_PROGRAM_IN_USE;
		}

		/* Check program size limits */
		load_gran = 1 << ns->load_program_gran;
		if (psize == 0 || (psize % load_gran) != 0) {
			pthread_mutex_unlock(&ns->lock);
			return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
		}

		if (ns->max_program_bytes > 0 &&
		    (ns->used_program_bytes + psize) > ns->max_program_bytes) {
			pthread_mutex_unlock(&ns->lock);
			return -SPDK_NVME_CPCS_SC_MAX_PROGRAM_BYTES_EXCEEDED;
		}

		/* Allocate new program */
		if (prog == NULL) {
			prog = calloc(1, sizeof(*prog));
			if (!prog) {
				pthread_mutex_unlock(&ns->lock);
				return -ENOMEM;
			}
			ns->programs[pind] = prog;
		}

		/* Initialize program */
		prog->pind = pind;
		prog->ptype = ptype;
		prog->pit = pit;
		prog->puid = puid;
		prog->total_size = psize;
		prog->loaded_bytes = 0;
		prog->state = CPCS_PROGRAM_STATE_LOADING;
		prog->activated = false;
		prog->exec_count = 0;
		prog->ns = ns;
		prog->runtime = NULL;

		/* Allocate program data buffer */
		prog->data = malloc(psize);
		if (!prog->data) {
			free(prog);
			ns->programs[pind] = NULL;
			pthread_mutex_unlock(&ns->lock);
			return -ENOMEM;
		}

		rc = pthread_mutex_init(&prog->lock, NULL);
		if (rc != 0) {
			free(prog->data);
			free(prog);
			ns->programs[pind] = NULL;
			pthread_mutex_unlock(&ns->lock);
			return -rc;
		}

		prog->peocc = SPDK_NVME_CPCS_PEOCC_DOWNLOADED;
	} else {
		/* Continuation load */
		if (prog == NULL || prog->state != CPCS_PROGRAM_STATE_LOADING) {
			pthread_mutex_unlock(&ns->lock);
			return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
		}

		if (loff != prog->loaded_bytes) {
			pthread_mutex_unlock(&ns->lock);
			return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
		}
	}

	/* Check bounds */
	if (loff + numb > prog->total_size) {
		pthread_mutex_unlock(&ns->lock);
		return -SPDK_NVME_CPCS_SC_PROGRAM_TOO_BIG;
	}

	/* Copy data */
	memcpy((uint8_t *)prog->data + loff, data, numb);
	prog->loaded_bytes += numb;

	/* Check if load is complete */
	if (prog->loaded_bytes == prog->total_size) {
		prog->state = CPCS_PROGRAM_STATE_LOADED;
		ns->used_program_bytes += prog->total_size;
		SPDK_NOTICELOG("Program %u fully loaded (%u bytes)\n",
			       pind, prog->total_size);
	}

	pthread_mutex_unlock(&ns->lock);
	return 0;
}

int
cpcs_program_unload(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind)
{
	struct cpcs_program *prog;
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

	/* Built-in device-defined programs are not unloadable by the host. */
	if (prog->peocc == SPDK_NVME_CPCS_PEOCC_DEVICE_DEFINED) {
		pthread_mutex_unlock(&ns->lock);
		return -SPDK_NVME_CPCS_SC_PROGRAM_INDEX_NOT_DOWNLOADABLE;
	}

	/* Check if program is activated */
	if (prog->activated) {
		pthread_mutex_unlock(&ns->lock);
		return -SPDK_NVME_CPCS_SC_PROGRAM_IN_USE;
	}

	rc = _cpcs_program_unload_locked(ns, pind, prog, false);

	pthread_mutex_unlock(&ns->lock);

	if (rc == 0) {
		SPDK_NOTICELOG("Unloaded program %u\n", pind);
	}
	return rc;
}

int
cpcs_program_unload_all(struct spdk_nvmf_cpcs_ns *ns)
{
	uint16_t i;
	int rc = 0;

	if (!ns) {
		return -EINVAL;
	}

	pthread_mutex_lock(&ns->lock);

	for (i = 0; i < ns->max_programs; i++) {
		struct cpcs_program *prog = ns->programs[i];
		int u_rc;

		if (prog == NULL) {
			continue;
		}

		u_rc = _cpcs_program_unload_locked(ns, i, prog, true);
		if (u_rc != 0 && u_rc != -SPDK_NVME_CPCS_SC_NO_PROGRAM) {
			SPDK_WARNLOG("Failed to unload program %u: %d\n", i, u_rc);
			rc = u_rc;
		}
	}

	pthread_mutex_unlock(&ns->lock);

	return rc;
}

struct cpcs_program *
cpcs_program_get(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind)
{
	if (!ns || pind >= ns->max_programs) {
		return NULL;
	}

	return ns->programs[pind];
}

bool
cpcs_program_index_downloadable(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind)
{
	if (!ns || pind >= ns->max_programs) {
		return false;
	}

	/* Built-in programs are device-defined and not downloadable by design. */
	return !cpcs_program_index_is_builtin(pind);
}

static int
_cpcs_program_free(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind, struct cpcs_program *prog)
{
	if (prog->data) {
		free(prog->data);
	}
	if (prog->runtime) {
		const struct cpcs_runtime_ops *ops = cpcs_runtime_get(prog->ptype);
		if (ops && ops->fini) {
			ops->fini(prog);
		}
	}

	pthread_mutex_destroy(&prog->lock);
	free(prog);
	ns->programs[pind] = NULL;
	return 0;
}

static int
_cpcs_program_unload_locked(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind,
			    struct cpcs_program *prog, bool allow_device_defined)
{
	if (prog == NULL) {
		return -SPDK_NVME_CPCS_SC_NO_PROGRAM;
	}

	if (!allow_device_defined && prog->peocc == SPDK_NVME_CPCS_PEOCC_DEVICE_DEFINED) {
		return -SPDK_NVME_CPCS_SC_PROGRAM_INDEX_NOT_DOWNLOADABLE;
	}

	if (prog->exec_count > 0) {
		return -SPDK_NVME_CPCS_SC_PROGRAM_IN_USE;
	}

	if (prog->activated) {
		if (ns->num_activated > 0) {
			ns->num_activated--;
		}
		prog->activated = false;
	}

	if (prog->state == CPCS_PROGRAM_STATE_LOADED ||
	    prog->state == CPCS_PROGRAM_STATE_ACTIVATED) {
		if (ns->used_program_bytes < prog->total_size) {
			SPDK_ERRLOG("Program byte accounting underflow: used=%" PRIu64 " size=%u\n",
				    ns->used_program_bytes, prog->total_size);
			ns->used_program_bytes = 0;
		} else {
			ns->used_program_bytes -= prog->total_size;
		}
	}

	if (ns->num_programs > 0) {
		ns->num_programs--;
	}

	return _cpcs_program_free(ns, pind, prog);
}

int
cpcs_program_validate(struct cpcs_program *prog)
{
	const struct cpcs_runtime_ops *runtime;
	int rc;

	if (!prog) {
		return -EINVAL;
	}

	if (prog->state != CPCS_PROGRAM_STATE_LOADED) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}

	if (!prog->data || prog->loaded_bytes != prog->total_size) {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}

	/* Get runtime for program type */
	runtime = cpcs_runtime_get(prog->ptype);
	if (!runtime) {
		SPDK_ERRLOG("No runtime for program type %u\n", prog->ptype);
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}

	/* Initialize runtime context if not already done */
	if (!prog->runtime) {
		rc = runtime->init(prog);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to initialize runtime: %d\n", rc);
			return rc;
		}
	}

	/* Validate program using runtime */
	if (runtime->validate) {
		rc = runtime->validate(prog);
		if (rc != 0) {
			SPDK_ERRLOG("Program validation failed: %d\n", rc);
			return rc;
		}
	}

	SPDK_DEBUGLOG(nvmf_cpcs, "Program %u validated successfully\n", prog->pind);
	return 0;
}

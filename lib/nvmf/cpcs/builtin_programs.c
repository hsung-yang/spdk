/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "builtin_programs.h"
#include "nvmf_cpcs.h"
#include "program.h"

#include "spdk/log.h"
#include "spdk/nvme_spec.h"

static uint64_t
_builtin_puid_for_pind(uint16_t pind)
{
	switch (pind) {
	case CPCS_BUILTIN_PIND_MEMCPY:
		return CPCS_BUILTIN_PUID_MEMCPY;
	case CPCS_BUILTIN_PIND_MEMFILL:
		return CPCS_BUILTIN_PUID_MEMFILL;
	case CPCS_BUILTIN_PIND_SUM64:
		return CPCS_BUILTIN_PUID_SUM64;
	case CPCS_BUILTIN_PIND_MAX64:
		return CPCS_BUILTIN_PUID_MAX64;
	case CPCS_BUILTIN_PIND_MIN64:
		return CPCS_BUILTIN_PUID_MIN64;
	case CPCS_BUILTIN_PIND_DOT_PRODUCT:
		return CPCS_BUILTIN_PUID_DOT_PRODUCT;
	case CPCS_BUILTIN_PIND_FILTER_GT:
		return CPCS_BUILTIN_PUID_FILTER_GT;
	case CPCS_BUILTIN_PIND_MEMCPY_INLINE:
		return CPCS_BUILTIN_PUID_MEMCPY_INLINE;
	case CPCS_BUILTIN_PIND_RLE_COMPRESS:
		return CPCS_BUILTIN_PUID_RLE_COMPRESS;
	case CPCS_BUILTIN_PIND_MULTI_AGG64:
		return CPCS_BUILTIN_PUID_MULTI_AGG64;
	case CPCS_BUILTIN_PIND_L2_DISTANCE_SQ:
		return CPCS_BUILTIN_PUID_L2_DISTANCE_SQ;
	case CPCS_BUILTIN_PIND_COSINE_SIMILARITY:
		return CPCS_BUILTIN_PUID_COSINE_SIMILARITY;
	case CPCS_BUILTIN_PIND_DIRECT_NS_AGG:
		return CPCS_BUILTIN_PUID_DIRECT_NS_AGG;
	default:
		return 0;
	}
}

bool
cpcs_program_index_is_builtin(uint16_t pind)
{
	return (pind == CPCS_BUILTIN_PIND_MEMCPY ||
		pind == CPCS_BUILTIN_PIND_MEMFILL ||
		pind == CPCS_BUILTIN_PIND_SUM64 ||
		pind == CPCS_BUILTIN_PIND_MAX64 ||
		pind == CPCS_BUILTIN_PIND_MIN64 ||
		pind == CPCS_BUILTIN_PIND_DOT_PRODUCT ||
		pind == CPCS_BUILTIN_PIND_FILTER_GT ||
		pind == CPCS_BUILTIN_PIND_MEMCPY_INLINE ||
		pind == CPCS_BUILTIN_PIND_RLE_COMPRESS ||
		pind == CPCS_BUILTIN_PIND_MULTI_AGG64 ||
		pind == CPCS_BUILTIN_PIND_L2_DISTANCE_SQ ||
		pind == CPCS_BUILTIN_PIND_COSINE_SIMILARITY ||
		pind == CPCS_BUILTIN_PIND_DIRECT_NS_AGG);
}

static int
_install_one_builtin(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind)
{
	struct cpcs_program *prog;
	uint64_t puid;
	int rc;

	puid = _builtin_puid_for_pind(pind);
	if (puid == 0) {
		return -EINVAL;
	}

	prog = ns->programs[pind];
	if (prog != NULL) {
		return 0;
	}

	prog = calloc(1, sizeof(*prog));
	if (prog == NULL) {
		return -ENOMEM;
	}

	rc = pthread_mutex_init(&prog->lock, NULL);
	if (rc != 0) {
		free(prog);
		return -rc;
	}

	prog->pind = pind;
	prog->ptype = SPDK_NVME_CPCS_PTYPE_DEVICE_DEFINED;
	prog->pit = SPDK_NVME_CPCS_PIT_PUID;
	prog->puid = puid;
	prog->peocc = SPDK_NVME_CPCS_PEOCC_DEVICE_DEFINED;
	/* TODO: default activation state for device-defined programs is unspecified. */
	prog->state = CPCS_PROGRAM_STATE_ACTIVATED;
	prog->activated = true;
	prog->ns = ns;

	ns->programs[pind] = prog;
	ns->num_programs++;
	ns->num_activated++;

	return 0;
}

int
cpcs_program_install_builtins(struct spdk_nvmf_cpcs_ns *ns)
{
	int rc;

	if (ns == NULL) {
		return -EINVAL;
	}

	pthread_mutex_lock(&ns->lock);

	if (ns->max_programs <= CPCS_BUILTIN_PIND_DIRECT_NS_AGG) {
		pthread_mutex_unlock(&ns->lock);
		return -EINVAL;
	}

	rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_MEMCPY);
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_MEMFILL);
	}
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_SUM64);
	}
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_MAX64);
	}
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_MIN64);
	}
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_DOT_PRODUCT);
	}
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_FILTER_GT);
	}
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_MEMCPY_INLINE);
	}
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_RLE_COMPRESS);
	}
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_MULTI_AGG64);
	}
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_L2_DISTANCE_SQ);
	}
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_COSINE_SIMILARITY);
	}
	if (rc == 0) {
		rc = _install_one_builtin(ns, CPCS_BUILTIN_PIND_DIRECT_NS_AGG);
	}

	pthread_mutex_unlock(&ns->lock);

	if (rc == 0) {
		SPDK_NOTICELOG("Installed CPCS built-ins: pind 0-12 (memcpy..cosine_similarity, direct_ns_agg)\n");
	}

	return rc;
}

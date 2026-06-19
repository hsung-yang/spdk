/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * CPCS Program Management
 */

#ifndef SPDK_NVMF_CPCS_PROGRAM_H
#define SPDK_NVMF_CPCS_PROGRAM_H

#include "spdk/stdinc.h"
#include "spdk/nvme_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct spdk_nvmf_cpcs_ns;
struct cpcs_runtime_ctx;

/**
 * Program state
 */
enum cpcs_program_state {
	CPCS_PROGRAM_STATE_EMPTY = 0,
	CPCS_PROGRAM_STATE_LOADING,
	CPCS_PROGRAM_STATE_LOADED,
	CPCS_PROGRAM_STATE_ACTIVATING,
	CPCS_PROGRAM_STATE_ACTIVATED,
	CPCS_PROGRAM_STATE_EXECUTING,
};

/**
 * Program structure
 */
struct cpcs_program {
	/* Identification */
	uint16_t                    pind;       /* Program Index */
	uint8_t                     ptype;      /* Program Type */
	uint8_t                     pit;        /* Program Identifier Type */
	uint64_t                    puid;       /* Program Unique ID */

	/* Occupancy */
	enum spdk_nvme_cpcs_peocc   peocc;

	/* State */
	enum cpcs_program_state     state;
	bool                        activated;
	uint32_t                    exec_count; /* Active executions */

	/* Program binary */
	void                       *data;
	uint32_t                    total_size;     /* PSIZE */
	uint32_t                    loaded_bytes;   /* Bytes loaded so far */

	/* Runtime context (eBPF VM, etc.) */
	struct cpcs_runtime_ctx    *runtime;

	/* Parent namespace */
	struct spdk_nvmf_cpcs_ns   *ns;

	/* Lock for concurrent execution */
	pthread_mutex_t             lock;
};

/**
 * Load program data
 *
 * Handles both initial load (LOFF=0) and subsequent chunks
 *
 * \param ns Compute namespace
 * \param pind Program Index
 * \param ptype Program Type
 * \param pit Program Identifier Type
 * \param puid Program Unique ID
 * \param psize Total program size
 * \param loff Load offset
 * \param numb Number of bytes in this chunk
 * \param data Program data chunk
 * \return 0 on success, negative errno on failure
 */
int cpcs_program_load(struct spdk_nvmf_cpcs_ns *ns,
		      uint16_t pind,
		      uint8_t ptype,
		      uint8_t pit,
		      uint64_t puid,
		      uint32_t psize,
		      uint32_t loff,
		      uint32_t numb,
		      const void *data);

/**
 * Unload a program
 *
 * \param ns Compute namespace
 * \param pind Program Index
 * \return 0 on success, negative errno on failure
 */
int cpcs_program_unload(struct spdk_nvmf_cpcs_ns *ns, uint16_t pind);

/**
 * Unload all programs
 *
 * \param ns Compute namespace
 * \return 0 on success, negative errno on failure
 */
int cpcs_program_unload_all(struct spdk_nvmf_cpcs_ns *ns);

/**
 * Get program by index
 *
 * \param ns Compute namespace
 * \param pind Program Index
 * \return Pointer to program or NULL if not found
 */
struct cpcs_program *cpcs_program_get(struct spdk_nvmf_cpcs_ns *ns,
				      uint16_t pind);

/**
 * Check if program index is valid for download
 *
 * \param ns Compute namespace
 * \param pind Program Index
 * \return true if downloadable, false otherwise
 */
bool cpcs_program_index_downloadable(struct spdk_nvmf_cpcs_ns *ns,
				     uint16_t pind);

/**
 * Validate program data
 *
 * \param prog Program to validate
 * \return 0 on success, negative errno on failure
 */
int cpcs_program_validate(struct cpcs_program *prog);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_PROGRAM_H */

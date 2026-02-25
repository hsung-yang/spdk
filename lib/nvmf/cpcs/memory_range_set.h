/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * CPCS Memory Range Set Management
 */

#ifndef SPDK_NVMF_CPCS_MRS_H
#define SPDK_NVMF_CPCS_MRS_H

#include "spdk/stdinc.h"
#include "spdk/nvme_cpcs_spec.h"
#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_nvmf_cpcs_ns;

/**
 * Memory Range
 */
struct cpcs_memory_range {
	uint32_t    mnsid;          /* Memory Namespace ID */
	uint64_t    starting_byte;  /* Starting byte offset */
	uint32_t    length;         /* Length in bytes */
};

/**
 * Memory Range Set
 */
struct cpcs_memory_range_set {
	uint16_t                        rsid;       /* Memory Range Set ID */
	struct cpcs_memory_range       *ranges;     /* Array of ranges */
	uint8_t                         range_count;

	/* State tracking */
	uint32_t                        ref_count;  /* Active Execute commands */

	TAILQ_ENTRY(cpcs_memory_range_set) link;
};

/**
 * Create a Memory Range Set
 *
 * Admin command handler for MRS Management (Create operation)
 *
 * \param ns Compute namespace
 * \param ranges Array of memory range descriptors
 * \param num_ranges Number of ranges
 * \param rsid_out Output pointer for allocated RSID
 * \return 0 on success, negative errno on failure
 */
int cpcs_mrs_create(struct spdk_nvmf_cpcs_ns *ns,
		    const struct spdk_nvme_cpcs_memory_range_descriptor *ranges,
		    uint8_t num_ranges,
		    uint16_t *rsid_out);

/**
 * Delete a Memory Range Set
 *
 * Admin command handler for MRS Management (Delete operation)
 *
 * \param ns Compute namespace
 * \param rsid Memory Range Set ID to delete
 * \return 0 on success, negative errno on failure
 */
int cpcs_mrs_delete(struct spdk_nvmf_cpcs_ns *ns, uint16_t rsid);

/**
 * Delete all Memory Range Sets
 *
 * \param ns Compute namespace
 * \return 0 on success, negative errno on failure
 */
int cpcs_mrs_delete_all(struct spdk_nvmf_cpcs_ns *ns);

/**
 * Get Memory Range Set by ID
 *
 * \param ns Compute namespace
 * \param rsid Memory Range Set ID
 * \return Pointer to MRS or NULL if not found
 */
struct cpcs_memory_range_set *cpcs_mrs_get(struct spdk_nvmf_cpcs_ns *ns,
		uint16_t rsid);

/**
 * Acquire reference to MRS for Execute Program
 *
 * \param mrs Memory Range Set
 * \return 0 on success, negative errno on failure
 */
int cpcs_mrs_acquire(struct cpcs_memory_range_set *mrs);

/**
 * Release reference to MRS
 *
 * \param mrs Memory Range Set
 */
void cpcs_mrs_release(struct cpcs_memory_range_set *mrs);

/**
 * Validate Memory Range Set
 *
 * Check for overlapping ranges and reachability
 *
 * \param ns Compute namespace
 * \param ranges Array of memory range descriptors
 * \param num_ranges Number of ranges
 * \return 0 on success, negative errno on failure
 */
int cpcs_mrs_validate(struct spdk_nvmf_cpcs_ns *ns,
		      const struct spdk_nvme_cpcs_memory_range_descriptor *ranges,
		      uint8_t num_ranges);

/**
 * Get buffer pointer for memory range
 *
 * Helper function for eBPF runtime to access SLM memory
 *
 * \param mrs Memory Range Set
 * \param mr_id Memory Range ID within the set (1-based)
 * \param offset Offset within the memory range
 * \param len Length to access
 * \param ptr_out Output pointer to buffer
 * \return 0 on success, negative errno on failure
 */
int cpcs_mrs_get_buffer(struct cpcs_memory_range_set *mrs,
			uint64_t mr_id, uint64_t offset, uint64_t len,
			void **ptr_out);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_MRS_H */

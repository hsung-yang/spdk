/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * CPCS Reachability Management
 */

#ifndef SPDK_NVMF_CPCS_REACHABILITY_H
#define SPDK_NVMF_CPCS_REACHABILITY_H

#include "spdk/stdinc.h"
#include "spdk/queue.h"

struct spdk_nvmf_ns;
struct spdk_nvmf_subsystem;
struct spdk_nvmf_cpcs_ns;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Namespace entry in a reachability group
 */
struct cpcs_reachability_ns_entry {
	uint32_t    nsid;
	struct spdk_nvmf_ns *ns; /* Pointer to actual namespace */
	TAILQ_ENTRY(cpcs_reachability_ns_entry) link;
};

/**
 * Reachability Group
 */
struct cpcs_reachability_group {
	uint16_t    group_id;
	TAILQ_HEAD(, cpcs_reachability_ns_entry) namespaces;
	uint32_t    ns_count;
	TAILQ_ENTRY(cpcs_reachability_group) link;
};

/**
 * Reachability Association
 */
struct cpcs_reachability_association {
	uint16_t    assoc_id;
	uint16_t   *group_ids;
	uint8_t     group_count;
	TAILQ_ENTRY(cpcs_reachability_association) link;
};

/**
 * Reachability Manager
 */
struct cpcs_reachability_manager {
	TAILQ_HEAD(, cpcs_reachability_group) groups;
	TAILQ_HEAD(, cpcs_reachability_association) associations;
	uint16_t    next_group_id;
	uint16_t    next_assoc_id;
	pthread_mutex_t lock;
};

/**
 * Initialize reachability manager
 *
 * \param mgr Manager to initialize
 * \return 0 on success, negative errno on failure
 */
int cpcs_reachability_init(struct cpcs_reachability_manager *mgr);

/**
 * Cleanup reachability manager
 *
 * \param mgr Manager to cleanup
 */
void cpcs_reachability_fini(struct cpcs_reachability_manager *mgr);

/**
 * Get or create a reachability manager for a subsystem
 *
 * \param subsystem NVMf subsystem
 * \return Manager pointer or NULL on failure
 */
struct cpcs_reachability_manager *cpcs_reachability_mgr_get(struct spdk_nvmf_subsystem *subsystem);

/**
 * Release a reachability manager reference for a subsystem
 *
 * \param subsystem NVMf subsystem
 */
void cpcs_reachability_mgr_put(struct spdk_nvmf_subsystem *subsystem);

/**
 * Create a reachability group
 *
 * \param mgr Reachability manager
 * \param group_id_out Output pointer for group ID
 * \return 0 on success, negative errno on failure
 */
int cpcs_reachability_create_group(struct cpcs_reachability_manager *mgr,
				   uint16_t *group_id_out);

/**
 * Delete a reachability group
 *
 * \param mgr Reachability manager
 * \param group_id Group ID to delete
 * \return 0 on success, negative errno on failure
 */
int cpcs_reachability_delete_group(struct cpcs_reachability_manager *mgr,
				   uint16_t group_id);

/**
 * Add namespace to a reachability group
 *
 * \param mgr Reachability manager
 * \param group_id Target group ID
 * \param ns Namespace pointer
 * \return 0 on success, negative errno on failure
 */
int cpcs_reachability_add_ns(struct cpcs_reachability_manager *mgr,
			     uint16_t group_id,
			     struct spdk_nvmf_ns *ns);

/**
 * Remove namespace from a reachability group
 *
 * \param mgr Reachability manager
 * \param group_id Group ID
 * \param nsid Namespace ID to remove
 * \return 0 on success, negative errno on failure
 */
int cpcs_reachability_remove_ns(struct cpcs_reachability_manager *mgr,
				uint16_t group_id,
				uint32_t nsid);

/**
 * Create a reachability association between groups
 *
 * \param mgr Reachability manager
 * \param group_ids Array of group IDs to associate
 * \param group_count Number of groups
 * \param assoc_id_out Output pointer for association ID
 * \return 0 on success, negative errno on failure
 */
int cpcs_reachability_create_association(struct cpcs_reachability_manager *mgr,
		uint16_t *group_ids,
		uint8_t group_count,
		uint16_t *assoc_id_out);

/**
 * Delete a reachability association
 *
 * \param mgr Reachability manager
 * \param assoc_id Association ID to delete
 * \return 0 on success, negative errno on failure
 */
int cpcs_reachability_delete_association(struct cpcs_reachability_manager *mgr,
		uint16_t assoc_id);

/**
 * Get all memory namespaces reachable by a compute namespace
 *
 * \param mgr Reachability manager
 * \param compute_ns Compute namespace
 * \param memory_nsids Output buffer for memory namespace IDs
 * \param count Input: buffer size, Output: number of NSIDs returned
 * \return 0 on success, negative errno on failure
 */
int cpcs_reachability_get_memory_ns(struct cpcs_reachability_manager *mgr,
				    struct spdk_nvmf_cpcs_ns *compute_ns,
				    uint32_t *memory_nsids,
				    uint32_t *count);

/**
 * Check if a memory namespace is reachable by a compute namespace
 *
 * \param mgr Reachability manager
 * \param compute_ns Compute namespace
 * \param mnsid Memory namespace ID
 * \return true if reachable, false otherwise
 */
bool cpcs_reachability_is_memory_ns_reachable(struct cpcs_reachability_manager *mgr,
		struct spdk_nvmf_cpcs_ns *compute_ns,
		uint32_t mnsid);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_REACHABILITY_H */

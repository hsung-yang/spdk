/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * vSLM (Virtual Subsystem Local Memory) bdev public interface
 *
 * vSLM provides a tiered memory bdev backed by a base block device.
 */

#ifndef SPDK_BDEV_VSLM_H
#define SPDK_BDEV_VSLM_H

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"

#ifdef __cplusplus
extern "C" {
#endif

enum spdk_bdev_vslm_semantics_mode {
	SPDK_BDEV_VSLM_SEMANTICS_LEASE = 0,
	SPDK_BDEV_VSLM_SEMANTICS_DOUBLE_BUFFER = 1,
};

enum spdk_bdev_vslm_writeback_policy {
	SPDK_BDEV_VSLM_WRITEBACK_ON_EVICT = 0,
	SPDK_BDEV_VSLM_WRITEBACK_AT_BOUNDARY = 1,
	SPDK_BDEV_VSLM_WRITEBACK_HYBRID = 2,
};

struct spdk_bdev_vslm_policy {
	enum spdk_bdev_vslm_semantics_mode semantics_mode;
	enum spdk_bdev_vslm_writeback_policy writeback_policy;
	bool admission_enabled;
	uint32_t admission_faults_per_sec_threshold;
};

struct spdk_bdev_vslm_opts {
	uint32_t nsid;
	struct spdk_bdev_vslm_policy policy;
	bool fdp_mode_enabled;
	uint16_t fdp_dspec;
};

/**
 * Initialize vSLM create options with defaults.
 *
 * Defaults:
 * - nsid: 0 (unset)
 * - semantics_mode: lease
 * - writeback_policy: on_evict
 * - admission_enabled: false
 * - admission_faults_per_sec_threshold: 0
 * - fdp_mode_enabled: false
 * - fdp_dspec: 0
 *
 * \param opts Options structure to initialize.
 */
void bdev_vslm_opts_init(struct spdk_bdev_vslm_opts *opts);

/**
 * Create a vSLM bdev.
 *
 * \param name Name of the vSLM bdev.
 * \param base_bdev_name Name of the base (backing) bdev.
 * \param sram_size_bytes Size of the SRAM cache in bytes.
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_create(const char *name, const char *base_bdev_name,
		     uint64_t sram_size_bytes);

/**
 * Create a vSLM bdev with options.
 *
 * \param name Name of the vSLM bdev.
 * \param base_bdev_name Name of the base (backing) bdev.
 * \param sram_size_bytes Size of the SRAM cache in bytes.
 * \param opts Optional create options (NULL uses defaults).
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_create_with_opts(const char *name, const char *base_bdev_name,
			       uint64_t sram_size_bytes,
			       const struct spdk_bdev_vslm_opts *opts);

/**
 * Delete a vSLM bdev.
 *
 * \param name Name of the vSLM bdev to delete.
 * \param cb_fn Callback function.
 * \param cb_arg Callback argument.
 */
void bdev_vslm_delete(const char *name,
		      spdk_bdev_unregister_cb cb_fn, void *cb_arg);

/**
 * Set the namespace ID for a vSLM bdev.
 *
 * \param name Name of the vSLM bdev.
 * \param nsid Namespace ID to assign.
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_set_nsid(const char *name, uint32_t nsid);

/**
 * Get direct pointer to vSLM buffer by namespace ID.
 * Only for internal CPCS use.
 *
 * \param nsid Namespace ID of the vSLM bdev.
 * \param offset Byte offset into the buffer.
 * \param length Length of the region.
 * \param[out] ptr Pointer to the buffer region.
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_get_buffer_ptr_by_nsid(uint32_t nsid, uint64_t offset,
				     uint64_t length, void **ptr);

/**
 * Read bytes from a vSLM namespace by NSID.
 *
 * This API resolves vSLM paging internally and supports accesses beyond SRAM
 * up to the configured virtual capacity.
 *
 * \param nsid Namespace ID of the vSLM bdev.
 * \param offset Byte offset to read from.
 * \param length Number of bytes to read.
 * \param[out] buf Output buffer.
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_read_by_nsid(uint32_t nsid, uint64_t offset,
			   uint64_t length, void *buf);

/**
 * Write bytes to a vSLM namespace by NSID.
 *
 * This API resolves vSLM paging internally and marks touched pages dirty.
 *
 * \param nsid Namespace ID of the vSLM bdev.
 * \param offset Byte offset to write to.
 * \param length Number of bytes to write.
 * \param buf Input buffer.
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_write_by_nsid(uint32_t nsid, uint64_t offset,
			    uint64_t length, const void *buf);

/**
 * Acquire a vSLM execution lease for one range.
 *
 * \param lease_id Caller-defined execution identifier.
 * \param nsid Memory namespace identifier.
 * \param offset Start offset in bytes.
 * \param length Length in bytes.
 * \return 0 on success, -ENOENT if nsid is not vSLM, -EAGAIN on overlap,
 *         negative errno on other failures.
 */
int bdev_vslm_lease_acquire(uint64_t lease_id, uint32_t nsid,
			    uint64_t offset, uint64_t length);

/**
 * Release all vSLM ranges held by a lease ID.
 *
 * \param lease_id Execution lease identifier.
 * \return 0 on success, -ENOENT if lease was not found.
 */
int bdev_vslm_lease_release(uint64_t lease_id);

/**
 * vSLM backing I/O statistics.
 */
struct spdk_bdev_vslm_stats {
	uint64_t backing_write_ops;
	uint64_t backing_write_bytes;
	uint64_t backing_write_tagged_bytes;
	uint64_t backing_write_untagged_bytes;
	uint64_t page_faults;
	uint64_t page_fault_bytes;
	uint64_t page_evictions;
	uint64_t page_eviction_bytes;
	uint64_t page_writebacks;
	uint64_t page_writeback_bytes;
	uint64_t dirty_resident_pages;
	uint64_t dirty_writeback_bytes;
	uint64_t lease_conflicts;
	uint64_t lease_blocked_ns;
	uint64_t admission_rejects;
};

/**
 * Configure FDP placement tagging policy for vSLM backing writes.
 *
 * \param name vSLM bdev name.
 * \param enabled true enables FDP tagging (separated placement).
 * \param dspec FDP directive specific selector (e.g., placement handle).
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_set_fdp_mode(const char *name, bool enabled, uint16_t dspec);

/**
 * Get vSLM backing I/O statistics.
 *
 * \param name vSLM bdev name.
 * \param stats Output statistics buffer.
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_get_stats(const char *name, struct spdk_bdev_vslm_stats *stats);

/**
 * Reset vSLM statistics.
 *
 * \param name vSLM bdev name.
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_reset_stats(const char *name);

/**
 * Set runtime policy for a vSLM bdev.
 *
 * \param name vSLM bdev name.
 * \param policy Runtime policy.
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_set_policy(const char *name, const struct spdk_bdev_vslm_policy *policy);

/**
 * Get runtime policy for a vSLM bdev.
 *
 * \param name vSLM bdev name.
 * \param[out] policy Output policy.
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_get_policy(const char *name, struct spdk_bdev_vslm_policy *policy);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_VSLM_H */

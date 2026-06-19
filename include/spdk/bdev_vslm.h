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
#include "spdk/bdev_slm.h"

#ifdef __cplusplus
extern "C" {
#endif

enum spdk_bdev_vslm_semantics_mode {
	SPDK_BDEV_VSLM_SEMANTICS_LEASE = 0,
};

enum spdk_bdev_vslm_writeback_policy {
	SPDK_BDEV_VSLM_WRITEBACK_ON_EVICT = 0,
};

enum spdk_bdev_vslm_page_source {
	SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING = 0,
	SPDK_BDEV_VSLM_PAGE_SOURCE_ALIAS_BDEV = 1,
};

enum spdk_bdev_vslm_lpage_state {
	SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS = 0,
	SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT = 1,
	SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT = 2,
	SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE = 3,
};

struct spdk_bdev_vslm_policy {
	enum spdk_bdev_vslm_semantics_mode semantics_mode;
	enum spdk_bdev_vslm_writeback_policy writeback_policy;
	bool admission_enabled;
	uint32_t admission_faults_per_sec_threshold;
	bool readahead_enabled;
	uint32_t readahead_pages;
};

struct spdk_bdev_vslm_opts {
	uint32_t nsid;
	struct spdk_bdev_vslm_policy policy;
	bool fdp_mode_enabled;
	uint16_t fdp_dspec;
	bool trace_events_enabled;
	char *trace_output_path;
	char *trace_run_id;
	/*
	 * Reserve an isolated backing region for evicted/spilled private pages
	 * (paper 3.2/4.4). When true, the upper half of the base namespace is
	 * dedicated to spill storage so the clean source data is preserved, and
	 * the exposed virtual size is halved. Defaults to false (legacy
	 * spill-in-place behavior, full base capacity exposed).
	 */
	bool backing_region_enabled;
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
 * - readahead_enabled: false
 * - readahead_pages: 4
 * - fdp_mode_enabled: false
 * - fdp_dspec: 0
 * - trace_events_enabled: false
 * - trace_output_path: NULL
 * - trace_run_id: NULL
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
 * \return -ENOTSUP for non-zero lengths. 0 with ptr == NULL for zero length.
 */
int bdev_vslm_get_buffer_ptr_by_nsid(uint32_t nsid, uint64_t offset,
				     uint64_t length, void **ptr);

/**
 * Pin one vSLM logical range and return SRAM SG entries.
 *
 * \param nsid Namespace ID of the vSLM bdev.
 * \param offset Byte offset into the logical range.
 * \param length Number of bytes to pin.
 * \param for_write true if caller may write pinned pages.
 * \param[out] entries Caller-provided SG entry array.
 * \param max_entries Capacity of `entries`.
 * \param[out] entry_count Number of returned entries.
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_pin_range_by_nsid(uint32_t nsid, uint64_t offset,
				uint64_t length, bool for_write,
				struct spdk_bdev_slm_sg_entry *entries,
				uint32_t max_entries, uint32_t *entry_count);

/**
 * Unpin vSLM SG entries previously returned by pin_range.
 *
 * \param nsid Namespace ID of the vSLM bdev.
 * \param entries SG entries returned by pin_range.
 * \param entry_count Number of entries in `entries`.
 * \param dirtied true if caller dirtied any pinned page.
 * \return 0 on success, negative errno on failure.
 */
int bdev_vslm_unpin_range_by_nsid(uint32_t nsid,
				  const struct spdk_bdev_slm_sg_entry *entries,
				  uint32_t entry_count, bool dirtied);

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
	uint64_t vslm_fault_clean_total;
	uint64_t vslm_fault_private_total;
	uint64_t page_evictions;
	uint64_t page_eviction_bytes;
	uint64_t vslm_eviction_clean_total;
	uint64_t vslm_eviction_private_total;
	uint64_t page_writebacks;
	uint64_t page_writeback_bytes;
	uint64_t vslm_spill_write_bytes;
	uint64_t vslm_spill_read_bytes;
	uint64_t vslm_fast_tier_bytes;
	uint64_t vslm_logical_working_set_bytes;
	uint64_t dirty_resident_pages;
	uint64_t vslm_resident_bytes_current;
	uint64_t vslm_resident_bytes_peak;
	uint64_t dirty_writeback_bytes;
	uint64_t lease_conflicts;
	uint64_t lease_blocked_ns;
	uint64_t admission_rejects;
	uint64_t vslm_lease_create_total;
	uint64_t vslm_lease_release_total;
	uint64_t vslm_host_read_during_execution_total;
	uint64_t vslm_host_write_conflict_total;
	uint64_t vslm_host_write_blocked_total;
	uint64_t vslm_host_write_nonconflict_total;
	uint64_t vslm_publish_total;
	uint64_t vslm_discard_total;
	uint64_t vslm_visibility_violation_total;
	uint64_t perf_fault_in_io_ns_total;
	uint64_t perf_fault_in_io_ns_max;
	uint64_t perf_mmu_lock_acquire_total;
	uint64_t perf_mmu_lock_hold_ns_total;
	uint64_t perf_mmu_lock_hold_ns_max;
	uint64_t perf_sync_base_io_total;
	uint64_t perf_sync_base_io_ns_total;
	uint64_t perf_sync_base_io_ns_max;
	uint64_t perf_sync_read_desc_io_total;
	uint64_t perf_sync_read_desc_io_ns_total;
	uint64_t perf_sync_read_desc_io_ns_max;
	uint64_t perf_fault_batch_attempt_total;
	uint64_t perf_fault_batch_fallback_total;
	uint64_t perf_prefetch_async_wait_total;
	uint64_t perf_prefetch_async_wait_ns_total;
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

/**
 * Debug/benchmark overrides for ablation studies (paper Sec. 5 mechanism
 * ablation). These reach internal knobs that have no production-facing RPC:
 * the partitioned-MMU shard count (normally adapted from the SRAM size), the
 * asynchronous execution / batching paths, and two mechanisms that are
 * normally auto-selected with no runtime knob -- the static DMA ring-buffer
 * fallback (paper 4.5) and the CoW full-overwrite media-bypass. Intended to be
 * called on an IDLE bdev (immediately after create, before any I/O); changing
 * the shard count requires the MMU to be empty and returns -EBUSY otherwise.
 *
 * Tri-state integer fields: -1 leaves the knob unchanged, 0 disables, 1
 * enables. num_shards == 0 leaves the shard count unchanged.
 */
struct spdk_bdev_vslm_debug {
	uint32_t num_shards;		/* 0 = unchanged; else re-shard idle MMU (clamped) */
	int async_exec;			/* -1 unchanged / 0 off / 1 on */
	int fault_batch;
	int prefetch_batch;
	int background_cleaner;
	int streaming_mode;
	int force_dma_fallback;		/* 1 = force DMA ring-buffer fallback (paper 4.5) */
	int disable_cow_bypass;		/* 1 = disable CoW full-overwrite media-bypass */
};

/** Initialize a debug-override struct to "change nothing". */
void bdev_vslm_debug_init(struct spdk_bdev_vslm_debug *dbg);

/**
 * Apply debug/benchmark overrides to a vSLM bdev (see struct
 * spdk_bdev_vslm_debug). For ablation experiments only.
 *
 * \param name vSLM bdev name.
 * \param dbg Overrides to apply.
 * \return 0 on success, negative errno on failure (-EBUSY if a shard-count
 *         change is requested while the MMU is not idle).
 */
int bdev_vslm_set_debug(const char *name, const struct spdk_bdev_vslm_debug *dbg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_VSLM_H */

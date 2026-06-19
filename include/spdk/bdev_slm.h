/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * SLM (Subsystem Local Memory) bdev public interface
 * Memory namespace for CPCS (Computational Programs Command Set)
 */

#ifndef SPDK_BDEV_SLM_H
#define SPDK_BDEV_SLM_H

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_bdev;
typedef void (*spdk_bdev_slm_io_completion_cb)(void *cb_arg, int status);

struct spdk_bdev_slm_sg_entry {
	void *addr;
	uint64_t logical_offset;
	uint32_t len;
	uint64_t vpn;
	uint64_t ppn;
	bool dirtied;
};

/**
 * Create a SLM (Subsystem Local Memory) bdev.
 *
 * Creates a memory-backed bdev for use as a Memory Namespace
 * in the CPCS command set. The SLM provides byte-granular
 * memory access for program execution staging.
 *
 * \param name Name of the bdev.
 * \param nsid Namespace ID for this memory namespace.
 * \param size_mb Size of the memory buffer in MiB.
 * \param granularity Access granularity in bytes (typically 4 for dword).
 * \return 0 on success, negative errno on failure.
 */
int bdev_slm_create(const char *name, uint32_t nsid,
		    uint64_t size_mb, uint32_t granularity);

/**
 * Delete a SLM bdev.
 *
 * \param name Name of the bdev to delete.
 * \param cb_fn Callback function.
 * \param cb_arg Callback argument.
 */
void bdev_slm_delete(const char *name,
		     spdk_bdev_unregister_cb cb_fn, void *cb_arg);

/**
 * Get direct pointer to SLM buffer for zero-copy access.
 * Only for internal CPCS use.
 *
 * This function provides direct memory access to the SLM buffer
 * for efficient data transfer during program execution.
 *
 * \param name Name of the SLM bdev.
 * \param offset Byte offset into the buffer.
 * \param length Length of the region.
 * \param[out] ptr Pointer to the buffer region.
 * \return 0 on success, negative errno on failure.
 */
int bdev_slm_get_buffer_ptr(const char *name, uint64_t offset,
			    uint64_t length, void **ptr);

/**
 * Get direct pointer to SLM/vSLM buffer by bdev.
 * Only for internal CPCS/NVMf use.
 *
 * \param bdev Memory namespace bdev.
 * \param offset Byte offset into the buffer.
 * \param length Length of the region.
 * \param[out] ptr Pointer to the buffer region.
 * \return 0 on success, negative errno on failure.
 */
int bdev_slm_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				    uint64_t length, void **ptr);

/**
 * Pin one logical range and return SRAM scatter-gather entries.
 *
 * The returned entries remain valid until `bdev_slm_unpin_range_by_bdev()`.
 *
 * \param bdev Memory namespace bdev.
 * \param offset Byte offset into the logical range.
 * \param length Length of the range in bytes.
 * \param for_write true if caller intends to write the pinned ranges.
 * \param[out] entries Caller-provided SG array.
 * \param max_entries Capacity of `entries`.
 * \param[out] entry_count Number of valid entries returned.
 * \return 0 on success, negative errno on failure.
 */
int bdev_slm_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			       uint64_t length, bool for_write,
			       struct spdk_bdev_slm_sg_entry *entries,
			       uint32_t max_entries, uint32_t *entry_count);

/**
 * Try to pin one logical range without triggering vSLM fault-in or wait.
 *
 * This is intended for execute runtimes that want a resident-page fast path
 * but must yield to asynchronous read/write when the range is not already
 * resident. Providers return -EAGAIN when the range is valid but would require
 * page loading or waiting, and -ENOTSUP when the provider has no try-pin
 * capability.
 *
 * \param bdev Memory namespace bdev.
 * \param offset Byte offset into the logical range.
 * \param length Length of the range in bytes.
 * \param for_write true if caller intends to write the pinned ranges.
 * \param[out] entries Caller-provided SG array.
 * \param max_entries Capacity of `entries`.
 * \param[out] entry_count Number of valid entries returned.
 * \return 0 on success, -EAGAIN when pin would block/fault, negative errno on failure.
 */
int bdev_slm_try_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				   uint64_t length, bool for_write,
				   struct spdk_bdev_slm_sg_entry *entries,
				   uint32_t max_entries, uint32_t *entry_count);

/**
 * Unpin previously pinned SRAM scatter-gather entries.
 *
 * \param bdev Memory namespace bdev.
 * \param entries SG entries returned by `bdev_slm_pin_range_by_bdev()`.
 * \param entry_count Number of entries in `entries`.
 * \param dirtied true if caller dirtied any pinned range.
 * \return 0 on success, negative errno on failure.
 */
int bdev_slm_unpin_range_by_bdev(struct spdk_bdev *bdev,
				 const struct spdk_bdev_slm_sg_entry *entries,
				 uint32_t entry_count, bool dirtied);
/**
 * Read bytes from a memory namespace (SLM or vSLM) by bdev.
 *
 * \param bdev Memory namespace bdev.
 * \param offset Byte offset to read from.
 * \param length Number of bytes to read.
 * \param[out] buf Output buffer.
 * \return 0 on success, negative errno on failure.
 */
int bdev_slm_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			  uint64_t length, void *buf);

/**
 * Asynchronously read bytes from a memory namespace (SLM or vSLM) by bdev.
 *
 * \param bdev Memory namespace bdev.
 * \param offset Byte offset to read from.
 * \param length Number of bytes to read.
 * \param[out] buf Output buffer.
 * \param cb_fn Completion callback.
 * \param cb_arg Callback argument.
 * \return 0 on successful submission, negative errno on failure.
 */
int bdev_slm_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
				uint64_t length, void *buf,
				spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);

/**
 * Write bytes to a memory namespace (SLM or vSLM) by bdev.
 *
 * \param bdev Memory namespace bdev.
 * \param offset Byte offset to write to.
 * \param length Number of bytes to write.
 * \param buf Input buffer.
 * \return 0 on success, negative errno on failure.
 */
int bdev_slm_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			   uint64_t length, const void *buf);

/**
 * Asynchronously write bytes to a memory namespace (SLM or vSLM) by bdev.
 *
 * \param bdev Memory namespace bdev.
 * \param offset Byte offset to write to.
 * \param length Number of bytes to write.
 * \param buf Input buffer.
 * \param cb_fn Completion callback.
 * \param cb_arg Callback argument.
 * \return 0 on successful submission, negative errno on failure.
 */
int bdev_slm_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
				 uint64_t length, const void *buf,
				 spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);

/**
 * Copy bytes between memory namespaces (SLM or vSLM) by bdev.
 *
 * Providers may optimize this path (for example, metadata alias staging on
 * eligible vSLM destinations). Implementations fall back to read+write copy
 * when optimization is unavailable.
 *
 * \param dst_bdev Destination memory namespace bdev.
 * \param dst_offset Destination byte offset.
 * \param src_bdev Source memory namespace bdev.
 * \param src_offset Source byte offset.
 * \param length Number of bytes to copy.
 * \return 0 on success, negative errno on failure.
 */
int bdev_slm_copy_by_bdev(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
			  struct spdk_bdev *src_bdev, uint64_t src_offset,
			  uint64_t length);

/**
 * Asynchronously copy bytes between memory namespaces (SLM or vSLM) by bdev.
 *
 * \param dst_bdev Destination memory namespace bdev.
 * \param dst_offset Destination byte offset.
 * \param src_bdev Source memory namespace bdev.
 * \param src_offset Source byte offset.
 * \param length Number of bytes to copy.
 * \param cb_fn Completion callback.
 * \param cb_arg Callback argument.
 * \return 0 on successful submission, negative errno on failure.
 */
int bdev_slm_copy_by_bdev_async(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
				struct spdk_bdev *src_bdev, uint64_t src_offset,
				uint64_t length, spdk_bdev_slm_io_completion_cb cb_fn,
				void *cb_arg);

/**
 * Read bytes from execute working view by bdev.
 *
 * \param bdev Memory namespace bdev.
 * \param offset Byte offset to read from.
 * \param length Number of bytes to read.
 * \param[out] buf Output buffer.
 * \return 0 on success, negative errno on failure.
 */
int bdev_slm_exec_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			       uint64_t length, void *buf);

/**
 * Asynchronously read bytes from execute working view by bdev.
 *
 * \param bdev Memory namespace bdev.
 * \param offset Byte offset to read from.
 * \param length Number of bytes to read.
 * \param[out] buf Output buffer.
 * \param cb_fn Completion callback.
 * \param cb_arg Callback argument.
 * \return 0 on successful submission, negative errno on failure.
 */
int bdev_slm_exec_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
				     uint64_t length, void *buf,
				     spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);

/**
 * Write bytes to execute working view by bdev.
 *
 * \param bdev Memory namespace bdev.
 * \param offset Byte offset to write to.
 * \param length Number of bytes to write.
 * \param buf Input buffer.
 * \return 0 on success, negative errno on failure.
 */
int bdev_slm_exec_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				uint64_t length, const void *buf);

/**
 * Asynchronously write bytes to execute working view by bdev.
 *
 * \param bdev Memory namespace bdev.
 * \param offset Byte offset to write to.
 * \param length Number of bytes to write.
 * \param buf Input buffer.
 * \param cb_fn Completion callback.
 * \param cb_arg Callback argument.
 * \return 0 on successful submission, negative errno on failure.
 */
int bdev_slm_exec_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
				      uint64_t length, const void *buf,
				      spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);

/**
 * Acquire an SLM/vSLM lease for one range by bdev.
 *
 * \param lease_id Caller-defined execution identifier.
 * \param bdev Memory namespace bdev.
 * \param offset Start offset in bytes.
 * \param length Length in bytes.
 * \return 0 on success, -EAGAIN on overlap, negative errno on failure.
 */
int bdev_slm_lease_acquire_by_bdev(uint64_t lease_id, struct spdk_bdev *bdev,
				   uint64_t offset, uint64_t length);

/**
 * Release all SLM/vSLM ranges held by a lease ID.
 *
 * \param lease_id Execution lease identifier.
 * \return 0 on success, -ENOENT if lease was not found.
 */
int bdev_slm_lease_release(uint64_t lease_id);

/**
 * Publish execute working view for one lease into committed view.
 *
 * \param lease_id Execution lease identifier.
 * \return 0 on success, -ENOENT if lease was not found.
 */
int bdev_slm_exec_publish_lease(uint64_t lease_id);

/**
 * Discard execute working view for one lease.
 *
 * \param lease_id Execution lease identifier.
 * \return 0 on success, -ENOENT if lease was not found.
 */
int bdev_slm_exec_discard_lease(uint64_t lease_id);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_SLM_H */

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

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_SLM_H */

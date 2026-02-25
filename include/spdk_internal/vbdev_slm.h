/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 CPCS Implementation Team. All rights reserved.
 */

#ifndef SPDK_INTERNAL_VBDEV_SLM_H
#define SPDK_INTERNAL_VBDEV_SLM_H

#include "spdk/stdinc.h"

struct spdk_bdev;

struct spdk_vbdev_slm_ops {
	const char *name;
	bool (*owns_bdev)(struct spdk_bdev *bdev);
	int (*get_buffer_ptr_by_bdev)(struct spdk_bdev *bdev, uint64_t offset,
				      uint64_t length, void **ptr);
	int (*read_by_bdev)(struct spdk_bdev *bdev, uint64_t offset,
			    uint64_t length, void *buf);
	int (*write_by_bdev)(struct spdk_bdev *bdev, uint64_t offset,
			     uint64_t length, const void *buf);
};

int vbdev_slm_register_ops(const struct spdk_vbdev_slm_ops *ops);
void vbdev_slm_unregister_ops(const struct spdk_vbdev_slm_ops *ops);

int vbdev_slm_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				     uint64_t length, void **ptr);
int vbdev_slm_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			   uint64_t length, void *buf);
int vbdev_slm_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			    uint64_t length, const void *buf);

#endif /* SPDK_INTERNAL_VBDEV_SLM_H */

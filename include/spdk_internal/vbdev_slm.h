/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 CPCS Implementation Team. All rights reserved.
 */

#ifndef SPDK_INTERNAL_VBDEV_SLM_H
#define SPDK_INTERNAL_VBDEV_SLM_H

#include "spdk/stdinc.h"
#include "spdk/bdev_slm.h"

struct spdk_bdev;

struct spdk_vbdev_slm_ops {
	const char *name;
	bool (*owns_bdev)(struct spdk_bdev *bdev);
	int (*get_buffer_ptr_by_bdev)(struct spdk_bdev *bdev, uint64_t offset,
				      uint64_t length, void **ptr);
	int (*pin_range_by_bdev)(struct spdk_bdev *bdev, uint64_t offset,
				 uint64_t length, bool for_write,
				 struct spdk_bdev_slm_sg_entry *entries,
				 uint32_t max_entries, uint32_t *entry_count);
	int (*try_pin_range_by_bdev)(struct spdk_bdev *bdev, uint64_t offset,
				     uint64_t length, bool for_write,
				     struct spdk_bdev_slm_sg_entry *entries,
				     uint32_t max_entries, uint32_t *entry_count);
	int (*unpin_range_by_bdev)(struct spdk_bdev *bdev,
				   const struct spdk_bdev_slm_sg_entry *entries,
				   uint32_t entry_count, bool dirtied);
	int (*read_by_bdev)(struct spdk_bdev *bdev, uint64_t offset,
			    uint64_t length, void *buf);
	int (*read_by_bdev_async)(struct spdk_bdev *bdev, uint64_t offset,
				  uint64_t length, void *buf,
				  spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);
	int (*write_by_bdev)(struct spdk_bdev *bdev, uint64_t offset,
			     uint64_t length, const void *buf);
	int (*write_by_bdev_async)(struct spdk_bdev *bdev, uint64_t offset,
				   uint64_t length, const void *buf,
				   spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);
	int (*copy_by_bdev)(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
			    struct spdk_bdev *src_bdev, uint64_t src_offset,
			    uint64_t length);
	int (*copy_by_bdev_async)(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
				  struct spdk_bdev *src_bdev, uint64_t src_offset,
				  uint64_t length, spdk_bdev_slm_io_completion_cb cb_fn,
				  void *cb_arg);
	int (*exec_read_by_bdev)(struct spdk_bdev *bdev, uint64_t offset,
				 uint64_t length, void *buf);
	int (*exec_read_by_bdev_async)(struct spdk_bdev *bdev, uint64_t offset,
				       uint64_t length, void *buf,
				       spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);
	int (*exec_write_by_bdev)(struct spdk_bdev *bdev, uint64_t offset,
				  uint64_t length, const void *buf);
	int (*exec_write_by_bdev_async)(struct spdk_bdev *bdev, uint64_t offset,
					uint64_t length, const void *buf,
					spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);
	int (*lease_acquire_by_bdev)(uint64_t lease_id, struct spdk_bdev *bdev,
				     uint64_t offset, uint64_t length);
	int (*exec_publish_lease)(uint64_t lease_id);
	int (*exec_discard_lease)(uint64_t lease_id);
	int (*lease_release)(uint64_t lease_id);
};

int vbdev_slm_register_ops(const struct spdk_vbdev_slm_ops *ops);
void vbdev_slm_unregister_ops(const struct spdk_vbdev_slm_ops *ops);

int vbdev_slm_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				     uint64_t length, void **ptr);
int vbdev_slm_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				uint64_t length, bool for_write,
				struct spdk_bdev_slm_sg_entry *entries,
				uint32_t max_entries, uint32_t *entry_count);
int vbdev_slm_try_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				    uint64_t length, bool for_write,
				    struct spdk_bdev_slm_sg_entry *entries,
				    uint32_t max_entries, uint32_t *entry_count);
int vbdev_slm_unpin_range_by_bdev(struct spdk_bdev *bdev,
				  const struct spdk_bdev_slm_sg_entry *entries,
				  uint32_t entry_count, bool dirtied);
int vbdev_slm_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			   uint64_t length, void *buf);
int vbdev_slm_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
				 uint64_t length, void *buf,
				 spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);
int vbdev_slm_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			    uint64_t length, const void *buf);
int vbdev_slm_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
				  uint64_t length, const void *buf,
				  spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);
int vbdev_slm_copy_by_bdev(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
			   struct spdk_bdev *src_bdev, uint64_t src_offset,
			   uint64_t length);
int vbdev_slm_copy_by_bdev_async(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
				 struct spdk_bdev *src_bdev, uint64_t src_offset,
				 uint64_t length, spdk_bdev_slm_io_completion_cb cb_fn,
				 void *cb_arg);
int vbdev_slm_exec_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				uint64_t length, void *buf);
int vbdev_slm_exec_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
				      uint64_t length, void *buf,
				      spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);
int vbdev_slm_exec_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				 uint64_t length, const void *buf);
int vbdev_slm_exec_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
				       uint64_t length, const void *buf,
				       spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);
int vbdev_slm_lease_acquire_by_bdev(uint64_t lease_id, struct spdk_bdev *bdev,
				    uint64_t offset, uint64_t length);
int vbdev_slm_exec_publish_lease(uint64_t lease_id);
int vbdev_slm_exec_discard_lease(uint64_t lease_id);
int vbdev_slm_lease_release(uint64_t lease_id);

const struct spdk_vbdev_slm_ops *vbdev_slm_lookup_ops_by_bdev(struct spdk_bdev *bdev);

#endif /* SPDK_INTERNAL_VBDEV_SLM_H */

/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 CPCS Implementation Team. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/bdev_vslm.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/nvme_spec.h"
#include "spdk/util.h"
#include "spdk_internal/vbdev_slm.h"

#define VSLM_PAGE_SIZE  4096
#define VSLM_PAGE_SHIFT 12

/* SLM command set opcodes (CSI 0x03) */
#define SPDK_NVME_SLM_OPC_MEMORY_COPY  0x01
#define SPDK_NVME_SLM_OPC_MEMORY_READ  0x02
#define SPDK_NVME_SLM_OPC_MEMORY_FILL  0x04
#define SPDK_NVME_SLM_OPC_MEMORY_WRITE 0x05

static const struct spdk_bdev_fn_table vbdev_vslm_fn_table;
static bool vbdev_vslm_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);

static TAILQ_HEAD(, vbdev_vslm) g_vslm_bdevs = TAILQ_HEAD_INITIALIZER(g_vslm_bdevs);

struct vslm_page {
	uint64_t vpn;
	uint64_t ppn;
	bool dirty;
	bool is_busy;
	bool in_lru;
	TAILQ_ENTRY(vslm_page) lru_link;
	LIST_ENTRY(vslm_page) hash_link;
};

struct vslm_lease {
	uint64_t lease_id;
	uint64_t offset;
	uint64_t length;
	TAILQ_ENTRY(vslm_lease) link;
};

struct vslm_blocked_cmd {
	struct spdk_bdev_io *bdev_io;
	uint64_t queued_ticks;
	TAILQ_ENTRY(vslm_blocked_cmd) link;
};

struct vbdev_vslm {
	/* Backing store (NAND) state */
	struct spdk_bdev *base_bdev;
	struct spdk_bdev_desc *base_desc;
	char *base_bdev_name;
	uint64_t virtual_size_bytes;

	/* SRAM cache state */
	uint8_t *sram_buffer;
	uint64_t sram_size_bytes;
	uint64_t num_sram_pages;

	/* MMU structures */
	struct vslm_page *page_array;
	TAILQ_HEAD(, vslm_page) lru_list;
	TAILQ_HEAD(, vslm_page) free_list;
	LIST_HEAD(, vslm_page) *hash_table;
	pthread_mutex_t mmu_lock;

	/* Lease semantics */
	pthread_mutex_t lease_lock;
	TAILQ_HEAD(, vslm_lease) leases;
	TAILQ_HEAD(, vslm_blocked_cmd) blocked_cmds;

	/* Policy runtime state */
	pthread_mutex_t policy_lock;
	uint64_t admission_window_start_ticks;
	uint64_t admission_faults_in_window;

	/* Endurance/FDP policy and telemetry */
	struct spdk_bdev_vslm_policy policy;
	bool fdp_mode_enabled;
	uint16_t fdp_dspec;
	bool base_fdp_supported;
	struct spdk_bdev_vslm_stats stats;

	/* SPDK integration */
	struct spdk_bdev vbdev;
	TAILQ_ENTRY(vbdev_vslm) link;
};

struct vslm_channel {
	struct spdk_io_channel *base_ch;
};

static int vbdev_vslm_module_init(void);
static void vbdev_vslm_module_fini(void);

static struct spdk_bdev_module vslm_if = {
	.name = "vslm",
	.module_init = vbdev_vslm_module_init,
	.module_fini = vbdev_vslm_module_fini,
};

static int vslm_get_direct_ptr(struct vbdev_vslm *vslm, uint64_t offset,
			       uint64_t length, void **ptr_out, bool mark_dirty);
static int vbdev_vslm_ch_create_cb(void *io_device, void *ctx_buf);
static void vbdev_vslm_ch_destroy_cb(void *io_device, void *ctx_buf);
static struct spdk_io_channel *vbdev_vslm_get_io_channel(void *ctx);
static void vbdev_vslm_submit_request(struct spdk_io_channel *ch,
				      struct spdk_bdev_io *bdev_io);
static void vbdev_vslm_submit_nvme_passthru(struct vbdev_vslm *vslm,
					    struct spdk_io_channel *base_ch,
					    struct spdk_bdev_io *bdev_io);
static bool vslm_range_overlap(uint64_t offset_a, uint64_t length_a,
			       uint64_t offset_b, uint64_t length_b);
static bool vslm_has_lease_conflict(struct vbdev_vslm *vslm, uint64_t offset,
				    uint64_t length);
static int vslm_queue_blocked_cmd(struct vbdev_vslm *vslm,
					  struct spdk_bdev_io *bdev_io,
					  uint64_t offset, uint64_t length);
static void vslm_process_blocked_cmds(struct vbdev_vslm *vslm);
static bool vslm_admission_allow_execute(struct vbdev_vslm *vslm);
static void vslm_note_page_fault(struct vbdev_vslm *vslm);
static void vslm_page_mark_dirty(struct vbdev_vslm *vslm, struct vslm_page *page);
static void vslm_page_mark_clean(struct vbdev_vslm *vslm, struct vslm_page *page);
static bool vslm_policy_is_boundary_flush(enum spdk_bdev_vslm_writeback_policy policy);
static int vslm_flush_dirty_pages(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch);

static void
vbdev_vslm_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		   void *event_ctx)
{
	(void)type;
	(void)bdev;
	(void)event_ctx;
}

static struct vbdev_vslm *
vbdev_vslm_get_by_nsid(uint32_t nsid)
{
	struct vbdev_vslm *vslm;

	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (vslm->vbdev.nsid == nsid) {
			return vslm;
		}
	}

	return NULL;
}

static struct vbdev_vslm *
vbdev_vslm_get_by_bdev(struct spdk_bdev *bdev)
{
	struct vbdev_vslm *vslm;

	if (bdev == NULL || bdev->module != &vslm_if) {
		return NULL;
	}

	vslm = bdev->ctxt;
	if (vslm == NULL || &vslm->vbdev != bdev) {
		return NULL;
	}

	return vslm;
}

static bool
vbdev_vslm_mem_owns_bdev(struct spdk_bdev *bdev)
{
	return vbdev_vslm_get_by_bdev(bdev) != NULL;
}

static int vbdev_vslm_mem_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
		uint64_t length, void **ptr);
static int vbdev_vslm_mem_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				       uint64_t length, void *buf);
static int vbdev_vslm_mem_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
					uint64_t length, const void *buf);

static const struct spdk_vbdev_slm_ops g_vslm_mem_ops = {
	.name = "vslm",
	.owns_bdev = vbdev_vslm_mem_owns_bdev,
	.get_buffer_ptr_by_bdev = vbdev_vslm_mem_get_buffer_ptr_by_bdev,
	.read_by_bdev = vbdev_vslm_mem_read_by_bdev,
	.write_by_bdev = vbdev_vslm_mem_write_by_bdev,
};

static int
vbdev_vslm_module_init(void)
{
	return vbdev_slm_register_ops(&g_vslm_mem_ops);
}

static void
vbdev_vslm_module_fini(void)
{
	vbdev_slm_unregister_ops(&g_vslm_mem_ops);
}

static void
vslm_page_mark_dirty(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	if (!page->dirty) {
		page->dirty = true;
		__atomic_add_fetch(&vslm->stats.dirty_resident_pages, 1, __ATOMIC_RELAXED);
	}
}

static void
vslm_page_mark_clean(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	if (page->dirty) {
		page->dirty = false;
		__atomic_sub_fetch(&vslm->stats.dirty_resident_pages, 1, __ATOMIC_RELAXED);
	}
}

static bool
vslm_policy_is_boundary_flush(enum spdk_bdev_vslm_writeback_policy policy)
{
	return policy == SPDK_BDEV_VSLM_WRITEBACK_AT_BOUNDARY ||
	       policy == SPDK_BDEV_VSLM_WRITEBACK_HYBRID;
}

static void
vslm_note_page_fault(struct vbdev_vslm *vslm)
{
	uint64_t now_ticks;
	uint64_t hz;

	__atomic_add_fetch(&vslm->stats.page_faults, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&vslm->stats.page_fault_bytes, VSLM_PAGE_SIZE, __ATOMIC_RELAXED);

	now_ticks = spdk_get_ticks();
	hz = spdk_get_ticks_hz();

	pthread_mutex_lock(&vslm->policy_lock);
	if (vslm->admission_window_start_ticks == 0 ||
	    hz == 0 ||
	    now_ticks < vslm->admission_window_start_ticks ||
	    (now_ticks - vslm->admission_window_start_ticks) >= hz) {
		vslm->admission_window_start_ticks = now_ticks;
		vslm->admission_faults_in_window = 0;
	}
	vslm->admission_faults_in_window++;
	pthread_mutex_unlock(&vslm->policy_lock);
}

static bool
vslm_admission_allow_execute(struct vbdev_vslm *vslm)
{
	bool allow = true;
	uint64_t now_ticks;
	uint64_t hz;

	now_ticks = spdk_get_ticks();
	hz = spdk_get_ticks_hz();

	pthread_mutex_lock(&vslm->policy_lock);
	if (!vslm->policy.admission_enabled) {
		goto out;
	}

	if (vslm->admission_window_start_ticks == 0 ||
	    hz == 0 ||
	    now_ticks < vslm->admission_window_start_ticks ||
	    (now_ticks - vslm->admission_window_start_ticks) >= hz) {
		vslm->admission_window_start_ticks = now_ticks;
		vslm->admission_faults_in_window = 0;
		goto out;
	}

	if (vslm->admission_faults_in_window >=
	    vslm->policy.admission_faults_per_sec_threshold) {
		allow = false;
		__atomic_add_fetch(&vslm->stats.admission_rejects, 1, __ATOMIC_RELAXED);
	}

out:
	pthread_mutex_unlock(&vslm->policy_lock);
	return allow;
}

static bool
vslm_range_overlap(uint64_t offset_a, uint64_t length_a,
		   uint64_t offset_b, uint64_t length_b)
{
	uint64_t end_a;
	uint64_t end_b;

	if (length_a == 0 || length_b == 0) {
		return false;
	}

	end_a = offset_a + length_a;
	end_b = offset_b + length_b;
	if (end_a < offset_a || end_b < offset_b) {
		return true;
	}

	return !(end_a <= offset_b || end_b <= offset_a);
}

static bool
vslm_has_lease_conflict(struct vbdev_vslm *vslm, uint64_t offset, uint64_t length)
{
	struct vslm_lease *lease;
	bool conflict = false;

	pthread_mutex_lock(&vslm->lease_lock);
	TAILQ_FOREACH(lease, &vslm->leases, link) {
		if (vslm_range_overlap(offset, length, lease->offset, lease->length)) {
			conflict = true;
			break;
		}
	}
	pthread_mutex_unlock(&vslm->lease_lock);

	return conflict;
}

static int
vslm_queue_blocked_cmd(struct vbdev_vslm *vslm,
		       struct spdk_bdev_io *bdev_io,
		       uint64_t offset, uint64_t length)
{
	struct vslm_blocked_cmd *blocked;
	struct vslm_lease *lease;
	bool conflict = false;
	int rc = 0;

	blocked = calloc(1, sizeof(*blocked));
	if (blocked == NULL) {
		return -ENOMEM;
	}

	pthread_mutex_lock(&vslm->lease_lock);
	TAILQ_FOREACH(lease, &vslm->leases, link) {
		if (vslm_range_overlap(offset, length, lease->offset, lease->length)) {
			conflict = true;
			break;
		}
	}
	if (!conflict) {
		rc = -EAGAIN;
		goto out_unlock;
	}

	blocked->bdev_io = bdev_io;
	blocked->queued_ticks = spdk_get_ticks();
	TAILQ_INSERT_TAIL(&vslm->blocked_cmds, blocked, link);
	__atomic_add_fetch(&vslm->stats.lease_conflicts, 1, __ATOMIC_RELAXED);
	SPDK_DEBUGLOG(bdev, "vSLM queued conflicting command nsid=%u off=%" PRIu64 " len=%" PRIu64 "\n",
		      vslm->vbdev.nsid, offset, length);
	pthread_mutex_unlock(&vslm->lease_lock);
	return 0;

out_unlock:
	pthread_mutex_unlock(&vslm->lease_lock);
	free(blocked);
	return rc;
}

static void
vslm_process_blocked_cmds(struct vbdev_vslm *vslm)
{
	struct vslm_blocked_cmd *blocked, *tmp;
	struct spdk_io_channel *io_ch;
	struct vslm_channel *vch;
	TAILQ_HEAD(, vslm_blocked_cmd) pending =
		TAILQ_HEAD_INITIALIZER(pending);

	pthread_mutex_lock(&vslm->lease_lock);
	TAILQ_FOREACH_SAFE(blocked, &vslm->blocked_cmds, link, tmp) {
		TAILQ_REMOVE(&vslm->blocked_cmds, blocked, link);
		TAILQ_INSERT_TAIL(&pending, blocked, link);
	}
	pthread_mutex_unlock(&vslm->lease_lock);

	TAILQ_FOREACH_SAFE(blocked, &pending, link, tmp) {
		uint64_t now_ticks = spdk_get_ticks();
		uint64_t hz = spdk_get_ticks_hz();

		TAILQ_REMOVE(&pending, blocked, link);
		if (hz != 0 && now_ticks > blocked->queued_ticks) {
			__atomic_add_fetch(&vslm->stats.lease_blocked_ns,
					   ((now_ticks - blocked->queued_ticks) * SPDK_SEC_TO_NSEC) / hz,
					   __ATOMIC_RELAXED);
		}
		io_ch = spdk_bdev_io_get_io_channel(blocked->bdev_io);
		if (io_ch == NULL) {
			spdk_bdev_io_complete(blocked->bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			free(blocked);
			continue;
		}

		vch = spdk_io_channel_get_ctx(io_ch);
		vbdev_vslm_submit_nvme_passthru(vslm, vch->base_ch, blocked->bdev_io);
		free(blocked);
	}
}

static int
vslm_init_mmu(struct vbdev_vslm *vslm)
{
	uint64_t i;

	vslm->num_sram_pages = vslm->sram_size_bytes / VSLM_PAGE_SIZE;

	vslm->sram_buffer = spdk_dma_malloc(vslm->sram_size_bytes, VSLM_PAGE_SIZE, NULL);
	if (!vslm->sram_buffer) {
		return -1;
	}

	vslm->page_array = calloc(vslm->num_sram_pages, sizeof(*vslm->page_array));
	if (!vslm->page_array) {
		spdk_dma_free(vslm->sram_buffer);
		vslm->sram_buffer = NULL;
		return -1;
	}

	vslm->hash_table = calloc(vslm->num_sram_pages, sizeof(*vslm->hash_table));
	if (!vslm->hash_table) {
		free(vslm->page_array);
		vslm->page_array = NULL;
		spdk_dma_free(vslm->sram_buffer);
		vslm->sram_buffer = NULL;
		return -1;
	}

	TAILQ_INIT(&vslm->lru_list);
	TAILQ_INIT(&vslm->free_list);

	for (i = 0; i < vslm->num_sram_pages; i++) {
		vslm->page_array[i].ppn = i;
		vslm->page_array[i].vpn = UINT64_MAX;
		TAILQ_INSERT_TAIL(&vslm->free_list, &vslm->page_array[i], lru_link);
		LIST_INIT(&vslm->hash_table[i]);
	}

	return 0;
}

static uint64_t
vslm_hash_func(struct vbdev_vslm *vslm, uint64_t vpn)
{
	return vpn % vslm->num_sram_pages;
}

static struct vslm_page *
vslm_lookup_page(struct vbdev_vslm *vslm, uint64_t vpn)
{
	struct vslm_page *page;
	uint64_t idx;

	idx = vslm_hash_func(vslm, vpn);
	LIST_FOREACH(page, &vslm->hash_table[idx], hash_link) {
		if (page->vpn == vpn) {
			return page;
		}
	}

	return NULL;
}

static void
vslm_hash_insert(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	uint64_t idx;

	idx = vslm_hash_func(vslm, page->vpn);
	LIST_INSERT_HEAD(&vslm->hash_table[idx], page, hash_link);
}

static void
vslm_hash_remove(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	(void)vslm;
	LIST_REMOVE(page, hash_link);
}

static void
vslm_lru_touch(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	if (page->in_lru) {
		TAILQ_REMOVE(&vslm->lru_list, page, lru_link);
	}
	TAILQ_INSERT_TAIL(&vslm->lru_list, page, lru_link);
	page->in_lru = true;
}

static void
vslm_lru_remove(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	TAILQ_REMOVE(&vslm->lru_list, page, lru_link);
	page->in_lru = false;
}

static struct vslm_page *
vslm_get_free_page(struct vbdev_vslm *vslm)
{
	struct vslm_page *page;

	page = TAILQ_FIRST(&vslm->free_list);
	if (page) {
		TAILQ_REMOVE(&vslm->free_list, page, lru_link);
		page->in_lru = false;
	}

	return page;
}

static struct vslm_page *
vslm_pick_victim(struct vbdev_vslm *vslm)
{
	struct vslm_page *page;

	TAILQ_FOREACH(page, &vslm->lru_list, lru_link) {
		if (!page->is_busy) {
			TAILQ_REMOVE(&vslm->lru_list, page, lru_link);
			page->in_lru = false;
			return page;
		}
	}

	return NULL;
}

static int
vslm_get_direct_ptr(struct vbdev_vslm *vslm, uint64_t offset, uint64_t length,
		    void **ptr_out, bool mark_dirty)
{
	struct vslm_page *page;
	uint64_t start_vpn;
	uint64_t end_vpn;
	uint64_t vpn;
	uint64_t page_offset;

	if (!ptr_out) {
		return -EINVAL;
	}

	if (length == 0) {
		*ptr_out = vslm->sram_buffer;
		return 0;
	}

	if (offset >= vslm->sram_size_bytes || length > vslm->sram_size_bytes - offset) {
		return -EINVAL;
	}

	start_vpn = offset / VSLM_PAGE_SIZE;
	end_vpn = (offset + length - 1) / VSLM_PAGE_SIZE;

	if (end_vpn >= vslm->num_sram_pages) {
		return -EINVAL;
	}

	pthread_mutex_lock(&vslm->mmu_lock);
	for (vpn = start_vpn; vpn <= end_vpn; vpn++) {
		page = &vslm->page_array[vpn];
		if (page->vpn == UINT64_MAX) {
			TAILQ_REMOVE(&vslm->free_list, page, lru_link);
			page->vpn = vpn;
			page->dirty = false;
			vslm_hash_insert(vslm, page);
			vslm_lru_touch(vslm, page);
			memset(vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE), 0, VSLM_PAGE_SIZE);
		} else if (page->vpn != vpn) {
			pthread_mutex_unlock(&vslm->mmu_lock);
			return -EBUSY;
		} else {
			vslm_lru_touch(vslm, page);
		}

		/* Direct pointer calls are synchronous. Keep pages evictable. */
		page->is_busy = false;

		if (mark_dirty) {
			vslm_page_mark_dirty(vslm, page);
		}
	}

	page_offset = offset % VSLM_PAGE_SIZE;
	*ptr_out = vslm->sram_buffer + (start_vpn * VSLM_PAGE_SIZE) + page_offset;
	pthread_mutex_unlock(&vslm->mmu_lock);
	return 0;
}

struct vslm_sync_io_ctx {
	bool done;
	int status;
};

static void
vslm_sync_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct vslm_sync_io_ctx *ctx = cb_arg;

	spdk_bdev_free_io(bdev_io);
	ctx->status = success ? 0 : -EIO;
	ctx->done = true;
}

static int
vslm_submit_sync_base_io(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
			 void *buf, uint64_t offset_blocks, uint64_t num_blocks, bool is_write)
{
	struct vslm_sync_io_ctx ctx = {};
	struct spdk_bdev_ext_io_opts io_opts = {};
	struct iovec iov = {};
	struct spdk_thread *thread;
	uint64_t block_size;
	uint64_t bytes;
	bool tagged = false;
	bool submitted = false;
	int rc;

	if (num_blocks == 0) {
		return 0;
	}

	thread = spdk_get_thread();
	if (thread == NULL) {
		return -EINVAL;
	}

	block_size = spdk_bdev_get_block_size(vslm->base_bdev);
	bytes = num_blocks * block_size;

	if (is_write && vslm->fdp_mode_enabled) {
		iov.iov_base = buf;
		iov.iov_len = bytes;
		io_opts.size = sizeof(io_opts);
		io_opts.nvme_cdw12.write.dtype = SPDK_NVME_DIRECTIVE_TYPE_DATA_PLACEMENT;
		io_opts.nvme_cdw13.write.dspec = vslm->fdp_dspec;

		rc = spdk_bdev_writev_blocks_ext(vslm->base_desc, base_ch,
						 &iov, 1, offset_blocks, num_blocks,
						 vslm_sync_io_completion_cb, &ctx, &io_opts);
		if (rc == 0) {
			tagged = true;
			submitted = true;
		} else if (rc == -ENOTSUP) {
			/* Fall back to mixed-placement write path. */
			rc = 0;
		}
	}

	if (!submitted) {
		if (is_write) {
			rc = spdk_bdev_write_blocks(vslm->base_desc, base_ch, buf, offset_blocks,
						    num_blocks, vslm_sync_io_completion_cb, &ctx);
		} else {
			rc = spdk_bdev_read_blocks(vslm->base_desc, base_ch, buf, offset_blocks,
						   num_blocks, vslm_sync_io_completion_cb, &ctx);
		}
		submitted = (rc == 0);
	}
	if (rc != 0) {
		return rc;
	}

	{
		uint64_t start_ticks = spdk_get_ticks();
		uint64_t hz = spdk_get_ticks_hz();
		bool warned = false;

		while (!ctx.done) {
			spdk_thread_poll(thread, 0, 0);
			if (!warned && hz > 0 &&
			    (spdk_get_ticks() - start_ticks) >= hz * 30) {
				SPDK_ERRLOG("vSLM sync base I/O pending >30s — possible backing device hang\n");
				warned = true;
			}
		}
	}

	if (is_write && submitted && ctx.status == 0) {
		__atomic_add_fetch(&vslm->stats.backing_write_ops, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&vslm->stats.backing_write_bytes, bytes, __ATOMIC_RELAXED);
		if (tagged) {
			__atomic_add_fetch(&vslm->stats.backing_write_tagged_bytes, bytes, __ATOMIC_RELAXED);
		} else {
			__atomic_add_fetch(&vslm->stats.backing_write_untagged_bytes, bytes, __ATOMIC_RELAXED);
		}
	}

	return ctx.status;
}

static int
vslm_writeback_page(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		    struct vslm_page *page)
{
	uint64_t block_size;
	uint64_t page_blocks;
	uint64_t offset_blocks;
	uint8_t *ptr;
	int rc;

	if (!page->dirty) {
		return 0;
	}

	block_size = spdk_bdev_get_block_size(vslm->base_bdev);
	page_blocks = VSLM_PAGE_SIZE / block_size;
	offset_blocks = page->vpn * page_blocks;
	ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE);

	rc = vslm_submit_sync_base_io(vslm, base_ch, ptr, offset_blocks, page_blocks, true);
	if (rc == 0) {
		vslm_page_mark_clean(vslm, page);
		__atomic_add_fetch(&vslm->stats.page_writebacks, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&vslm->stats.page_writeback_bytes, VSLM_PAGE_SIZE, __ATOMIC_RELAXED);
		__atomic_add_fetch(&vslm->stats.dirty_writeback_bytes, VSLM_PAGE_SIZE, __ATOMIC_RELAXED);
	}

	return rc;
}

static int
vslm_fault_in_page(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		   struct vslm_page *page, uint64_t vpn)
{
	uint64_t block_size;
	uint64_t page_blocks;
	uint64_t offset_blocks;
	uint8_t *ptr;

	block_size = spdk_bdev_get_block_size(vslm->base_bdev);
	page_blocks = VSLM_PAGE_SIZE / block_size;
	offset_blocks = vpn * page_blocks;
	ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE);

	return vslm_submit_sync_base_io(vslm, base_ch, ptr, offset_blocks, page_blocks, false);
}

static int
vslm_resolve_page(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		  uint64_t vpn, struct vslm_page **out_page)
{
	struct vslm_page *page;
	uint64_t max_vpn;
	uint64_t victim_vpn;
	int rc;

	max_vpn = vslm->virtual_size_bytes / VSLM_PAGE_SIZE;
	if (vpn >= max_vpn) {
		return -EINVAL;
	}

	page = vslm_lookup_page(vslm, vpn);
	if (page != NULL) {
		vslm_lru_touch(vslm, page);
		*out_page = page;
		return 0;
	}

	page = vslm_get_free_page(vslm);
	if (page == NULL) {
		page = vslm_pick_victim(vslm);
		if (page == NULL) {
			return -ENOMEM;
		}

		__atomic_add_fetch(&vslm->stats.page_evictions, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&vslm->stats.page_eviction_bytes, VSLM_PAGE_SIZE, __ATOMIC_RELAXED);
		victim_vpn = page->vpn;
		vslm_hash_remove(vslm, page);
		if (page->dirty) {
			rc = vslm_writeback_page(vslm, base_ch, page);
			if (rc != 0) {
				page->vpn = victim_vpn;
				vslm_hash_insert(vslm, page);
				vslm_lru_touch(vslm, page);
				return rc;
			}
		}
	}

	page->vpn = vpn;
	vslm_page_mark_clean(vslm, page);
	page->is_busy = false;
	vslm_hash_insert(vslm, page);
	vslm_lru_touch(vslm, page);

	rc = vslm_fault_in_page(vslm, base_ch, page, vpn);
	if (rc != 0) {
		vslm_hash_remove(vslm, page);
		page->vpn = UINT64_MAX;
		vslm_page_mark_clean(vslm, page);
		TAILQ_INSERT_TAIL(&vslm->free_list, page, lru_link);
		return rc;
	}

	vslm_note_page_fault(vslm);
	*out_page = page;
	return 0;
}

static int
vslm_copy_range(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		uint64_t starting_byte, uint32_t length, uint8_t *buf, bool is_write)
{
	struct vslm_page *page;
	uint64_t offset = 0;
	uint64_t absolute;
	uint64_t vpn;
	uint64_t page_offset;
	uint64_t chunk;
	uint8_t *ptr;
	int rc;

	if (length == 0) {
		return 0;
	}

	if (starting_byte >= vslm->virtual_size_bytes ||
	    length > vslm->virtual_size_bytes - starting_byte) {
		return -EINVAL;
	}

	pthread_mutex_lock(&vslm->mmu_lock);
	while (offset < length) {
		absolute = starting_byte + offset;
		vpn = absolute / VSLM_PAGE_SIZE;
		page_offset = absolute % VSLM_PAGE_SIZE;
		chunk = spdk_min((uint64_t)(VSLM_PAGE_SIZE - page_offset), (uint64_t)length - offset);

		rc = vslm_resolve_page(vslm, base_ch, vpn, &page);
		if (rc != 0) {
			pthread_mutex_unlock(&vslm->mmu_lock);
			return rc;
		}

		ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset;
		if (is_write) {
			memcpy(ptr, buf + offset, chunk);
			vslm_page_mark_dirty(vslm, page);
		} else {
			memcpy(buf + offset, ptr, chunk);
		}

		vslm_lru_touch(vslm, page);
		offset += chunk;
	}

	pthread_mutex_unlock(&vslm->mmu_lock);
	return 0;
}

static int
vslm_fill_range(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		uint64_t starting_byte, uint32_t length)
{
	struct vslm_page *page;
	uint64_t offset = 0;
	uint64_t absolute;
	uint64_t vpn;
	uint64_t page_offset;
	uint64_t chunk;
	uint8_t *ptr;
	int rc;

	if (length == 0) {
		return 0;
	}

	if (starting_byte >= vslm->virtual_size_bytes ||
	    length > vslm->virtual_size_bytes - starting_byte) {
		return -EINVAL;
	}

	pthread_mutex_lock(&vslm->mmu_lock);
	while (offset < length) {
		absolute = starting_byte + offset;
		vpn = absolute / VSLM_PAGE_SIZE;
		page_offset = absolute % VSLM_PAGE_SIZE;
		chunk = spdk_min((uint64_t)(VSLM_PAGE_SIZE - page_offset), (uint64_t)length - offset);

		rc = vslm_resolve_page(vslm, base_ch, vpn, &page);
		if (rc != 0) {
			pthread_mutex_unlock(&vslm->mmu_lock);
			return rc;
		}

		ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset;
		memset(ptr, 0, chunk);
		vslm_page_mark_dirty(vslm, page);
		vslm_lru_touch(vslm, page);
		offset += chunk;
	}

	pthread_mutex_unlock(&vslm->mmu_lock);
	return 0;
}

static int
vslm_flush_dirty_pages(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch)
{
	struct vslm_page *page;
	uint64_t i;
	int rc = 0;

	pthread_mutex_lock(&vslm->mmu_lock);
	for (i = 0; i < vslm->num_sram_pages; i++) {
		page = &vslm->page_array[i];
		if (page->vpn == UINT64_MAX || !page->dirty) {
			continue;
		}

		rc = vslm_writeback_page(vslm, base_ch, page);
		if (rc != 0) {
			break;
		}
	}
	pthread_mutex_unlock(&vslm->mmu_lock);
	return rc;
}

static int
vbdev_vslm_destruct(void *ctx)
{
	struct vbdev_vslm *vslm = ctx;
	struct vslm_lease *lease, *lease_tmp;
	struct vslm_blocked_cmd *blocked, *blocked_tmp;

	TAILQ_REMOVE(&g_vslm_bdevs, vslm, link);
	spdk_io_device_unregister(vslm, NULL);
	if (vslm->base_desc) {
		spdk_bdev_close(vslm->base_desc);
	}

	pthread_mutex_lock(&vslm->lease_lock);
	TAILQ_FOREACH_SAFE(lease, &vslm->leases, link, lease_tmp) {
		TAILQ_REMOVE(&vslm->leases, lease, link);
		free(lease);
	}
	TAILQ_FOREACH_SAFE(blocked, &vslm->blocked_cmds, link, blocked_tmp) {
		TAILQ_REMOVE(&vslm->blocked_cmds, blocked, link);
		spdk_bdev_io_complete(blocked->bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		free(blocked);
	}
	pthread_mutex_unlock(&vslm->lease_lock);
	pthread_mutex_destroy(&vslm->lease_lock);
	pthread_mutex_destroy(&vslm->mmu_lock);
	pthread_mutex_destroy(&vslm->policy_lock);

	spdk_dma_free(vslm->sram_buffer);
	free(vslm->page_array);
	free(vslm->hash_table);
	free(vslm->base_bdev_name);
	free(vslm->vbdev.name);
	free(vslm);
	return 0;
}

void
bdev_vslm_opts_init(struct spdk_bdev_vslm_opts *opts)
{
	if (opts == NULL) {
		return;
	}

	memset(opts, 0, sizeof(*opts));
	opts->policy.semantics_mode = SPDK_BDEV_VSLM_SEMANTICS_LEASE;
	opts->policy.writeback_policy = SPDK_BDEV_VSLM_WRITEBACK_ON_EVICT;
	opts->policy.admission_enabled = false;
	opts->policy.admission_faults_per_sec_threshold = 0;
	opts->fdp_mode_enabled = false;
	opts->fdp_dspec = 0;
}

static int
vslm_validate_policy(const struct spdk_bdev_vslm_policy *policy)
{
	if (policy == NULL) {
		return -EINVAL;
	}

	if (policy->semantics_mode != SPDK_BDEV_VSLM_SEMANTICS_LEASE) {
		return -ENOTSUP;
	}

	switch (policy->writeback_policy) {
	case SPDK_BDEV_VSLM_WRITEBACK_ON_EVICT:
	case SPDK_BDEV_VSLM_WRITEBACK_AT_BOUNDARY:
	case SPDK_BDEV_VSLM_WRITEBACK_HYBRID:
		break;
	default:
		return -EINVAL;
	}

	if (policy->admission_enabled &&
	    policy->admission_faults_per_sec_threshold == 0) {
		return -EINVAL;
	}

	return 0;
}

static int
vslm_apply_policy(struct vbdev_vslm *vslm, const struct spdk_bdev_vslm_policy *policy)
{
	int rc;

	rc = vslm_validate_policy(policy);
	if (rc != 0) {
		return rc;
	}

	pthread_mutex_lock(&vslm->policy_lock);
	vslm->policy = *policy;
	if (!vslm->policy.admission_enabled) {
		vslm->admission_window_start_ticks = 0;
		vslm->admission_faults_in_window = 0;
	}
	pthread_mutex_unlock(&vslm->policy_lock);
	return 0;
}

static int
vslm_create_common(const char *bdev_name, const char *base_bdev_name,
		   uint64_t sram_size_bytes, const struct spdk_bdev_vslm_opts *opts)
{
	struct spdk_bdev_vslm_opts default_opts;
	const struct spdk_bdev_vslm_opts *create_opts;
	struct vbdev_vslm *vslm;
	struct vbdev_vslm *iter;
	bool io_device_registered = false;
	bool lease_lock_initialized = false;
	bool mmu_lock_initialized = false;
	bool policy_lock_initialized = false;
	uint32_t base_block_size;
	int rc;

	if (opts == NULL) {
		bdev_vslm_opts_init(&default_opts);
		create_opts = &default_opts;
	} else {
		create_opts = opts;
	}

	rc = vslm_validate_policy(&create_opts->policy);
	if (rc != 0) {
		return rc;
	}

	if (!bdev_name || !base_bdev_name || sram_size_bytes == 0) {
		SPDK_ERRLOG("Invalid vSLM parameters\n");
		return -EINVAL;
	}

	if (sram_size_bytes < VSLM_PAGE_SIZE || (sram_size_bytes % VSLM_PAGE_SIZE) != 0) {
		SPDK_ERRLOG("sram_size_bytes must be page-aligned and >= %u bytes\n",
			    VSLM_PAGE_SIZE);
		return -EINVAL;
	}

	TAILQ_FOREACH(iter, &g_vslm_bdevs, link) {
		if (strcmp(iter->vbdev.name, bdev_name) == 0) {
			SPDK_ERRLOG("vSLM bdev '%s' already exists\n", bdev_name);
			return -EEXIST;
		}
	}

	vslm = calloc(1, sizeof(*vslm));
	if (!vslm) {
		return -ENOMEM;
	}

	rc = pthread_mutex_init(&vslm->lease_lock, NULL);
	if (rc != 0) {
		free(vslm);
		return -rc;
	}
	lease_lock_initialized = true;
	TAILQ_INIT(&vslm->leases);
	TAILQ_INIT(&vslm->blocked_cmds);

	rc = pthread_mutex_init(&vslm->mmu_lock, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&vslm->lease_lock);
		free(vslm);
		return -rc;
	}
	mmu_lock_initialized = true;

	rc = pthread_mutex_init(&vslm->policy_lock, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&vslm->mmu_lock);
		pthread_mutex_destroy(&vslm->lease_lock);
		free(vslm);
		return -rc;
	}
	policy_lock_initialized = true;
	vslm->admission_window_start_ticks = 0;
	vslm->admission_faults_in_window = 0;

	vslm->sram_size_bytes = sram_size_bytes;
	vslm->base_bdev_name = strdup(base_bdev_name);
	if (!vslm->base_bdev_name) {
		pthread_mutex_destroy(&vslm->policy_lock);
		pthread_mutex_destroy(&vslm->mmu_lock);
		pthread_mutex_destroy(&vslm->lease_lock);
		free(vslm);
		return -ENOMEM;
	}

	rc = spdk_bdev_open_ext(base_bdev_name, true, vbdev_vslm_bdev_event_cb, vslm, &vslm->base_desc);
	if (rc != 0) {
		goto err;
	}

	vslm->base_bdev = spdk_bdev_desc_get_bdev(vslm->base_desc);
	base_block_size = spdk_bdev_get_block_size(vslm->base_bdev);
	vslm->virtual_size_bytes = spdk_bdev_get_num_blocks(vslm->base_bdev) * (uint64_t)base_block_size;
	vslm->base_fdp_supported = spdk_bdev_get_nvme_ctratt(vslm->base_bdev).bits.fdps;
	if ((VSLM_PAGE_SIZE % base_block_size) != 0 ||
	    (sram_size_bytes % base_block_size) != 0) {
		SPDK_ERRLOG("vSLM requires block/page alignment: block_size=%u page_size=%u sram=%" PRIu64 "\n",
			    base_block_size, VSLM_PAGE_SIZE, sram_size_bytes);
		rc = -EINVAL;
		goto err;
	}
	if (vslm->virtual_size_bytes < sram_size_bytes) {
		SPDK_ERRLOG("vSLM backing size must be >= SRAM size: backing=%" PRIu64 " sram=%" PRIu64 "\n",
			    vslm->virtual_size_bytes, sram_size_bytes);
		rc = -EINVAL;
		goto err;
	}
	if ((vslm->virtual_size_bytes % VSLM_PAGE_SIZE) != 0) {
		SPDK_ERRLOG("vSLM backing size must be page-aligned: backing=%" PRIu64 " page=%u\n",
			    vslm->virtual_size_bytes, VSLM_PAGE_SIZE);
		rc = -EINVAL;
		goto err;
	}

	rc = vslm_init_mmu(vslm);
	if (rc != 0) {
		goto err;
	}

	vslm->vbdev.name = strdup(bdev_name);
	if (!vslm->vbdev.name) {
		rc = -ENOMEM;
		goto err;
	}

	rc = vslm_apply_policy(vslm, &create_opts->policy);
	if (rc != 0) {
		goto err;
	}

	if (create_opts->fdp_mode_enabled && !vslm->base_fdp_supported) {
		rc = -ENOTSUP;
		goto err;
	}
	vslm->fdp_mode_enabled = create_opts->fdp_mode_enabled;
	vslm->fdp_dspec = create_opts->fdp_dspec;

	vslm->vbdev.product_name = "vSLM Device";
	vslm->vbdev.write_cache = 0;
	vslm->vbdev.blocklen = base_block_size;
	vslm->vbdev.blockcnt = vslm->virtual_size_bytes / base_block_size;
	vslm->vbdev.ctxt = vslm;
	vslm->vbdev.fn_table = &vbdev_vslm_fn_table;
	vslm->vbdev.module = &vslm_if;
	vslm->vbdev.slm = true;
	vslm->vbdev.nsid = create_opts->nsid;

	spdk_io_device_register(vslm, vbdev_vslm_ch_create_cb, vbdev_vslm_ch_destroy_cb,
				sizeof(struct vslm_channel), vslm->vbdev.name);
	io_device_registered = true;

	rc = spdk_bdev_register(&vslm->vbdev);
	if (rc != 0) {
		goto err;
	}

	TAILQ_INSERT_TAIL(&g_vslm_bdevs, vslm, link);
	return 0;

err:
	if (vslm->base_desc) {
		spdk_bdev_close(vslm->base_desc);
	}
	if (io_device_registered) {
		spdk_io_device_unregister(vslm, NULL);
	}
	spdk_dma_free(vslm->sram_buffer);
	free(vslm->page_array);
	free(vslm->hash_table);
	free(vslm->base_bdev_name);
	free(vslm->vbdev.name);
	if (mmu_lock_initialized) {
		pthread_mutex_destroy(&vslm->mmu_lock);
	}
	if (policy_lock_initialized) {
		pthread_mutex_destroy(&vslm->policy_lock);
	}
	if (lease_lock_initialized) {
		pthread_mutex_destroy(&vslm->lease_lock);
	}
	free(vslm);
	return rc;
}

int
bdev_vslm_create(const char *bdev_name, const char *base_bdev_name,
		 uint64_t sram_size_bytes)
{
	return vslm_create_common(bdev_name, base_bdev_name, sram_size_bytes, NULL);
}

int
bdev_vslm_create_with_opts(const char *bdev_name, const char *base_bdev_name,
			   uint64_t sram_size_bytes,
			   const struct spdk_bdev_vslm_opts *opts)
{
	return vslm_create_common(bdev_name, base_bdev_name, sram_size_bytes, opts);
}

void
bdev_vslm_delete(const char *name,
		 spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct vbdev_vslm *vslm;

	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (strcmp(vslm->vbdev.name, name) == 0) {
			spdk_bdev_unregister(&vslm->vbdev, cb_fn, cb_arg);
			return;
		}
	}

	if (cb_fn) {
		cb_fn(cb_arg, -ENOENT);
	}
}

int
bdev_vslm_set_nsid(const char *name, uint32_t nsid)
{
	struct vbdev_vslm *vslm;

	if (!name || nsid == 0) {
		return -EINVAL;
	}

	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (strcmp(vslm->vbdev.name, name) == 0) {
			if (vslm->vbdev.nsid == nsid) {
				return 0;
			}

			vslm->vbdev.nsid = nsid;
			return 0;
		}
	}

	return -ENOENT;
}

int
bdev_vslm_get_buffer_ptr_by_nsid(uint32_t nsid, uint64_t offset,
				 uint64_t length, void **ptr)
{
	struct vbdev_vslm *vslm;
	int rc;

	if (!ptr || nsid == 0) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_nsid(nsid);
	if (vslm == NULL) {
		SPDK_ERRLOG("vSLM bdev not found for NSID %u\n", nsid);
		return -ENOENT;
	}

	rc = vbdev_vslm_mem_get_buffer_ptr_by_bdev(&vslm->vbdev, offset, length, ptr);
	if (rc == -ENOTSUP) {
		return -ENOENT;
	}

	return rc;
}

static int
vbdev_vslm_mem_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				      uint64_t length, void **ptr)
{
	struct vbdev_vslm *vslm;
	int rc;

	if (!ptr || bdev == NULL) {
		return -EINVAL;
	}

	if (length == 0) {
		*ptr = NULL;
		return 0;
	}

	vslm = vbdev_vslm_get_by_bdev(bdev);
	if (vslm == NULL) {
		return -ENOTSUP;
	}

	rc = vslm_get_direct_ptr(vslm, offset, length, ptr, false);
	if (rc != 0) {
		SPDK_ERRLOG("vSLM buffer access failed: NSID=%u offset=%lu len=%lu rc=%d\n",
			    vslm->vbdev.nsid, offset, length, rc);
	}

	return rc;
}

static int
vslm_rw_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf, bool is_write)
{
	struct vbdev_vslm *vslm;
	struct spdk_io_channel *base_ch;
	uint64_t processed = 0;
	uint32_t chunk;
	uint8_t *buf_u8 = buf;
	int rc;

	if (bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	if (length == 0) {
		return 0;
	}

	vslm = vbdev_vslm_get_by_bdev(bdev);
	if (vslm == NULL) {
		return -ENOTSUP;
	}

	base_ch = spdk_bdev_get_io_channel(vslm->base_desc);
	if (base_ch == NULL) {
		return -ENOMEM;
	}

	while (processed < length) {
		chunk = (uint32_t)spdk_min(length - processed, (uint64_t)UINT32_MAX);
		rc = vslm_copy_range(vslm, base_ch, offset + processed, chunk, buf_u8 + processed, is_write);
		if (rc != 0) {
			break;
		}
		processed += chunk;
	}
	spdk_put_io_channel(base_ch);
	return rc;
}

static int
vslm_rw_by_nsid(uint32_t nsid, uint64_t offset, uint64_t length, void *buf, bool is_write)
{
	struct vbdev_vslm *vslm;

	if (nsid == 0 || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_nsid(nsid);
	if (vslm == NULL) {
		return -ENOENT;
	}

	return vslm_rw_by_bdev(&vslm->vbdev, offset, length, buf, is_write);
}

static int
vbdev_vslm_mem_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	return vslm_rw_by_bdev(bdev, offset, length, buf, false);
}

static int
vbdev_vslm_mem_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			     uint64_t length, const void *buf)
{
	return vslm_rw_by_bdev(bdev, offset, length, (void *)(uintptr_t)buf, true);
}

int
bdev_vslm_read_by_nsid(uint32_t nsid, uint64_t offset, uint64_t length, void *buf)
{
	return vslm_rw_by_nsid(nsid, offset, length, buf, false);
}

int
bdev_vslm_write_by_nsid(uint32_t nsid, uint64_t offset, uint64_t length, const void *buf)
{
	return vslm_rw_by_nsid(nsid, offset, length, (void *)(uintptr_t)buf, true);
}

int
bdev_vslm_lease_acquire(uint64_t lease_id, uint32_t nsid,
			uint64_t offset, uint64_t length)
{
	struct vbdev_vslm *vslm;
	struct vslm_lease *lease, *new_lease;

	if (lease_id == 0 || nsid == 0 || length == 0) {
		return -EINVAL;
	}

	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (vslm->vbdev.nsid != nsid) {
			continue;
		}

		if (offset >= vslm->virtual_size_bytes ||
		    length > vslm->virtual_size_bytes - offset) {
			return -EINVAL;
		}

		pthread_mutex_lock(&vslm->lease_lock);
		TAILQ_FOREACH(lease, &vslm->leases, link) {
			if (lease->lease_id == lease_id &&
			    lease->offset == offset &&
			    lease->length == length) {
				pthread_mutex_unlock(&vslm->lease_lock);
				return 0;
			}

			if (lease->lease_id != lease_id &&
			    vslm_range_overlap(offset, length, lease->offset, lease->length)) {
				pthread_mutex_unlock(&vslm->lease_lock);
				return -EAGAIN;
			}
		}

		if (!vslm_admission_allow_execute(vslm)) {
			pthread_mutex_unlock(&vslm->lease_lock);
			return -EBUSY;
		}

		new_lease = calloc(1, sizeof(*new_lease));
		if (new_lease == NULL) {
			pthread_mutex_unlock(&vslm->lease_lock);
			return -ENOMEM;
		}
		new_lease->lease_id = lease_id;
		new_lease->offset = offset;
		new_lease->length = length;
		TAILQ_INSERT_TAIL(&vslm->leases, new_lease, link);
		pthread_mutex_unlock(&vslm->lease_lock);
		return 0;
	}

	return -ENOENT;
}

int
bdev_vslm_lease_release(uint64_t lease_id)
{
	struct vbdev_vslm *vslm;
	struct vslm_lease *lease, *lease_tmp;
	struct spdk_io_channel *base_ch;
	enum spdk_bdev_vslm_writeback_policy writeback_policy;
	bool found = false;
	bool released;
	int rc = 0;
	int flush_rc;

	if (lease_id == 0) {
		return -EINVAL;
	}

	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		released = false;

		pthread_mutex_lock(&vslm->lease_lock);
		TAILQ_FOREACH_SAFE(lease, &vslm->leases, link, lease_tmp) {
			if (lease->lease_id == lease_id) {
				TAILQ_REMOVE(&vslm->leases, lease, link);
				free(lease);
				released = true;
			}
		}
		pthread_mutex_unlock(&vslm->lease_lock);

		if (released) {
			found = true;
			pthread_mutex_lock(&vslm->policy_lock);
			writeback_policy = vslm->policy.writeback_policy;
			pthread_mutex_unlock(&vslm->policy_lock);

			if (vslm_policy_is_boundary_flush(writeback_policy)) {
				base_ch = spdk_bdev_get_io_channel(vslm->base_desc);
				if (base_ch == NULL) {
					if (rc == 0) {
						rc = -ENOMEM;
					}
				} else {
					flush_rc = vslm_flush_dirty_pages(vslm, base_ch);
					spdk_put_io_channel(base_ch);
					if (flush_rc != 0 && rc == 0) {
						rc = flush_rc;
					}
				}
			}

			vslm_process_blocked_cmds(vslm);
		}
	}

	if (!found) {
		return -ENOENT;
	}

	return rc;
}

int
bdev_vslm_set_policy(const char *name, const struct spdk_bdev_vslm_policy *policy)
{
	struct vbdev_vslm *vslm;
	int rc;

	if (name == NULL || policy == NULL) {
		return -EINVAL;
	}

	rc = vslm_validate_policy(policy);
	if (rc != 0) {
		return rc;
	}

	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (strcmp(vslm->vbdev.name, name) != 0) {
			continue;
		}

		return vslm_apply_policy(vslm, policy);
	}

	return -ENOENT;
}

int
bdev_vslm_get_policy(const char *name, struct spdk_bdev_vslm_policy *policy)
{
	struct vbdev_vslm *vslm;

	if (name == NULL || policy == NULL) {
		return -EINVAL;
	}

	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (strcmp(vslm->vbdev.name, name) != 0) {
			continue;
		}

		pthread_mutex_lock(&vslm->policy_lock);
		*policy = vslm->policy;
		pthread_mutex_unlock(&vslm->policy_lock);
		return 0;
	}

	return -ENOENT;
}

int
bdev_vslm_set_fdp_mode(const char *name, bool enabled, uint16_t dspec)
{
	struct vbdev_vslm *vslm;

	if (name == NULL) {
		return -EINVAL;
	}

	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (strcmp(vslm->vbdev.name, name) != 0) {
			continue;
		}

		if (enabled && !vslm->base_fdp_supported) {
			return -ENOTSUP;
		}

		vslm->fdp_mode_enabled = enabled;
		vslm->fdp_dspec = dspec;
		return 0;
	}

	return -ENOENT;
}

int
bdev_vslm_reset_stats(const char *name)
{
	struct vbdev_vslm *vslm;
	uint64_t dirty_pages = 0;
	uint64_t i;

	if (name == NULL) {
		return -EINVAL;
	}

	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (strcmp(vslm->vbdev.name, name) != 0) {
			continue;
		}

		memset(&vslm->stats, 0, sizeof(vslm->stats));
		pthread_mutex_lock(&vslm->policy_lock);
		vslm->admission_window_start_ticks = 0;
		vslm->admission_faults_in_window = 0;
		pthread_mutex_unlock(&vslm->policy_lock);

		pthread_mutex_lock(&vslm->mmu_lock);
		for (i = 0; i < vslm->num_sram_pages; i++) {
			if (vslm->page_array[i].vpn != UINT64_MAX &&
			    vslm->page_array[i].dirty) {
				dirty_pages++;
			}
		}
		pthread_mutex_unlock(&vslm->mmu_lock);
		__atomic_store_n(&vslm->stats.dirty_resident_pages, dirty_pages, __ATOMIC_RELAXED);
		return 0;
	}

	return -ENOENT;
}

int
bdev_vslm_get_stats(const char *name, struct spdk_bdev_vslm_stats *stats)
{
	struct vbdev_vslm *vslm;

	if (name == NULL || stats == NULL) {
		return -EINVAL;
	}

	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (strcmp(vslm->vbdev.name, name) != 0) {
			continue;
		}

		stats->backing_write_ops = __atomic_load_n(&vslm->stats.backing_write_ops, __ATOMIC_RELAXED);
		stats->backing_write_bytes = __atomic_load_n(&vslm->stats.backing_write_bytes, __ATOMIC_RELAXED);
		stats->backing_write_tagged_bytes = __atomic_load_n(&vslm->stats.backing_write_tagged_bytes,
						 __ATOMIC_RELAXED);
		stats->backing_write_untagged_bytes = __atomic_load_n(&vslm->stats.backing_write_untagged_bytes,
						   __ATOMIC_RELAXED);
		stats->page_faults = __atomic_load_n(&vslm->stats.page_faults, __ATOMIC_RELAXED);
		stats->page_fault_bytes = __atomic_load_n(&vslm->stats.page_fault_bytes, __ATOMIC_RELAXED);
		stats->page_evictions = __atomic_load_n(&vslm->stats.page_evictions, __ATOMIC_RELAXED);
		stats->page_eviction_bytes = __atomic_load_n(&vslm->stats.page_eviction_bytes, __ATOMIC_RELAXED);
		stats->page_writebacks = __atomic_load_n(&vslm->stats.page_writebacks, __ATOMIC_RELAXED);
		stats->page_writeback_bytes = __atomic_load_n(&vslm->stats.page_writeback_bytes, __ATOMIC_RELAXED);
		stats->dirty_resident_pages = __atomic_load_n(&vslm->stats.dirty_resident_pages, __ATOMIC_RELAXED);
		stats->dirty_writeback_bytes = __atomic_load_n(&vslm->stats.dirty_writeback_bytes,
						 __ATOMIC_RELAXED);
		stats->lease_conflicts = __atomic_load_n(&vslm->stats.lease_conflicts, __ATOMIC_RELAXED);
		stats->lease_blocked_ns = __atomic_load_n(&vslm->stats.lease_blocked_ns, __ATOMIC_RELAXED);
		stats->admission_rejects = __atomic_load_n(&vslm->stats.admission_rejects, __ATOMIC_RELAXED);
		return 0;
	}

	return -ENOENT;
}

static const struct spdk_bdev_fn_table vbdev_vslm_fn_table = {
	.destruct = vbdev_vslm_destruct,
	.submit_request = vbdev_vslm_submit_request,
	.io_type_supported = vbdev_vslm_io_type_supported,
	.get_io_channel = vbdev_vslm_get_io_channel,
};

SPDK_BDEV_MODULE_REGISTER(vslm, &vslm_if)

static bool
vbdev_vslm_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	(void)ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
		return true;
	default:
		return false;
	}
}
static int
vbdev_vslm_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_vslm *vslm = io_device;
	struct vslm_channel *ch = ctx_buf;

	ch->base_ch = spdk_bdev_get_io_channel(vslm->base_desc);
	if (!ch->base_ch) {
		return -ENOMEM;
	}

	return 0;
}

static void
vbdev_vslm_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct vslm_channel *ch = ctx_buf;

	(void)io_device;
	spdk_put_io_channel(ch->base_ch);
}

static struct spdk_io_channel *
vbdev_vslm_get_io_channel(void *ctx)
{
	struct vbdev_vslm *vslm = ctx;

	return spdk_get_io_channel(vslm);
}

static void
vbdev_vslm_complete_nvme_status(struct spdk_bdev_io *bdev_io, int sct, int sc)
{
	spdk_bdev_io_complete_nvme_status(bdev_io, 0, sct, sc);
}

static int
vbdev_vslm_nvme_prepare_iovs(struct spdk_bdev_io *bdev_io, uint32_t length,
			     struct iovec *local_iov, struct iovec **iovs_out,
			     int *iovcnt_out)
{
	struct iovec *iovs;
	int iovcnt;
	size_t nbytes;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_NVME_IOV_MD) {
		iovs = bdev_io->u.nvme_passthru.iovs;
		iovcnt = bdev_io->u.nvme_passthru.iovcnt;
		nbytes = bdev_io->u.nvme_passthru.nbytes;
	} else {
		local_iov->iov_base = bdev_io->u.nvme_passthru.buf;
		local_iov->iov_len = bdev_io->u.nvme_passthru.nbytes;
		iovs = local_iov;
		iovcnt = 1;
		nbytes = bdev_io->u.nvme_passthru.nbytes;
	}

	if (iovs == NULL || iovcnt <= 0 || nbytes == 0 || length > nbytes) {
		return -EINVAL;
	}

	*iovs_out = iovs;
	*iovcnt_out = iovcnt;
	return 0;
}

static int
vbdev_vslm_nvme_memory_read(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
			    uint64_t starting_byte, uint32_t length,
			    struct iovec *iovs, int iovcnt)
{
	uint8_t *tmp;
	int rc;

	tmp = malloc(length);
	if (tmp == NULL) {
		return -ENOMEM;
	}

	rc = vslm_copy_range(vslm, base_ch, starting_byte, length, tmp, false);
	if (rc != 0) {
		free(tmp);
		return rc;
	}

	spdk_copy_buf_to_iovs(iovs, iovcnt, tmp, length);
	free(tmp);
	return 0;
}

static int
vbdev_vslm_nvme_memory_write(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
			     uint64_t starting_byte, uint32_t length,
			     struct iovec *iovs, int iovcnt)
{
	uint8_t *tmp;
	int rc;

	tmp = malloc(length);
	if (tmp == NULL) {
		return -ENOMEM;
	}

	spdk_copy_iovs_to_buf(tmp, length, iovs, iovcnt);
	rc = vslm_copy_range(vslm, base_ch, starting_byte, length, tmp, true);
	if (rc != 0) {
		free(tmp);
		return rc;
	}

	free(tmp);
	return 0;
}

static int
vbdev_vslm_nvme_memory_copy(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
			    struct spdk_bdev_io *bdev_io, int *sct)
{
	const struct spdk_nvme_cmd *cmd = &bdev_io->u.nvme_passthru.cmd;
	struct spdk_nvme_slm_copy_desc_format_4 *descs;
	uint64_t sdaddr;
	uint64_t len_field;
	enum spdk_nvme_slm_copy_desc_fmt desc_fmt;
	uint32_t nr;
	uint32_t desc_bytes;
	uint64_t copied = 0;
	uint64_t total_len = 0;
	uint64_t dest_end;
	uint64_t buffer_size = vslm->virtual_size_bytes;
	uint8_t *tmp = NULL;
	struct iovec *iovs = NULL;
	struct iovec local_iov;
	int iovcnt = 0;
	int rc = 0;
	uint32_t i;

	if (sct != NULL) {
		*sct = SPDK_NVME_SCT_GENERIC;
	}

	sdaddr = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
	if ((sdaddr & 0x3) != 0) {
		SPDK_ERRLOG("vSLM copy invalid: sdaddr not dword aligned (0x%jx)\n", sdaddr);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc_fmt = (enum spdk_nvme_slm_copy_desc_fmt)SPDK_NVME_SLM_COPY_DESC_FMT_FROM_CDW12(cmd->cdw12);
	if (desc_fmt != SPDK_NVME_SLM_COPY_DESC_FMT_4H) {
		SPDK_ERRLOG("vSLM copy invalid: desc_fmt=0x%x (expected 0x4), cdw12=0x%x\n",
			    (unsigned)desc_fmt, cmd->cdw12);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	nr = cmd->cdw12 & 0xFFu;
	desc_bytes = (nr + 1) * sizeof(*descs);

	len_field = ((uint64_t)cmd->rsvd3 << 32) | cmd->rsvd2;
	if ((len_field & 0x3) != 0) {
		SPDK_ERRLOG("vSLM copy invalid: len_field not dword aligned (0x%jx)\n", len_field);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	rc = vbdev_vslm_nvme_prepare_iovs(bdev_io, desc_bytes, &local_iov, &iovs, &iovcnt);
	if (rc != 0) {
		SPDK_ERRLOG("vSLM copy invalid: desc_bytes=%u nbytes=%zu iovcnt=%d rc=%d\n",
			    desc_bytes, bdev_io->u.nvme_passthru.nbytes, iovcnt, rc);
		return -SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
	}

	descs = malloc(desc_bytes);
	if (!descs) {
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	spdk_copy_iovs_to_buf(descs, desc_bytes, iovs, iovcnt);

	for (i = 0; i < nr + 1; i++) {
		uint32_t nbyte = descs[i].nbyte;
		uint64_t saddr = descs[i].saddr;

		if (descs[i].snsid == 0 || descs[i].snsid == 0xFFFFFFFFu) {
			SPDK_ERRLOG("vSLM copy invalid: snsid=0x%x\n", descs[i].snsid);
			rc = -SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		if (descs[i].sopt & SPDK_NVME_SLM_COPY_DESC_SOPT_FCO) {
			SPDK_ERRLOG("vSLM copy invalid: fast copy only not supported\n");
			if (sct != NULL) {
				*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			}
			rc = -SPDK_NVME_SLM_SC_FAST_COPY_NOT_POSSIBLE;
			goto out;
		}

		if ((saddr & 0x3) != 0 || (nbyte & 0x3) != 0) {
			SPDK_ERRLOG("vSLM copy invalid: saddr=0x%jx nbyte=%u\n", saddr, nbyte);
			rc = -SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		if (nbyte == 0) {
			continue;
		}
		if (total_len + nbyte < total_len) {
			rc = -SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}
		total_len += nbyte;
	}

	if (total_len != len_field) {
		SPDK_ERRLOG("vSLM copy invalid: total_len=%ju len_field=%ju\n",
			    total_len, len_field);
		rc = -SPDK_NVME_SC_INVALID_FIELD;
		goto out;
	}

	if (total_len > buffer_size) {
		SPDK_ERRLOG("vSLM copy invalid: total_len=%ju buffer_size=%ju\n",
			    total_len, buffer_size);
		rc = -SPDK_NVME_SC_INVALID_FIELD;
		goto out;
	}

	dest_end = sdaddr + total_len;
	if (dest_end < sdaddr || dest_end > buffer_size) {
		SPDK_ERRLOG("vSLM copy invalid: dest_end=0x%jx sdaddr=0x%jx buffer_size=%ju\n",
			    dest_end, sdaddr, buffer_size);
		rc = -SPDK_NVME_SC_INVALID_FIELD;
		goto out;
	}

	if (total_len > 0 && vslm_has_lease_conflict(vslm, sdaddr, total_len)) {
		rc = vslm_queue_blocked_cmd(vslm, bdev_io, sdaddr, total_len);
		if (rc == 0) {
			rc = -EAGAIN;
			goto out;
		}

		rc = -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto out;
	}

	for (i = 0; i < nr + 1; i++) {
		uint32_t nbyte = descs[i].nbyte;
		uint64_t saddr = descs[i].saddr;

		if (nbyte == 0) {
			continue;
		}

		if (descs[i].snsid == cmd->nsid) {
			uint64_t src_end = saddr + nbyte;

			if (src_end < saddr) {
				rc = -SPDK_NVME_SC_INVALID_FIELD;
				goto out;
			}

			if (src_end > sdaddr && saddr < dest_end) {
				SPDK_ERRLOG("vSLM copy invalid: overlapping I/O range\n");
				if (sct != NULL) {
					*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
				}
				rc = -SPDK_NVME_SLM_SC_OVERLAPPING_IO_RANGE;
				goto out;
			}

			tmp = malloc(nbyte);
			if (tmp == NULL) {
				rc = -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				goto out;
			}

			rc = vslm_copy_range(vslm, base_ch, saddr, nbyte, tmp, false);
		} else {
			SPDK_ERRLOG("vSLM copy invalid: snsid=%u (only same-NSID supported)\n",
				    descs[i].snsid);
			rc = -SPDK_NVME_SC_INVALID_FIELD;
		}

		if (rc != 0) {
			SPDK_ERRLOG("vSLM copy invalid: snsid=%u saddr=0x%jx nbyte=%u rc=%d\n",
				    descs[i].snsid, saddr, nbyte, rc);
			rc = (rc == -EINVAL) ? -SPDK_NVME_SC_INVALID_FIELD :
			     -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}

		rc = vslm_copy_range(vslm, base_ch, sdaddr + copied, nbyte, tmp, true);
		free(tmp);
		tmp = NULL;
		if (rc != 0) {
			rc = (rc == -EINVAL) ? -SPDK_NVME_SC_INVALID_FIELD :
			     -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}
		copied += nbyte;
	}

out:
	free(tmp);
	free(descs);
	return rc;
}

static void
vbdev_vslm_submit_nvme_passthru(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
				struct spdk_bdev_io *bdev_io)
{
	const struct spdk_nvme_cmd *cmd = &bdev_io->u.nvme_passthru.cmd;
	uint64_t starting_byte;
	uint32_t length;
	uint64_t end;
	uint64_t buffer_size = vslm->virtual_size_bytes;
	struct iovec *iovs = NULL;
	struct iovec local_iov;
	int iovcnt = 0;
	int sct = SPDK_NVME_SCT_GENERIC;
	int sc = SPDK_NVME_SC_SUCCESS;
	int rc;

	switch (cmd->opc) {
	case SPDK_NVME_SLM_OPC_MEMORY_COPY:
		rc = vbdev_vslm_nvme_memory_copy(vslm, base_ch, bdev_io, &sct);
		if (rc == -EAGAIN) {
			return;
		}
		if (rc != 0) {
			sc = -rc;
		}
		break;
	case SPDK_NVME_SLM_OPC_MEMORY_FILL:
		starting_byte = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
		length = cmd->cdw12;

		if ((starting_byte & 0x3) != 0 || (length & 0x3) != 0) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		end = starting_byte + length;
		if (end < starting_byte || end > buffer_size) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		if (length == 0) {
			goto out;
		}

		if (vslm_has_lease_conflict(vslm, starting_byte, length)) {
			rc = vslm_queue_blocked_cmd(vslm, bdev_io, starting_byte, length);
			if (rc == 0) {
				return;
			}

			sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}

		rc = vslm_fill_range(vslm, base_ch, starting_byte, length);
		if (rc != 0) {
			sc = (rc == -EINVAL) ? SPDK_NVME_SC_INVALID_FIELD :
			     SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}
		goto out;
	case SPDK_NVME_SLM_OPC_MEMORY_READ:
		starting_byte = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
		length = cmd->cdw12;

		if ((starting_byte & 0x3) != 0 || (length & 0x3) != 0) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		end = starting_byte + length;
		if (end < starting_byte || end > buffer_size) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		if (length == 0) {
			goto out;
		}

		rc = vbdev_vslm_nvme_prepare_iovs(bdev_io, length, &local_iov, &iovs, &iovcnt);
		if (rc != 0) {
			sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			goto out;
		}

		rc = vbdev_vslm_nvme_memory_read(vslm, base_ch, starting_byte, length, iovs, iovcnt);
		if (rc != 0) {
			sc = (rc == -EINVAL) ? SPDK_NVME_SC_INVALID_FIELD :
			     SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
		break;
	case SPDK_NVME_SLM_OPC_MEMORY_WRITE:
		starting_byte = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
		length = cmd->cdw12;

		if ((starting_byte & 0x3) != 0 || (length & 0x3) != 0) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		end = starting_byte + length;
		if (end < starting_byte || end > buffer_size) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		if (length == 0) {
			goto out;
		}

		if (vslm_has_lease_conflict(vslm, starting_byte, length)) {
			rc = vslm_queue_blocked_cmd(vslm, bdev_io, starting_byte, length);
			if (rc == 0) {
				return;
			}

			sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}

		rc = vbdev_vslm_nvme_prepare_iovs(bdev_io, length, &local_iov, &iovs, &iovcnt);
		if (rc != 0) {
			sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			goto out;
		}

		rc = vbdev_vslm_nvme_memory_write(vslm, base_ch, starting_byte, length, iovs, iovcnt);
		if (rc != 0) {
			sc = (rc == -EINVAL) ? SPDK_NVME_SC_INVALID_FIELD :
			     SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
		break;
	default:
		sc = SPDK_NVME_SC_INVALID_OPCODE;
		goto out;
	}

out:
	vbdev_vslm_complete_nvme_status(bdev_io, sct, sc);
}

static int
vbdev_vslm_rw_request(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		      struct spdk_bdev_io *bdev_io)
{
	uint64_t block_size;
	uint64_t offset;
	uint64_t length;
	uint64_t remaining;
	uint32_t chunk;
	uint8_t *tmp = NULL;
	uint8_t *cursor;
	struct iovec *iovs;
	int iovcnt;
	bool is_write;
	int rc = 0;

	block_size = bdev_io->bdev->blocklen;
	offset = bdev_io->u.bdev.offset_blocks * block_size;
	length = bdev_io->u.bdev.num_blocks * block_size;
	is_write = (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE);
	iovs = bdev_io->u.bdev.iovs;
	iovcnt = bdev_io->u.bdev.iovcnt;

	if (length == 0) {
		return 0;
	}

	if (iovs == NULL || iovcnt <= 0 || length > SIZE_MAX) {
		return -EINVAL;
	}

	tmp = malloc((size_t)length);
	if (tmp == NULL) {
		return -ENOMEM;
	}

	if (is_write) {
		spdk_copy_iovs_to_buf(tmp, length, iovs, iovcnt);
	}

	remaining = length;
	cursor = tmp;
	while (remaining > 0) {
		chunk = (uint32_t)spdk_min(remaining, (uint64_t)UINT32_MAX);
		rc = vslm_copy_range(vslm, base_ch, offset, chunk, cursor, is_write);
		if (rc != 0) {
			break;
		}

		offset += chunk;
		cursor += chunk;
		remaining -= chunk;
	}

	if (!is_write && rc == 0) {
		spdk_copy_buf_to_iovs(iovs, iovcnt, tmp, length);
	}

	free(tmp);
	return rc;
}

static void
vbdev_vslm_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vslm_channel *vch;
	struct vbdev_vslm *vslm;
	int rc;

	vslm = bdev_io->bdev->ctxt;
	vch = spdk_io_channel_get_ctx(ch);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = vbdev_vslm_rw_request(vslm, vch->base_ch, bdev_io);
		if (rc == 0) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
		break;
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
		vbdev_vslm_submit_nvme_passthru(vslm, vch->base_ch, bdev_io);
		break;
	default:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

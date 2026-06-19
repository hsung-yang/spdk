/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 CPCS Implementation Team. All rights reserved.
 */

#include "spdk_internal/cunit.h"
#include "spdk_internal/mock.h"

#include "spdk/bdev.h"
#include "spdk/stdinc.h"

/* Include the actual implementation */
#include "bdev/vslm/vbdev_vslm.c"

static struct spdk_bdev g_base_bdev;
static struct spdk_bdev g_alias_bdev;
static struct spdk_bdev_desc *g_base_desc;
static struct spdk_bdev_desc *g_alias_desc = (struct spdk_bdev_desc *)0x2;
static uint8_t g_read_pattern;
static uint8_t g_alias_read_pattern;
static struct spdk_bdev *g_last_read_bdev;
static uint64_t g_last_read_offset;
static uint64_t g_last_read_blocks;
static bool g_write_called;
static uint64_t g_last_write_offset;
static uint64_t g_last_write_blocks;
static void *g_last_write_buf;
static uint64_t g_ticks;
static struct spdk_thread *g_ut_thread = (struct spdk_thread *)0x1;
static bool g_delay_next_read;
static bool g_pending_read;
static spdk_bdev_io_completion_cb g_pending_read_cb;
static void *g_pending_read_cb_arg;
struct spdk_log_flag SPDK_LOG_bdev = {
	.name = "bdev",
	.enabled = false,
};

static void
reset_io_state(void)
{
	g_write_called = false;
	g_last_write_offset = 0;
	g_last_write_blocks = 0;
	g_last_write_buf = NULL;
	g_last_read_bdev = NULL;
	g_last_read_offset = 0;
	g_last_read_blocks = 0;
	g_delay_next_read = false;
	g_pending_read = false;
	g_pending_read_cb = NULL;
	g_pending_read_cb_arg = NULL;
}

DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *bdev), 0);
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
DEFINE_STUB_V(spdk_bdev_io_get_buf, (struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb,
				     uint64_t len));
DEFINE_STUB_V(spdk_put_io_channel, (struct spdk_io_channel *ch));
DEFINE_STUB(vbdev_slm_register_ops, int, (const struct spdk_vbdev_slm_ops *ops), 0);
DEFINE_STUB_V(vbdev_slm_unregister_ops, (const struct spdk_vbdev_slm_ops *ops));
DEFINE_STUB_V(spdk_bdev_io_complete_nvme_status, (struct spdk_bdev_io *bdev_io,
		uint32_t cdw0, int sct, int sc));
DEFINE_STUB_V(spdk_copy_buf_to_iovs, (struct iovec *iovs, int iovcnt,
				      void *buf, size_t buf_len));
DEFINE_STUB_V(spdk_copy_iovs_to_buf, (void *buf, size_t buf_len,
				      struct iovec *iovs, int iovcnt));
DEFINE_STUB_V(spdk_io_device_register, (void *io_device, spdk_io_channel_create_cb create_cb,
					spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size, const char *name));
DEFINE_STUB_V(spdk_io_device_unregister, (void *io_device,
		spdk_io_device_unregister_cb unregister_cb));
DEFINE_STUB(spdk_get_io_channel, struct spdk_io_channel *, (void *io_device),
	    (struct spdk_io_channel *)0x1);
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc),
	    (struct spdk_io_channel *)0x1);
DEFINE_STUB(spdk_bdev_io_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_io *bdev_io),
	    (struct spdk_io_channel *)0x1);

int
spdk_bdev_open_ext(const char *bdev_name, bool write,
		   spdk_bdev_event_cb_t event_cb, void *event_ctx,
		   struct spdk_bdev_desc **desc)
{
	(void)bdev_name;
	(void)write;
	(void)event_cb;
	(void)event_ctx;

	if (bdev_name != NULL &&
	    g_alias_bdev.name != NULL &&
	    strcmp(bdev_name, g_alias_bdev.name) == 0) {
		*desc = g_alias_desc;
		return 0;
	}

	g_base_desc = (struct spdk_bdev_desc *)0x1;
	*desc = g_base_desc;
	return 0;
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	if (desc == g_alias_desc) {
		return &g_alias_bdev;
	}

	return &g_base_bdev;
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	if (bdev && bdev->fn_table && bdev->fn_table->destruct) {
		bdev->fn_table->destruct(bdev->ctxt);
	}
	if (cb_fn) {
		cb_fn(cb_arg, 0);
	}
}

union spdk_bdev_nvme_ctratt
	spdk_bdev_get_nvme_ctratt(struct spdk_bdev *bdev)
{
	union spdk_bdev_nvme_ctratt ctratt = {};

	(void)bdev;
	return ctratt;
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return bdev->blocklen;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->blockcnt;
}

void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = NULL;

	if (phys_addr) {
		*phys_addr = 0;
	}

	if (posix_memalign(&buf, align, size) != 0) {
		return NULL;
	}
	memset(buf, 0, size);
	return buf;
}

void
spdk_dma_free(void *buf)
{
	free(buf);
}

uint64_t
spdk_get_ticks(void)
{
	return ++g_ticks;
}

uint64_t
spdk_get_ticks_hz(void)
{
	return SPDK_SEC_TO_NSEC;
}

int
spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *src_bdev;

	(void)ch;

	src_bdev = spdk_bdev_desc_get_bdev(desc);
	g_last_read_bdev = src_bdev;
	g_last_read_offset = offset_blocks;
	g_last_read_blocks = num_blocks;

	if (src_bdev == &g_alias_bdev) {
		memset(buf, g_alias_read_pattern, num_blocks * g_alias_bdev.blocklen);
	} else {
		memset(buf, g_read_pattern, num_blocks * g_base_bdev.blocklen);
	}

	if (g_delay_next_read) {
		g_delay_next_read = false;
		g_pending_read = true;
		g_pending_read_cb = cb;
		g_pending_read_cb_arg = cb_arg;
		return 0;
	}

	cb((struct spdk_bdev_io *)0x1, true, cb_arg);
	return 0;
}

static void
complete_pending_read(bool success)
{
	spdk_bdev_io_completion_cb cb;
	void *cb_arg;

	CU_ASSERT(g_pending_read);
	cb = g_pending_read_cb;
	cb_arg = g_pending_read_cb_arg;
	g_pending_read = false;
	g_pending_read_cb = NULL;
	g_pending_read_cb_arg = NULL;
	cb((struct spdk_bdev_io *)0x1, success, cb_arg);
}

int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iov, int iovcnt,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *src_bdev;
	uint8_t pattern;
	int i;

	(void)ch;

	src_bdev = spdk_bdev_desc_get_bdev(desc);
	g_last_read_bdev = src_bdev;
	g_last_read_offset = offset_blocks;
	g_last_read_blocks = num_blocks;

	pattern = (src_bdev == &g_alias_bdev) ? g_alias_read_pattern : g_read_pattern;
	for (i = 0; i < iovcnt; i++) {
		if (iov[i].iov_base != NULL && iov[i].iov_len > 0) {
			memset(iov[i].iov_base, pattern, iov[i].iov_len);
		}
	}

	if (g_delay_next_read) {
		g_delay_next_read = false;
		g_pending_read = true;
		g_pending_read_cb = cb;
		g_pending_read_cb_arg = cb_arg;
		return 0;
	}

	cb((struct spdk_bdev_io *)0x1, true, cb_arg);
	return 0;
}

int
spdk_bdev_writev_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
			    spdk_bdev_io_completion_cb cb, void *cb_arg,
			    struct spdk_bdev_ext_io_opts *opts)
{
	(void)desc;
	(void)ch;
	(void)iov;
	(void)iovcnt;
	(void)offset_blocks;
	(void)num_blocks;
	(void)cb;
	(void)cb_arg;
	(void)opts;
	return -ENOTSUP;
}

int
spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	(void)desc;
	(void)ch;

	g_write_called = true;
	g_last_write_offset = offset_blocks;
	g_last_write_blocks = num_blocks;
	g_last_write_buf = buf;

	cb((struct spdk_bdev_io *)0x2, true, cb_arg);
	return 0;
}

struct spdk_thread *
spdk_get_thread(void)
{
	return g_ut_thread;
}

int
spdk_thread_poll(struct spdk_thread *thread, uint32_t max_msgs, uint64_t now)
{
	(void)thread;
	(void)max_msgs;
	(void)now;
	return 0;
}

int
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx)
{
	(void)thread;
	fn(ctx);
	return 0;
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	(void)bdev_io;
	(void)status;
}

struct vslm_async_test_cb {
	bool done;
	int status;
};

static void
vslm_async_test_done(void *cb_arg, int status)
{
	struct vslm_async_test_cb *cb = cb_arg;

	cb->done = true;
	cb->status = status;
}

static void
cleanup_vslm(struct vbdev_vslm *vslm)
{
	pthread_mutex_destroy(&vslm->policy_lock);
	spdk_dma_free(vslm->sram_buffer);
	free(vslm->page_array);
	vslm_free_shards(vslm);
}

static void
test_vslm_hash_lookup(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_page *page = NULL;
	int rc;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 4;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	rc = vslm_init_mmu(&vslm);
	CU_ASSERT(rc == 0);

	page = vslm_get_free_page(&vslm, &vslm.shards[0]);
	CU_ASSERT(page != NULL);
	page->vpn = 5;
	page->load_state = VSLM_PAGE_LOAD_RESIDENT;
	vslm_hash_insert(&vslm, page);
	vslm_lru_touch(&vslm, page);

	CU_ASSERT(vslm_lookup_page(&vslm, 5) == page);

	vslm_hash_remove(&vslm, page);
	CU_ASSERT(vslm_lookup_page(&vslm, 5) == NULL);

	cleanup_vslm(&vslm);
}

static void
test_vslm_reshard_idle(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_page *p;
	uint64_t i, counted;
	int rc;

	/* 4096 frames -> a multi-shard MMU under the adaptive policy. */
	vslm.sram_size_bytes = (uint64_t)VSLM_PAGE_SIZE * 4096;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	rc = vslm_init_mmu(&vslm);
	CU_ASSERT(rc == 0);
	CU_ASSERT(vslm.num_sram_pages == 4096);

	/* Collapse to a single shard; all frames land on shard 0's free list. */
	rc = vslm_reshard_idle(&vslm, 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(vslm.num_shards == 1);
	counted = 0;
	TAILQ_FOREACH(p, &vslm.shards[0].free_list, lru_link) {
		counted++;
	}
	CU_ASSERT(counted == 4096);

	/* Re-shard to 8; every frame appears on exactly one shard's free list. */
	rc = vslm_reshard_idle(&vslm, 8);
	CU_ASSERT(rc == 0);
	CU_ASSERT(vslm.num_shards == 8);
	counted = 0;
	for (i = 0; i < vslm.num_shards; i++) {
		TAILQ_FOREACH(p, &vslm.shards[i].free_list, lru_link) {
			counted++;
		}
	}
	CU_ASSERT(counted == 4096);

	/* A request above the cap clamps to VSLM_MMU_MAX_SHARDS. */
	rc = vslm_reshard_idle(&vslm, 10000);
	CU_ASSERT(rc == 0);
	CU_ASSERT(vslm.num_shards == VSLM_MMU_MAX_SHARDS);

	/* A non-idle MMU refuses to re-shard and keeps its shard count. */
	vslm.page_array[0].load_state = VSLM_PAGE_LOAD_RESIDENT;
	rc = vslm_reshard_idle(&vslm, 2);
	CU_ASSERT(rc == -EBUSY);
	CU_ASSERT(vslm.num_shards == VSLM_MMU_MAX_SHARDS);
	vslm.page_array[0].load_state = VSLM_PAGE_LOAD_FREE;

	cleanup_vslm(&vslm);
}

static void
test_vslm_copy_range_read_miss(void)
{
	struct vbdev_vslm vslm = {};
	uint8_t buf[VSLM_PAGE_SIZE];
	size_t i;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 1024;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = g_base_desc;
	vslm.virtual_size_bytes = g_base_bdev.blockcnt * g_base_bdev.blocklen;

	memset(buf, 0, sizeof(buf));

	reset_io_state();
	g_read_pattern = 0x5A;
	rc = vslm_copy_range(&vslm, (struct spdk_io_channel *)0x1, 0, sizeof(buf), buf, false);
	CU_ASSERT(rc == 0);
	for (i = 0; i < sizeof(buf); i++) {
		CU_ASSERT(buf[i] == 0x5A);
	}
	CU_ASSERT(vslm_lookup_page(&vslm, 0) != NULL);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_faults, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_fault_bytes, __ATOMIC_RELAXED) == VSLM_PAGE_SIZE);

	cleanup_vslm(&vslm);
}

static void
test_vslm_copy_range_write_evict(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_page *page;
	uint8_t buf[VSLM_PAGE_SIZE];
	size_t i;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 1024;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = g_base_desc;
	vslm.virtual_size_bytes = g_base_bdev.blockcnt * g_base_bdev.blocklen;

	page = vslm_get_free_page(&vslm, &vslm.shards[0]);
	CU_ASSERT(page != NULL);
	page->vpn = 0;
	page->dirty = true;
	page->is_busy = false;
	page->load_state = VSLM_PAGE_LOAD_RESIDENT;
	__atomic_store_n(&vslm.stats.dirty_resident_pages, 1, __ATOMIC_RELAXED);
	memset(vslm.sram_buffer, 0xCC, VSLM_PAGE_SIZE);
	vslm_hash_insert(&vslm, page);
	vslm_lru_touch(&vslm, page);

	memset(buf, 0xA5, sizeof(buf));

	reset_io_state();
	g_read_pattern = 0x00;
	rc = vslm_copy_range(&vslm, (struct spdk_io_channel *)0x1,
			     VSLM_PAGE_SIZE, sizeof(buf), buf, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_write_called);
	CU_ASSERT(g_last_write_offset == 0);
	CU_ASSERT(g_last_write_blocks == (VSLM_PAGE_SIZE / g_base_bdev.blocklen));
	CU_ASSERT(g_last_write_buf == vslm.sram_buffer);

	for (i = 0; i < VSLM_PAGE_SIZE; i++) {
		CU_ASSERT(vslm.sram_buffer[i] == 0xA5);
	}

	page = vslm_lookup_page(&vslm, 1);
	CU_ASSERT(page != NULL);
	if (page) {
		CU_ASSERT(page->dirty == true);
	}
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_evictions, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_writebacks, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.dirty_resident_pages, __ATOMIC_RELAXED) == 1);

	cleanup_vslm(&vslm);
}

static void
test_vslm_readahead_exec_read_prefetches_next(void)
{
	struct vbdev_vslm vslm = {};
	uint8_t buf[VSLM_PAGE_SIZE];
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 4;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	vslm.policy.readahead_enabled = true;
	vslm.policy.readahead_pages = 1;

	reset_io_state();
	memset(buf, 0, sizeof(buf));
	g_read_pattern = 0x77;
	/*
	 * The generalized strided prefetcher (paper Sec. 4.5) learns a range-local
	 * access trend before issuing speculative reads, so an isolated first touch
	 * does not prefetch. One sequential follow-on read establishes the +1 trend,
	 * after which the next page is staged ahead of demand.
	 */
	rc = vbdev_vslm_mem_exec_read_by_bdev(&vslm.vbdev, 0, sizeof(buf), buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(vslm_lookup_page(&vslm, 0) != NULL);
	CU_ASSERT(vslm_lookup_page(&vslm, 1) == NULL);

	rc = vbdev_vslm_mem_exec_read_by_bdev(&vslm.vbdev, VSLM_PAGE_SIZE, sizeof(buf), buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(vslm_lookup_page(&vslm, 1) != NULL);
	CU_ASSERT(vslm_lookup_page(&vslm, 2) != NULL);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_faults, __ATOMIC_RELAXED) == 2);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_fault_bytes, __ATOMIC_RELAXED) ==
		  2 * VSLM_PAGE_SIZE);

	cleanup_vslm(&vslm);
}

static void
test_vslm_async_exec_read_cold_default_backing(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_async_test_cb cb = {};
	struct vslm_page *page;
	uint8_t buf[VSLM_PAGE_SIZE];
	size_t i;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 2;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.inflight_lock, NULL) == 0);
	CU_ASSERT(pthread_cond_init(&vslm.inflight_cond, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	CU_ASSERT(vslm.async_exec_enabled);

	reset_io_state();
	memset(buf, 0, sizeof(buf));
	g_read_pattern = 0x6C;
	rc = vbdev_vslm_mem_exec_read_by_bdev_async(&vslm.vbdev, 0, sizeof(buf),
			buf, vslm_async_test_done, &cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb.done);
	CU_ASSERT(cb.status == 0);
	for (i = 0; i < sizeof(buf); i++) {
		CU_ASSERT(buf[i] == 0x6C);
	}

	page = vslm_lookup_page(&vslm, 0);
	CU_ASSERT(page != NULL);
	if (page != NULL) {
		CU_ASSERT(page->load_state == VSLM_PAGE_LOAD_RESIDENT);
	}
	CU_ASSERT(g_last_read_bdev == &g_base_bdev);
	CU_ASSERT(g_last_read_offset == 0);
	CU_ASSERT(g_last_read_blocks == (VSLM_PAGE_SIZE / g_base_bdev.blocklen));
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_faults, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_base_io_total, __ATOMIC_RELAXED) == 0);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_read_desc_io_total, __ATOMIC_RELAXED) == 0);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.fault_in_4k_total, __ATOMIC_RELAXED) == 1);

	pthread_cond_destroy(&vslm.inflight_cond);
	pthread_mutex_destroy(&vslm.inflight_lock);
	cleanup_vslm(&vslm);
}

static void
test_vslm_async_exec_read_loading_waiter_fanout(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_async_test_cb first_cb = {};
	struct vslm_async_test_cb second_cb = {};
	struct vslm_page *page;
	uint8_t first_buf[VSLM_PAGE_SIZE];
	uint8_t second_buf[VSLM_PAGE_SIZE];
	size_t i;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 2;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.inflight_lock, NULL) == 0);
	CU_ASSERT(pthread_cond_init(&vslm.inflight_cond, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	CU_ASSERT(vslm.async_exec_enabled);

	reset_io_state();
	memset(first_buf, 0, sizeof(first_buf));
	memset(second_buf, 0, sizeof(second_buf));
	g_read_pattern = 0x58;
	g_delay_next_read = true;

	rc = vbdev_vslm_mem_exec_read_by_bdev_async(&vslm.vbdev, 0, sizeof(first_buf),
			first_buf, vslm_async_test_done, &first_cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!first_cb.done);
	CU_ASSERT(g_pending_read);

	page = vslm_lookup_page(&vslm, 0);
	CU_ASSERT(page != NULL);
	if (page != NULL) {
		CU_ASSERT(page->load_state == VSLM_PAGE_LOAD_LOADING);
		CU_ASSERT(TAILQ_EMPTY(&page->waiters));
	}

	rc = vbdev_vslm_mem_exec_read_by_bdev_async(&vslm.vbdev, 0, sizeof(second_buf),
			second_buf, vslm_async_test_done, &second_cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!first_cb.done);
	CU_ASSERT(!second_cb.done);
	if (page != NULL) {
		CU_ASSERT(page->load_state == VSLM_PAGE_LOAD_LOADING);
		CU_ASSERT(!TAILQ_EMPTY(&page->waiters));
	}
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.prefetch_async_wait_total,
				  __ATOMIC_RELAXED) == 1);

	complete_pending_read(true);
	CU_ASSERT(first_cb.done);
	CU_ASSERT(second_cb.done);
	CU_ASSERT(first_cb.status == 0);
	CU_ASSERT(second_cb.status == 0);
	for (i = 0; i < sizeof(first_buf); i++) {
		CU_ASSERT(first_buf[i] == 0x58);
		CU_ASSERT(second_buf[i] == 0x58);
	}
	if (page != NULL) {
		CU_ASSERT(page->load_state == VSLM_PAGE_LOAD_RESIDENT);
	}
	CU_ASSERT(g_last_read_bdev == &g_base_bdev);
	CU_ASSERT(g_last_read_blocks == (VSLM_PAGE_SIZE / g_base_bdev.blocklen));
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_base_io_total, __ATOMIC_RELAXED) == 0);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_read_desc_io_total, __ATOMIC_RELAXED) == 0);

	pthread_cond_destroy(&vslm.inflight_cond);
	pthread_mutex_destroy(&vslm.inflight_lock);
	cleanup_vslm(&vslm);
}

static void
test_vslm_async_exec_write_full_page_cold_default_backing(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_async_test_cb cb = {};
	struct vslm_page *page;
	uint8_t buf[VSLM_PAGE_SIZE];
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 2;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.inflight_lock, NULL) == 0);
	CU_ASSERT(pthread_cond_init(&vslm.inflight_cond, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	CU_ASSERT(vslm.async_exec_enabled);

	reset_io_state();
	memset(buf, 0xA9, sizeof(buf));
	rc = vbdev_vslm_mem_exec_write_by_bdev_async(&vslm.vbdev, 0, sizeof(buf),
			buf, vslm_async_test_done, &cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb.done);
	CU_ASSERT(cb.status == 0);

	page = vslm_lookup_page(&vslm, 0);
	CU_ASSERT(page != NULL);
	if (page != NULL) {
		CU_ASSERT(page->load_state == VSLM_PAGE_LOAD_RESIDENT);
		CU_ASSERT(page->dirty);
		CU_ASSERT(memcmp(vslm.sram_buffer + page->ppn * VSLM_PAGE_SIZE,
				 buf, sizeof(buf)) == 0);
	}
	CU_ASSERT(g_last_read_bdev == NULL);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_faults, __ATOMIC_RELAXED) == 0);
	CU_ASSERT(__atomic_load_n(&vslm.stats.dirty_resident_pages, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_base_io_total, __ATOMIC_RELAXED) == 0);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_read_desc_io_total, __ATOMIC_RELAXED) == 0);

	pthread_cond_destroy(&vslm.inflight_cond);
	pthread_mutex_destroy(&vslm.inflight_lock);
	cleanup_vslm(&vslm);
}

static void
test_vslm_async_exec_write_partial_page_faults_without_sync_io(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_async_test_cb cb = {};
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	uint8_t buf[128];
	uint64_t page_blocks;
	size_t i;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;
	page_blocks = VSLM_PAGE_SIZE / g_base_bdev.blocklen;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 2;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.inflight_lock, NULL) == 0);
	CU_ASSERT(pthread_cond_init(&vslm.inflight_cond, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	CU_ASSERT(vslm.async_exec_enabled);

	reset_io_state();
	g_read_pattern = 0x44;
	memset(buf, 0xE7, sizeof(buf));
	rc = vbdev_vslm_mem_exec_write_by_bdev_async(&vslm.vbdev, 64, sizeof(buf),
			buf, vslm_async_test_done, &cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb.done);
	CU_ASSERT(cb.status == 0);

	page = vslm_lookup_page(&vslm, 0);
	CU_ASSERT(page != NULL);
	if (page != NULL) {
		CU_ASSERT(page->load_state == VSLM_PAGE_LOAD_RESIDENT);
		CU_ASSERT(page->dirty);
		for (i = 0; i < 64; i++) {
			CU_ASSERT(vslm.sram_buffer[page->ppn * VSLM_PAGE_SIZE + i] == 0x44);
		}
		for (i = 64; i < 64 + sizeof(buf); i++) {
			CU_ASSERT(vslm.sram_buffer[page->ppn * VSLM_PAGE_SIZE + i] == 0xE7);
		}
		for (i = 64 + sizeof(buf); i < VSLM_PAGE_SIZE; i++) {
			CU_ASSERT(vslm.sram_buffer[page->ppn * VSLM_PAGE_SIZE + i] == 0x44);
		}
	}
	lpage = vslm_lookup_lpage(&vslm, 0);
	CU_ASSERT(lpage != NULL);
	if (lpage != NULL) {
		CU_ASSERT(lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT);
	}
	CU_ASSERT(!g_write_called);
	CU_ASSERT(g_last_read_bdev == &g_base_bdev);
	CU_ASSERT(g_last_read_offset == 0);
	CU_ASSERT(g_last_read_blocks == page_blocks);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_faults, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.vslm_fault_clean_total,
				  __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.dirty_resident_pages, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_base_io_total, __ATOMIC_RELAXED) == 0);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_read_desc_io_total, __ATOMIC_RELAXED) == 0);

	pthread_cond_destroy(&vslm.inflight_cond);
	pthread_mutex_destroy(&vslm.inflight_lock);
	cleanup_vslm(&vslm);
}

static void
test_vslm_async_exec_read_clean_eviction_no_sync_io(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_async_test_cb first_cb = {};
	struct vslm_async_test_cb second_cb = {};
	struct vslm_page *page;
	uint8_t first_buf[VSLM_PAGE_SIZE];
	uint8_t second_buf[VSLM_PAGE_SIZE];
	uint64_t page_blocks;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;
	page_blocks = VSLM_PAGE_SIZE / g_base_bdev.blocklen;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.inflight_lock, NULL) == 0);
	CU_ASSERT(pthread_cond_init(&vslm.inflight_cond, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	CU_ASSERT(vslm.async_exec_enabled);

	reset_io_state();
	g_read_pattern = 0x29;
	memset(first_buf, 0, sizeof(first_buf));
	rc = vbdev_vslm_mem_exec_read_by_bdev_async(&vslm.vbdev, 0, sizeof(first_buf),
			first_buf, vslm_async_test_done, &first_cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(first_cb.done);
	CU_ASSERT(first_cb.status == 0);
	CU_ASSERT(vslm_lookup_page(&vslm, 0) != NULL);

	reset_io_state();
	g_read_pattern = 0x3A;
	memset(second_buf, 0, sizeof(second_buf));
	rc = vbdev_vslm_mem_exec_read_by_bdev_async(&vslm.vbdev, VSLM_PAGE_SIZE,
			sizeof(second_buf), second_buf, vslm_async_test_done, &second_cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(second_cb.done);
	CU_ASSERT(second_cb.status == 0);
	CU_ASSERT(second_buf[0] == 0x3A);

	CU_ASSERT(vslm_lookup_page(&vslm, 0) == NULL);
	page = vslm_lookup_page(&vslm, 1);
	CU_ASSERT(page != NULL);
	if (page != NULL) {
		CU_ASSERT(page->load_state == VSLM_PAGE_LOAD_RESIDENT);
		CU_ASSERT(!page->dirty);
	}
	CU_ASSERT(g_last_read_bdev == &g_base_bdev);
	CU_ASSERT(g_last_read_offset == page_blocks);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_evictions, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.vslm_eviction_clean_total,
				  __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_base_io_total, __ATOMIC_RELAXED) == 0);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_read_desc_io_total,
				  __ATOMIC_RELAXED) == 0);

	pthread_cond_destroy(&vslm.inflight_cond);
	pthread_mutex_destroy(&vslm.inflight_lock);
	cleanup_vslm(&vslm);
}

static void
test_vslm_async_exec_read_dirty_eviction_no_sync_io(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_async_test_cb write_cb = {};
	struct vslm_async_test_cb read_cb = {};
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	uint8_t write_buf[VSLM_PAGE_SIZE];
	uint8_t read_buf[VSLM_PAGE_SIZE];
	uint64_t page_blocks;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;
	page_blocks = VSLM_PAGE_SIZE / g_base_bdev.blocklen;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.inflight_lock, NULL) == 0);
	CU_ASSERT(pthread_cond_init(&vslm.inflight_cond, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	CU_ASSERT(vslm.async_exec_enabled);

	reset_io_state();
	memset(write_buf, 0xB3, sizeof(write_buf));
	rc = vbdev_vslm_mem_exec_write_by_bdev_async(&vslm.vbdev, 0, sizeof(write_buf),
			write_buf, vslm_async_test_done, &write_cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(write_cb.done);
	CU_ASSERT(write_cb.status == 0);
	page = vslm_lookup_page(&vslm, 0);
	CU_ASSERT(page != NULL);
	if (page != NULL) {
		CU_ASSERT(page->dirty);
	}

	reset_io_state();
	g_read_pattern = 0x4B;
	memset(read_buf, 0, sizeof(read_buf));
	rc = vbdev_vslm_mem_exec_read_by_bdev_async(&vslm.vbdev, VSLM_PAGE_SIZE,
			sizeof(read_buf), read_buf, vslm_async_test_done, &read_cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(read_cb.done);
	CU_ASSERT(read_cb.status == 0);
	CU_ASSERT(read_buf[0] == 0x4B);
	CU_ASSERT(g_write_called);
	CU_ASSERT(g_last_write_offset == 0);
	CU_ASSERT(g_last_write_blocks == page_blocks);
	CU_ASSERT(g_last_read_bdev == &g_base_bdev);
	CU_ASSERT(g_last_read_offset == page_blocks);

	CU_ASSERT(vslm_lookup_page(&vslm, 0) == NULL);
	page = vslm_lookup_page(&vslm, 1);
	CU_ASSERT(page != NULL);
	if (page != NULL) {
		CU_ASSERT(page->load_state == VSLM_PAGE_LOAD_RESIDENT);
		CU_ASSERT(!page->dirty);
	}
	lpage = vslm_lookup_lpage(&vslm, 0);
	CU_ASSERT(lpage != NULL);
	if (lpage != NULL) {
		CU_ASSERT(lpage->state == SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE);
	}
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_evictions, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_writebacks, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.vslm_eviction_private_total,
				  __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.dirty_resident_pages, __ATOMIC_RELAXED) == 0);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_base_io_total, __ATOMIC_RELAXED) == 0);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_read_desc_io_total,
				  __ATOMIC_RELAXED) == 0);

	pthread_cond_destroy(&vslm.inflight_cond);
	pthread_mutex_destroy(&vslm.inflight_lock);
	cleanup_vslm(&vslm);
}

static void
test_vslm_async_exec_read_spilled_private_refault_no_sync_io(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_async_test_cb write_cb = {};
	struct vslm_async_test_cb evict_cb = {};
	struct vslm_async_test_cb refault_cb = {};
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	uint8_t write_buf[VSLM_PAGE_SIZE];
	uint8_t evict_buf[VSLM_PAGE_SIZE];
	uint8_t refault_buf[VSLM_PAGE_SIZE];
	uint64_t page_blocks;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;
	page_blocks = VSLM_PAGE_SIZE / g_base_bdev.blocklen;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.inflight_lock, NULL) == 0);
	CU_ASSERT(pthread_cond_init(&vslm.inflight_cond, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	CU_ASSERT(vslm.async_exec_enabled);

	reset_io_state();
	memset(write_buf, 0xD1, sizeof(write_buf));
	rc = vbdev_vslm_mem_exec_write_by_bdev_async(&vslm.vbdev, 0, sizeof(write_buf),
			write_buf, vslm_async_test_done, &write_cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(write_cb.done);
	CU_ASSERT(write_cb.status == 0);

	reset_io_state();
	g_read_pattern = 0x31;
	memset(evict_buf, 0, sizeof(evict_buf));
	rc = vbdev_vslm_mem_exec_read_by_bdev_async(&vslm.vbdev, VSLM_PAGE_SIZE,
			sizeof(evict_buf), evict_buf, vslm_async_test_done, &evict_cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(evict_cb.done);
	CU_ASSERT(evict_cb.status == 0);
	CU_ASSERT(g_write_called);
	CU_ASSERT(g_last_write_offset == 0);
	CU_ASSERT(g_last_write_blocks == page_blocks);

	lpage = vslm_lookup_lpage(&vslm, 0);
	CU_ASSERT(lpage != NULL);
	if (lpage != NULL) {
		CU_ASSERT(lpage->state == SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE);
		CU_ASSERT(lpage->source_bdev == &g_base_bdev);
		CU_ASSERT(lpage->source_offset_bytes == 0);
	}

	reset_io_state();
	g_read_pattern = 0xD1;
	memset(refault_buf, 0, sizeof(refault_buf));
	rc = vbdev_vslm_mem_exec_read_by_bdev_async(&vslm.vbdev, 0,
			sizeof(refault_buf), refault_buf, vslm_async_test_done,
			&refault_cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(refault_cb.done);
	CU_ASSERT(refault_cb.status == 0);
	CU_ASSERT(refault_buf[0] == 0xD1);
	CU_ASSERT(!g_write_called);
	CU_ASSERT(g_last_read_bdev == &g_base_bdev);
	CU_ASSERT(g_last_read_offset == 0);
	CU_ASSERT(g_last_read_blocks == page_blocks);

	page = vslm_lookup_page(&vslm, 0);
	CU_ASSERT(page != NULL);
	if (page != NULL) {
		CU_ASSERT(page->load_state == VSLM_PAGE_LOAD_RESIDENT);
		CU_ASSERT(!page->dirty);
	}
	lpage = vslm_lookup_lpage(&vslm, 0);
	CU_ASSERT(lpage != NULL);
	if (lpage != NULL) {
		CU_ASSERT(lpage->state == SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT);
		CU_ASSERT(lpage->private_authoritative);
	}
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_faults, __ATOMIC_RELAXED) == 2);
	CU_ASSERT(__atomic_load_n(&vslm.stats.vslm_fault_clean_total,
				  __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.vslm_fault_private_total,
				  __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.vslm_spill_read_bytes,
				  __ATOMIC_RELAXED) == VSLM_PAGE_SIZE);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_writebacks, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_base_io_total, __ATOMIC_RELAXED) == 0);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_read_desc_io_total,
				  __ATOMIC_RELAXED) == 0);

	pthread_cond_destroy(&vslm.inflight_cond);
	pthread_mutex_destroy(&vslm.inflight_lock);
	cleanup_vslm(&vslm);
}

static void
test_vslm_readahead_disabled_no_prefetch(void)
{
	struct vbdev_vslm vslm = {};
	uint8_t buf[VSLM_PAGE_SIZE];
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 4;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	vslm.policy.readahead_enabled = false;
	vslm.policy.readahead_pages = 4;

	reset_io_state();
	memset(buf, 0, sizeof(buf));
	g_read_pattern = 0x42;
	rc = vbdev_vslm_mem_exec_read_by_bdev(&vslm.vbdev, 0, sizeof(buf), buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(vslm_lookup_page(&vslm, 0) != NULL);
	CU_ASSERT(vslm_lookup_page(&vslm, 1) == NULL);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_faults, __ATOMIC_RELAXED) == 1);

	cleanup_vslm(&vslm);
}

static void
test_vslm_readahead_does_not_increment_demand_fault_stats(void)
{
	struct vbdev_vslm vslm = {};
	uint8_t buf[VSLM_PAGE_SIZE];
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 4;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	vslm.policy.readahead_enabled = true;
	vslm.policy.readahead_pages = 2;

	reset_io_state();
	memset(buf, 0, sizeof(buf));
	g_read_pattern = 0x18;
	/*
	 * Establish a sequential trend (pages 0 then 1) so the strided prefetcher
	 * stages the following readahead_pages pages. The speculative loads must
	 * not be counted as demand page faults.
	 */
	rc = vbdev_vslm_mem_exec_read_by_bdev(&vslm.vbdev, 0, sizeof(buf), buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(vslm_lookup_page(&vslm, 0) != NULL);
	CU_ASSERT(vslm_lookup_page(&vslm, 1) == NULL);

	rc = vbdev_vslm_mem_exec_read_by_bdev(&vslm.vbdev, VSLM_PAGE_SIZE, sizeof(buf), buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(vslm_lookup_page(&vslm, 1) != NULL);
	CU_ASSERT(vslm_lookup_page(&vslm, 2) != NULL);
	CU_ASSERT(vslm_lookup_page(&vslm, 3) != NULL);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_faults, __ATOMIC_RELAXED) == 2);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_fault_bytes, __ATOMIC_RELAXED) ==
		  2 * VSLM_PAGE_SIZE);

	cleanup_vslm(&vslm);
}

static void
test_vslm_readahead_skips_when_no_free_frames(void)
{
	struct vbdev_vslm vslm = {};
	uint8_t buf[VSLM_PAGE_SIZE];
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	vslm.policy.readahead_enabled = true;
	vslm.policy.readahead_pages = 4;

	reset_io_state();
	g_read_pattern = 0x21;
	rc = vslm_copy_range(&vslm, (struct spdk_io_channel *)0x1, 0, sizeof(buf), buf, false);
	CU_ASSERT(rc == 0);
	CU_ASSERT(vslm_lookup_page(&vslm, 0) != NULL);

	memset(&vslm.stats, 0, sizeof(vslm.stats));
	memset(buf, 0, sizeof(buf));
	g_read_pattern = 0x39;
	rc = vbdev_vslm_mem_exec_read_by_bdev(&vslm.vbdev, VSLM_PAGE_SIZE, sizeof(buf), buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(vslm_lookup_page(&vslm, 1) != NULL);
	CU_ASSERT(vslm_lookup_page(&vslm, 2) == NULL);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_faults, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_evictions, __ATOMIC_RELAXED) == 1);
	CU_ASSERT(__atomic_load_n(&vslm.stats.page_writebacks, __ATOMIC_RELAXED) == 0);

	cleanup_vslm(&vslm);
}

static void
test_vslm_policy_validation_readahead_bounds(void)
{
	struct spdk_bdev_vslm_policy policy = {
		.semantics_mode = SPDK_BDEV_VSLM_SEMANTICS_LEASE,
		.writeback_policy = SPDK_BDEV_VSLM_WRITEBACK_ON_EVICT,
		.admission_enabled = false,
		.admission_faults_per_sec_threshold = 0,
		.readahead_enabled = true,
		.readahead_pages = 0,
	};

	CU_ASSERT(vslm_validate_policy(&policy) == -EINVAL);

	policy.readahead_pages = 65;
	CU_ASSERT(vslm_validate_policy(&policy) == -EINVAL);

	policy.readahead_pages = 8;
	CU_ASSERT(vslm_validate_policy(&policy) == 0);

	policy.readahead_enabled = false;
	policy.readahead_pages = 0;
	CU_ASSERT(vslm_validate_policy(&policy) == 0);
}

static void
test_vslm_alias_fault_in_prefers_source(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_page *page = NULL;
	struct vslm_lpage *lpage;
	uint64_t expected_offset_blocks;
	int rc;

	g_base_bdev.name = "base0";
	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;

	g_alias_bdev.name = "alias0";
	g_alias_bdev.blocklen = 512;
	g_alias_bdev.blockcnt = 8192;

	g_read_pattern = 0x11;
	g_alias_read_pattern = 0xC7;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.lease_lock, NULL) == 0);
	TAILQ_INIT(&vslm.leases);
	TAILQ_INIT(&vslm.blocked_cmds);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = g_base_bdev.blockcnt * g_base_bdev.blocklen;

	rc = vslm_stage_alias_range(&vslm, 0, VSLM_PAGE_SIZE, &g_alias_bdev, 2 * VSLM_PAGE_SIZE);
	CU_ASSERT(rc == 0);

	reset_io_state();
	rc = vslm_resolve_page(&vslm, (struct spdk_io_channel *)0x1, 0, &page);
	CU_ASSERT(rc == 0);
	CU_ASSERT_PTR_NOT_NULL_FATAL(page);

	expected_offset_blocks = (2 * VSLM_PAGE_SIZE) / g_alias_bdev.blocklen;
	CU_ASSERT(g_last_read_bdev == &g_alias_bdev);
	CU_ASSERT(g_last_read_offset == expected_offset_blocks);
	CU_ASSERT(g_last_read_blocks == (VSLM_PAGE_SIZE / g_alias_bdev.blocklen));
	CU_ASSERT(vslm.sram_buffer[0] == g_alias_read_pattern);

	lpage = vslm_lookup_lpage(&vslm, 0);
	CU_ASSERT_PTR_NOT_NULL_FATAL(lpage);
	CU_ASSERT(lpage->state == SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT);
	CU_ASSERT(lpage->source_type == SPDK_BDEV_VSLM_PAGE_SOURCE_ALIAS_BDEV);
	CU_ASSERT(lpage->source_bdev == &g_alias_bdev);

	cleanup_vslm(&vslm);
	pthread_mutex_destroy(&vslm.lease_lock);
}

static void
test_vslm_async_exec_read_alias_uses_async_source(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_async_test_cb cb = {};
	uint8_t buf[VSLM_PAGE_SIZE];
	uint64_t expected_offset_blocks;
	int rc;

	g_base_bdev.name = "base_async_alias";
	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;

	g_alias_bdev.name = "alias_async";
	g_alias_bdev.blocklen = 512;
	g_alias_bdev.blockcnt = 8192;

	g_read_pattern = 0x12;
	g_alias_read_pattern = 0xD4;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.inflight_lock, NULL) == 0);
	CU_ASSERT(pthread_cond_init(&vslm.inflight_cond, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 8;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	CU_ASSERT(vslm.async_exec_enabled);

	rc = vslm_stage_alias_range(&vslm, 0, VSLM_PAGE_SIZE, &g_alias_bdev,
				    3 * VSLM_PAGE_SIZE);
	CU_ASSERT(rc == 0);

	reset_io_state();
	memset(buf, 0, sizeof(buf));
	rc = vbdev_vslm_mem_exec_read_by_bdev_async(&vslm.vbdev, 0, sizeof(buf),
			buf, vslm_async_test_done, &cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb.done);
	CU_ASSERT(cb.status == 0);
	CU_ASSERT(buf[0] == g_alias_read_pattern);

	expected_offset_blocks = (3 * VSLM_PAGE_SIZE) / g_alias_bdev.blocklen;
	CU_ASSERT(g_last_read_bdev == &g_alias_bdev);
	CU_ASSERT(g_last_read_offset == expected_offset_blocks);
	CU_ASSERT(__atomic_load_n(&vslm.perf_stats.sync_read_desc_io_total,
				  __ATOMIC_RELAXED) == 0);

	pthread_cond_destroy(&vslm.inflight_cond);
	pthread_mutex_destroy(&vslm.inflight_lock);
	cleanup_vslm(&vslm);
}

static void
test_vslm_alias_cow_authority_transition(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_lpage *lpage;
	uint8_t write_buf[VSLM_PAGE_SIZE];
	uint8_t read_buf[VSLM_PAGE_SIZE];
	int rc;

	g_base_bdev.name = "base1";
	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;

	g_alias_bdev.name = "alias1";
	g_alias_bdev.blocklen = 512;
	g_alias_bdev.blockcnt = 8192;

	g_read_pattern = 0x31;
	g_alias_read_pattern = 0xA1;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.lease_lock, NULL) == 0);
	TAILQ_INIT(&vslm.leases);
	TAILQ_INIT(&vslm.blocked_cmds);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 8;

	rc = vslm_stage_alias_range(&vslm, 0, VSLM_PAGE_SIZE, &g_alias_bdev, 0);
	CU_ASSERT(rc == 0);

	memset(write_buf, 0x5E, sizeof(write_buf));
	rc = vslm_copy_range(&vslm, (struct spdk_io_channel *)0x1, 0, sizeof(write_buf),
			     write_buf, true);
	CU_ASSERT(rc == 0);

	lpage = vslm_lookup_lpage(&vslm, 0);
	CU_ASSERT_PTR_NOT_NULL_FATAL(lpage);
	CU_ASSERT(lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT);
	CU_ASSERT(lpage->private_authoritative == true);
	CU_ASSERT(lpage->source_bdev == NULL);
	CU_ASSERT(lpage->has_superseded_source == true);
	CU_ASSERT(lpage->superseded_source_bdev == &g_alias_bdev);

	reset_io_state();
	memset(read_buf, 0, sizeof(read_buf));
	rc = vslm_copy_range(&vslm, (struct spdk_io_channel *)0x1, VSLM_PAGE_SIZE,
			     sizeof(read_buf), read_buf, false);
	CU_ASSERT(rc == 0);

	CU_ASSERT(g_write_called == true);
	CU_ASSERT(g_last_write_offset == 0);

	lpage = vslm_lookup_lpage(&vslm, 0);
	CU_ASSERT_PTR_NOT_NULL_FATAL(lpage);
	CU_ASSERT(lpage->state == SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE);
	CU_ASSERT(lpage->private_authoritative == true);
	CU_ASSERT(lpage->source_type == SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING);
	CU_ASSERT(lpage->source_bdev == &g_base_bdev);
	CU_ASSERT(lpage->pending_publish == false);
	CU_ASSERT(lpage->has_superseded_source == false);

	cleanup_vslm(&vslm);
	pthread_mutex_destroy(&vslm.lease_lock);
}

static void
test_vslm_copy_by_bdev_conflict_classification(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_lpage *dst_lpage;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 2;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.lease_lock, NULL) == 0);
	TAILQ_INIT(&vslm.leases);
	TAILQ_INIT(&vslm.blocked_cmds);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	vslm.vbdev.nsid = 403;
	vslm.vbdev.blocklen = 512;
	vslm.vbdev.blockcnt = vslm.virtual_size_bytes / vslm.vbdev.blocklen;
	TAILQ_INSERT_TAIL(&g_vslm_bdevs, &vslm, link);

	rc = vbdev_vslm_mem_lease_acquire_by_bdev(100, &vslm.vbdev, 0, VSLM_PAGE_SIZE);
	CU_ASSERT(rc == 0);

	/* Lease on source region must not block alias staging. */
	rc = vbdev_vslm_mem_copy_by_bdev(&vslm.vbdev, VSLM_PAGE_SIZE, &vslm.vbdev, 0, VSLM_PAGE_SIZE);
	CU_ASSERT(rc == 0);

	dst_lpage = vslm_lookup_lpage(&vslm, 1);
	CU_ASSERT_PTR_NOT_NULL_FATAL(dst_lpage);
	CU_ASSERT(dst_lpage->state == SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS);
	CU_ASSERT(dst_lpage->source_bdev == &vslm.vbdev);
	CU_ASSERT(dst_lpage->source_offset_bytes == 0);

	/* Lease on destination region must block. */
	rc = vbdev_vslm_mem_copy_by_bdev(&vslm.vbdev, 0, &vslm.vbdev, VSLM_PAGE_SIZE, VSLM_PAGE_SIZE);
	CU_ASSERT(rc == -EAGAIN);

	rc = vbdev_vslm_mem_lease_release(100);
	CU_ASSERT(rc == 0);

	TAILQ_REMOVE(&g_vslm_bdevs, &vslm, link);
	cleanup_vslm(&vslm);
	pthread_mutex_destroy(&vslm.lease_lock);
}

static void
test_vslm_bdev_callbacks(void)
{
	struct vbdev_vslm vslm = {};
	struct spdk_bdev fake_bdev = {};
	uint8_t write_buf[64];
	uint8_t read_buf[64];
	void *ptr = (void *)0x1;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 1024;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = g_base_bdev.blockcnt * g_base_bdev.blocklen;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	vslm.vbdev.nsid = 401;
	TAILQ_INSERT_TAIL(&g_vslm_bdevs, &vslm, link);

	memset(write_buf, 0x3C, sizeof(write_buf));
	memset(read_buf, 0x00, sizeof(read_buf));

	rc = vbdev_vslm_mem_write_by_bdev(&vslm.vbdev, 64, sizeof(write_buf), write_buf);
	CU_ASSERT(rc == 0);

	rc = vbdev_vslm_mem_read_by_bdev(&vslm.vbdev, 64, sizeof(read_buf), read_buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(memcmp(read_buf, write_buf, sizeof(write_buf)) == 0);

	rc = vbdev_vslm_mem_get_buffer_ptr_by_bdev(&vslm.vbdev, 64, sizeof(write_buf), &ptr);
	CU_ASSERT(rc == -ENOTSUP);
	CU_ASSERT(ptr == NULL);

	ptr = (void *)0x1;
	rc = bdev_vslm_get_buffer_ptr_by_nsid(vslm.vbdev.nsid, 64, sizeof(write_buf), &ptr);
	CU_ASSERT(rc == -ENOTSUP);
	CU_ASSERT(ptr == NULL);

	rc = vbdev_vslm_mem_read_by_bdev(&fake_bdev, 0, sizeof(read_buf), read_buf);
	CU_ASSERT(rc == -ENOTSUP);

	rc = vbdev_vslm_mem_write_by_bdev(&fake_bdev, 0, sizeof(write_buf), write_buf);
	CU_ASSERT(rc == -ENOTSUP);

	rc = vbdev_vslm_mem_get_buffer_ptr_by_bdev(&fake_bdev, 0, sizeof(write_buf), &ptr);
	CU_ASSERT(rc == -ENOTSUP);

	ptr = (void *)0x1;
	rc = vbdev_vslm_mem_get_buffer_ptr_by_bdev(&vslm.vbdev, 0, 0, &ptr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ptr == NULL);

	TAILQ_REMOVE(&g_vslm_bdevs, &vslm, link);
	cleanup_vslm(&vslm);
}

static void
test_vslm_bdev_pin_unpin_range(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_page *page;
	struct spdk_bdev_slm_sg_entry entries[4] = {};
	uint32_t entry_count = 0;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 4096;
	g_read_pattern = 0x6B;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 2;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = g_base_bdev.blockcnt * g_base_bdev.blocklen;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	vslm.vbdev.nsid = 406;
	TAILQ_INSERT_TAIL(&g_vslm_bdevs, &vslm, link);

	reset_io_state();
	rc = vbdev_vslm_mem_try_pin_range_by_bdev(&vslm.vbdev, 32, 96, false,
			entries, SPDK_COUNTOF(entries), &entry_count);
	CU_ASSERT(rc == -EAGAIN);
	CU_ASSERT(entry_count == 0);
	CU_ASSERT(g_last_read_bdev == NULL);

	rc = vbdev_vslm_mem_pin_range_by_bdev(&vslm.vbdev, 32, 96, false,
					      entries, SPDK_COUNTOF(entries), &entry_count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(entry_count == 1);
	CU_ASSERT(entries[0].logical_offset == 32);
	CU_ASSERT(entries[0].len == 96);
	CU_ASSERT(entries[0].addr != NULL);
	CU_ASSERT(((uint8_t *)entries[0].addr)[0] == g_read_pattern);

	page = vslm_lookup_page(&vslm, 0);
	CU_ASSERT_PTR_NOT_NULL_FATAL(page);
	CU_ASSERT(page->pin_count == 1);
	CU_ASSERT(page->dirty == false);

	rc = vbdev_vslm_mem_unpin_range_by_bdev(&vslm.vbdev, entries, entry_count, false);
	CU_ASSERT(rc == 0);
	CU_ASSERT(page->pin_count == 0);
	CU_ASSERT(page->dirty == false);

	entry_count = 0;
	rc = vbdev_vslm_mem_try_pin_range_by_bdev(&vslm.vbdev, 32, 96, false,
			entries, SPDK_COUNTOF(entries), &entry_count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(entry_count == 1);
	CU_ASSERT(entries[0].logical_offset == 32);
	CU_ASSERT(entries[0].len == 96);
	CU_ASSERT(page->pin_count == 1);

	rc = vbdev_vslm_mem_unpin_range_by_bdev(&vslm.vbdev, entries, entry_count, false);
	CU_ASSERT(rc == 0);
	CU_ASSERT(page->pin_count == 0);

	rc = bdev_vslm_pin_range_by_nsid(vslm.vbdev.nsid, 0, 32, false,
					 entries, SPDK_COUNTOF(entries), &entry_count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(entry_count == 1);

	rc = bdev_vslm_unpin_range_by_nsid(vslm.vbdev.nsid, entries, entry_count, false);
	CU_ASSERT(rc == 0);

	rc = bdev_vslm_pin_range_by_nsid(0, 0, 32, false, entries, SPDK_COUNTOF(entries),
					 &entry_count);
	CU_ASSERT(rc == -EINVAL);

	rc = bdev_vslm_pin_range_by_nsid(9999, 0, 32, false, entries, SPDK_COUNTOF(entries),
					 &entry_count);
	CU_ASSERT(rc == -ENOENT);

	rc = vbdev_vslm_mem_pin_range_by_bdev(&vslm.vbdev, 0, 32, false,
					      entries, 0, &entry_count);
	CU_ASSERT(rc == -EINVAL);

	rc = vbdev_vslm_mem_unpin_range_by_bdev(&vslm.vbdev, NULL, 1, false);
	CU_ASSERT(rc == -EINVAL);

	TAILQ_REMOVE(&g_vslm_bdevs, &vslm, link);
	cleanup_vslm(&vslm);
}

static void
test_vslm_get_stats_exports_perf_counters(void)
{
	struct vbdev_vslm vslm = {};
	struct spdk_bdev_vslm_stats stats = {};
	int rc;

	vslm.vbdev.name = "stats_vslm";
	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 2;
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.perf_stats.fault_in_io_ns_total = 11;
	vslm.perf_stats.fault_in_io_ns_max = 12;
	vslm.perf_stats.mmu_lock_acquire_total = 13;
	vslm.perf_stats.mmu_lock_hold_ns_total = 14;
	vslm.perf_stats.mmu_lock_hold_ns_max = 15;
	vslm.perf_stats.sync_base_io_total = 16;
	vslm.perf_stats.sync_base_io_ns_total = 17;
	vslm.perf_stats.sync_base_io_ns_max = 18;
	vslm.perf_stats.sync_read_desc_io_total = 19;
	vslm.perf_stats.sync_read_desc_io_ns_total = 20;
	vslm.perf_stats.sync_read_desc_io_ns_max = 21;
	vslm.perf_stats.fault_batch_attempt_total = 22;
	vslm.perf_stats.fault_batch_fallback_total = 23;
	vslm.perf_stats.prefetch_async_wait_total = 24;
	vslm.perf_stats.prefetch_async_wait_ns_total = 25;
	TAILQ_INSERT_TAIL(&g_vslm_bdevs, &vslm, link);

	rc = bdev_vslm_get_stats("stats_vslm", &stats);
	CU_ASSERT(rc == 0);
	CU_ASSERT(stats.vslm_fast_tier_bytes == VSLM_PAGE_SIZE * 2);
	CU_ASSERT(stats.vslm_logical_working_set_bytes == VSLM_PAGE_SIZE * 16);
	CU_ASSERT(stats.perf_fault_in_io_ns_total == 11);
	CU_ASSERT(stats.perf_fault_in_io_ns_max == 12);
	CU_ASSERT(stats.perf_mmu_lock_acquire_total == 13);
	CU_ASSERT(stats.perf_mmu_lock_hold_ns_total == 14);
	CU_ASSERT(stats.perf_mmu_lock_hold_ns_max == 15);
	CU_ASSERT(stats.perf_sync_base_io_total == 16);
	CU_ASSERT(stats.perf_sync_base_io_ns_total == 17);
	CU_ASSERT(stats.perf_sync_base_io_ns_max == 18);
	CU_ASSERT(stats.perf_sync_read_desc_io_total == 19);
	CU_ASSERT(stats.perf_sync_read_desc_io_ns_total == 20);
	CU_ASSERT(stats.perf_sync_read_desc_io_ns_max == 21);
	CU_ASSERT(stats.perf_fault_batch_attempt_total == 22);
	CU_ASSERT(stats.perf_fault_batch_fallback_total == 23);
	CU_ASSERT(stats.perf_prefetch_async_wait_total == 24);
	CU_ASSERT(stats.perf_prefetch_async_wait_ns_total == 25);

	TAILQ_REMOVE(&g_vslm_bdevs, &vslm, link);
}

static void
test_vslm_bdev_unpin_dirtied_entry_marks_page_dirty(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	struct spdk_bdev_slm_sg_entry entries[2] = {};
	uint32_t entry_count = 0;
	int rc;

	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 4096;
	g_read_pattern = 0x34;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = (struct spdk_bdev_desc *)0x1;
	vslm.virtual_size_bytes = g_base_bdev.blockcnt * g_base_bdev.blocklen;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	vslm.vbdev.nsid = 407;
	TAILQ_INSERT_TAIL(&g_vslm_bdevs, &vslm, link);

	rc = vbdev_vslm_mem_pin_range_by_bdev(&vslm.vbdev, 0, 64, true,
					      entries, SPDK_COUNTOF(entries), &entry_count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(entry_count == 1);

	entries[0].dirtied = true;
	rc = vbdev_vslm_mem_unpin_range_by_bdev(&vslm.vbdev, entries, entry_count, false);
	CU_ASSERT(rc == 0);

	page = vslm_lookup_page(&vslm, 0);
	CU_ASSERT_PTR_NOT_NULL_FATAL(page);
	CU_ASSERT(page->pin_count == 0);
	CU_ASSERT(page->dirty == true);

	lpage = vslm_lookup_lpage(&vslm, 0);
	CU_ASSERT_PTR_NOT_NULL_FATAL(lpage);
	CU_ASSERT(lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT);

	TAILQ_REMOVE(&g_vslm_bdevs, &vslm, link);
	cleanup_vslm(&vslm);
}

static void
test_vslm_bdev_lease_callbacks(void)
{
	struct vbdev_vslm vslm = {};
	struct spdk_bdev fake_bdev = {};
	int rc;

	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.lease_lock, NULL) == 0);
	TAILQ_INIT(&vslm.leases);
	TAILQ_INIT(&vslm.blocked_cmds);
	vslm.virtual_size_bytes = VSLM_PAGE_SIZE * 16;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	vslm.vbdev.nsid = 402;
	TAILQ_INSERT_TAIL(&g_vslm_bdevs, &vslm, link);

	rc = vbdev_vslm_mem_lease_acquire_by_bdev(10, &vslm.vbdev, 0, VSLM_PAGE_SIZE);
	CU_ASSERT(rc == 0);

	rc = vbdev_vslm_mem_lease_acquire_by_bdev(10, &vslm.vbdev, 0, VSLM_PAGE_SIZE);
	CU_ASSERT(rc == 0);

	rc = vbdev_vslm_mem_lease_acquire_by_bdev(11, &vslm.vbdev, VSLM_PAGE_SIZE / 2, VSLM_PAGE_SIZE);
	CU_ASSERT(rc == -EAGAIN);

	rc = vbdev_vslm_mem_lease_acquire_by_bdev(12, &fake_bdev, 0, VSLM_PAGE_SIZE);
	CU_ASSERT(rc == -ENOTSUP);

	rc = vbdev_vslm_mem_lease_release(10);
	CU_ASSERT(rc == 0);

	rc = vbdev_vslm_mem_lease_release(10);
	CU_ASSERT(rc == -ENOENT);

	TAILQ_REMOVE(&g_vslm_bdevs, &vslm, link);
	pthread_mutex_destroy(&vslm.lease_lock);
	pthread_mutex_destroy(&vslm.policy_lock);
}

static void
test_vslm_host_read_during_lease_uses_committed_view(void)
{
	struct vbdev_vslm vslm = {};
	uint8_t read_buf[64];
	int rc;

	g_base_bdev.name = "base_lease_read";
	g_base_bdev.blocklen = 512;
	g_base_bdev.blockcnt = 8192;
	g_read_pattern = 0x6D;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.lease_lock, NULL) == 0);
	TAILQ_INIT(&vslm.leases);
	TAILQ_INIT(&vslm.blocked_cmds);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = g_base_desc;
	vslm.virtual_size_bytes = g_base_bdev.blockcnt * g_base_bdev.blocklen;
	vslm.vbdev.module = &vslm_if;
	vslm.vbdev.ctxt = &vslm;
	vslm.vbdev.nsid = 405;
	vslm.vbdev.blocklen = g_base_bdev.blocklen;
	vslm.vbdev.blockcnt = vslm.virtual_size_bytes / vslm.vbdev.blocklen;
	TAILQ_INSERT_TAIL(&g_vslm_bdevs, &vslm, link);

	rc = vbdev_vslm_mem_lease_acquire_by_bdev(50, &vslm.vbdev, 0, VSLM_PAGE_SIZE);
	CU_ASSERT(rc == 0);

	reset_io_state();
	memset(read_buf, 0x00, sizeof(read_buf));
	rc = vbdev_vslm_mem_read_by_bdev(&vslm.vbdev, 0, sizeof(read_buf), read_buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_last_read_bdev == &g_base_bdev);
	CU_ASSERT(read_buf[0] == g_read_pattern);
	CU_ASSERT(__atomic_load_n(&vslm.stats.vslm_host_read_during_execution_total,
				  __ATOMIC_RELAXED) == 1);

	rc = vbdev_vslm_mem_lease_release(50);
	CU_ASSERT(rc == 0);

	TAILQ_REMOVE(&g_vslm_bdevs, &vslm, link);
	cleanup_vslm(&vslm);
	pthread_mutex_destroy(&vslm.lease_lock);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();
	suite = CU_add_suite("vslm", NULL, NULL);
	CU_ADD_TEST(suite, test_vslm_hash_lookup);
	CU_ADD_TEST(suite, test_vslm_reshard_idle);
	CU_ADD_TEST(suite, test_vslm_copy_range_read_miss);
	CU_ADD_TEST(suite, test_vslm_copy_range_write_evict);
	CU_ADD_TEST(suite, test_vslm_readahead_exec_read_prefetches_next);
	CU_ADD_TEST(suite, test_vslm_async_exec_read_cold_default_backing);
	CU_ADD_TEST(suite, test_vslm_async_exec_read_loading_waiter_fanout);
	CU_ADD_TEST(suite, test_vslm_async_exec_write_full_page_cold_default_backing);
	CU_ADD_TEST(suite, test_vslm_async_exec_write_partial_page_faults_without_sync_io);
	CU_ADD_TEST(suite, test_vslm_async_exec_read_clean_eviction_no_sync_io);
	CU_ADD_TEST(suite, test_vslm_async_exec_read_dirty_eviction_no_sync_io);
	CU_ADD_TEST(suite, test_vslm_async_exec_read_spilled_private_refault_no_sync_io);
	CU_ADD_TEST(suite, test_vslm_readahead_disabled_no_prefetch);
	CU_ADD_TEST(suite, test_vslm_readahead_does_not_increment_demand_fault_stats);
	CU_ADD_TEST(suite, test_vslm_readahead_skips_when_no_free_frames);
	CU_ADD_TEST(suite, test_vslm_policy_validation_readahead_bounds);
	CU_ADD_TEST(suite, test_vslm_alias_fault_in_prefers_source);
	CU_ADD_TEST(suite, test_vslm_async_exec_read_alias_uses_async_source);
	CU_ADD_TEST(suite, test_vslm_alias_cow_authority_transition);
	CU_ADD_TEST(suite, test_vslm_copy_by_bdev_conflict_classification);
	CU_ADD_TEST(suite, test_vslm_bdev_callbacks);
	CU_ADD_TEST(suite, test_vslm_bdev_pin_unpin_range);
	CU_ADD_TEST(suite, test_vslm_get_stats_exports_perf_counters);
	CU_ADD_TEST(suite, test_vslm_bdev_unpin_dirtied_entry_marks_page_dirty);
	CU_ADD_TEST(suite, test_vslm_bdev_lease_callbacks);
	CU_ADD_TEST(suite, test_vslm_host_read_during_lease_uses_committed_view);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures == 0 ? 0 : 1;
}

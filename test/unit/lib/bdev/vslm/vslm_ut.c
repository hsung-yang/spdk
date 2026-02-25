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
static struct spdk_bdev_desc *g_base_desc;
static uint8_t g_read_pattern;
static bool g_write_called;
static uint64_t g_last_write_offset;
static uint64_t g_last_write_blocks;
static void *g_last_write_buf;
static uint64_t g_ticks;
static struct spdk_thread *g_ut_thread = (struct spdk_thread *)0x1;
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
}

DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *bdev), 0);
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
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
DEFINE_STUB_V(spdk_io_device_unregister, (void *io_device, spdk_io_device_unregister_cb unregister_cb));
DEFINE_STUB(spdk_get_io_channel, struct spdk_io_channel *, (void *io_device), (struct spdk_io_channel *)0x1);
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

	g_base_desc = (struct spdk_bdev_desc *)0x1;
	*desc = g_base_desc;
	return 0;
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	(void)desc;
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
	(void)desc;
	(void)ch;
	(void)offset_blocks;
	(void)num_blocks;

	memset(buf, g_read_pattern, num_blocks * g_base_bdev.blocklen);
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

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	(void)bdev_io;
	(void)status;
}

static void
cleanup_vslm(struct vbdev_vslm *vslm)
{
	pthread_mutex_destroy(&vslm->policy_lock);
	pthread_mutex_destroy(&vslm->mmu_lock);
	spdk_dma_free(vslm->sram_buffer);
	free(vslm->page_array);
	free(vslm->hash_table);
}

static void
test_vslm_hash_lookup(void)
{
	struct vbdev_vslm vslm = {};
	struct vslm_page *page = NULL;
	int rc;

	vslm.sram_size_bytes = VSLM_PAGE_SIZE * 4;
	CU_ASSERT(pthread_mutex_init(&vslm.policy_lock, NULL) == 0);
	CU_ASSERT(pthread_mutex_init(&vslm.mmu_lock, NULL) == 0);
	rc = vslm_init_mmu(&vslm);
	CU_ASSERT(rc == 0);

	page = vslm_get_free_page(&vslm);
	CU_ASSERT(page != NULL);
	page->vpn = 5;
	vslm_hash_insert(&vslm, page);
	vslm_lru_touch(&vslm, page);

	CU_ASSERT(vslm_lookup_page(&vslm, 5) == page);

	vslm_hash_remove(&vslm, page);
	CU_ASSERT(vslm_lookup_page(&vslm, 5) == NULL);

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
	CU_ASSERT(pthread_mutex_init(&vslm.mmu_lock, NULL) == 0);
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
	CU_ASSERT(pthread_mutex_init(&vslm.mmu_lock, NULL) == 0);
	CU_ASSERT(vslm_init_mmu(&vslm) == 0);
	vslm.base_bdev = &g_base_bdev;
	vslm.base_desc = g_base_desc;
	vslm.virtual_size_bytes = g_base_bdev.blockcnt * g_base_bdev.blocklen;

	page = vslm_get_free_page(&vslm);
	CU_ASSERT(page != NULL);
	page->vpn = 0;
	page->dirty = true;
	page->is_busy = false;
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
	CU_ASSERT(pthread_mutex_init(&vslm.mmu_lock, NULL) == 0);
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
	CU_ASSERT(rc == 0);
	CU_ASSERT_PTR_NOT_NULL(ptr);
	CU_ASSERT(memcmp(ptr, write_buf, sizeof(write_buf)) == 0);

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

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();
	suite = CU_add_suite("vslm", NULL, NULL);
	CU_ADD_TEST(suite, test_vslm_hash_lookup);
	CU_ADD_TEST(suite, test_vslm_copy_range_read_miss);
	CU_ADD_TEST(suite, test_vslm_copy_range_write_evict);
	CU_ADD_TEST(suite, test_vslm_bdev_callbacks);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures == 0 ? 0 : 1;
}

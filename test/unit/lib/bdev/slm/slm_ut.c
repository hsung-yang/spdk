/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

#include "spdk_internal/cunit.h"
#include "spdk_internal/mock.h"

#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

/* Include the actual implementation */
#include "bdev/slm/vbdev_slm.c"
#include "bdev/slm/vbdev_pslm.c"

/* Mock implementations */
DEFINE_STUB_V(spdk_bdev_module_examine_done, (struct spdk_bdev_module *module));
DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *bdev), 0);
DEFINE_STUB_V(spdk_bdev_io_complete, (struct spdk_bdev_io *bdev_io,
		enum spdk_bdev_io_status status));
DEFINE_STUB_V(spdk_bdev_io_complete_nvme_status, (struct spdk_bdev_io *bdev_io,
		uint32_t cdw0, int sct, int sc));
DEFINE_STUB_V(spdk_bdev_io_get_buf, (struct spdk_bdev_io *bdev_io,
		spdk_bdev_io_get_buf_cb cb, uint64_t len));
DEFINE_STUB_V(spdk_copy_buf_to_iovs, (struct iovec *iovs, int iovcnt,
		void *buf, size_t buf_len));
DEFINE_STUB_V(spdk_copy_iovs_to_buf, (void *buf, size_t buf_len,
		struct iovec *iovs, int iovcnt));
DEFINE_STUB(spdk_bdev_first, struct spdk_bdev *, (void), NULL);
DEFINE_STUB(spdk_bdev_next, struct spdk_bdev *, (struct spdk_bdev *prev), NULL);
DEFINE_STUB(spdk_bdev_get_nvme_nsid, uint32_t, (struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_is_slm, bool, (const struct spdk_bdev *bdev), false);
DEFINE_STUB(spdk_bdev_get_block_size, uint32_t, (const struct spdk_bdev *bdev), 0);

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	struct spdk_bdev_slm *slm;

	if (!bdev_name) {
		return NULL;
	}

	TAILQ_FOREACH(slm, &g_slm_bdevs, link) {
		if (strcmp(bdev_name, slm->bdev.name) == 0) {
			return &slm->bdev;
		}
	}

	return NULL;
}

/* Custom mock for spdk_bdev_unregister that calls destruct */
void
spdk_bdev_unregister(struct spdk_bdev *bdev,
		     spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	if (bdev && bdev->fn_table && bdev->fn_table->destruct) {
		bdev->fn_table->destruct(bdev->ctxt);
	}
	if (cb_fn) {
		cb_fn(cb_arg, 0);
	}
}

static void
test_slm_create_delete(void)
{
	int rc;
	const char *name = "test_slm";
	uint32_t nsid = 100;
	uint64_t size_mb = 64;
	uint32_t granularity = 4;

	/* Test creation */
	rc = bdev_slm_create(name, nsid, size_mb, granularity);
	CU_ASSERT(rc == 0);

	/* Verify bdev exists */
	struct spdk_bdev *bdev = spdk_bdev_get_by_name(name);
	CU_ASSERT(bdev != NULL);
	if (bdev) {
		CU_ASSERT(bdev->blocklen == granularity);
		CU_ASSERT(bdev->blockcnt == (size_mb * 1024 * 1024 / granularity));
		CU_ASSERT(bdev->nsid == nsid);
		CU_ASSERT(bdev->slm == true);
	}

	/* Test deletion */
	bdev_slm_delete(name, NULL, NULL);
}

static void
test_slm_invalid_params(void)
{
	int rc;

	/* Test NULL name */
	rc = bdev_slm_create(NULL, 100, 64, 4);
	CU_ASSERT(rc == -EINVAL);

	/* Test zero size */
	rc = bdev_slm_create("test", 100, 0, 4);
	CU_ASSERT(rc == -EINVAL);

	/* Test zero granularity */
	rc = bdev_slm_create("test", 100, 64, 0);
	CU_ASSERT(rc == -EINVAL);
}

static void
test_slm_buffer_access(void)
{
	int rc;
	void *ptr = NULL;
	const char *name = "test_slm_buf";
	uint32_t nsid = 200;
	uint64_t size_mb = 16;
	uint32_t granularity = 4;

	/* Create SLM bdev */
	rc = bdev_slm_create(name, nsid, size_mb, granularity);
	CU_ASSERT(rc == 0);

	/* Test valid buffer access */
	rc = bdev_slm_get_buffer_ptr(name, 0, 1024, &ptr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ptr != NULL);

	/* Test out of bounds access */
	rc = bdev_slm_get_buffer_ptr(name, 0, 17 * 1024 * 1024, &ptr);
	CU_ASSERT(rc == -EINVAL);

	/* Test non-existent bdev */
	rc = bdev_slm_get_buffer_ptr("nonexistent", 0, 1024, &ptr);
	CU_ASSERT(rc == -ENOENT);

	/* Test NULL pointer */
	rc = bdev_slm_get_buffer_ptr(name, 0, 1024, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Cleanup */
	bdev_slm_delete(name, NULL, NULL);
}

static void
test_slm_multiple_instances(void)
{
	int rc;
	const char *name1 = "slm1";
	const char *name2 = "slm2";
	const char *name3 = "slm3";

	/* Create multiple SLM bdevs */
	rc = bdev_slm_create(name1, 101, 32, 4);
	CU_ASSERT(rc == 0);

	rc = bdev_slm_create(name2, 102, 64, 4);
	CU_ASSERT(rc == 0);

	rc = bdev_slm_create(name3, 103, 128, 4);
	CU_ASSERT(rc == 0);

	/* Verify all exist */
	CU_ASSERT(spdk_bdev_get_by_name(name1) != NULL);
	CU_ASSERT(spdk_bdev_get_by_name(name2) != NULL);
	CU_ASSERT(spdk_bdev_get_by_name(name3) != NULL);

	/* Delete all */
	bdev_slm_delete(name1, NULL, NULL);
	bdev_slm_delete(name2, NULL, NULL);
	bdev_slm_delete(name3, NULL, NULL);
}

static void
test_slm_bdev_access(void)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev fake_bdev = {};
	void *ptr = (void *)0x1;
	uint8_t write_buf[16];
	uint8_t read_buf[16];
	int rc;

	rc = bdev_slm_create("test_slm_bdev", 301, 8, 4);
	CU_ASSERT(rc == 0);

	bdev = spdk_bdev_get_by_name("test_slm_bdev");
	CU_ASSERT_PTR_NOT_NULL_FATAL(bdev);

	memset(write_buf, 0xA5, sizeof(write_buf));
	memset(read_buf, 0x00, sizeof(read_buf));

	rc = bdev_slm_write_by_bdev(bdev, 128, sizeof(write_buf), write_buf);
	CU_ASSERT(rc == 0);

	rc = bdev_slm_read_by_bdev(bdev, 128, sizeof(read_buf), read_buf);
	CU_ASSERT(rc == 0);
	CU_ASSERT(memcmp(read_buf, write_buf, sizeof(write_buf)) == 0);

	rc = bdev_slm_get_buffer_ptr_by_bdev(bdev, 128, sizeof(write_buf), &ptr);
	CU_ASSERT(rc == 0);
	CU_ASSERT_PTR_NOT_NULL(ptr);
	CU_ASSERT(memcmp(ptr, write_buf, sizeof(write_buf)) == 0);

	rc = bdev_slm_read_by_bdev(bdev, 0, 0, NULL);
	CU_ASSERT(rc == 0);

	ptr = (void *)0x1;
	rc = bdev_slm_get_buffer_ptr_by_bdev(bdev, 0, 0, &ptr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ptr == NULL);

	rc = bdev_slm_read_by_bdev(NULL, 0, 1, read_buf);
	CU_ASSERT(rc == -EINVAL);

	rc = bdev_slm_write_by_bdev(NULL, 0, 1, write_buf);
	CU_ASSERT(rc == -EINVAL);

	rc = bdev_slm_get_buffer_ptr_by_bdev(NULL, 0, 1, &ptr);
	CU_ASSERT(rc == -EINVAL);

	rc = bdev_slm_read_by_bdev(&fake_bdev, 0, 1, read_buf);
	CU_ASSERT(rc == -ENOTSUP);

	rc = bdev_slm_write_by_bdev(&fake_bdev, 0, 1, write_buf);
	CU_ASSERT(rc == -ENOTSUP);

	rc = bdev_slm_get_buffer_ptr_by_bdev(&fake_bdev, 0, 1, &ptr);
	CU_ASSERT(rc == -ENOTSUP);

	bdev_slm_delete("test_slm_bdev", NULL, NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;
	int rc;

	CU_initialize_registry();

	suite = CU_add_suite("slm", NULL, NULL);
	CU_ADD_TEST(suite, test_slm_create_delete);
	CU_ADD_TEST(suite, test_slm_invalid_params);
	CU_ADD_TEST(suite, test_slm_buffer_access);
	CU_ADD_TEST(suite, test_slm_multiple_instances);
	CU_ADD_TEST(suite, test_slm_bdev_access);

	allocate_threads(1);
	set_thread(0);

	rc = vbdev_pslm_initialize();
	if (rc != 0) {
		CU_cleanup_registry();
		free_threads();
		return 1;
	}

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	vbdev_pslm_finish();
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}

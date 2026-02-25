/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 CPCS Implementation Team.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/bdev_vslm.h"

#include "module/bdev/malloc/bdev_malloc.h"

static struct spdk_thread *g_thread;
static bool g_bdev_initialized;
static bool g_iobuf_initialized;
static bool g_bdev_init_done;
static int g_bdev_init_rc;
static bool g_bdev_fini_done;
static bool g_iobuf_fini_done;
static bool g_unreg_done;
static int g_unreg_rc;
static bool g_malloc_del_done;
static int g_malloc_del_rc;

static void
poll_threads(void)
{
	while (spdk_thread_poll(g_thread, 0, 0) > 0) {
	}
}

static void
bdev_init_cb(void *cb_arg, int rc)
{
	(void)cb_arg;
	g_bdev_init_rc = rc;
	g_bdev_init_done = true;
}

static void
bdev_fini_cb(void *cb_arg)
{
	(void)cb_arg;
	g_bdev_fini_done = true;
}

static void
iobuf_fini_cb(void *cb_arg)
{
	(void)cb_arg;
	g_iobuf_fini_done = true;
}

static void
bdev_unregister_cb(void *cb_arg, int rc)
{
	(void)cb_arg;
	g_unreg_rc = rc;
	g_unreg_done = true;
}

static void
malloc_delete_cb(void *cb_arg, int rc)
{
	(void)cb_arg;
	g_malloc_del_rc = rc;
	g_malloc_del_done = true;
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct malloc_bdev_opts malloc_opts = {};
	struct spdk_bdev *base_bdev = NULL;
	struct spdk_bdev *vslm_bdev = NULL;
	int rc = 0;

	(void)argc;
	(void)argv;

	spdk_log_open(NULL);
	spdk_log_set_level(SPDK_LOG_WARN);
	spdk_log_set_print_level(SPDK_LOG_WARN);

	spdk_env_opts_init(&opts);
	opts.name = "vslm_smoke";
	opts.core_mask = "[0]";
	rc = spdk_env_init(&opts);
	if (rc < 0) {
		fprintf(stderr, "spdk_env_init failed: %d\n", rc);
		spdk_log_close();
		return 1;
	}

	rc = spdk_thread_lib_init(NULL, 0);
	if (rc < 0) {
		fprintf(stderr, "spdk_thread_lib_init failed: %d\n", rc);
		spdk_env_fini();
		spdk_log_close();
		return 1;
	}

	g_thread = spdk_thread_create("vslm_smoke", NULL);
	if (!g_thread) {
		fprintf(stderr, "Failed to create SPDK thread\n");
		rc = -ENOMEM;
		goto cleanup;
	}
	spdk_set_thread(g_thread);

	rc = spdk_iobuf_initialize();
	if (rc < 0) {
		fprintf(stderr, "spdk_iobuf_initialize failed: %d\n", rc);
		goto cleanup;
	}
	g_iobuf_initialized = true;

	g_bdev_init_done = false;
	g_bdev_init_rc = 0;
	spdk_bdev_initialize(bdev_init_cb, NULL);
	while (!g_bdev_init_done) {
		poll_threads();
	}
	if (g_bdev_init_rc != 0) {
		fprintf(stderr, "spdk_bdev_initialize failed: %d\n", g_bdev_init_rc);
		rc = g_bdev_init_rc;
		goto cleanup;
	}
	g_bdev_initialized = true;

	malloc_opts.name = "Malloc0";
	malloc_opts.num_blocks = 1024;
	malloc_opts.block_size = 512;
	malloc_opts.physical_block_size = 512;
	malloc_opts.md_size = 0;
	malloc_opts.md_interleave = false;
	malloc_opts.dif_type = SPDK_DIF_DISABLE;
	malloc_opts.dif_is_head_of_md = false;
	malloc_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = create_malloc_disk(&base_bdev, &malloc_opts);
	if (rc != 0) {
		fprintf(stderr, "create_malloc_disk failed: %d\n", rc);
		goto cleanup;
	}

	rc = bdev_vslm_create("vslm0", "Malloc0", 8 * 1024);
	if (rc != 0) {
		fprintf(stderr, "bdev_vslm_create failed: %d\n", rc);
		goto cleanup;
	}

	vslm_bdev = spdk_bdev_get_by_name("vslm0");
	if (!vslm_bdev) {
		fprintf(stderr, "vSLM bdev not found after creation\n");
		rc = -ENOENT;
		goto cleanup;
	}

cleanup:
	if (vslm_bdev) {
		g_unreg_done = false;
		g_unreg_rc = 0;
		spdk_bdev_unregister(vslm_bdev, bdev_unregister_cb, NULL);
		while (!g_unreg_done) {
			poll_threads();
		}
		if (g_unreg_rc != 0) {
			rc = g_unreg_rc;
		}
	}

	if (base_bdev) {
		g_malloc_del_done = false;
		g_malloc_del_rc = 0;
		delete_malloc_disk("Malloc0", malloc_delete_cb, NULL);
		while (!g_malloc_del_done) {
			poll_threads();
		}
		if (g_malloc_del_rc != 0) {
			rc = g_malloc_del_rc;
		}
	}

	if (g_bdev_initialized) {
		g_bdev_fini_done = false;
		spdk_bdev_finish(bdev_fini_cb, NULL);
		while (!g_bdev_fini_done) {
			poll_threads();
		}
		g_bdev_initialized = false;
	}

	if (g_iobuf_initialized) {
		g_iobuf_fini_done = false;
		spdk_iobuf_finish(iobuf_fini_cb, NULL);
		while (!g_iobuf_fini_done) {
			poll_threads();
		}
		g_iobuf_initialized = false;
	}

	if (g_thread) {
		spdk_thread_exit(g_thread);
		while (!spdk_thread_is_exited(g_thread)) {
			spdk_thread_poll(g_thread, 0, 0);
		}
		spdk_thread_destroy(g_thread);
		spdk_set_thread(NULL);
		g_thread = NULL;
	}

	spdk_thread_lib_fini();
	spdk_env_fini();
	spdk_log_close();

	return rc == 0 ? 0 : 1;
}

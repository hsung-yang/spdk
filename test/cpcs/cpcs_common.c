/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

#include "cpcs_common.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"

static bool g_env_initialized;
static int g_env_refcount;
static bool g_thread_initialized;
static bool g_bdev_initialized;
static struct spdk_thread *g_app_thread;
static bool g_log_initialized;

static bool g_bdev_init_done;
static int g_bdev_init_rc;
static bool g_bdev_fini_done;
static bool g_iobuf_fini_done;

static void
cpcs_test_poll(void)
{
	while (spdk_thread_poll(g_app_thread, 0, 0) > 0) {
	}
}

static void
cpcs_bdev_init_cb(void *arg, int rc)
{
	(void)arg;
	g_bdev_init_rc = rc;
	g_bdev_init_done = true;
}

static void
cpcs_bdev_fini_cb(void *arg)
{
	(void)arg;
	g_bdev_fini_done = true;
}

static void
cpcs_iobuf_fini_cb(void *arg)
{
	(void)arg;
	g_iobuf_fini_done = true;
}

int
cpcs_test_init(struct cpcs_test_ctx *ctx)
{
	struct spdk_env_opts opts;
	int rc;

	if (!ctx) {
		return -EINVAL;
	}

	if (!g_log_initialized) {
		spdk_log_open(NULL);
		spdk_log_set_level(SPDK_LOG_NOTICE);
		spdk_log_set_print_level(SPDK_LOG_NOTICE);
		g_log_initialized = true;
	}

	if (!g_env_initialized) {
		opts.opts_size = sizeof(opts);
		spdk_env_opts_init(&opts);
		opts.name = "cpcs_test";
		/* Use -l core list to avoid deprecated -c coremask. */
		opts.core_mask = "[0]";
		rc = spdk_env_init(&opts);
		if (rc < 0) {
			SPDK_ERRLOG("Unable to initialize SPDK env\n");
			return rc;
		}
		g_env_initialized = true;
	}

	if (!g_thread_initialized) {
		spdk_thread_lib_init(NULL, 0);
		g_app_thread = spdk_thread_create("cpcs_test", NULL);
		if (!g_app_thread) {
			SPDK_ERRLOG("Unable to create SPDK app thread\n");
			return -ENOMEM;
		}
		spdk_set_thread(g_app_thread);
		g_thread_initialized = true;
	}

	if (!g_bdev_initialized) {
		rc = spdk_iobuf_initialize();
		if (rc < 0) {
			SPDK_ERRLOG("Unable to initialize iobuf\n");
			return rc;
		}

		g_bdev_init_done = false;
		g_bdev_init_rc = 0;
		spdk_bdev_initialize(cpcs_bdev_init_cb, NULL);
		while (!g_bdev_init_done) {
			cpcs_test_poll();
		}
		if (g_bdev_init_rc != 0) {
			SPDK_ERRLOG("Unable to initialize bdev subsystem\n");
			return g_bdev_init_rc;
		}
		g_bdev_initialized = true;
	}
	g_env_refcount++;

	memset(ctx, 0, sizeof(*ctx));
	ctx->is_initialized = true;

	SPDK_NOTICELOG("CPCS test context initialized\n");
	return 0;
}

void
cpcs_test_cleanup(struct cpcs_test_ctx *ctx)
{
	if (!ctx) {
		return;
	}

	ctx->is_initialized = false;
	SPDK_NOTICELOG("CPCS test context cleaned up (errors: %d)\n", ctx->error_count);

	if (g_env_initialized && g_env_refcount > 0) {
		g_env_refcount--;
		if (g_env_refcount == 0) {
			if (g_bdev_initialized) {
				g_bdev_fini_done = false;
				spdk_bdev_finish(cpcs_bdev_fini_cb, NULL);
				while (!g_bdev_fini_done) {
					cpcs_test_poll();
				}

				g_iobuf_fini_done = false;
				spdk_iobuf_finish(cpcs_iobuf_fini_cb, NULL);
				while (!g_iobuf_fini_done) {
					cpcs_test_poll();
				}

				g_bdev_initialized = false;
			}

			if (g_thread_initialized) {
				spdk_thread_exit(g_app_thread);
				while (!spdk_thread_is_exited(g_app_thread)) {
					spdk_thread_poll(g_app_thread, 0, 0);
				}
				spdk_thread_destroy(g_app_thread);
				spdk_set_thread(NULL);
				spdk_thread_lib_fini();
				g_app_thread = NULL;
				g_thread_initialized = false;
			}

			spdk_env_fini();
			g_env_initialized = false;

			spdk_log_close();
			g_log_initialized = false;
		}
	}
}

int
cpcs_test_create_slm(struct cpcs_test_ctx *ctx, const char *name,
		     uint32_t nsid, uint64_t size_mb)
{
	int rc;

	if (!ctx || !name) {
		return -EINVAL;
	}

	rc = bdev_slm_create(name, nsid, size_mb, 4);
	if (rc) {
		SPDK_ERRLOG("Failed to create SLM bdev '%s': %d\n", name, rc);
		ctx->error_count++;
		return rc;
	}
	cpcs_test_poll();

	SPDK_NOTICELOG("Created test SLM bdev '%s': %lu MiB, NSID=%u\n",
		       name, size_mb, nsid);
	return 0;
}

void
cpcs_test_delete_slm(struct cpcs_test_ctx *ctx, const char *name)
{
	if (!ctx || !name) {
		return;
	}

	bdev_slm_delete(name, NULL, NULL);
	cpcs_test_poll();
	SPDK_NOTICELOG("Deleted test SLM bdev '%s'\n", name);
}

int
cpcs_test_verify_buffer(struct cpcs_test_ctx *ctx, const char *name,
			uint64_t offset, const void *expected_data, uint64_t len)
{
	void *buf;
	int rc;

	if (!ctx || !name || !expected_data) {
		return -EINVAL;
	}

	rc = bdev_slm_get_buffer_ptr(name, offset, len, &buf);
	if (rc) {
		SPDK_ERRLOG("Failed to get buffer pointer: %d\n", rc);
		ctx->error_count++;
		return rc;
	}

	if (memcmp(buf, expected_data, len) != 0) {
		SPDK_ERRLOG("Buffer verification failed\n");
		ctx->error_count++;
		return -EINVAL;
	}

	return 0;
}

int
cpcs_test_write_pattern(struct cpcs_test_ctx *ctx, const char *name,
			uint64_t offset, uint64_t len, uint8_t pattern)
{
	void *buf;
	int rc;

	if (!ctx || !name) {
		return -EINVAL;
	}

	rc = bdev_slm_get_buffer_ptr(name, offset, len, &buf);
	if (rc) {
		SPDK_ERRLOG("Failed to get buffer pointer: %d\n", rc);
		ctx->error_count++;
		return rc;
	}

	memset(buf, pattern, len);
	return 0;
}

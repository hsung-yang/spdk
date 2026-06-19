/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 CPCS Implementation Team. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev_slm.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk_internal/vbdev_slm.h"

#define VBDEV_SLM_COPY_FALLBACK_CHUNK_SIZE (1U << 20)

struct vbdev_slm_provider {
	const struct spdk_vbdev_slm_ops *ops;
	TAILQ_ENTRY(vbdev_slm_provider)		link;
};

static TAILQ_HEAD(, vbdev_slm_provider) g_vbdev_slm_providers =
	TAILQ_HEAD_INITIALIZER(g_vbdev_slm_providers);
static pthread_rwlock_t g_vbdev_slm_lock = PTHREAD_RWLOCK_INITIALIZER;
static bool g_sync_block_warned;

struct vbdev_slm_sync_wait_ctx {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool done;
	int status;
};

struct vbdev_slm_copy_async_ctx {
	struct spdk_bdev *dst_bdev;
	struct spdk_bdev *src_bdev;
	uint64_t dst_offset;
	uint64_t src_offset;
	uint64_t length;
	uint64_t processed;
	uint8_t *tmp;
	spdk_bdev_slm_io_completion_cb cb_fn;
	void *cb_arg;
};

static bool
vbdev_slm_sync_is_disallowed(const char *op_name)
{
	if (spdk_get_thread() == NULL) {
		return false;
	}

	if (!g_sync_block_warned) {
		SPDK_WARNLOG("SLM sync API '%s' is disallowed on SPDK thread; use async API\n",
			     op_name);
		g_sync_block_warned = true;
	}

	return true;
}

static void
vbdev_slm_sync_wait_done(void *cb_arg, int status)
{
	struct vbdev_slm_sync_wait_ctx *ctx = cb_arg;

	pthread_mutex_lock(&ctx->mutex);
	ctx->status = status;
	ctx->done = true;
	pthread_cond_signal(&ctx->cond);
	pthread_mutex_unlock(&ctx->mutex);
}

static int
vbdev_slm_sync_wait_run(int submit_rc, struct vbdev_slm_sync_wait_ctx *ctx)
{
	int rc;

	if (submit_rc != 0) {
		pthread_cond_destroy(&ctx->cond);
		pthread_mutex_destroy(&ctx->mutex);
		return submit_rc;
	}

	pthread_mutex_lock(&ctx->mutex);
	while (!ctx->done) {
		pthread_cond_wait(&ctx->cond, &ctx->mutex);
	}
	rc = ctx->status;
	pthread_mutex_unlock(&ctx->mutex);

	pthread_cond_destroy(&ctx->cond);
	pthread_mutex_destroy(&ctx->mutex);
	return rc;
}

static struct vbdev_slm_provider *
vbdev_slm_find_provider_locked(const struct spdk_vbdev_slm_ops *ops)
{
	struct vbdev_slm_provider *provider;

	TAILQ_FOREACH(provider, &g_vbdev_slm_providers, link) {
		if (provider->ops == ops) {
			return provider;
		}
	}

	return NULL;
}

static const struct spdk_vbdev_slm_ops *
vbdev_slm_lookup_ops_by_bdev(struct spdk_bdev *bdev)
{
	struct vbdev_slm_provider *provider;
	const struct spdk_vbdev_slm_ops *ops = NULL;

	pthread_rwlock_rdlock(&g_vbdev_slm_lock);
	TAILQ_FOREACH(provider, &g_vbdev_slm_providers, link) {
		if (provider->ops->owns_bdev != NULL && provider->ops->owns_bdev(bdev)) {
			ops = provider->ops;
			break;
		}
	}
	pthread_rwlock_unlock(&g_vbdev_slm_lock);

	return ops;
}

int
vbdev_slm_register_ops(const struct spdk_vbdev_slm_ops *ops)
{
	struct vbdev_slm_provider *provider;
	struct vbdev_slm_provider *existing;

	if (ops == NULL || ops->name == NULL || ops->owns_bdev == NULL ||
	    ops->get_buffer_ptr_by_bdev == NULL ||
	    (ops->read_by_bdev == NULL && ops->read_by_bdev_async == NULL) ||
	    (ops->write_by_bdev == NULL && ops->write_by_bdev_async == NULL)) {
		return -EINVAL;
	}

	provider = calloc(1, sizeof(*provider));
	if (provider == NULL) {
		return -ENOMEM;
	}

	provider->ops = ops;

	pthread_rwlock_wrlock(&g_vbdev_slm_lock);
	existing = vbdev_slm_find_provider_locked(ops);
	if (existing != NULL) {
		pthread_rwlock_unlock(&g_vbdev_slm_lock);
		free(provider);
		return 0;
	}

	TAILQ_INSERT_TAIL(&g_vbdev_slm_providers, provider, link);
	pthread_rwlock_unlock(&g_vbdev_slm_lock);

	return 0;
}

void
vbdev_slm_unregister_ops(const struct spdk_vbdev_slm_ops *ops)
{
	struct vbdev_slm_provider *provider, *tmp_provider;

	if (ops == NULL) {
		return;
	}

	pthread_rwlock_wrlock(&g_vbdev_slm_lock);
	TAILQ_FOREACH_SAFE(provider, &g_vbdev_slm_providers, link, tmp_provider) {
		if (provider->ops == ops) {
			TAILQ_REMOVE(&g_vbdev_slm_providers, provider, link);
			free(provider);
			break;
		}
	}
	pthread_rwlock_unlock(&g_vbdev_slm_lock);
}

int
vbdev_slm_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				 uint64_t length, void **ptr)
{
	const struct spdk_vbdev_slm_ops *ops;

	if (bdev == NULL || ptr == NULL) {
		return -EINVAL;
	}

	if (length == 0) {
		*ptr = NULL;
		return 0;
	}

	ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (ops == NULL) {
		return -ENOTSUP;
	}

	if (ops->get_buffer_ptr_by_bdev == NULL) {
		return -ENOTSUP;
	}

	return ops->get_buffer_ptr_by_bdev(bdev, offset, length, ptr);
}

int
vbdev_slm_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			    uint64_t length, bool for_write,
			    struct spdk_bdev_slm_sg_entry *entries,
			    uint32_t max_entries, uint32_t *entry_count)
{
	const struct spdk_vbdev_slm_ops *ops;

	if (bdev == NULL || entries == NULL || entry_count == NULL || max_entries == 0) {
		return -EINVAL;
	}

	if (length == 0) {
		*entry_count = 0;
		return 0;
	}

	ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (ops == NULL || ops->pin_range_by_bdev == NULL) {
		return -ENOTSUP;
	}

	return ops->pin_range_by_bdev(bdev, offset, length, for_write,
				      entries, max_entries, entry_count);
}

int
vbdev_slm_try_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				uint64_t length, bool for_write,
				struct spdk_bdev_slm_sg_entry *entries,
				uint32_t max_entries, uint32_t *entry_count)
{
	const struct spdk_vbdev_slm_ops *ops;

	if (bdev == NULL || entries == NULL || entry_count == NULL || max_entries == 0) {
		return -EINVAL;
	}

	if (length == 0) {
		*entry_count = 0;
		return 0;
	}

	ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (ops == NULL || ops->try_pin_range_by_bdev == NULL) {
		return -ENOTSUP;
	}

	return ops->try_pin_range_by_bdev(bdev, offset, length, for_write,
					  entries, max_entries, entry_count);
}

int
vbdev_slm_unpin_range_by_bdev(struct spdk_bdev *bdev,
			      const struct spdk_bdev_slm_sg_entry *entries,
			      uint32_t entry_count, bool dirtied)
{
	const struct spdk_vbdev_slm_ops *ops;

	if (bdev == NULL || (entry_count != 0 && entries == NULL)) {
		return -EINVAL;
	}

	if (entry_count == 0) {
		return 0;
	}

	ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (ops == NULL || ops->unpin_range_by_bdev == NULL) {
		return -ENOTSUP;
	}

	return ops->unpin_range_by_bdev(bdev, entries, entry_count, dirtied);
}

int
vbdev_slm_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf,
			     spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	const struct spdk_vbdev_slm_ops *ops;
	int rc;

	if (cb_fn == NULL || bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return 0;
	}

	ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (ops == NULL) {
		return -ENOTSUP;
	}

	if (ops->read_by_bdev_async != NULL) {
		return ops->read_by_bdev_async(bdev, offset, length, buf, cb_fn, cb_arg);
	}

	if (spdk_get_thread() != NULL) {
		return -EWOULDBLOCK;
	}

	rc = ops->read_by_bdev(bdev, offset, length, buf);
	cb_fn(cb_arg, rc);
	return 0;
}

int
vbdev_slm_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	struct vbdev_slm_sync_wait_ctx ctx = {};
	int submit_rc;

	if (vbdev_slm_sync_is_disallowed("read_by_bdev")) {
		return -EWOULDBLOCK;
	}

	pthread_mutex_init(&ctx.mutex, NULL);
	pthread_cond_init(&ctx.cond, NULL);
	ctx.done = false;
	ctx.status = 0;

	submit_rc = vbdev_slm_read_by_bdev_async(bdev, offset, length, buf,
			vbdev_slm_sync_wait_done, &ctx);
	return vbdev_slm_sync_wait_run(submit_rc, &ctx);
}

int
vbdev_slm_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
			      const void *buf, spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	const struct spdk_vbdev_slm_ops *ops;
	int rc;

	if (cb_fn == NULL || bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return 0;
	}

	ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (ops == NULL) {
		return -ENOTSUP;
	}

	if (ops->write_by_bdev_async != NULL) {
		return ops->write_by_bdev_async(bdev, offset, length, buf, cb_fn, cb_arg);
	}

	if (spdk_get_thread() != NULL) {
		return -EWOULDBLOCK;
	}

	rc = ops->write_by_bdev(bdev, offset, length, buf);
	cb_fn(cb_arg, rc);
	return 0;
}

int
vbdev_slm_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, const void *buf)
{
	struct vbdev_slm_sync_wait_ctx ctx = {};
	int submit_rc;

	if (vbdev_slm_sync_is_disallowed("write_by_bdev")) {
		return -EWOULDBLOCK;
	}

	pthread_mutex_init(&ctx.mutex, NULL);
	pthread_cond_init(&ctx.cond, NULL);
	ctx.done = false;
	ctx.status = 0;

	submit_rc = vbdev_slm_write_by_bdev_async(bdev, offset, length, buf,
			vbdev_slm_sync_wait_done, &ctx);
	return vbdev_slm_sync_wait_run(submit_rc, &ctx);
}

static void vbdev_slm_copy_by_bdev_async_step(struct vbdev_slm_copy_async_ctx *ctx);

static void
vbdev_slm_copy_by_bdev_async_finish(struct vbdev_slm_copy_async_ctx *ctx, int status)
{
	spdk_bdev_slm_io_completion_cb cb_fn = ctx->cb_fn;
	void *cb_arg = ctx->cb_arg;

	spdk_free(ctx->tmp);
	free(ctx);
	cb_fn(cb_arg, status);
}

static void
vbdev_slm_copy_by_bdev_async_continue(void *arg)
{
	struct vbdev_slm_copy_async_ctx *ctx = arg;

	vbdev_slm_copy_by_bdev_async_step(ctx);
}

static void
vbdev_slm_copy_by_bdev_async_write_done(void *cb_arg, int status)
{
	struct vbdev_slm_copy_async_ctx *ctx = cb_arg;
	struct spdk_thread *thread;
	int rc;

	if (status != 0) {
		vbdev_slm_copy_by_bdev_async_finish(ctx, status);
		return;
	}

	ctx->processed += spdk_min(ctx->length - ctx->processed,
				   (uint64_t)VBDEV_SLM_COPY_FALLBACK_CHUNK_SIZE);
	thread = spdk_get_thread();
	if (thread != NULL) {
		rc = spdk_thread_send_msg(thread, vbdev_slm_copy_by_bdev_async_continue, ctx);
		if (rc != 0) {
			vbdev_slm_copy_by_bdev_async_finish(ctx, rc);
		}
	} else {
		vbdev_slm_copy_by_bdev_async_step(ctx);
	}
}

static void
vbdev_slm_copy_by_bdev_async_read_done(void *cb_arg, int status)
{
	struct vbdev_slm_copy_async_ctx *ctx = cb_arg;
	uint64_t chunk_len;
	int rc;

	if (status != 0) {
		vbdev_slm_copy_by_bdev_async_finish(ctx, status);
		return;
	}

	chunk_len = spdk_min(ctx->length - ctx->processed, (uint64_t)VBDEV_SLM_COPY_FALLBACK_CHUNK_SIZE);
	rc = vbdev_slm_write_by_bdev_async(ctx->dst_bdev, ctx->dst_offset + ctx->processed,
					   chunk_len, ctx->tmp,
					   vbdev_slm_copy_by_bdev_async_write_done, ctx);
	if (rc != 0) {
		vbdev_slm_copy_by_bdev_async_finish(ctx, rc);
	}
}

static void
vbdev_slm_copy_by_bdev_async_step(struct vbdev_slm_copy_async_ctx *ctx)
{
	uint64_t chunk_len;
	int rc;

	if (ctx->processed >= ctx->length) {
		vbdev_slm_copy_by_bdev_async_finish(ctx, 0);
		return;
	}

	chunk_len = spdk_min(ctx->length - ctx->processed, (uint64_t)VBDEV_SLM_COPY_FALLBACK_CHUNK_SIZE);
	rc = vbdev_slm_read_by_bdev_async(ctx->src_bdev, ctx->src_offset + ctx->processed,
					  chunk_len, ctx->tmp,
					  vbdev_slm_copy_by_bdev_async_read_done, ctx);
	if (rc != 0) {
		vbdev_slm_copy_by_bdev_async_finish(ctx, rc);
	}
}

int
vbdev_slm_copy_by_bdev_async(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
			     struct spdk_bdev *src_bdev, uint64_t src_offset,
			     uint64_t length, spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	const struct spdk_vbdev_slm_ops *dst_ops;
	const struct spdk_vbdev_slm_ops *src_ops;
	struct vbdev_slm_copy_async_ctx *ctx;
	int rc;

	if (cb_fn == NULL || dst_bdev == NULL || src_bdev == NULL) {
		return -EINVAL;
	}

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return 0;
	}

	if (dst_offset > UINT64_MAX - length || src_offset > UINT64_MAX - length) {
		return -EINVAL;
	}

	dst_ops = vbdev_slm_lookup_ops_by_bdev(dst_bdev);
	if (dst_ops == NULL) {
		return -ENOTSUP;
	}

	if (dst_ops->copy_by_bdev_async != NULL) {
		return dst_ops->copy_by_bdev_async(dst_bdev, dst_offset, src_bdev, src_offset,
						   length, cb_fn, cb_arg);
	}

	if (dst_ops->copy_by_bdev != NULL && spdk_get_thread() == NULL) {
		rc = dst_ops->copy_by_bdev(dst_bdev, dst_offset, src_bdev, src_offset, length);
		cb_fn(cb_arg, rc);
		return 0;
	}

	src_ops = vbdev_slm_lookup_ops_by_bdev(src_bdev);
	if (src_ops == NULL) {
		/* Generic async fallback can only use SLM provider read/write paths. */
		return -ENOTSUP;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->tmp = spdk_zmalloc(VBDEV_SLM_COPY_FALLBACK_CHUNK_SIZE, 4096, NULL,
				SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (ctx->tmp == NULL) {
		free(ctx);
		return -ENOMEM;
	}

	ctx->dst_bdev = dst_bdev;
	ctx->src_bdev = src_bdev;
	ctx->dst_offset = dst_offset;
	ctx->src_offset = src_offset;
	ctx->length = length;
	ctx->processed = 0;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	vbdev_slm_copy_by_bdev_async_step(ctx);
	return 0;
}

int
vbdev_slm_copy_by_bdev(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
		       struct spdk_bdev *src_bdev, uint64_t src_offset,
		       uint64_t length)
{
	struct vbdev_slm_sync_wait_ctx ctx = {};
	int submit_rc;

	if (vbdev_slm_sync_is_disallowed("copy_by_bdev")) {
		return -EWOULDBLOCK;
	}

	pthread_mutex_init(&ctx.mutex, NULL);
	pthread_cond_init(&ctx.cond, NULL);
	ctx.done = false;
	ctx.status = 0;

	submit_rc = vbdev_slm_copy_by_bdev_async(dst_bdev, dst_offset, src_bdev, src_offset,
			length, vbdev_slm_sync_wait_done, &ctx);
	return vbdev_slm_sync_wait_run(submit_rc, &ctx);
}

int
vbdev_slm_exec_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
				  void *buf, spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	const struct spdk_vbdev_slm_ops *ops;
	int rc;

	if (cb_fn == NULL || bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return 0;
	}

	ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (ops == NULL) {
		return -ENOTSUP;
	}

	if (ops->exec_read_by_bdev_async != NULL) {
		return ops->exec_read_by_bdev_async(bdev, offset, length, buf, cb_fn, cb_arg);
	}
	if (ops->exec_read_by_bdev != NULL && spdk_get_thread() == NULL) {
		rc = ops->exec_read_by_bdev(bdev, offset, length, buf);
		cb_fn(cb_arg, rc);
		return 0;
	}

	return vbdev_slm_read_by_bdev_async(bdev, offset, length, buf, cb_fn, cb_arg);
}

int
vbdev_slm_exec_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	struct vbdev_slm_sync_wait_ctx ctx = {};
	int submit_rc;

	if (vbdev_slm_sync_is_disallowed("exec_read_by_bdev")) {
		return -EWOULDBLOCK;
	}

	pthread_mutex_init(&ctx.mutex, NULL);
	pthread_cond_init(&ctx.cond, NULL);
	ctx.done = false;
	ctx.status = 0;

	submit_rc = vbdev_slm_exec_read_by_bdev_async(bdev, offset, length, buf,
			vbdev_slm_sync_wait_done, &ctx);
	return vbdev_slm_sync_wait_run(submit_rc, &ctx);
}

int
vbdev_slm_exec_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
				   const void *buf, spdk_bdev_slm_io_completion_cb cb_fn,
				   void *cb_arg)
{
	const struct spdk_vbdev_slm_ops *ops;
	int rc;

	if (cb_fn == NULL || bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return 0;
	}

	ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (ops == NULL) {
		return -ENOTSUP;
	}

	if (ops->exec_write_by_bdev_async != NULL) {
		return ops->exec_write_by_bdev_async(bdev, offset, length, buf, cb_fn, cb_arg);
	}
	if (ops->exec_write_by_bdev != NULL && spdk_get_thread() == NULL) {
		rc = ops->exec_write_by_bdev(bdev, offset, length, buf);
		cb_fn(cb_arg, rc);
		return 0;
	}

	return vbdev_slm_write_by_bdev_async(bdev, offset, length, buf, cb_fn, cb_arg);
}

int
vbdev_slm_exec_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
			     const void *buf)
{
	struct vbdev_slm_sync_wait_ctx ctx = {};
	int submit_rc;

	if (vbdev_slm_sync_is_disallowed("exec_write_by_bdev")) {
		return -EWOULDBLOCK;
	}

	pthread_mutex_init(&ctx.mutex, NULL);
	pthread_cond_init(&ctx.cond, NULL);
	ctx.done = false;
	ctx.status = 0;

	submit_rc = vbdev_slm_exec_write_by_bdev_async(bdev, offset, length, buf,
			vbdev_slm_sync_wait_done, &ctx);
	return vbdev_slm_sync_wait_run(submit_rc, &ctx);
}

int
vbdev_slm_lease_acquire_by_bdev(uint64_t lease_id, struct spdk_bdev *bdev,
				uint64_t offset, uint64_t length)
{
	const struct spdk_vbdev_slm_ops *ops;

	if (lease_id == 0 || bdev == NULL || length == 0) {
		return -EINVAL;
	}

	ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (ops == NULL || ops->lease_acquire_by_bdev == NULL) {
		return -ENOTSUP;
	}

	return ops->lease_acquire_by_bdev(lease_id, bdev, offset, length);
}

static int
vbdev_slm_exec_lease_op(uint64_t lease_id, bool is_publish)
{
	struct vbdev_slm_provider *provider;
	int rc, first_err = 0;
	bool found = false;
	bool callback_seen = false;

	if (lease_id == 0) {
		return -EINVAL;
	}

	pthread_rwlock_rdlock(&g_vbdev_slm_lock);
	TAILQ_FOREACH(provider, &g_vbdev_slm_providers, link) {
		if (is_publish) {
			if (provider->ops->exec_publish_lease == NULL) {
				continue;
			}
			rc = provider->ops->exec_publish_lease(lease_id);
		} else {
			if (provider->ops->exec_discard_lease == NULL) {
				continue;
			}
			rc = provider->ops->exec_discard_lease(lease_id);
		}

		callback_seen = true;
		if (rc == 0) {
			found = true;
		} else if (rc == -ENOENT) {
			continue;
		} else if (first_err == 0) {
			first_err = rc;
		}
	}
	pthread_rwlock_unlock(&g_vbdev_slm_lock);

	if (first_err != 0) {
		return first_err;
	}

	if (!callback_seen) {
		/* Providers without explicit execute-view callbacks are no-op. */
		return 0;
	}

	return found ? 0 : -ENOENT;
}

int
vbdev_slm_exec_publish_lease(uint64_t lease_id)
{
	return vbdev_slm_exec_lease_op(lease_id, true);
}

int
vbdev_slm_exec_discard_lease(uint64_t lease_id)
{
	return vbdev_slm_exec_lease_op(lease_id, false);
}

int
vbdev_slm_lease_release(uint64_t lease_id)
{
	struct vbdev_slm_provider *provider;
	int rc, first_err = 0;
	bool found = false;

	if (lease_id == 0) {
		return -EINVAL;
	}

	pthread_rwlock_rdlock(&g_vbdev_slm_lock);
	TAILQ_FOREACH(provider, &g_vbdev_slm_providers, link) {
		if (provider->ops->lease_release == NULL) {
			continue;
		}

		rc = provider->ops->lease_release(lease_id);
		if (rc == 0) {
			found = true;
		} else if (rc == -ENOENT) {
			continue;
		} else if (first_err == 0) {
			first_err = rc;
		}
	}
	pthread_rwlock_unlock(&g_vbdev_slm_lock);

	if (first_err != 0) {
		return first_err;
	}

	return found ? 0 : -ENOENT;
}

int
bdev_slm_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				uint64_t length, void **ptr)
{
	return vbdev_slm_get_buffer_ptr_by_bdev(bdev, offset, length, ptr);
}

int
bdev_slm_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			   uint64_t length, bool for_write,
			   struct spdk_bdev_slm_sg_entry *entries,
			   uint32_t max_entries, uint32_t *entry_count)
{
	return vbdev_slm_pin_range_by_bdev(bdev, offset, length, for_write,
					   entries, max_entries, entry_count);
}

int
bdev_slm_try_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			       uint64_t length, bool for_write,
			       struct spdk_bdev_slm_sg_entry *entries,
			       uint32_t max_entries, uint32_t *entry_count)
{
	return vbdev_slm_try_pin_range_by_bdev(bdev, offset, length, for_write,
					       entries, max_entries, entry_count);
}

int
bdev_slm_unpin_range_by_bdev(struct spdk_bdev *bdev,
			     const struct spdk_bdev_slm_sg_entry *entries,
			     uint32_t entry_count, bool dirtied)
{
	return vbdev_slm_unpin_range_by_bdev(bdev, entries, entry_count, dirtied);
}

int
bdev_slm_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	return vbdev_slm_read_by_bdev(bdev, offset, length, buf);
}

int
bdev_slm_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf,
			    spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	return vbdev_slm_read_by_bdev_async(bdev, offset, length, buf, cb_fn, cb_arg);
}

int
bdev_slm_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, const void *buf)
{
	return vbdev_slm_write_by_bdev(bdev, offset, length, buf);
}

int
bdev_slm_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
			     const void *buf, spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	return vbdev_slm_write_by_bdev_async(bdev, offset, length, buf, cb_fn, cb_arg);
}

int
bdev_slm_copy_by_bdev(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
		      struct spdk_bdev *src_bdev, uint64_t src_offset,
		      uint64_t length)
{
	return vbdev_slm_copy_by_bdev(dst_bdev, dst_offset, src_bdev, src_offset, length);
}

int
bdev_slm_copy_by_bdev_async(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
			    struct spdk_bdev *src_bdev, uint64_t src_offset, uint64_t length,
			    spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	return vbdev_slm_copy_by_bdev_async(dst_bdev, dst_offset, src_bdev, src_offset, length,
					    cb_fn, cb_arg);
}

int
bdev_slm_exec_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	return vbdev_slm_exec_read_by_bdev(bdev, offset, length, buf);
}

int
bdev_slm_exec_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
				 void *buf, spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	return vbdev_slm_exec_read_by_bdev_async(bdev, offset, length, buf, cb_fn, cb_arg);
}

int
bdev_slm_exec_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
			    const void *buf)
{
	return vbdev_slm_exec_write_by_bdev(bdev, offset, length, buf);
}

int
bdev_slm_exec_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
				  const void *buf, spdk_bdev_slm_io_completion_cb cb_fn,
				  void *cb_arg)
{
	return vbdev_slm_exec_write_by_bdev_async(bdev, offset, length, buf, cb_fn, cb_arg);
}

int
bdev_slm_lease_acquire_by_bdev(uint64_t lease_id, struct spdk_bdev *bdev,
			       uint64_t offset, uint64_t length)
{
	return vbdev_slm_lease_acquire_by_bdev(lease_id, bdev, offset, length);
}

int
bdev_slm_lease_release(uint64_t lease_id)
{
	return vbdev_slm_lease_release(lease_id);
}

int
bdev_slm_exec_publish_lease(uint64_t lease_id)
{
	return vbdev_slm_exec_publish_lease(lease_id);
}

int
bdev_slm_exec_discard_lease(uint64_t lease_id)
{
	return vbdev_slm_exec_discard_lease(lease_id);
}

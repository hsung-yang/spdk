/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 CPCS Implementation Team. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev_slm.h"
#include "spdk/log.h"
#include "spdk_internal/vbdev_slm.h"

struct vbdev_slm_provider {
	const struct spdk_vbdev_slm_ops *ops;
	TAILQ_ENTRY(vbdev_slm_provider)		link;
};

static TAILQ_HEAD(, vbdev_slm_provider) g_vbdev_slm_providers =
	TAILQ_HEAD_INITIALIZER(g_vbdev_slm_providers);
static pthread_rwlock_t g_vbdev_slm_lock = PTHREAD_RWLOCK_INITIALIZER;

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
	    ops->get_buffer_ptr_by_bdev == NULL || ops->read_by_bdev == NULL ||
	    ops->write_by_bdev == NULL) {
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
vbdev_slm_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	const struct spdk_vbdev_slm_ops *ops;

	if (bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	if (length == 0) {
		return 0;
	}

	ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (ops == NULL) {
		return -ENOTSUP;
	}

	return ops->read_by_bdev(bdev, offset, length, buf);
}

int
vbdev_slm_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, const void *buf)
{
	const struct spdk_vbdev_slm_ops *ops;

	if (bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	if (length == 0) {
		return 0;
	}

	ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (ops == NULL) {
		return -ENOTSUP;
	}

	return ops->write_by_bdev(bdev, offset, length, buf);
}

int
bdev_slm_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				uint64_t length, void **ptr)
{
	return vbdev_slm_get_buffer_ptr_by_bdev(bdev, offset, length, ptr);
}

int
bdev_slm_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	return vbdev_slm_read_by_bdev(bdev, offset, length, buf);
}

int
bdev_slm_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, const void *buf)
{
	return vbdev_slm_write_by_bdev(bdev, offset, length, buf);
}

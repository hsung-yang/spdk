/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme_spec.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/bdev_slm.h"
#include "spdk_internal/vbdev_slm.h"

#include "vbdev_pslm.h"

static int vbdev_pslm_initialize(void);
static void vbdev_pslm_finish(void);
static int vbdev_pslm_get_ctx_size(void);

static struct spdk_bdev_module slm_if = {
	.name = "slm",
	.module_init = vbdev_pslm_initialize,
	.module_fini = vbdev_pslm_finish,
	.get_ctx_size = vbdev_pslm_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(slm, &slm_if)

static TAILQ_HEAD(, spdk_bdev_slm) g_slm_bdevs =
	TAILQ_HEAD_INITIALIZER(g_slm_bdevs);
/*
 * g_slm_bdevs_lock guards every insert/remove/traverse of g_slm_bdevs.
 * It is the outermost lock: the per-bdev slm->lease_lock is always taken
 * INSIDE this lock (never the reverse) to keep a single ABBA-free ordering
 * across poll-group and RPC threads.
 */
static pthread_mutex_t g_slm_bdevs_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_slm_io_device_registered;

/* I/O channel for SLM bdev */
struct slm_io_channel {
	struct spdk_bdev_slm *slm_bdev;
};

struct pslm_lease {
	uint64_t lease_id;
	uint64_t offset;
	uint64_t length;
	TAILQ_ENTRY(pslm_lease) link;
};

struct pslm_async_complete_ctx {
	spdk_bdev_slm_io_completion_cb cb_fn;
	void *cb_arg;
	int status;
};

static const struct spdk_vbdev_slm_ops g_pslm_mem_ops;

static void
pslm_async_complete_msg(void *arg)
{
	struct pslm_async_complete_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, ctx->status);
	free(ctx);
}

/*
 * Reserve the deferred-completion context BEFORE the caller mutates SLM state,
 * so a ctx-alloc failure can be reported without leaving a half-applied write.
 * On a non-SPDK thread there is no deferral; *ctx_out is left NULL and the
 * caller must complete inline via pslm_send_async_ctx().
 * Returns 0 on success (resource reserved or inline), negative errno on failure.
 */
static int
pslm_alloc_async_ctx(spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg,
		     struct pslm_async_complete_ctx **ctx_out)
{
	struct pslm_async_complete_ctx *ctx;

	*ctx_out = NULL;

	if (spdk_get_thread() == NULL) {
		return 0;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	*ctx_out = ctx;
	return 0;
}

/*
 * Complete using a context reserved by pslm_alloc_async_ctx(). When ctx is NULL
 * (non-SPDK thread) the completion is invoked inline.
 */
static int
pslm_send_async_ctx(struct pslm_async_complete_ctx *ctx,
		    spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg, int status)
{
	struct spdk_thread *thread;
	int rc;

	if (ctx == NULL) {
		cb_fn(cb_arg, status);
		return 0;
	}

	thread = spdk_get_thread();
	ctx->status = status;
	rc = spdk_thread_send_msg(thread, pslm_async_complete_msg, ctx);
	if (rc != 0) {
		free(ctx);
		return rc;
	}

	return 0;
}

static int
pslm_complete_async(spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg, int status)
{
	struct pslm_async_complete_ctx *ctx;
	int rc;

	rc = pslm_alloc_async_ctx(cb_fn, cb_arg, &ctx);
	if (rc != 0) {
		return rc;
	}

	return pslm_send_async_ctx(ctx, cb_fn, cb_arg, status);
}

static int
vbdev_pslm_ch_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
vbdev_pslm_ch_destroy_cb(void *io_device, void *ctx_buf)
{
}

static void
vbdev_pslm_register_io_device(void)
{
	if (g_slm_io_device_registered) {
		return;
	}

	spdk_io_device_register(&g_slm_bdevs, vbdev_pslm_ch_create_cb,
				vbdev_pslm_ch_destroy_cb, sizeof(struct slm_io_channel),
				"bdev_slm");
	g_slm_io_device_registered = true;
}

static void
vbdev_pslm_unregister_io_device(void)
{
	if (!g_slm_io_device_registered) {
		return;
	}

	spdk_io_device_unregister(&g_slm_bdevs, NULL);
	g_slm_io_device_registered = false;
}

static int
vbdev_pslm_destruct(void *ctx)
{
	struct spdk_bdev_slm *slm = ctx;
	struct pslm_lease *lease, *tmp;

	pthread_mutex_lock(&g_slm_bdevs_lock);
	TAILQ_REMOVE(&g_slm_bdevs, slm, link);
	pthread_mutex_unlock(&g_slm_bdevs_lock);

	pthread_mutex_lock(&slm->lease_lock);
	TAILQ_FOREACH_SAFE(lease, &slm->leases, link, tmp) {
		TAILQ_REMOVE(&slm->leases, lease, link);
		free(lease);
	}
	pthread_mutex_unlock(&slm->lease_lock);
	pthread_mutex_destroy(&slm->lease_lock);

	if (slm->buffer) {
		spdk_free(slm->buffer);
	}

	free(slm->bdev.name);
	free(slm);
	return 0;
}

static void
vbdev_pslm_complete_nvme_status(struct spdk_bdev_io *bdev_io, int sct, int sc)
{
	spdk_bdev_io_complete_nvme_status(bdev_io, 0, sct, sc);
}

static void
vbdev_pslm_nvme_memory_fill(struct spdk_bdev_slm *slm, uint64_t starting_byte, uint32_t length)
{
	memset((uint8_t *)slm->buffer + starting_byte, 0, length);
}

static int
vbdev_pslm_nvme_prepare_iovs(struct spdk_bdev_io *bdev_io, uint32_t length,
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
vbdev_pslm_nvme_memory_read(struct spdk_bdev_slm *slm, uint64_t starting_byte, uint32_t length,
			    struct iovec *iovs, int iovcnt)
{
	uint8_t *src = (uint8_t *)slm->buffer + starting_byte;

	spdk_copy_buf_to_iovs(iovs, iovcnt, src, length);
	return 0;
}

static int
vbdev_pslm_nvme_memory_write(struct spdk_bdev_slm *slm, uint64_t starting_byte, uint32_t length,
			     struct iovec *iovs, int iovcnt)
{
	uint8_t *dst = (uint8_t *)slm->buffer + starting_byte;

	spdk_copy_iovs_to_buf(dst, length, iovs, iovcnt);
	return 0;
}


/*
 * The host-facing NVMe MEMORY_READ/WRITE/FILL data path below touches
 * slm->buffer directly and intentionally takes NO lease/concurrency guard.
 * pSLM provides NO host/exec isolation: it does not check whether the
 * referenced range is currently leased by an executing program. CPCS is
 * responsible for not issuing host NVMe memory ops against ranges that are
 * leased to an in-flight execution; pSLM trusts the caller to serialize
 * host and execute access to overlapping ranges.
 */
static void
vbdev_pslm_submit_nvme_passthru(struct spdk_bdev_slm *slm, struct spdk_bdev_io *bdev_io)
{
	const struct spdk_nvme_cmd *cmd = &bdev_io->u.nvme_passthru.cmd;
	uint64_t starting_byte;
	uint32_t length;
	uint64_t end;
	struct iovec *iovs = NULL;
	struct iovec local_iov;
	int iovcnt = 0;
	int sct = SPDK_NVME_SCT_GENERIC;
	int sc = SPDK_NVME_SC_SUCCESS;
	int rc;

	switch (cmd->opc) {
	case SPDK_NVME_SLM_OPC_MEMORY_FILL:
		starting_byte = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
		length = cmd->cdw12;
		SPDK_DEBUGLOG(pslm, "pSLM NVMe MEMORY_FILL nsid=%u start=%" PRIu64 " len=%u\n",
			      slm->bdev.nsid, starting_byte, length);

		if ((starting_byte & 0x3) != 0 || (length & 0x3) != 0) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		end = starting_byte + length;
		if (end < starting_byte || end > slm->buffer_size) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		vbdev_pslm_nvme_memory_fill(slm, starting_byte, length);
		goto out;
	case SPDK_NVME_SLM_OPC_MEMORY_READ:
		starting_byte = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
		length = cmd->cdw12;
		SPDK_DEBUGLOG(pslm, "pSLM NVMe MEMORY_READ nsid=%u start=%" PRIu64 " len=%u\n",
			      slm->bdev.nsid, starting_byte, length);

		if ((starting_byte & 0x3) != 0 || (length & 0x3) != 0) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		end = starting_byte + length;
		if (end < starting_byte || end > slm->buffer_size) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		if (length == 0) {
			goto out;
		}

		rc = vbdev_pslm_nvme_prepare_iovs(bdev_io, length, &local_iov, &iovs, &iovcnt);
		if (rc != 0) {
			sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			goto out;
		}

		rc = vbdev_pslm_nvme_memory_read(slm, starting_byte, length, iovs, iovcnt);
		if (rc != 0) {
			sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
		break;
	case SPDK_NVME_SLM_OPC_MEMORY_WRITE:
		starting_byte = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
		length = cmd->cdw12;
		SPDK_DEBUGLOG(pslm, "pSLM NVMe MEMORY_WRITE nsid=%u start=%" PRIu64 " len=%u\n",
			      slm->bdev.nsid, starting_byte, length);

		if ((starting_byte & 0x3) != 0 || (length & 0x3) != 0) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		end = starting_byte + length;
		if (end < starting_byte || end > slm->buffer_size) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		if (length == 0) {
			goto out;
		}

		rc = vbdev_pslm_nvme_prepare_iovs(bdev_io, length, &local_iov, &iovs, &iovcnt);
		if (rc != 0) {
			sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			goto out;
		}

		rc = vbdev_pslm_nvme_memory_write(slm, starting_byte, length, iovs, iovcnt);
		if (rc != 0) {
			sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
		break;
	default:
		SPDK_DEBUGLOG(pslm, "pSLM NVMe unsupported opcode=0x%x nsid=%u\n",
			      cmd->opc, slm->bdev.nsid);
		sc = SPDK_NVME_SC_INVALID_OPCODE;
		goto out;
	}

out:
	SPDK_DEBUGLOG(pslm, "pSLM NVMe completion opcode=0x%x nsid=%u sct=%d sc=%d\n",
		      cmd->opc, slm->bdev.nsid, sct, sc);
	vbdev_pslm_complete_nvme_status(bdev_io, sct, sc);
}

static void
vbdev_pslm_submit_request(struct spdk_io_channel *ch,
			  struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_slm *slm = SPDK_CONTAINEROF(bdev_io->bdev,
				    struct spdk_bdev_slm, bdev);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
		vbdev_pslm_submit_nvme_passthru(slm, bdev_io);
		break;

	default:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

static bool
vbdev_pslm_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
vbdev_pslm_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_slm_bdevs);
}

static const struct spdk_bdev_fn_table slm_fn_table = {
	.destruct           = vbdev_pslm_destruct,
	.submit_request     = vbdev_pslm_submit_request,
	.io_type_supported  = vbdev_pslm_io_type_supported,
	.get_io_channel     = vbdev_pslm_get_io_channel,
};

int
bdev_slm_create(const char *name, uint32_t nsid,
		uint64_t size_mb, uint32_t granularity)
{
	struct spdk_bdev_slm *slm;
	uint64_t size_bytes;
	int rc;

	if (!name || size_mb == 0 || granularity == 0) {
		SPDK_ERRLOG("Invalid parameters\n");
		return -EINVAL;
	}

	if (granularity % 4 != 0) {
		SPDK_ERRLOG("Granularity must be dword-aligned per SLM spec\n");
		return -EINVAL;
	}

	/* Reject size_mb that would overflow the MiB->bytes conversion. */
	if (size_mb > UINT64_MAX / (1024 * 1024)) {
		SPDK_ERRLOG("size_mb %" PRIu64 " too large\n", size_mb);
		return -EINVAL;
	}

	size_bytes = size_mb * 1024 * 1024;

	/* Reject sizes too small to hold even one block at this granularity. */
	if (size_bytes / granularity == 0) {
		SPDK_ERRLOG("size_mb %" PRIu64 " is below one block of granularity %u\n",
			    size_mb, granularity);
		return -EINVAL;
	}

	vbdev_pslm_register_io_device();

	slm = calloc(1, sizeof(*slm));
	if (!slm) {
		SPDK_ERRLOG("Failed to allocate SLM bdev\n");
		return -ENOMEM;
	}

	rc = pthread_mutex_init(&slm->lease_lock, NULL);
	if (rc != 0) {
		free(slm);
		return -rc;
	}
	TAILQ_INIT(&slm->leases);

	/* Allocate memory buffer */
	slm->buffer = spdk_zmalloc(size_bytes, 4096, NULL,
				   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!slm->buffer) {
		SPDK_ERRLOG("Failed to allocate SLM buffer\n");
		pthread_mutex_destroy(&slm->lease_lock);
		free(slm);
		return -ENOMEM;
	}

	slm->buffer_size = size_bytes;

	/* Initialize bdev structure */
	slm->bdev.name = strdup(name);
	slm->bdev.product_name = "SLM Memory Namespace";
	slm->bdev.blocklen = granularity;
	slm->bdev.blockcnt = size_bytes / granularity;
	slm->bdev.nsid = nsid;
	slm->bdev.slm = true;
	slm->bdev.ctxt = slm;
	slm->bdev.fn_table = &slm_fn_table;
	slm->bdev.module = &slm_if;

	rc = spdk_bdev_register(&slm->bdev);
	if (rc) {
		SPDK_ERRLOG("Failed to register SLM bdev\n");
		spdk_free(slm->buffer);
		pthread_mutex_destroy(&slm->lease_lock);
		free(slm->bdev.name);
		free(slm);
		return rc;
	}

	pthread_mutex_lock(&g_slm_bdevs_lock);
	TAILQ_INSERT_TAIL(&g_slm_bdevs, slm, link);
	pthread_mutex_unlock(&g_slm_bdevs_lock);

	SPDK_NOTICELOG("Created SLM bdev '%s': %lu MiB, NSID=%u\n",
		       name, size_mb, nsid);

	return 0;
}

void
bdev_slm_delete(const char *name,
		spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev_slm *slm;
	struct spdk_bdev *found = NULL;

	if (name == NULL) {
		if (cb_fn) {
			cb_fn(cb_arg, -EINVAL);
		}
		return;
	}

	pthread_mutex_lock(&g_slm_bdevs_lock);
	TAILQ_FOREACH(slm, &g_slm_bdevs, link) {
		if (strcmp(slm->bdev.name, name) == 0) {
			found = &slm->bdev;
			break;
		}
	}
	pthread_mutex_unlock(&g_slm_bdevs_lock);

	/*
	 * Drop the global lock before unregistering. spdk_bdev_unregister()
	 * completes the destruct (which takes g_slm_bdevs_lock) asynchronously
	 * and re-validates the bdev status, so it is safe to release here.
	 */
	if (found != NULL) {
		spdk_bdev_unregister(found, cb_fn, cb_arg);
		return;
	}

	if (cb_fn) {
		cb_fn(cb_arg, -ENOENT);
	}
}

int
bdev_slm_get_buffer_ptr(const char *name, uint64_t offset,
			uint64_t length, void **ptr)
{
	struct spdk_bdev_slm *slm;

	if (!name || !ptr) {
		return -EINVAL;
	}

	pthread_mutex_lock(&g_slm_bdevs_lock);
	TAILQ_FOREACH(slm, &g_slm_bdevs, link) {
		if (strcmp(slm->bdev.name, name) == 0) {
			if (offset > slm->buffer_size || length > slm->buffer_size - offset) {
				pthread_mutex_unlock(&g_slm_bdevs_lock);
				return -EINVAL;
			}
			*ptr = (uint8_t *)slm->buffer + offset;
			pthread_mutex_unlock(&g_slm_bdevs_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_slm_bdevs_lock);

	return -ENOENT;
}

static struct spdk_bdev_slm *
vbdev_pslm_get_by_bdev(struct spdk_bdev *bdev)
{
	struct spdk_bdev_slm *slm;

	if (bdev == NULL || bdev->module != &slm_if) {
		return NULL;
	}

	slm = bdev->ctxt;
	if (slm == NULL || &slm->bdev != bdev) {
		return NULL;
	}

	return slm;
}

static bool
vbdev_pslm_mem_owns_bdev(struct spdk_bdev *bdev)
{
	return vbdev_pslm_get_by_bdev(bdev) != NULL;
}

static int
vbdev_pslm_mem_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				      uint64_t length, void **ptr)
{
	struct spdk_bdev_slm *slm;

	if (ptr == NULL || bdev == NULL) {
		return -EINVAL;
	}

	if (length == 0) {
		*ptr = NULL;
		return 0;
	}

	slm = vbdev_pslm_get_by_bdev(bdev);
	if (slm == NULL) {
		return -ENOTSUP;
	}

	if (offset > slm->buffer_size || length > slm->buffer_size - offset) {
		SPDK_ERRLOG("Access out of bounds: offset=%lu len=%lu size=%lu\n",
			    offset, length, slm->buffer_size);
		return -EINVAL;
	}

	*ptr = (uint8_t *)slm->buffer + offset;
	return 0;
}

static int
vbdev_pslm_mem_validate_access(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
			       struct spdk_bdev_slm **slm_out)
{
	struct spdk_bdev_slm *slm;

	if (bdev == NULL) {
		return -EINVAL;
	}

	if (length == 0) {
		*slm_out = NULL;
		return 0;
	}

	slm = vbdev_pslm_get_by_bdev(bdev);
	if (slm == NULL) {
		return -ENOTSUP;
	}

	if (offset > slm->buffer_size || length > slm->buffer_size - offset) {
		SPDK_ERRLOG("Access out of bounds: offset=%lu len=%lu size=%lu\n",
			    offset, length, slm->buffer_size);
		return -EINVAL;
	}

	*slm_out = slm;
	return 0;
}

static int
vbdev_pslm_mem_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
				  void *buf, spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev_slm *slm;
	struct pslm_async_complete_ctx *ctx;
	int rc;

	if (cb_fn == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	rc = vbdev_pslm_mem_validate_access(bdev, offset, length, &slm);
	if (rc != 0) {
		return rc;
	}

	if (length == 0) {
		return pslm_complete_async(cb_fn, cb_arg, 0);
	}

	/*
	 * Reserve the completion context before copying into the caller buffer so
	 * that a ctx-alloc failure cannot leave a partially-filled buffer while
	 * the caller is told the operation failed.
	 */
	rc = pslm_alloc_async_ctx(cb_fn, cb_arg, &ctx);
	if (rc != 0) {
		return rc;
	}

	memcpy(buf, (uint8_t *)slm->buffer + offset, length);
	SPDK_DEBUGLOG(pslm, "pSLM mem_read nsid=%u off=%" PRIu64 " len=%" PRIu64 "\n",
		      bdev->nsid, offset, length);
	return pslm_send_async_ctx(ctx, cb_fn, cb_arg, 0);
}

static int
vbdev_pslm_mem_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	struct spdk_bdev_slm *slm;
	int rc;

	if ((length != 0 && buf == NULL) || bdev == NULL) {
		return -EINVAL;
	}

	rc = vbdev_pslm_mem_validate_access(bdev, offset, length, &slm);
	if (rc != 0 || length == 0) {
		return rc;
	}

	memcpy(buf, (uint8_t *)slm->buffer + offset, length);
	SPDK_DEBUGLOG(pslm, "pSLM mem_read nsid=%u off=%" PRIu64 " len=%" PRIu64 "\n",
		      bdev->nsid, offset, length);
	return 0;
}

static int
vbdev_pslm_mem_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
				   const void *buf, spdk_bdev_slm_io_completion_cb cb_fn,
				   void *cb_arg)
{
	struct spdk_bdev_slm *slm;
	struct pslm_async_complete_ctx *ctx;
	int rc;

	if (cb_fn == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	rc = vbdev_pslm_mem_validate_access(bdev, offset, length, &slm);
	if (rc != 0) {
		return rc;
	}

	if (length == 0) {
		return pslm_complete_async(cb_fn, cb_arg, 0);
	}

	/*
	 * Reserve the completion context before mutating slm->buffer so that a
	 * ctx-alloc failure cannot leave the buffer in a partially-written state
	 * while the caller is told the operation failed.
	 */
	rc = pslm_alloc_async_ctx(cb_fn, cb_arg, &ctx);
	if (rc != 0) {
		return rc;
	}

	memcpy((uint8_t *)slm->buffer + offset, buf, length);
	SPDK_DEBUGLOG(pslm, "pSLM mem_write nsid=%u off=%" PRIu64 " len=%" PRIu64 "\n",
		      bdev->nsid, offset, length);
	return pslm_send_async_ctx(ctx, cb_fn, cb_arg, 0);
}

static int
vbdev_pslm_mem_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
			     const void *buf)
{
	struct spdk_bdev_slm *slm;
	int rc;

	if ((length != 0 && buf == NULL) || bdev == NULL) {
		return -EINVAL;
	}

	rc = vbdev_pslm_mem_validate_access(bdev, offset, length, &slm);
	if (rc != 0 || length == 0) {
		return rc;
	}

	memcpy((uint8_t *)slm->buffer + offset, buf, length);
	SPDK_DEBUGLOG(pslm, "pSLM mem_write nsid=%u off=%" PRIu64 " len=%" PRIu64 "\n",
		      bdev->nsid, offset, length);
	return 0;
}

static bool
vbdev_pslm_ranges_overlap(uint64_t offset1, uint64_t length1,
			  uint64_t offset2, uint64_t length2)
{
	uint64_t end1 = offset1 + length1;
	uint64_t end2 = offset2 + length2;

	return !(end1 <= offset2 || end2 <= offset1);
}

static int
vbdev_pslm_mem_lease_acquire_by_bdev(uint64_t lease_id, struct spdk_bdev *bdev,
				     uint64_t offset, uint64_t length)
{
	struct spdk_bdev_slm *slm;
	struct pslm_lease *lease, *new_lease;

	if (lease_id == 0 || bdev == NULL || length == 0) {
		return -EINVAL;
	}

	slm = vbdev_pslm_get_by_bdev(bdev);
	if (slm == NULL) {
		return -ENOTSUP;
	}

	if (offset > slm->buffer_size || length > slm->buffer_size - offset) {
		return -EINVAL;
	}
	SPDK_DEBUGLOG(pslm, "pSLM lease acquire req lease=%" PRIu64 " nsid=%u off=%" PRIu64
		      " len=%" PRIu64 "\n", lease_id, bdev->nsid, offset, length);

	pthread_mutex_lock(&slm->lease_lock);
	TAILQ_FOREACH(lease, &slm->leases, link) {
		if (lease->lease_id == lease_id &&
		    lease->offset == offset &&
		    lease->length == length) {
			pthread_mutex_unlock(&slm->lease_lock);
			SPDK_DEBUGLOG(pslm, "pSLM lease acquire idempotent lease=%" PRIu64
				      " nsid=%u off=%" PRIu64 " len=%" PRIu64 "\n",
				      lease_id, bdev->nsid, offset, length);
			return 0;
		}

		if (lease->lease_id != lease_id &&
		    vbdev_pslm_ranges_overlap(offset, length, lease->offset, lease->length)) {
			pthread_mutex_unlock(&slm->lease_lock);
			SPDK_DEBUGLOG(pslm, "pSLM lease acquire conflict req_lease=%" PRIu64
				      " hold_lease=%" PRIu64 " nsid=%u req=[%" PRIu64 ",%" PRIu64
				      ") hold=[%" PRIu64 ",%" PRIu64 ")\n",
				      lease_id, lease->lease_id, bdev->nsid,
				      offset, offset + length, lease->offset, lease->offset + lease->length);
			return -EAGAIN;
		}
	}

	new_lease = calloc(1, sizeof(*new_lease));
	if (new_lease == NULL) {
		pthread_mutex_unlock(&slm->lease_lock);
		return -ENOMEM;
	}

	new_lease->lease_id = lease_id;
	new_lease->offset = offset;
	new_lease->length = length;
	TAILQ_INSERT_TAIL(&slm->leases, new_lease, link);
	pthread_mutex_unlock(&slm->lease_lock);
	SPDK_DEBUGLOG(pslm, "pSLM lease acquire success lease=%" PRIu64 " nsid=%u off=%" PRIu64
		      " len=%" PRIu64 "\n", lease_id, bdev->nsid, offset, length);

	return 0;
}

static int
vbdev_pslm_mem_lease_release(uint64_t lease_id)
{
	struct spdk_bdev_slm *slm;
	struct pslm_lease *lease, *tmp;
	bool found = false;

	if (lease_id == 0) {
		return -EINVAL;
	}
	SPDK_DEBUGLOG(pslm, "pSLM lease release req lease=%" PRIu64 "\n", lease_id);

	pthread_mutex_lock(&g_slm_bdevs_lock);
	TAILQ_FOREACH(slm, &g_slm_bdevs, link) {
		pthread_mutex_lock(&slm->lease_lock);
		TAILQ_FOREACH_SAFE(lease, &slm->leases, link, tmp) {
			if (lease->lease_id == lease_id) {
				TAILQ_REMOVE(&slm->leases, lease, link);
				free(lease);
				found = true;
			}
		}
		pthread_mutex_unlock(&slm->lease_lock);
	}
	pthread_mutex_unlock(&g_slm_bdevs_lock);
	SPDK_DEBUGLOG(pslm, "pSLM lease release lease=%" PRIu64 " found=%d\n", lease_id, found);

	return found ? 0 : -ENOENT;
}

static int
vbdev_pslm_mem_exec_lease_op(uint64_t lease_id)
{
	struct spdk_bdev_slm *slm;
	struct pslm_lease *lease;

	if (lease_id == 0) {
		return -EINVAL;
	}
	SPDK_DEBUGLOG(pslm, "pSLM exec lease lookup lease=%" PRIu64 "\n", lease_id);

	pthread_mutex_lock(&g_slm_bdevs_lock);
	TAILQ_FOREACH(slm, &g_slm_bdevs, link) {
		pthread_mutex_lock(&slm->lease_lock);
		TAILQ_FOREACH(lease, &slm->leases, link) {
			if (lease->lease_id == lease_id) {
				pthread_mutex_unlock(&slm->lease_lock);
				pthread_mutex_unlock(&g_slm_bdevs_lock);
				SPDK_DEBUGLOG(pslm, "pSLM exec lease lookup hit lease=%" PRIu64
					      " nsid=%u\n", lease_id, slm->bdev.nsid);
				return 0;
			}
		}
		pthread_mutex_unlock(&slm->lease_lock);
	}
	pthread_mutex_unlock(&g_slm_bdevs_lock);
	SPDK_DEBUGLOG(pslm, "pSLM exec lease lookup miss lease=%" PRIu64 "\n", lease_id);

	return -ENOENT;
}

static int
vbdev_pslm_mem_exec_publish_lease(uint64_t lease_id)
{
	/* pSLM has no execute-view indirection; publish is a lease existence check. */
	return vbdev_pslm_mem_exec_lease_op(lease_id);
}

static int
vbdev_pslm_mem_exec_discard_lease(uint64_t lease_id)
{
	/* pSLM has no execute-view indirection; discard is a lease existence check. */
	return vbdev_pslm_mem_exec_lease_op(lease_id);
}

static const struct spdk_vbdev_slm_ops g_pslm_mem_ops = {
	.name = "slm",
	.owns_bdev = vbdev_pslm_mem_owns_bdev,
	.get_buffer_ptr_by_bdev = vbdev_pslm_mem_get_buffer_ptr_by_bdev,
	.read_by_bdev = vbdev_pslm_mem_read_by_bdev,
	.read_by_bdev_async = vbdev_pslm_mem_read_by_bdev_async,
	.write_by_bdev = vbdev_pslm_mem_write_by_bdev,
	.write_by_bdev_async = vbdev_pslm_mem_write_by_bdev_async,
	.lease_acquire_by_bdev = vbdev_pslm_mem_lease_acquire_by_bdev,
	.exec_publish_lease = vbdev_pslm_mem_exec_publish_lease,
	.exec_discard_lease = vbdev_pslm_mem_exec_discard_lease,
	.lease_release = vbdev_pslm_mem_lease_release,
};

/* Module initialization/cleanup */
static int
vbdev_pslm_initialize(void)
{
	int rc;

	rc = vbdev_slm_register_ops(&g_pslm_mem_ops);
	if (rc != 0) {
		return rc;
	}

	vbdev_pslm_register_io_device();
	return 0;
}

static void
vbdev_pslm_finish(void)
{
	vbdev_slm_unregister_ops(&g_pslm_mem_ops);
	vbdev_pslm_unregister_io_device();
}

static int
vbdev_pslm_get_ctx_size(void)
{
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(pslm)

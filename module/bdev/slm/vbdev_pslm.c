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
static bool g_slm_io_device_registered;

/* I/O channel for SLM bdev */
struct slm_io_channel {
	struct spdk_bdev_slm *slm_bdev;
};

struct vbdev_pslm_copy_source_range {
	uint32_t	snsid;
	struct spdk_bdev *src_bdev;
	uint64_t	saddr;
	uint64_t	nbyte;
};

static const struct spdk_vbdev_slm_ops g_pslm_mem_ops;

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

	TAILQ_REMOVE(&g_slm_bdevs, slm, link);

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

static struct spdk_bdev *
vbdev_pslm_get_bdev_by_nsid(uint32_t nsid)
{
	struct spdk_bdev *bdev;

	for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
		if (spdk_bdev_get_nvme_nsid(bdev) == nsid) {
			return bdev;
		}
	}

	return NULL;
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

static int
vbdev_pslm_nvme_memory_copy(struct spdk_bdev_slm *slm, struct spdk_bdev_io *bdev_io, int *sct)
{
	const struct spdk_nvme_cmd *cmd = &bdev_io->u.nvme_passthru.cmd;
	struct spdk_nvme_slm_copy_desc_format_4 *descs_4 = NULL;
	struct spdk_nvme_slm_copy_desc_format_2_3 *descs_23 = NULL;
	struct vbdev_pslm_copy_source_range *ranges = NULL;
	uint64_t sdaddr;
	uint64_t len_field;
	enum spdk_nvme_slm_copy_desc_fmt desc_fmt;
	uint32_t nr;
	uint32_t desc_bytes;
	uint64_t copied = 0;
	uint64_t total_len = 0;
	uint64_t dest_end;
	uint8_t *dest;
	void *src;
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
		SPDK_ERRLOG("SLM copy invalid: sdaddr not dword aligned (0x%jx)\n", sdaddr);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc_fmt = (enum spdk_nvme_slm_copy_desc_fmt)SPDK_NVME_SLM_COPY_DESC_FMT_FROM_CDW12(cmd->cdw12);
	if (desc_fmt != SPDK_NVME_SLM_COPY_DESC_FMT_2H &&
	    desc_fmt != SPDK_NVME_SLM_COPY_DESC_FMT_3H &&
	    desc_fmt != SPDK_NVME_SLM_COPY_DESC_FMT_4H) {
		SPDK_ERRLOG("SLM copy invalid: desc_fmt=0x%x (expected 0x2/0x3/0x4), cdw12=0x%x\n",
			    (unsigned)desc_fmt, cmd->cdw12);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	nr = cmd->cdw12 & 0xFFu;
	desc_bytes = (nr + 1) * sizeof(*descs_4);

	len_field = ((uint64_t)cmd->rsvd3 << 32) | cmd->rsvd2;
	if ((len_field & 0x3) != 0) {
		SPDK_ERRLOG("SLM copy invalid: len_field not dword aligned (0x%jx)\n", len_field);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	rc = vbdev_pslm_nvme_prepare_iovs(bdev_io, desc_bytes, &local_iov, &iovs, &iovcnt);
	if (rc != 0) {
		SPDK_ERRLOG("SLM copy invalid: desc_bytes=%u nbytes=%zu iovcnt=%d rc=%d\n",
			    desc_bytes, bdev_io->u.nvme_passthru.nbytes, iovcnt, rc);
		return -SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
	}

	if (desc_fmt == SPDK_NVME_SLM_COPY_DESC_FMT_4H) {
		descs_4 = malloc(desc_bytes);
	} else {
		descs_23 = malloc(desc_bytes);
	}
	if (descs_4 == NULL && descs_23 == NULL) {
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	if (desc_fmt == SPDK_NVME_SLM_COPY_DESC_FMT_4H) {
		spdk_copy_iovs_to_buf(descs_4, desc_bytes, iovs, iovcnt);
	} else {
		spdk_copy_iovs_to_buf(descs_23, desc_bytes, iovs, iovcnt);
	}

	ranges = calloc(nr + 1, sizeof(*ranges));
	if (ranges == NULL) {
		rc = -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto out;
	}

	for (i = 0; i < nr + 1; i++) {
		struct spdk_bdev *src_bdev = NULL;
		uint32_t snsid;
		uint64_t saddr;
		uint64_t nbyte;
		uint16_t sopt;

		if (desc_fmt == SPDK_NVME_SLM_COPY_DESC_FMT_4H) {
			snsid = descs_4[i].snsid;
			saddr = descs_4[i].saddr;
			nbyte = descs_4[i].nbyte;
			sopt = descs_4[i].sopt;

			if ((saddr & 0x3) != 0 || (nbyte & 0x3) != 0) {
				SPDK_ERRLOG("SLM copy invalid: saddr=0x%jx nbyte=%ju\n", saddr, nbyte);
				rc = -SPDK_NVME_SC_INVALID_FIELD;
				goto out;
			}
			src_bdev = vbdev_pslm_get_bdev_by_nsid(snsid);
			if (src_bdev == NULL) {
				SPDK_ERRLOG("SLM copy invalid: source NSID %u not found\n", snsid);
				rc = -SPDK_NVME_SC_INVALID_FIELD;
				goto out;
			}
		} else {
			uint32_t block_size;
			uint64_t nlb_plus_one;

			snsid = descs_23[i].snsid;
			sopt = descs_23[i].reserved1;

			if (snsid == cmd->nsid) {
				SPDK_ERRLOG("SLM copy invalid: format %u disallows SNSID matching destination NSID (%u)\n",
					    (unsigned)desc_fmt, snsid);
				if (sct != NULL) {
					*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
				}
				rc = -SPDK_NVME_SLM_SC_INCOMPATIBLE_NAMESPACE_OR_FORMAT;
				goto out;
			}

			src_bdev = vbdev_pslm_get_bdev_by_nsid(snsid);
			if (src_bdev == NULL) {
				SPDK_ERRLOG("SLM copy invalid: source NSID %u not found\n", snsid);
				rc = -SPDK_NVME_SC_INVALID_FIELD;
				goto out;
			}

			if (spdk_bdev_is_slm(src_bdev)) {
				SPDK_ERRLOG("SLM copy invalid: source NSID %u must be LBA-based for desc_fmt=%u\n",
					    snsid, (unsigned)desc_fmt);
				rc = -SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
				goto out;
			}

			block_size = spdk_bdev_get_block_size(src_bdev);
			if (block_size == 0) {
				SPDK_ERRLOG("SLM copy invalid: source NSID %u block size is zero\n", snsid);
				rc = -SPDK_NVME_SC_INVALID_FIELD;
				goto out;
			}

			nlb_plus_one = (uint64_t)descs_23[i].nlb + 1;
			if (nlb_plus_one > UINT64_MAX / block_size) {
				SPDK_ERRLOG("SLM copy invalid: NLB overflow for source NSID %u\n", snsid);
				rc = -SPDK_NVME_SC_INVALID_FIELD;
				goto out;
			}
			nbyte = nlb_plus_one * block_size;

			if ((nbyte & 0x3) != 0) {
				SPDK_ERRLOG("SLM copy invalid: source NSID %u nbyte=%ju is not dword granular\n",
					    snsid, nbyte);
				rc = -SPDK_NVME_SC_INVALID_FIELD;
				goto out;
			}

			if (descs_23[i].slba > UINT64_MAX / block_size) {
				SPDK_ERRLOG("SLM copy invalid: SLBA overflow for source NSID %u\n", snsid);
				rc = -SPDK_NVME_SC_INVALID_FIELD;
				goto out;
			}
			saddr = descs_23[i].slba * block_size;
		}

		if (snsid == 0 || snsid == 0xFFFFFFFFu) {
			SPDK_ERRLOG("SLM copy invalid: snsid=0x%x\n", snsid);
			rc = -SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		if (sopt & SPDK_NVME_SLM_COPY_DESC_SOPT_FCO) {
			SPDK_ERRLOG("SLM copy invalid: fast copy only not supported\n");
			if (sct != NULL) {
				*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			}
			rc = -SPDK_NVME_SLM_SC_FAST_COPY_NOT_POSSIBLE;
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

		ranges[i].snsid = snsid;
		ranges[i].src_bdev = src_bdev;
		ranges[i].saddr = saddr;
		ranges[i].nbyte = nbyte;
	}

	if (total_len != len_field) {
		SPDK_ERRLOG("SLM copy invalid: total_len=%ju len_field=%ju\n",
			    total_len, len_field);
		rc = -SPDK_NVME_SC_INVALID_FIELD;
		goto out;
	}

	if (total_len > slm->buffer_size) {
		SPDK_ERRLOG("SLM copy invalid: total_len=%ju buffer_size=%ju\n",
			    total_len, slm->buffer_size);
		rc = -SPDK_NVME_SC_INVALID_FIELD;
		goto out;
	}

	dest_end = sdaddr + total_len;
	if (dest_end < sdaddr || dest_end > slm->buffer_size) {
		SPDK_ERRLOG("SLM copy invalid: dest_end=0x%jx sdaddr=0x%jx buffer_size=%ju\n",
			    dest_end, sdaddr, slm->buffer_size);
		rc = -SPDK_NVME_SC_INVALID_FIELD;
		goto out;
	}

	dest = (uint8_t *)slm->buffer + sdaddr;
	for (i = 0; i < nr + 1; i++) {
		uint32_t snsid = ranges[i].snsid;
		struct spdk_bdev *src_bdev = ranges[i].src_bdev;
		uint64_t saddr = ranges[i].saddr;
		uint64_t nbyte = ranges[i].nbyte;

		if (nbyte == 0) {
			continue;
		}

		if (desc_fmt == SPDK_NVME_SLM_COPY_DESC_FMT_4H && snsid == cmd->nsid) {
			uint64_t src_end = saddr + nbyte;

			if (src_end < saddr) {
				rc = -SPDK_NVME_SC_INVALID_FIELD;
				goto out;
			}

			if (src_end > sdaddr && saddr < dest_end) {
				SPDK_ERRLOG("SLM copy invalid: overlapping I/O range\n");
				if (sct != NULL) {
					*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
				}
				rc = -SPDK_NVME_SLM_SC_OVERLAPPING_IO_RANGE;
				goto out;
			}
		}

		if (desc_fmt == SPDK_NVME_SLM_COPY_DESC_FMT_4H) {
			rc = bdev_slm_get_buffer_ptr_by_bdev(src_bdev, saddr, nbyte, &src);
			if (rc != 0) {
				SPDK_ERRLOG("SLM copy invalid: snsid=%u saddr=0x%jx nbyte=%ju rc=%d\n",
					    snsid, saddr, nbyte, rc);
				rc = -SPDK_NVME_SC_INVALID_FIELD;
				goto out;
			}

			memmove(dest + copied, src, nbyte);
		} else {
			rc = bdev_slm_read_by_bdev(src_bdev, saddr, nbyte, dest + copied);
			if (rc != 0) {
				SPDK_ERRLOG("SLM copy invalid: failed to read source NSID %u saddr=0x%jx nbyte=%ju rc=%d\n",
					    snsid, saddr, nbyte, rc);
				rc = -SPDK_NVME_SC_INVALID_FIELD;
				goto out;
			}
		}

		copied += nbyte;
	}

out:
	free(ranges);
	free(descs_4);
	free(descs_23);
	return rc;
}

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
	case SPDK_NVME_SLM_OPC_MEMORY_COPY:
		rc = vbdev_pslm_nvme_memory_copy(slm, bdev_io, &sct);
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
		if (end < starting_byte || end > slm->buffer_size) {
			sc = SPDK_NVME_SC_INVALID_FIELD;
			goto out;
		}

		vbdev_pslm_nvme_memory_fill(slm, starting_byte, length);
		goto out;
	case SPDK_NVME_SLM_OPC_MEMORY_READ:
		starting_byte = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
		length = cmd->cdw12;

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
		sc = SPDK_NVME_SC_INVALID_OPCODE;
		goto out;
	}

out:
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
	uint64_t size_bytes = size_mb * 1024 * 1024;
	int rc;

	if (!name || size_mb == 0 || granularity == 0) {
		SPDK_ERRLOG("Invalid parameters\n");
		return -EINVAL;
	}

	if (granularity % 4 != 0) {
		SPDK_ERRLOG("Granularity must be dword-aligned per SLM spec\n");
		return -EINVAL;
	}

	vbdev_pslm_register_io_device();

	slm = calloc(1, sizeof(*slm));
	if (!slm) {
		SPDK_ERRLOG("Failed to allocate SLM bdev\n");
		return -ENOMEM;
	}

	/* Allocate memory buffer */
	slm->buffer = spdk_zmalloc(size_bytes, 4096, NULL,
				   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!slm->buffer) {
		SPDK_ERRLOG("Failed to allocate SLM buffer\n");
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
		free(slm->bdev.name);
		free(slm);
		return rc;
	}

	TAILQ_INSERT_TAIL(&g_slm_bdevs, slm, link);

	SPDK_NOTICELOG("Created SLM bdev '%s': %lu MiB, NSID=%u\n",
		       name, size_mb, nsid);

	return 0;
}

void
bdev_slm_delete(const char *name,
		spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev_slm *slm;

	TAILQ_FOREACH(slm, &g_slm_bdevs, link) {
		if (strcmp(slm->bdev.name, name) == 0) {
			spdk_bdev_unregister(&slm->bdev, cb_fn, cb_arg);
			return;
		}
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

	TAILQ_FOREACH(slm, &g_slm_bdevs, link) {
		if (strcmp(slm->bdev.name, name) == 0) {
			if (offset + length > slm->buffer_size) {
				return -EINVAL;
			}
			*ptr = (uint8_t *)slm->buffer + offset;
			return 0;
		}
	}

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
vbdev_pslm_mem_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	struct spdk_bdev_slm *slm;

	if ((length != 0 && buf == NULL) || bdev == NULL) {
		return -EINVAL;
	}

	if (length == 0) {
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

	memcpy(buf, (uint8_t *)slm->buffer + offset, length);
	return 0;
}

static int
vbdev_pslm_mem_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
			     const void *buf)
{
	struct spdk_bdev_slm *slm;

	if ((length != 0 && buf == NULL) || bdev == NULL) {
		return -EINVAL;
	}

	if (length == 0) {
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

	memcpy((uint8_t *)slm->buffer + offset, buf, length);
	return 0;
}

static const struct spdk_vbdev_slm_ops g_pslm_mem_ops = {
	.name = "slm",
	.owns_bdev = vbdev_pslm_mem_owns_bdev,
	.get_buffer_ptr_by_bdev = vbdev_pslm_mem_get_buffer_ptr_by_bdev,
	.read_by_bdev = vbdev_pslm_mem_read_by_bdev,
	.write_by_bdev = vbdev_pslm_mem_write_by_bdev,
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

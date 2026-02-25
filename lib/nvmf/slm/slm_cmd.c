/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd. All rights reserved.
 */

#include "slm_cmd.h"

#include "nvmf_internal.h"

#include "spdk/nvme_spec.h"
#include "spdk/nvmf_cmd.h"

static int
nvmf_slm_set_status(struct spdk_nvmf_request *req, uint8_t sct, uint8_t sc)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	rsp->status.sct = sct;
	rsp->status.sc = sc;
	return -EINVAL;
}

int
nvmf_slm_parse_copy_lba_cmd(struct spdk_nvmf_request *req, struct spdk_nvmf_ns *dest_ns,
			    enum spdk_nvme_slm_copy_desc_fmt *desc_fmt_out, uint64_t *sdaddr,
			    uint64_t *total_nbytes_out,
			    struct nvmf_slm_copy_lba_range **ranges_out,
			    uint32_t *range_count_out)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_slm_copy_desc_format_2_3 *descs_23 = NULL;
	struct spdk_nvme_slm_copy_desc_format_4 *descs_4 = NULL;
	struct nvmf_slm_copy_lba_range *ranges = NULL;
	struct spdk_nvmf_ns *src_ns;
	struct spdk_bdev *src_bdev;
	struct spdk_bdev_desc *src_desc;
	struct spdk_io_channel *src_ch;
	struct spdk_iov_xfer ix;
	uint64_t len;
	uint64_t total = 0;
	uint64_t dest_size;
	uint64_t dest_end;
	uint64_t src_end;
	uint64_t nbytes;
	uint32_t nr;
	uint32_t snsid;
	uint32_t blocklen;
	enum spdk_nvme_slm_copy_desc_fmt desc_fmt;
	uint32_t desc_bytes;
	uint32_t i;
	int rc;

	if (desc_fmt_out == NULL || sdaddr == NULL || total_nbytes_out == NULL ||
	    ranges_out == NULL || range_count_out == NULL) {
		return -EINVAL;
	}

	*desc_fmt_out = SPDK_NVME_SLM_COPY_DESC_FMT_NONE;
	*sdaddr = 0;
	*total_nbytes_out = 0;
	*ranges_out = NULL;
	*range_count_out = 0;

	desc_fmt = (enum spdk_nvme_slm_copy_desc_fmt)SPDK_NVME_SLM_COPY_DESC_FMT_FROM_CDW12(cmd->cdw12);
	if (desc_fmt != SPDK_NVME_SLM_COPY_DESC_FMT_2H &&
	    desc_fmt != SPDK_NVME_SLM_COPY_DESC_FMT_3H &&
	    desc_fmt != SPDK_NVME_SLM_COPY_DESC_FMT_4H) {
		return nvmf_slm_set_status(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INVALID_FIELD);
	}
	*desc_fmt_out = desc_fmt;

	if (dest_ns == NULL || dest_ns->csi != SPDK_NVME_CSI_SLM) {
		return nvmf_slm_set_status(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INVALID_FIELD);
	}

	len = ((uint64_t)cmd->rsvd3 << 32) | cmd->rsvd2;
	*sdaddr = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
	nr = cmd->cdw12 & 0xFFu;
	desc_bytes = (nr + 1) * sizeof(struct spdk_nvme_slm_copy_desc_format_4);

	if ((len & 0x3) != 0 || (*sdaddr & 0x3) != 0) {
		return nvmf_slm_set_status(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INVALID_FIELD);
	}

	if (req->length != desc_bytes) {
		return nvmf_slm_set_status(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID);
	}

	if (desc_fmt == SPDK_NVME_SLM_COPY_DESC_FMT_4H) {
		descs_4 = calloc(nr + 1, sizeof(*descs_4));
	} else {
		descs_23 = calloc(nr + 1, sizeof(*descs_23));
	}
	if (descs_4 == NULL && descs_23 == NULL) {
		return nvmf_slm_set_status(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
	if (desc_fmt == SPDK_NVME_SLM_COPY_DESC_FMT_4H) {
		spdk_iov_xfer_to_buf(&ix, descs_4, desc_bytes);
	} else {
		spdk_iov_xfer_to_buf(&ix, descs_23, desc_bytes);
	}

	ranges = calloc(nr + 1, sizeof(*ranges));
	if (ranges == NULL) {
		nvmf_slm_set_status(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
		goto error;
	}

	for (i = 0; i < nr + 1; i++) {
		if (desc_fmt == SPDK_NVME_SLM_COPY_DESC_FMT_4H) {
			snsid = descs_4[i].snsid;
		} else {
			snsid = descs_23[i].snsid;
		}

		if (snsid == 0 || snsid == 0xFFFFFFFFu) {
			goto invalid_field;
		}

		src_ns = nvmf_ctrlr_get_ns(ctrlr, snsid);
		if (src_ns == NULL) {
			goto invalid_field;
		}

		rc = spdk_nvmf_request_get_bdev(snsid, req, &src_bdev, &src_desc, &src_ch);
		if (rc != 0 || src_bdev == NULL) {
			goto invalid_field;
		}

		ranges[i].snsid = snsid;
		ranges[i].src_bdev = src_bdev;
		ranges[i].dest_offset = total;

		if (desc_fmt == SPDK_NVME_SLM_COPY_DESC_FMT_4H) {
			nbytes = descs_4[i].nbyte;
			ranges[i].saddr = descs_4[i].saddr;

			if ((ranges[i].saddr & 0x3) != 0 || (nbytes & 0x3) != 0) {
				goto invalid_field;
			}

			if (descs_4[i].sopt & SPDK_NVME_SLM_COPY_DESC_SOPT_FCO) {
				nvmf_slm_set_status(req, SPDK_NVME_SCT_COMMAND_SPECIFIC,
						    SPDK_NVME_SLM_SC_FAST_COPY_NOT_POSSIBLE);
				goto error;
			}

			if (src_ns->csi != SPDK_NVME_CSI_SLM) {
				nvmf_slm_set_status(req, SPDK_NVME_SCT_GENERIC,
						    SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
				goto error;
			}
		} else {
			if (snsid == cmd->nsid) {
				nvmf_slm_set_status(req, SPDK_NVME_SCT_COMMAND_SPECIFIC,
						    SPDK_NVME_SLM_SC_INCOMPATIBLE_NAMESPACE_OR_FORMAT);
				goto error;
			}

			if (src_ns->csi == SPDK_NVME_CSI_SLM) {
				nvmf_slm_set_status(req, SPDK_NVME_SCT_GENERIC,
						    SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
				goto error;
			}

			ranges[i].slba = descs_23[i].slba;
			ranges[i].nlb = descs_23[i].nlb;

			blocklen = spdk_bdev_get_block_size(src_bdev);
			nbytes = ((uint64_t)ranges[i].nlb + 1) * blocklen;
			if (nbytes == 0 || (nbytes & 0x3) != 0) {
				goto invalid_field;
			}

			ranges[i].desc = src_desc;
			ranges[i].ch = src_ch;
		}

		if (total + nbytes < total) {
			goto invalid_field;
		}
		total += nbytes;
		ranges[i].nbytes = nbytes;
	}

	if (len != total) {
		goto invalid_field;
	}

	dest_size = spdk_bdev_get_num_blocks(dest_ns->bdev) *
		    spdk_bdev_get_block_size(dest_ns->bdev);
	if (*sdaddr > dest_size || len > dest_size - *sdaddr) {
		goto invalid_field;
	}

	if (desc_fmt == SPDK_NVME_SLM_COPY_DESC_FMT_4H && len != 0) {
		dest_end = *sdaddr + len;
		if (dest_end < *sdaddr) {
			goto invalid_field;
		}

		for (i = 0; i < nr + 1; i++) {
			if (ranges[i].snsid != cmd->nsid || ranges[i].nbytes == 0) {
				continue;
			}

			src_end = ranges[i].saddr + ranges[i].nbytes;
			if (src_end < ranges[i].saddr) {
				goto invalid_field;
			}

			if (src_end > *sdaddr && ranges[i].saddr < dest_end) {
				nvmf_slm_set_status(req, SPDK_NVME_SCT_COMMAND_SPECIFIC,
						    SPDK_NVME_SLM_SC_OVERLAPPING_IO_RANGE);
				goto error;
			}
		}
	}

	*ranges_out = ranges;
	*range_count_out = nr + 1;
	*total_nbytes_out = total;
	free(descs_23);
	free(descs_4);
	return 0;

invalid_field:
	nvmf_slm_set_status(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_INVALID_FIELD);
error:
	free(descs_23);
	free(descs_4);
	free(ranges);
	*desc_fmt_out = SPDK_NVME_SLM_COPY_DESC_FMT_NONE;
	*sdaddr = 0;
	*total_nbytes_out = 0;
	return -EINVAL;
}

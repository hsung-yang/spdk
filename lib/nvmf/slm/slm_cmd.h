/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd. All rights reserved.
 */

#ifndef SPDK_NVMF_SLM_CMD_H
#define SPDK_NVMF_SLM_CMD_H

#include "spdk/stdinc.h"
#include "spdk/nvme_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_bdev_desc;
struct spdk_io_channel;
struct spdk_nvmf_ns;
struct spdk_nvmf_request;

struct nvmf_slm_copy_lba_range {
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*ch;
	uint64_t		slba;
	uint32_t		nlb;
	uint64_t		nbytes;
	uint64_t		dest_offset;
	uint32_t		snsid;
	uint64_t		saddr;
};

int nvmf_slm_parse_copy_lba_cmd(struct spdk_nvmf_request *req,
				struct spdk_nvmf_ns *dest_ns,
				enum spdk_nvme_slm_copy_desc_fmt *desc_fmt_out,
				uint64_t *sdaddr,
				uint64_t *total_nbytes_out,
				struct nvmf_slm_copy_lba_range **ranges_out,
				uint32_t *range_count_out);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_SLM_CMD_H */

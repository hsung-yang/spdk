/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * pSLM bdev internal header
 */

#ifndef SPDK_VBDEV_PSLM_H
#define SPDK_VBDEV_PSLM_H

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_bdev_slm {
	struct spdk_bdev bdev;

	/* Memory buffer */
	void *buffer;
	uint64_t buffer_size;    /* bytes */

	/* Namespace identification */
	uint32_t nsid;           /* Memory Namespace ID */

	/* Granularity */
	uint32_t granularity;    /* Access granularity (bytes) */

	TAILQ_ENTRY(spdk_bdev_slm) link;
};

#ifdef __cplusplus
}
#endif

#endif /* SPDK_VBDEV_PSLM_H */

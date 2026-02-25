/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

#include "memory_range_set.h"
#include "nvmf_cpcs.h"

#include "spdk/log.h"
#include "spdk/nvme_spec.h"
#include "spdk/bdev_slm.h"

static int cpcs_mrs_validate_locked(struct spdk_nvmf_cpcs_ns *ns,
				    const struct spdk_nvme_cpcs_memory_range_descriptor *ranges,
				    uint8_t num_ranges);

int
cpcs_mrs_create(struct spdk_nvmf_cpcs_ns *ns,
		const struct spdk_nvme_cpcs_memory_range_descriptor *ranges,
		uint8_t num_ranges,
		uint16_t *rsid_out)
{
	struct cpcs_memory_range_set *mrs;
	int rc;
	uint8_t i;

	if (!ns || !ranges || num_ranges == 0 || !rsid_out) {
		return -EINVAL;
	}

	pthread_mutex_lock(&ns->lock);

	/* Check limits */
	if (ns->max_mrs > 0 && ns->mrs_count >= ns->max_mrs) {
		pthread_mutex_unlock(&ns->lock);
		return -SPDK_NVME_CPCS_SC_MAX_MEMORY_RANGE_SETS_EXCEEDED;
	}

	if (ns->max_ranges_per_mrs > 0 && num_ranges > ns->max_ranges_per_mrs) {
		pthread_mutex_unlock(&ns->lock);
		return -SPDK_NVME_CPCS_SC_MAX_MEMORY_RANGES_EXCEEDED;
	}

	/* Validate ranges */
	rc = cpcs_mrs_validate_locked(ns, ranges, num_ranges);
	if (rc) {
		pthread_mutex_unlock(&ns->lock);
		return rc;
	}

	/* Allocate MRS */
	mrs = calloc(1, sizeof(*mrs));
	if (!mrs) {
		pthread_mutex_unlock(&ns->lock);
		return -ENOMEM;
	}

	/* Allocate ranges array */
	mrs->ranges = calloc(num_ranges, sizeof(struct cpcs_memory_range));
	if (!mrs->ranges) {
		free(mrs);
		pthread_mutex_unlock(&ns->lock);
		return -ENOMEM;
	}

	/* Copy ranges */
	for (i = 0; i < num_ranges; i++) {
		mrs->ranges[i].mnsid = ranges[i].mnsid;
		mrs->ranges[i].starting_byte = ranges[i].starting_byte;
		mrs->ranges[i].length = ranges[i].length;
	}

	/* Initialize MRS */
	mrs->rsid = ns->next_rsid++;
	mrs->range_count = num_ranges;
	mrs->ref_count = 0;

	/* Add to list */
	TAILQ_INSERT_TAIL(&ns->mrs_list, mrs, link);
	ns->mrs_count++;

	*rsid_out = mrs->rsid;
	pthread_mutex_unlock(&ns->lock);

	SPDK_NOTICELOG("Created MRS RSID=%u with %u ranges\n", mrs->rsid, num_ranges);
	return 0;
}

int
cpcs_mrs_delete(struct spdk_nvmf_cpcs_ns *ns, uint16_t rsid)
{
	struct cpcs_memory_range_set *mrs;

	if (!ns) {
		return -EINVAL;
	}

	pthread_mutex_lock(&ns->lock);

	/* Find MRS */
	TAILQ_FOREACH(mrs, &ns->mrs_list, link) {
		if (mrs->rsid == rsid) {
			/* Check if in use */
			if (mrs->ref_count > 0) {
				pthread_mutex_unlock(&ns->lock);
				return -SPDK_NVME_CPCS_SC_MEMORY_RANGE_SET_IN_USE;
			}

			/* Remove and free */
			TAILQ_REMOVE(&ns->mrs_list, mrs, link);
			ns->mrs_count--;
			free(mrs->ranges);
			free(mrs);

			pthread_mutex_unlock(&ns->lock);
			SPDK_NOTICELOG("Deleted MRS RSID=%u\n", rsid);
			return 0;
		}
	}

	pthread_mutex_unlock(&ns->lock);
	return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_RANGE_SET_ID;
}

int
cpcs_mrs_delete_all(struct spdk_nvmf_cpcs_ns *ns)
{
	struct cpcs_memory_range_set *mrs, *tmp;

	if (!ns) {
		return -EINVAL;
	}

	pthread_mutex_lock(&ns->lock);

	TAILQ_FOREACH_SAFE(mrs, &ns->mrs_list, link, tmp) {
		TAILQ_REMOVE(&ns->mrs_list, mrs, link);
		free(mrs->ranges);
		free(mrs);
	}

	ns->mrs_count = 0;
	pthread_mutex_unlock(&ns->lock);

	return 0;
}

struct cpcs_memory_range_set *
cpcs_mrs_get(struct spdk_nvmf_cpcs_ns *ns, uint16_t rsid)
{
	struct cpcs_memory_range_set *mrs;

	if (!ns) {
		return NULL;
	}

	pthread_mutex_lock(&ns->lock);

	TAILQ_FOREACH(mrs, &ns->mrs_list, link) {
		if (mrs->rsid == rsid) {
			pthread_mutex_unlock(&ns->lock);
			return mrs;
		}
	}

	pthread_mutex_unlock(&ns->lock);
	return NULL;
}

int
cpcs_mrs_acquire(struct cpcs_memory_range_set *mrs)
{
	if (!mrs) {
		return -EINVAL;
	}

	__atomic_add_fetch(&mrs->ref_count, 1, __ATOMIC_SEQ_CST);

	return 0;
}

void
cpcs_mrs_release(struct cpcs_memory_range_set *mrs)
{
	if (!mrs) {
		return;
	}

	__atomic_sub_fetch(&mrs->ref_count, 1, __ATOMIC_SEQ_CST);
}

int
cpcs_mrs_validate(struct spdk_nvmf_cpcs_ns *ns,
		  const struct spdk_nvme_cpcs_memory_range_descriptor *ranges,
		  uint8_t num_ranges)
{
	int rc;

	if (!ns || !ranges) {
		return -EINVAL;
	}

	pthread_mutex_lock(&ns->lock);
	rc = cpcs_mrs_validate_locked(ns, ranges, num_ranges);
	pthread_mutex_unlock(&ns->lock);

	return rc;
}

static int
cpcs_mrs_validate_locked(struct spdk_nvmf_cpcs_ns *ns,
			 const struct spdk_nvme_cpcs_memory_range_descriptor *ranges,
			 uint8_t num_ranges)
{
	struct cpcs_memory_range_set *mrs;
	struct cpcs_memory_range *existing;
	uint64_t granularity;
	uint64_t start1, end1, start2, end2;
	uint8_t i, j;

	granularity = 1ULL << ns->mrs_granularity;

	for (i = 0; i < num_ranges; i++) {
		start1 = ranges[i].starting_byte;
		end1 = start1 + ranges[i].length;

		/* Validate granularity alignment */
		if ((start1 % granularity) != 0) {
			SPDK_ERRLOG("Starting byte not aligned to granularity\n");
			return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_RANGE_SET;
		}

		if ((ranges[i].length % granularity) != 0) {
			SPDK_ERRLOG("Length not aligned to granularity\n");
			return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_RANGE_SET;
		}

		/* Check for overlapping ranges within the request */
		for (j = i + 1; j < num_ranges; j++) {
			/* Same memory namespace */
			if (ranges[i].mnsid == ranges[j].mnsid) {
				start2 = ranges[j].starting_byte;
				end2 = start2 + ranges[j].length;

				/* Check overlap */
				if (!(end1 <= start2 || end2 <= start1)) {
					SPDK_ERRLOG("Overlapping ranges detected\n");
					return -SPDK_NVME_CPCS_SC_OVERLAPPING_MEMORY_RANGES;
				}
			}
		}

		/* Check for overlapping ranges with existing MRSs */
		TAILQ_FOREACH(mrs, &ns->mrs_list, link) {
			for (j = 0; j < mrs->range_count; j++) {
				existing = &mrs->ranges[j];
				if (existing->mnsid != ranges[i].mnsid) {
					continue;
				}

				start2 = existing->starting_byte;
				end2 = start2 + existing->length;
				if (!(end1 <= start2 || end2 <= start1)) {
					SPDK_ERRLOG("Overlapping ranges with existing MRS detected\n");
					return -SPDK_NVME_CPCS_SC_OVERLAPPING_MEMORY_RANGES;
				}
			}
		}
	}

	return 0;
}

int
cpcs_mrs_get_buffer(struct cpcs_memory_range_set *mrs,
		    uint64_t mr_id, uint64_t offset, uint64_t len,
		    void **ptr_out)
{
	struct cpcs_memory_range *mr;
	uint64_t range_idx;
	int rc;

	if (!mrs || !ptr_out) {
		return -EINVAL;
	}

	/* Memory Range IDs are 1-based */
	if (mr_id == 0 || mr_id > mrs->range_count) {
		SPDK_ERRLOG("Invalid memory range ID: %lu (count=%u)\n",
			    mr_id, mrs->range_count);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	/* Convert to 0-based index */
	range_idx = mr_id - 1;
	mr = &mrs->ranges[range_idx];

	/* Validate offset and length are within range */
	if (offset + len > mr->length) {
		SPDK_ERRLOG("Access out of bounds: offset=%lu len=%lu range_len=%u\n",
			    offset, len, mr->length);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	/* Get actual SLM memory buffer from memory namespace
	 * The SLM bdev provides direct memory access for efficient data transfer
	 */
	rc = bdev_slm_get_buffer_ptr_by_nsid(mr->mnsid,
					     mr->starting_byte + offset,
					     len, ptr_out);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to get SLM buffer: MNSID=%u offset=%lu len=%lu rc=%d\n",
			    mr->mnsid, mr->starting_byte + offset, len, rc);
		if (rc == -ENOENT) {
			return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
		}
		return rc;
	}

	SPDK_DEBUGLOG(nvmf_cpcs, "Got SLM buffer: MNSID=%u offset=%lu len=%lu\n",
		      mr->mnsid, mr->starting_byte + offset, len);
	return 0;
}

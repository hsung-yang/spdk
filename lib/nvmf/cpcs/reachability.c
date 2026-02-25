/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

#include "reachability.h"
#include "nvmf_cpcs.h"
#include "nvmf_internal.h"
#include "spdk/log.h"
#include "spdk/nvme_spec.h"

struct cpcs_reachability_subsystem {
	struct spdk_nvmf_subsystem *subsystem;
	struct cpcs_reachability_manager mgr;
	uint32_t ref_count;
	TAILQ_ENTRY(cpcs_reachability_subsystem) link;
};

static TAILQ_HEAD(, cpcs_reachability_subsystem) g_reachability_subsystems =
	TAILQ_HEAD_INITIALIZER(g_reachability_subsystems);
static pthread_mutex_t g_reachability_subsystems_lock = PTHREAD_MUTEX_INITIALIZER;

static struct cpcs_reachability_group *
cpcs_reachability_find_group(struct cpcs_reachability_manager *mgr, uint16_t group_id)
{
	struct cpcs_reachability_group *group;

	TAILQ_FOREACH(group, &mgr->groups, link) {
		if (group->group_id == group_id) {
			return group;
		}
	}

	return NULL;
}

static bool
cpcs_reachability_group_has_memory_nsid(struct cpcs_reachability_group *group, uint32_t mnsid)
{
	struct cpcs_reachability_ns_entry *entry;

	TAILQ_FOREACH(entry, &group->namespaces, link) {
		if (entry->nsid != mnsid) {
			continue;
		}

		if (entry->ns && entry->ns->csi == SPDK_NVME_CSI_CPCS) {
			continue;
		}

		return true;
	}

	return false;
}

static bool
cpcs_reachability_nsid_seen(const uint32_t *memory_nsids, uint32_t count, uint32_t nsid)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		if (memory_nsids[i] == nsid) {
			return true;
		}
	}

	return false;
}

int
cpcs_reachability_init(struct cpcs_reachability_manager *mgr)
{
	if (!mgr) {
		return -EINVAL;
	}

	TAILQ_INIT(&mgr->groups);
	TAILQ_INIT(&mgr->associations);
	mgr->next_group_id = 1;
	mgr->next_assoc_id = 1;

	if (pthread_mutex_init(&mgr->lock, NULL) != 0) {
		return -ENOMEM;
	}

	return 0;
}

void
cpcs_reachability_fini(struct cpcs_reachability_manager *mgr)
{
	struct cpcs_reachability_group *group, *group_tmp;
	struct cpcs_reachability_association *assoc, *assoc_tmp;
	struct cpcs_reachability_ns_entry *entry, *entry_tmp;

	if (!mgr) {
		return;
	}

	/* Free all groups and their namespace entries */
	TAILQ_FOREACH_SAFE(group, &mgr->groups, link, group_tmp) {
		TAILQ_FOREACH_SAFE(entry, &group->namespaces, link, entry_tmp) {
			TAILQ_REMOVE(&group->namespaces, entry, link);
			free(entry);
		}
		TAILQ_REMOVE(&mgr->groups, group, link);
		free(group);
	}

	/* Free all associations */
	TAILQ_FOREACH_SAFE(assoc, &mgr->associations, link, assoc_tmp) {
		TAILQ_REMOVE(&mgr->associations, assoc, link);
		free(assoc->group_ids);
		free(assoc);
	}

	pthread_mutex_destroy(&mgr->lock);
}

struct cpcs_reachability_manager *
cpcs_reachability_mgr_get(struct spdk_nvmf_subsystem *subsystem)
{
	struct cpcs_reachability_subsystem *entry;
	int rc;

	if (!subsystem) {
		return NULL;
	}

	pthread_mutex_lock(&g_reachability_subsystems_lock);

	TAILQ_FOREACH(entry, &g_reachability_subsystems, link) {
		if (entry->subsystem == subsystem) {
			entry->ref_count++;
			pthread_mutex_unlock(&g_reachability_subsystems_lock);
			return &entry->mgr;
		}
	}

	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		pthread_mutex_unlock(&g_reachability_subsystems_lock);
		return NULL;
	}

	entry->subsystem = subsystem;
	entry->ref_count = 1;
	rc = cpcs_reachability_init(&entry->mgr);
	if (rc != 0) {
		free(entry);
		pthread_mutex_unlock(&g_reachability_subsystems_lock);
		return NULL;
	}

	TAILQ_INSERT_TAIL(&g_reachability_subsystems, entry, link);
	pthread_mutex_unlock(&g_reachability_subsystems_lock);

	return &entry->mgr;
}

void
cpcs_reachability_mgr_put(struct spdk_nvmf_subsystem *subsystem)
{
	struct cpcs_reachability_subsystem *entry;

	if (!subsystem) {
		return;
	}

	pthread_mutex_lock(&g_reachability_subsystems_lock);

	TAILQ_FOREACH(entry, &g_reachability_subsystems, link) {
		if (entry->subsystem != subsystem) {
			continue;
		}

		if (--entry->ref_count == 0) {
			TAILQ_REMOVE(&g_reachability_subsystems, entry, link);
			pthread_mutex_unlock(&g_reachability_subsystems_lock);
			cpcs_reachability_fini(&entry->mgr);
			free(entry);
			return;
		}

		break;
	}

	pthread_mutex_unlock(&g_reachability_subsystems_lock);
}

int
cpcs_reachability_create_group(struct cpcs_reachability_manager *mgr,
			       uint16_t *group_id_out)
{
	struct cpcs_reachability_group *group;

	if (!mgr || !group_id_out) {
		return -EINVAL;
	}

	group = calloc(1, sizeof(*group));
	if (!group) {
		return -ENOMEM;
	}

	pthread_mutex_lock(&mgr->lock);
	group->group_id = mgr->next_group_id++;
	TAILQ_INIT(&group->namespaces);
	group->ns_count = 0;

	TAILQ_INSERT_TAIL(&mgr->groups, group, link);
	*group_id_out = group->group_id;
	pthread_mutex_unlock(&mgr->lock);

	SPDK_NOTICELOG("Created reachability group %u\n", group->group_id);
	return 0;
}

int
cpcs_reachability_delete_group(struct cpcs_reachability_manager *mgr,
			       uint16_t group_id)
{
	struct cpcs_reachability_group *group;
	struct cpcs_reachability_ns_entry *entry, *tmp;

	if (!mgr) {
		return -EINVAL;
	}

	pthread_mutex_lock(&mgr->lock);

	TAILQ_FOREACH(group, &mgr->groups, link) {
		if (group->group_id == group_id) {
			/* Free all namespace entries */
			TAILQ_FOREACH_SAFE(entry, &group->namespaces, link, tmp) {
				TAILQ_REMOVE(&group->namespaces, entry, link);
				free(entry);
			}
			TAILQ_REMOVE(&mgr->groups, group, link);
			free(group);
			pthread_mutex_unlock(&mgr->lock);
			return 0;
		}
	}

	pthread_mutex_unlock(&mgr->lock);
	return -ENOENT;
}

int
cpcs_reachability_add_ns(struct cpcs_reachability_manager *mgr,
			 uint16_t group_id,
			 struct spdk_nvmf_ns *ns)
{
	struct cpcs_reachability_group *group;
	struct cpcs_reachability_ns_entry *entry;

	if (!mgr || !ns) {
		return -EINVAL;
	}

	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		return -ENOMEM;
	}

	entry->nsid = ns->nsid;
	entry->ns = ns;

	pthread_mutex_lock(&mgr->lock);

	/* Find the group */
	TAILQ_FOREACH(group, &mgr->groups, link) {
		if (group->group_id == group_id) {
			TAILQ_INSERT_TAIL(&group->namespaces, entry, link);
			group->ns_count++;
			pthread_mutex_unlock(&mgr->lock);
			return 0;
		}
	}

	pthread_mutex_unlock(&mgr->lock);
	free(entry);
	return -ENOENT;
}

int
cpcs_reachability_remove_ns(struct cpcs_reachability_manager *mgr,
			    uint16_t group_id,
			    uint32_t nsid)
{
	struct cpcs_reachability_group *group;
	struct cpcs_reachability_ns_entry *entry, *tmp;

	if (!mgr) {
		return -EINVAL;
	}

	pthread_mutex_lock(&mgr->lock);

	TAILQ_FOREACH(group, &mgr->groups, link) {
		if (group->group_id == group_id) {
			TAILQ_FOREACH_SAFE(entry, &group->namespaces, link, tmp) {
				if (entry->nsid == nsid) {
					TAILQ_REMOVE(&group->namespaces, entry, link);
					group->ns_count--;
					free(entry);
					pthread_mutex_unlock(&mgr->lock);
					return 0;
				}
			}
		}
	}

	pthread_mutex_unlock(&mgr->lock);
	return -ENOENT;
}

int
cpcs_reachability_create_association(struct cpcs_reachability_manager *mgr,
				     uint16_t *group_ids,
				     uint8_t group_count,
				     uint16_t *assoc_id_out)
{
	struct cpcs_reachability_association *assoc;
	size_t size;

	if (!mgr || !group_ids || group_count == 0 || !assoc_id_out) {
		return -EINVAL;
	}

	assoc = calloc(1, sizeof(*assoc));
	if (!assoc) {
		return -ENOMEM;
	}

	size = group_count * sizeof(uint16_t);
	assoc->group_ids = malloc(size);
	if (!assoc->group_ids) {
		free(assoc);
		return -ENOMEM;
	}

	memcpy(assoc->group_ids, group_ids, size);
	assoc->group_count = group_count;

	pthread_mutex_lock(&mgr->lock);
	assoc->assoc_id = mgr->next_assoc_id++;
	TAILQ_INSERT_TAIL(&mgr->associations, assoc, link);
	*assoc_id_out = assoc->assoc_id;
	pthread_mutex_unlock(&mgr->lock);

	return 0;
}

int
cpcs_reachability_delete_association(struct cpcs_reachability_manager *mgr,
				     uint16_t assoc_id)
{
	struct cpcs_reachability_association *assoc;

	if (!mgr) {
		return -EINVAL;
	}

	pthread_mutex_lock(&mgr->lock);

	TAILQ_FOREACH(assoc, &mgr->associations, link) {
		if (assoc->assoc_id == assoc_id) {
			TAILQ_REMOVE(&mgr->associations, assoc, link);
			free(assoc->group_ids);
			free(assoc);
			pthread_mutex_unlock(&mgr->lock);
			return 0;
		}
	}

	pthread_mutex_unlock(&mgr->lock);
	return -ENOENT;
}

int
cpcs_reachability_get_memory_ns(struct cpcs_reachability_manager *mgr,
				struct spdk_nvmf_cpcs_ns *compute_ns,
				uint32_t *memory_nsids,
				uint32_t *count)
{
	struct cpcs_reachability_association *assoc;
	struct cpcs_reachability_group *group;
	struct cpcs_reachability_ns_entry *entry;
	uint32_t idx = 0;
	bool assoc_has_group;
	uint8_t i;

	if (!mgr || !compute_ns || !memory_nsids || !count || *count == 0) {
		return -EINVAL;
	}

	pthread_mutex_lock(&mgr->lock);

	if (compute_ns->reach_group_id == 0) {
		pthread_mutex_unlock(&mgr->lock);
		*count = 0;
		return 0;
	}

	group = cpcs_reachability_find_group(mgr, compute_ns->reach_group_id);
	if (!group) {
		pthread_mutex_unlock(&mgr->lock);
		*count = 0;
		return -ENOENT;
	}

	TAILQ_FOREACH(entry, &group->namespaces, link) {
		if (entry->ns && entry->ns->csi == SPDK_NVME_CSI_CPCS) {
			continue;
		}

		if (idx >= *count) {
			break;
		}

		if (!cpcs_reachability_nsid_seen(memory_nsids, idx, entry->nsid)) {
			memory_nsids[idx++] = entry->nsid;
		}
	}

	TAILQ_FOREACH(assoc, &mgr->associations, link) {
		assoc_has_group = false;

		for (i = 0; i < assoc->group_count; i++) {
			if (assoc->group_ids[i] == group->group_id) {
				assoc_has_group = true;
				break;
			}
		}

		if (!assoc_has_group) {
			continue;
		}

		for (i = 0; i < assoc->group_count; i++) {
			group = cpcs_reachability_find_group(mgr, assoc->group_ids[i]);
			if (!group) {
				continue;
			}

			TAILQ_FOREACH(entry, &group->namespaces, link) {
				if (entry->ns && entry->ns->csi == SPDK_NVME_CSI_CPCS) {
					continue;
				}

				if (idx >= *count) {
					break;
				}

				if (!cpcs_reachability_nsid_seen(memory_nsids, idx, entry->nsid)) {
					memory_nsids[idx++] = entry->nsid;
				}
			}
		}
	}

	pthread_mutex_unlock(&mgr->lock);
	*count = idx;
	return 0;
}

bool
cpcs_reachability_is_memory_ns_reachable(struct cpcs_reachability_manager *mgr,
					 struct spdk_nvmf_cpcs_ns *compute_ns,
					 uint32_t mnsid)
{
	struct cpcs_reachability_association *assoc;
	struct cpcs_reachability_group *group;
	bool assoc_has_group;
	bool reachable = false;
	uint8_t i;

	if (!mgr || !compute_ns) {
		return false;
	}

	if (compute_ns->reach_group_id == 0) {
		return true;
	}

	pthread_mutex_lock(&mgr->lock);

	group = cpcs_reachability_find_group(mgr, compute_ns->reach_group_id);
	if (!group) {
		goto out;
	}

	if (cpcs_reachability_group_has_memory_nsid(group, mnsid)) {
		reachable = true;
		goto out;
	}

	TAILQ_FOREACH(assoc, &mgr->associations, link) {
		assoc_has_group = false;
		for (i = 0; i < assoc->group_count; i++) {
			if (assoc->group_ids[i] == group->group_id) {
				assoc_has_group = true;
				break;
			}
		}

		if (!assoc_has_group) {
			continue;
		}

		for (i = 0; i < assoc->group_count; i++) {
			group = cpcs_reachability_find_group(mgr, assoc->group_ids[i]);
			if (!group) {
				continue;
			}

			if (cpcs_reachability_group_has_memory_nsid(group, mnsid)) {
				reachable = true;
				goto out;
			}
		}
	}

out:
	pthread_mutex_unlock(&mgr->lock);
	return reachable;
}

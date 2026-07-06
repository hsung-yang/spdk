/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * CPCS RPC Commands
 *
 * Provides RPC interface for CPCS namespace and program management
 */

#include "nvmf_cpcs.h"
#include "builtin_programs.h"
#include "passthrough_runtime.h"
#include "program.h"
#include "program_activation.h"
#include "memory_range_set.h"

#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/nvmf.h"

/* Helper function to get subsystem by NQN */
static struct spdk_nvmf_subsystem *
get_subsystem_by_nqn(const char *nqn)
{
	struct spdk_nvmf_tgt *tgt;

	/* Get the first (and typically only) NVMf target */
	tgt = spdk_nvmf_get_first_tgt();
	if (!tgt) {
		return NULL;
	}

	return spdk_nvmf_tgt_find_subsystem(tgt, nqn);
}

/* RPC: cpcs_ns_create */
struct rpc_cpcs_ns_create {
	char *subsystem_nqn;
	uint32_t nsid;
	uint16_t max_activated;
	uint16_t max_mrs;
	uint16_t max_ranges_per_mrs;
	uint8_t mrs_granularity;
	uint32_t max_program_bytes;
	uint8_t load_program_gran;
	uint16_t reach_group_id;
};

static void
free_rpc_cpcs_ns_create(struct rpc_cpcs_ns_create *req)
{
	free(req->subsystem_nqn);
}

static const struct spdk_json_object_decoder rpc_cpcs_ns_create_decoders[] = {
	{"subsystem_nqn", offsetof(struct rpc_cpcs_ns_create, subsystem_nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_cpcs_ns_create, nsid), spdk_json_decode_uint32},
	{"max_activated", offsetof(struct rpc_cpcs_ns_create, max_activated), spdk_json_decode_uint16, true},
	{"max_mrs", offsetof(struct rpc_cpcs_ns_create, max_mrs), spdk_json_decode_uint16, true},
	{"max_ranges_per_mrs", offsetof(struct rpc_cpcs_ns_create, max_ranges_per_mrs), spdk_json_decode_uint16, true},
	{"mrs_granularity", offsetof(struct rpc_cpcs_ns_create, mrs_granularity), spdk_json_decode_uint8, true},
	{"max_program_bytes", offsetof(struct rpc_cpcs_ns_create, max_program_bytes), spdk_json_decode_uint32, true},
	{"load_program_gran", offsetof(struct rpc_cpcs_ns_create, load_program_gran), spdk_json_decode_uint8, true},
	{"reach_group_id", offsetof(struct rpc_cpcs_ns_create, reach_group_id), spdk_json_decode_uint16, true},
};

static void
rpc_cpcs_ns_create(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_cpcs_ns_create req = {};
	struct spdk_nvmf_cpcs_ns_opts opts;
	struct spdk_nvmf_cpcs_ns *ns;
	int rc;

	if (spdk_json_decode_object(params, rpc_cpcs_ns_create_decoders,
				    SPDK_COUNTOF(rpc_cpcs_ns_create_decoders), &req)) {
		SPDK_ERRLOG("Failed to decode RPC parameters\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	/* Initialize options with defaults */
	spdk_nvmf_cpcs_ns_opts_init(&opts);
	opts.nsid = req.nsid;

	/* Override with user-specified values */
	if (req.max_activated != 0) {
		opts.max_activated = req.max_activated;
	}
	if (req.max_mrs != 0) {
		opts.max_mrs = req.max_mrs;
	}
	if (req.max_ranges_per_mrs != 0) {
		opts.max_ranges_per_mrs = req.max_ranges_per_mrs;
	}
	if (req.mrs_granularity != 0) {
		opts.mrs_granularity = req.mrs_granularity;
	}
	if (req.max_program_bytes != 0) {
		opts.max_program_bytes = req.max_program_bytes;
	}
	if (req.load_program_gran != 0) {
		opts.load_program_gran = req.load_program_gran;
	}
	if (req.reach_group_id != 0) {
		opts.reach_group_id = req.reach_group_id;
	}

	/* Create compute namespace */
	struct spdk_nvmf_subsystem *subsystem = get_subsystem_by_nqn(req.subsystem_nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Subsystem not found: %s\n", req.subsystem_nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Subsystem not found: %s", req.subsystem_nqn);
		goto cleanup;
	}

	rc = spdk_nvmf_cpcs_ns_create(subsystem, &opts, &ns);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create CPCS namespace: %d\n", rc);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to create namespace: %d", rc);
		goto cleanup;
	}

	/* Send success response */
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_cpcs_ns_create(&req);
}
SPDK_RPC_REGISTER("cpcs_ns_create", rpc_cpcs_ns_create, SPDK_RPC_RUNTIME)

/* RPC: cpcs_ns_delete */
struct rpc_cpcs_ns_delete {
	char *subsystem_nqn;
	uint32_t nsid;
};

static void
free_rpc_cpcs_ns_delete(struct rpc_cpcs_ns_delete *req)
{
	free(req->subsystem_nqn);
}

static const struct spdk_json_object_decoder rpc_cpcs_ns_delete_decoders[] = {
	{"subsystem_nqn", offsetof(struct rpc_cpcs_ns_delete, subsystem_nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_cpcs_ns_delete, nsid), spdk_json_decode_uint32},
};

static void
rpc_cpcs_ns_delete(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_cpcs_ns_delete req = {};

	if (spdk_json_decode_object(params, rpc_cpcs_ns_delete_decoders,
				    SPDK_COUNTOF(rpc_cpcs_ns_delete_decoders), &req)) {
		SPDK_ERRLOG("Failed to decode RPC parameters\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	/* Delete compute namespace */
	struct spdk_nvmf_subsystem *subsystem = get_subsystem_by_nqn(req.subsystem_nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Subsystem not found: %s\n", req.subsystem_nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Subsystem not found: %s", req.subsystem_nqn);
		goto cleanup;
	}

	struct spdk_nvmf_cpcs_ns *ns = spdk_nvmf_cpcs_ns_get_by_nsid(subsystem, req.nsid);
	if (!ns) {
		SPDK_ERRLOG("Namespace not found: NSID %u\n", req.nsid);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Namespace not found: NSID %u", req.nsid);
		goto cleanup;
	}

	spdk_nvmf_cpcs_ns_destroy(ns);

	/* Send success response */
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_cpcs_ns_delete(&req);
}
SPDK_RPC_REGISTER("cpcs_ns_delete", rpc_cpcs_ns_delete, SPDK_RPC_RUNTIME)

/* RPC: cpcs_program_list */
struct rpc_cpcs_program_list {
	char *subsystem_nqn;
	uint32_t nsid;
};

static void
free_rpc_cpcs_program_list(struct rpc_cpcs_program_list *req)
{
	free(req->subsystem_nqn);
}

static const struct spdk_json_object_decoder rpc_cpcs_program_list_decoders[] = {
	{"subsystem_nqn", offsetof(struct rpc_cpcs_program_list, subsystem_nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_cpcs_program_list, nsid), spdk_json_decode_uint32},
};

static void
rpc_cpcs_program_list(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_cpcs_program_list req = {};
	struct spdk_nvmf_cpcs_ns *ns;
	struct spdk_json_write_ctx *w;
	uint16_t i;

	if (spdk_json_decode_object(params, rpc_cpcs_program_list_decoders,
				    SPDK_COUNTOF(rpc_cpcs_program_list_decoders), &req)) {
		SPDK_ERRLOG("Failed to decode RPC parameters\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	/* Get namespace */
	struct spdk_nvmf_subsystem *subsystem = get_subsystem_by_nqn(req.subsystem_nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Subsystem not found: %s\n", req.subsystem_nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Subsystem not found: %s", req.subsystem_nqn);
		goto cleanup;
	}

	ns = spdk_nvmf_cpcs_ns_get_by_nsid(subsystem, req.nsid);
	if (!ns) {
		SPDK_ERRLOG("Namespace not found: NSID %u\n", req.nsid);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Namespace not found: NSID %u", req.nsid);
		goto cleanup;
	}

	/* Build response */
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	for (i = 0; i < ns->max_programs; i++) {
		struct cpcs_program *prog = ns->programs[i];
		if (prog && prog->peocc != SPDK_NVME_CPCS_PEOCC_EMPTY) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_uint32(w, "pind", prog->pind);
			spdk_json_write_named_uint32(w, "ptype", prog->ptype);
			spdk_json_write_named_uint64(w, "puid", prog->puid);
			spdk_json_write_named_uint32(w, "total_size", prog->total_size);
			spdk_json_write_named_uint32(w, "loaded_bytes", prog->loaded_bytes);
			spdk_json_write_named_bool(w, "activated", prog->activated);
			spdk_json_write_named_uint32(w, "exec_count", prog->exec_count);
			spdk_json_write_object_end(w);
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_cpcs_program_list(&req);
}
SPDK_RPC_REGISTER("cpcs_program_list", rpc_cpcs_program_list, SPDK_RPC_RUNTIME)

/* RPC: cpcs_program_install_builtins */
struct rpc_cpcs_program_install_builtins {
	char *subsystem_nqn;
	uint32_t nsid;
};

static void
free_rpc_cpcs_program_install_builtins(struct rpc_cpcs_program_install_builtins *req)
{
	free(req->subsystem_nqn);
}

static const struct spdk_json_object_decoder rpc_cpcs_program_install_builtins_decoders[] = {
	{"subsystem_nqn", offsetof(struct rpc_cpcs_program_install_builtins, subsystem_nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_cpcs_program_install_builtins, nsid), spdk_json_decode_uint32},
};

static void
rpc_cpcs_program_install_builtins(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_cpcs_program_install_builtins req = {};
	struct spdk_nvmf_cpcs_ns *ns;
	int rc;

	if (spdk_json_decode_object(params, rpc_cpcs_program_install_builtins_decoders,
				    SPDK_COUNTOF(rpc_cpcs_program_install_builtins_decoders), &req)) {
		SPDK_ERRLOG("Failed to decode RPC parameters\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	struct spdk_nvmf_subsystem *subsystem = get_subsystem_by_nqn(req.subsystem_nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Subsystem not found: %s\n", req.subsystem_nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Subsystem not found: %s", req.subsystem_nqn);
		goto cleanup;
	}

	ns = spdk_nvmf_cpcs_ns_get_by_nsid(subsystem, req.nsid);
	if (!ns) {
		SPDK_ERRLOG("Namespace not found: NSID %u\n", req.nsid);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Namespace not found: NSID %u", req.nsid);
		goto cleanup;
	}

	rc = cpcs_program_install_builtins(ns);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to install builtins: %d\n", rc);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to install builtins: %d", rc);
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_cpcs_program_install_builtins(&req);
}
SPDK_RPC_REGISTER("cpcs_program_install_builtins", rpc_cpcs_program_install_builtins,
		  SPDK_RPC_RUNTIME)

/* RPC: cpcs_program_install_passthrough */
struct rpc_cpcs_program_install_passthrough {
	char *subsystem_nqn;
	uint32_t nsid;
	uint32_t pind;
};

static void
free_rpc_cpcs_program_install_passthrough(struct rpc_cpcs_program_install_passthrough *req)
{
	free(req->subsystem_nqn);
}

static const struct spdk_json_object_decoder rpc_cpcs_program_install_passthrough_decoders[] = {
	{"subsystem_nqn", offsetof(struct rpc_cpcs_program_install_passthrough, subsystem_nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_cpcs_program_install_passthrough, nsid), spdk_json_decode_uint32},
	{"pind", offsetof(struct rpc_cpcs_program_install_passthrough, pind), spdk_json_decode_uint32},
};

static void
rpc_cpcs_program_install_passthrough(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_cpcs_program_install_passthrough req = {};
	struct spdk_nvmf_cpcs_ns *ns;
	struct cpcs_program *prog;

	if (spdk_json_decode_object(params, rpc_cpcs_program_install_passthrough_decoders,
				     SPDK_COUNTOF(rpc_cpcs_program_install_passthrough_decoders), &req)) {
		SPDK_ERRLOG("Failed to decode RPC parameters\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	struct spdk_nvmf_subsystem *subsystem = get_subsystem_by_nqn(req.subsystem_nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Subsystem not found: %s\n", req.subsystem_nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						      "Subsystem not found: %s", req.subsystem_nqn);
		goto cleanup;
	}

	ns = spdk_nvmf_cpcs_ns_get_by_nsid(subsystem, req.nsid);
	if (!ns) {
		SPDK_ERRLOG("Namespace not found: NSID %u\n", req.nsid);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						      "Namespace not found: NSID %u", req.nsid);
		goto cleanup;
	}

	if (req.pind >= ns->max_programs) {
		SPDK_ERRLOG("PIND %u exceeds max_programs\n", req.pind);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						      "PIND %u exceeds max_programs", req.pind);
		goto cleanup;
	}

	pthread_mutex_lock(&ns->lock);
	if (ns->programs[req.pind] != NULL) {
		pthread_mutex_unlock(&ns->lock);
		SPDK_ERRLOG("PIND %u already occupied\n", req.pind);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						      "PIND %u already occupied", req.pind);
		goto cleanup;
	}

	prog = calloc(1, sizeof(*prog));
	if (prog == NULL) {
		pthread_mutex_unlock(&ns->lock);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation failed");
		goto cleanup;
	}

	pthread_mutex_init(&prog->lock, NULL);
	prog->pind = (uint16_t)req.pind;
	prog->ptype = CPCS_PTYPE_PASSTHROUGH;
	prog->pit = 0;
	prog->puid = 0;
	prog->peocc = SPDK_NVME_CPCS_PEOCC_DEVICE_DEFINED;
	prog->state = CPCS_PROGRAM_STATE_ACTIVATED;
	prog->activated = true;
	prog->ns = ns;

	ns->programs[req.pind] = prog;
	ns->num_programs++;
	ns->num_activated++;
	pthread_mutex_unlock(&ns->lock);

	SPDK_NOTICELOG("Installed passthrough program at PIND %u (ptype=0x%02x)\n",
		       req.pind, CPCS_PTYPE_PASSTHROUGH);
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_cpcs_program_install_passthrough(&req);
}
SPDK_RPC_REGISTER("cpcs_program_install_passthrough", rpc_cpcs_program_install_passthrough,
		  SPDK_RPC_RUNTIME)

/* RPC: cpcs_mrs_list */
struct rpc_cpcs_mrs_list {
	char *subsystem_nqn;
	uint32_t nsid;
};

static void
free_rpc_cpcs_mrs_list(struct rpc_cpcs_mrs_list *req)
{
	free(req->subsystem_nqn);
}

static const struct spdk_json_object_decoder rpc_cpcs_mrs_list_decoders[] = {
	{"subsystem_nqn", offsetof(struct rpc_cpcs_mrs_list, subsystem_nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_cpcs_mrs_list, nsid), spdk_json_decode_uint32},
};

static void
rpc_cpcs_mrs_list(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct rpc_cpcs_mrs_list req = {};
	struct spdk_nvmf_cpcs_ns *ns;
	struct cpcs_memory_range_set *mrs;
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_cpcs_mrs_list_decoders,
				    SPDK_COUNTOF(rpc_cpcs_mrs_list_decoders), &req)) {
		SPDK_ERRLOG("Failed to decode RPC parameters\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	/* Get namespace */
	struct spdk_nvmf_subsystem *subsystem = get_subsystem_by_nqn(req.subsystem_nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Subsystem not found: %s\n", req.subsystem_nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Subsystem not found: %s", req.subsystem_nqn);
		goto cleanup;
	}

	ns = spdk_nvmf_cpcs_ns_get_by_nsid(subsystem, req.nsid);
	if (!ns) {
		SPDK_ERRLOG("Namespace not found: NSID %u\n", req.nsid);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Namespace not found: NSID %u", req.nsid);
		goto cleanup;
	}

	/* Build response */
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(mrs, &ns->mrs_list, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint32(w, "rsid", mrs->rsid);
		spdk_json_write_named_uint32(w, "range_count", mrs->range_count);
		spdk_json_write_named_uint32(w, "ref_count", mrs->ref_count);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_cpcs_mrs_list(&req);
}
SPDK_RPC_REGISTER("cpcs_mrs_list", rpc_cpcs_mrs_list, SPDK_RPC_RUNTIME)

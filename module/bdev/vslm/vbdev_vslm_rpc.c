/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 CPCS Implementation Team. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/bdev_vslm.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

static int
rpc_decode_semantics_mode(const struct spdk_json_val *val, void *out)
{
	enum spdk_bdev_vslm_semantics_mode *mode = out;

	if (spdk_json_strequal(val, "lease")) {
		*mode = SPDK_BDEV_VSLM_SEMANTICS_LEASE;
		return 0;
	}

	if (spdk_json_strequal(val, "double_buffer")) {
		*mode = SPDK_BDEV_VSLM_SEMANTICS_DOUBLE_BUFFER;
		return 0;
	}

	return -EINVAL;
}

static int
rpc_decode_writeback_policy(const struct spdk_json_val *val, void *out)
{
	enum spdk_bdev_vslm_writeback_policy *policy = out;

	if (spdk_json_strequal(val, "on_evict")) {
		*policy = SPDK_BDEV_VSLM_WRITEBACK_ON_EVICT;
		return 0;
	}

	if (spdk_json_strequal(val, "at_boundary")) {
		*policy = SPDK_BDEV_VSLM_WRITEBACK_AT_BOUNDARY;
		return 0;
	}

	if (spdk_json_strequal(val, "hybrid")) {
		*policy = SPDK_BDEV_VSLM_WRITEBACK_HYBRID;
		return 0;
	}

	return -EINVAL;
}

static const char *
rpc_semantics_mode_to_string(enum spdk_bdev_vslm_semantics_mode mode)
{
	switch (mode) {
	case SPDK_BDEV_VSLM_SEMANTICS_LEASE:
		return "lease";
	case SPDK_BDEV_VSLM_SEMANTICS_DOUBLE_BUFFER:
		return "double_buffer";
	default:
		return "unknown";
	}
}

static const char *
rpc_writeback_policy_to_string(enum spdk_bdev_vslm_writeback_policy policy)
{
	switch (policy) {
	case SPDK_BDEV_VSLM_WRITEBACK_ON_EVICT:
		return "on_evict";
	case SPDK_BDEV_VSLM_WRITEBACK_AT_BOUNDARY:
		return "at_boundary";
	case SPDK_BDEV_VSLM_WRITEBACK_HYBRID:
		return "hybrid";
	default:
		return "unknown";
	}
}

struct rpc_bdev_vslm_create {
	char *name;
	char *base_bdev_name;
	uint64_t sram_size_mb;
	struct spdk_bdev_vslm_opts opts;
};

static void
free_rpc_bdev_vslm_create(struct rpc_bdev_vslm_create *req)
{
	free(req->name);
	free(req->base_bdev_name);
}

static const struct spdk_json_object_decoder rpc_bdev_vslm_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_vslm_create, name), spdk_json_decode_string},
	{"base_bdev_name", offsetof(struct rpc_bdev_vslm_create, base_bdev_name), spdk_json_decode_string},
	{"sram_size_mb", offsetof(struct rpc_bdev_vslm_create, sram_size_mb), spdk_json_decode_uint64},
	{"nsid", offsetof(struct rpc_bdev_vslm_create, opts.nsid), spdk_json_decode_uint32, true},
	{"fdp_mode_enabled", offsetof(struct rpc_bdev_vslm_create, opts.fdp_mode_enabled), spdk_json_decode_bool, true},
	{"fdp_dspec", offsetof(struct rpc_bdev_vslm_create, opts.fdp_dspec), spdk_json_decode_uint16, true},
	{"semantics_mode", offsetof(struct rpc_bdev_vslm_create, opts.policy.semantics_mode),
	 rpc_decode_semantics_mode, true},
	{"writeback_policy", offsetof(struct rpc_bdev_vslm_create, opts.policy.writeback_policy),
	 rpc_decode_writeback_policy, true},
	{"admission_enabled", offsetof(struct rpc_bdev_vslm_create, opts.policy.admission_enabled),
	 spdk_json_decode_bool, true},
	{"admission_faults_per_sec_threshold",
	 offsetof(struct rpc_bdev_vslm_create, opts.policy.admission_faults_per_sec_threshold),
	 spdk_json_decode_uint32, true},
};

static void
rpc_bdev_vslm_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_vslm_create req = {0};
	uint64_t sram_size_bytes;
	int rc;

	bdev_vslm_opts_init(&req.opts);
	if (spdk_json_decode_object(params, rpc_bdev_vslm_create_decoders,
				     SPDK_COUNTOF(rpc_bdev_vslm_create_decoders),
				     &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						  "spdk_json_decode_object failed");
		goto cleanup;
	}

	sram_size_bytes = req.sram_size_mb * 1024 * 1024;
	rc = bdev_vslm_create_with_opts(req.name, req.base_bdev_name, sram_size_bytes, &req.opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_vslm_create(&req);
}
SPDK_RPC_REGISTER("bdev_vslm_create", rpc_bdev_vslm_create, SPDK_RPC_RUNTIME)

struct rpc_bdev_vslm_delete {
	char *name;
};

static void
free_rpc_bdev_vslm_delete(struct rpc_bdev_vslm_delete *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_vslm_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_vslm_delete, name), spdk_json_decode_string},
};

static void
rpc_bdev_vslm_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_vslm_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_vslm_delete req = {NULL};

	if (spdk_json_decode_object(params, rpc_bdev_vslm_delete_decoders,
				     SPDK_COUNTOF(rpc_bdev_vslm_delete_decoders),
				     &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						  "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_vslm_delete(req.name, rpc_bdev_vslm_delete_cb, request);

cleanup:
	free_rpc_bdev_vslm_delete(&req);
}
SPDK_RPC_REGISTER("bdev_vslm_delete", rpc_bdev_vslm_delete, SPDK_RPC_RUNTIME)

struct rpc_bdev_vslm_lease_acquire {
	uint64_t lease_id;
	uint32_t nsid;
	uint64_t offset;
	uint64_t length;
};

static const struct spdk_json_object_decoder rpc_bdev_vslm_lease_acquire_decoders[] = {
	{"lease_id", offsetof(struct rpc_bdev_vslm_lease_acquire, lease_id), spdk_json_decode_uint64},
	{"nsid", offsetof(struct rpc_bdev_vslm_lease_acquire, nsid), spdk_json_decode_uint32},
	{"offset", offsetof(struct rpc_bdev_vslm_lease_acquire, offset), spdk_json_decode_uint64},
	{"length", offsetof(struct rpc_bdev_vslm_lease_acquire, length), spdk_json_decode_uint64},
};

static void
rpc_bdev_vslm_lease_acquire(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_bdev_vslm_lease_acquire req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_vslm_lease_acquire_decoders,
				    SPDK_COUNTOF(rpc_bdev_vslm_lease_acquire_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = bdev_vslm_lease_acquire(req.lease_id, req.nsid, req.offset, req.length);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("bdev_vslm_lease_acquire", rpc_bdev_vslm_lease_acquire, SPDK_RPC_RUNTIME)

struct rpc_bdev_vslm_lease_release {
	uint64_t lease_id;
};

static const struct spdk_json_object_decoder rpc_bdev_vslm_lease_release_decoders[] = {
	{"lease_id", offsetof(struct rpc_bdev_vslm_lease_release, lease_id), spdk_json_decode_uint64},
};

static void
rpc_bdev_vslm_lease_release(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_bdev_vslm_lease_release req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_vslm_lease_release_decoders,
				    SPDK_COUNTOF(rpc_bdev_vslm_lease_release_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = bdev_vslm_lease_release(req.lease_id);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("bdev_vslm_lease_release", rpc_bdev_vslm_lease_release, SPDK_RPC_RUNTIME)

struct rpc_bdev_vslm_set_policy {
	char *name;
	struct spdk_bdev_vslm_policy policy;
};

static void
free_rpc_bdev_vslm_set_policy(struct rpc_bdev_vslm_set_policy *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_vslm_set_policy_decoders[] = {
	{"name", offsetof(struct rpc_bdev_vslm_set_policy, name), spdk_json_decode_string},
	{"semantics_mode", offsetof(struct rpc_bdev_vslm_set_policy, policy.semantics_mode),
	 rpc_decode_semantics_mode},
	{"writeback_policy", offsetof(struct rpc_bdev_vslm_set_policy, policy.writeback_policy),
	 rpc_decode_writeback_policy},
	{"admission_enabled", offsetof(struct rpc_bdev_vslm_set_policy, policy.admission_enabled),
	 spdk_json_decode_bool},
	{"admission_faults_per_sec_threshold",
	 offsetof(struct rpc_bdev_vslm_set_policy, policy.admission_faults_per_sec_threshold),
	 spdk_json_decode_uint32, true},
};

static void
rpc_bdev_vslm_set_policy(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_vslm_set_policy req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_vslm_set_policy_decoders,
				    SPDK_COUNTOF(rpc_bdev_vslm_set_policy_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_vslm_set_policy(req.name, &req.policy);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_vslm_set_policy(&req);
}
SPDK_RPC_REGISTER("bdev_vslm_set_policy", rpc_bdev_vslm_set_policy, SPDK_RPC_RUNTIME)

struct rpc_bdev_vslm_get_policy {
	char *name;
};

static void
free_rpc_bdev_vslm_get_policy(struct rpc_bdev_vslm_get_policy *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_vslm_get_policy_decoders[] = {
	{"name", offsetof(struct rpc_bdev_vslm_get_policy, name), spdk_json_decode_string},
};

static void
rpc_bdev_vslm_get_policy(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_vslm_get_policy req = {0};
	struct spdk_bdev_vslm_policy policy = {};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_vslm_get_policy_decoders,
				    SPDK_COUNTOF(rpc_bdev_vslm_get_policy_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_vslm_get_policy(req.name, &policy);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", req.name);
	spdk_json_write_named_string(w, "semantics_mode",
				     rpc_semantics_mode_to_string(policy.semantics_mode));
	spdk_json_write_named_string(w, "writeback_policy",
				     rpc_writeback_policy_to_string(policy.writeback_policy));
	spdk_json_write_named_bool(w, "admission_enabled", policy.admission_enabled);
	spdk_json_write_named_uint32(w, "admission_faults_per_sec_threshold",
				     policy.admission_faults_per_sec_threshold);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_vslm_get_policy(&req);
}
SPDK_RPC_REGISTER("bdev_vslm_get_policy", rpc_bdev_vslm_get_policy, SPDK_RPC_RUNTIME)

struct rpc_bdev_vslm_set_fdp_mode {
	char *name;
	bool enabled;
	uint16_t dspec;
};

static void
free_rpc_bdev_vslm_set_fdp_mode(struct rpc_bdev_vslm_set_fdp_mode *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_vslm_set_fdp_mode_decoders[] = {
	{"name", offsetof(struct rpc_bdev_vslm_set_fdp_mode, name), spdk_json_decode_string},
	{"enabled", offsetof(struct rpc_bdev_vslm_set_fdp_mode, enabled), spdk_json_decode_bool},
	{"dspec", offsetof(struct rpc_bdev_vslm_set_fdp_mode, dspec), spdk_json_decode_uint16, true},
};

static void
rpc_bdev_vslm_set_fdp_mode(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_bdev_vslm_set_fdp_mode req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_vslm_set_fdp_mode_decoders,
				    SPDK_COUNTOF(rpc_bdev_vslm_set_fdp_mode_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_vslm_set_fdp_mode(req.name, req.enabled, req.dspec);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_vslm_set_fdp_mode(&req);
}
SPDK_RPC_REGISTER("bdev_vslm_set_fdp_mode", rpc_bdev_vslm_set_fdp_mode, SPDK_RPC_RUNTIME)

struct rpc_bdev_vslm_get_stats {
	char *name;
};

static void
free_rpc_bdev_vslm_get_stats(struct rpc_bdev_vslm_get_stats *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_vslm_get_stats_decoders[] = {
	{"name", offsetof(struct rpc_bdev_vslm_get_stats, name), spdk_json_decode_string},
};

static void
rpc_bdev_vslm_get_stats(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_bdev_vslm_get_stats req = {0};
	struct spdk_bdev_vslm_stats stats = {0};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_vslm_get_stats_decoders,
				    SPDK_COUNTOF(rpc_bdev_vslm_get_stats_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_vslm_get_stats(req.name, &stats);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", req.name);
	spdk_json_write_named_uint64(w, "backing_write_ops", stats.backing_write_ops);
	spdk_json_write_named_uint64(w, "backing_write_bytes", stats.backing_write_bytes);
	spdk_json_write_named_uint64(w, "backing_write_tagged_bytes", stats.backing_write_tagged_bytes);
	spdk_json_write_named_uint64(w, "backing_write_untagged_bytes", stats.backing_write_untagged_bytes);
	spdk_json_write_named_uint64(w, "page_faults", stats.page_faults);
	spdk_json_write_named_uint64(w, "page_fault_bytes", stats.page_fault_bytes);
	spdk_json_write_named_uint64(w, "page_evictions", stats.page_evictions);
	spdk_json_write_named_uint64(w, "page_eviction_bytes", stats.page_eviction_bytes);
	spdk_json_write_named_uint64(w, "page_writebacks", stats.page_writebacks);
	spdk_json_write_named_uint64(w, "page_writeback_bytes", stats.page_writeback_bytes);
	spdk_json_write_named_uint64(w, "dirty_resident_pages", stats.dirty_resident_pages);
	spdk_json_write_named_uint64(w, "dirty_writeback_bytes", stats.dirty_writeback_bytes);
	spdk_json_write_named_uint64(w, "lease_conflicts", stats.lease_conflicts);
	spdk_json_write_named_uint64(w, "lease_blocked_ns", stats.lease_blocked_ns);
	spdk_json_write_named_uint64(w, "admission_rejects", stats.admission_rejects);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_vslm_get_stats(&req);
}
SPDK_RPC_REGISTER("bdev_vslm_get_stats", rpc_bdev_vslm_get_stats, SPDK_RPC_RUNTIME)

struct rpc_bdev_vslm_reset_stats {
	char *name;
};

static void
free_rpc_bdev_vslm_reset_stats(struct rpc_bdev_vslm_reset_stats *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_vslm_reset_stats_decoders[] = {
	{"name", offsetof(struct rpc_bdev_vslm_reset_stats, name), spdk_json_decode_string},
};

static void
rpc_bdev_vslm_reset_stats(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_vslm_reset_stats req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_vslm_reset_stats_decoders,
				    SPDK_COUNTOF(rpc_bdev_vslm_reset_stats_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_vslm_reset_stats(req.name);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_vslm_reset_stats(&req);
}
SPDK_RPC_REGISTER("bdev_vslm_reset_stats", rpc_bdev_vslm_reset_stats, SPDK_RPC_RUNTIME)

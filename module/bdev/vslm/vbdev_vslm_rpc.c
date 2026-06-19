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

	return -EINVAL;
}

static const char *
rpc_semantics_mode_to_string(enum spdk_bdev_vslm_semantics_mode mode)
{
	switch (mode) {
	case SPDK_BDEV_VSLM_SEMANTICS_LEASE:
		return "lease";
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
	free(req->opts.trace_output_path);
	free(req->opts.trace_run_id);
}

static const struct spdk_json_object_decoder rpc_bdev_vslm_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_vslm_create, name), spdk_json_decode_string},
	{"base_bdev_name", offsetof(struct rpc_bdev_vslm_create, base_bdev_name), spdk_json_decode_string},
	{"sram_size_mb", offsetof(struct rpc_bdev_vslm_create, sram_size_mb), spdk_json_decode_uint64},
	{"nsid", offsetof(struct rpc_bdev_vslm_create, opts.nsid), spdk_json_decode_uint32, true},
	{"fdp_mode_enabled", offsetof(struct rpc_bdev_vslm_create, opts.fdp_mode_enabled), spdk_json_decode_bool, true},
	{"fdp_dspec", offsetof(struct rpc_bdev_vslm_create, opts.fdp_dspec), spdk_json_decode_uint16, true},
	{
		"semantics_mode", offsetof(struct rpc_bdev_vslm_create, opts.policy.semantics_mode),
		rpc_decode_semantics_mode, true
	},
	{
		"writeback_policy", offsetof(struct rpc_bdev_vslm_create, opts.policy.writeback_policy),
		rpc_decode_writeback_policy, true
	},
	{
		"admission_enabled", offsetof(struct rpc_bdev_vslm_create, opts.policy.admission_enabled),
		spdk_json_decode_bool, true
	},
	{
		"admission_faults_per_sec_threshold",
		offsetof(struct rpc_bdev_vslm_create, opts.policy.admission_faults_per_sec_threshold),
		spdk_json_decode_uint32, true
	},
	{
		"readahead_enabled", offsetof(struct rpc_bdev_vslm_create, opts.policy.readahead_enabled),
		spdk_json_decode_bool, true
	},
	{
		"readahead_pages", offsetof(struct rpc_bdev_vslm_create, opts.policy.readahead_pages),
		spdk_json_decode_uint32, true
	},
	{
		"backing_region_enabled", offsetof(struct rpc_bdev_vslm_create, opts.backing_region_enabled),
		spdk_json_decode_bool, true
	},
	{
		"trace_events_enabled", offsetof(struct rpc_bdev_vslm_create, opts.trace_events_enabled),
		spdk_json_decode_bool, true
	},
	{
		"trace_output_path", offsetof(struct rpc_bdev_vslm_create, opts.trace_output_path),
		spdk_json_decode_string, true
	},
	{
		"trace_run_id", offsetof(struct rpc_bdev_vslm_create, opts.trace_run_id),
		spdk_json_decode_string, true
	},
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
	{
		"semantics_mode", offsetof(struct rpc_bdev_vslm_set_policy, policy.semantics_mode),
		rpc_decode_semantics_mode
	},
	{
		"writeback_policy", offsetof(struct rpc_bdev_vslm_set_policy, policy.writeback_policy),
		rpc_decode_writeback_policy
	},
	{
		"admission_enabled", offsetof(struct rpc_bdev_vslm_set_policy, policy.admission_enabled),
		spdk_json_decode_bool
	},
	{
		"admission_faults_per_sec_threshold",
		offsetof(struct rpc_bdev_vslm_set_policy, policy.admission_faults_per_sec_threshold),
		spdk_json_decode_uint32, true
	},
	{
		"readahead_enabled", offsetof(struct rpc_bdev_vslm_set_policy, policy.readahead_enabled),
		spdk_json_decode_bool, true
	},
	{
		"readahead_pages", offsetof(struct rpc_bdev_vslm_set_policy, policy.readahead_pages),
		spdk_json_decode_uint32, true
	},
};

static void
rpc_bdev_vslm_set_policy(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_vslm_set_policy req = {0};
	struct spdk_bdev_vslm_opts opts = {};
	int rc;

	bdev_vslm_opts_init(&opts);
	req.policy = opts.policy;

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
	spdk_json_write_named_bool(w, "readahead_enabled", policy.readahead_enabled);
	spdk_json_write_named_uint32(w, "readahead_pages", policy.readahead_pages);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_vslm_get_policy(&req);
}
SPDK_RPC_REGISTER("bdev_vslm_get_policy", rpc_bdev_vslm_get_policy, SPDK_RPC_RUNTIME)

struct rpc_bdev_vslm_set_debug {
	char *name;
	struct spdk_bdev_vslm_debug dbg;
};

static void
free_rpc_bdev_vslm_set_debug(struct rpc_bdev_vslm_set_debug *req)
{
	free(req->name);
}

/*
 * Decode a tri-state debug knob. The CLI sends a JSON boolean (true/false) via
 * the generic client dispatcher, while programmatic callers (the Python binding,
 * the benchmark harness) may send a JSON number (0/1). Accept either and
 * normalize to the int tri-state (1 = on, 0 = off). Absence leaves the field at
 * its bdev_vslm_debug_init() default of -1 (unchanged).
 */
static int
rpc_decode_vslm_tristate(const struct spdk_json_val *val, void *out)
{
	int *dst = out;
	bool enabled;
	int32_t num;

	if (spdk_json_decode_bool(val, &enabled) == 0) {
		*dst = enabled ? 1 : 0;
		return 0;
	}
	if (spdk_json_decode_int32(val, &num) == 0) {
		*dst = num ? 1 : 0;
		return 0;
	}
	return -EINVAL;
}

static const struct spdk_json_object_decoder rpc_bdev_vslm_set_debug_decoders[] = {
	{"name", offsetof(struct rpc_bdev_vslm_set_debug, name), spdk_json_decode_string},
	{
		"num_shards", offsetof(struct rpc_bdev_vslm_set_debug, dbg.num_shards),
		spdk_json_decode_uint32, true
	},
	{
		"async_exec", offsetof(struct rpc_bdev_vslm_set_debug, dbg.async_exec),
		rpc_decode_vslm_tristate, true
	},
	{
		"fault_batch", offsetof(struct rpc_bdev_vslm_set_debug, dbg.fault_batch),
		rpc_decode_vslm_tristate, true
	},
	{
		"prefetch_batch", offsetof(struct rpc_bdev_vslm_set_debug, dbg.prefetch_batch),
		rpc_decode_vslm_tristate, true
	},
	{
		"background_cleaner", offsetof(struct rpc_bdev_vslm_set_debug, dbg.background_cleaner),
		rpc_decode_vslm_tristate, true
	},
	{
		"streaming_mode", offsetof(struct rpc_bdev_vslm_set_debug, dbg.streaming_mode),
		rpc_decode_vslm_tristate, true
	},
};

static void
rpc_bdev_vslm_set_debug(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_bdev_vslm_set_debug req = {0};
	int rc;

	bdev_vslm_debug_init(&req.dbg);
	if (spdk_json_decode_object(params, rpc_bdev_vslm_set_debug_decoders,
				    SPDK_COUNTOF(rpc_bdev_vslm_set_debug_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_vslm_set_debug(req.name, &req.dbg);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_vslm_set_debug(&req);
}
SPDK_RPC_REGISTER("bdev_vslm_set_debug", rpc_bdev_vslm_set_debug, SPDK_RPC_RUNTIME)

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
	spdk_json_write_named_uint64(w, "vslm_fault_clean_total", stats.vslm_fault_clean_total);
	spdk_json_write_named_uint64(w, "vslm_fault_private_total", stats.vslm_fault_private_total);
	spdk_json_write_named_uint64(w, "page_evictions", stats.page_evictions);
	spdk_json_write_named_uint64(w, "page_eviction_bytes", stats.page_eviction_bytes);
	spdk_json_write_named_uint64(w, "vslm_eviction_clean_total", stats.vslm_eviction_clean_total);
	spdk_json_write_named_uint64(w, "vslm_eviction_private_total", stats.vslm_eviction_private_total);
	spdk_json_write_named_uint64(w, "page_writebacks", stats.page_writebacks);
	spdk_json_write_named_uint64(w, "page_writeback_bytes", stats.page_writeback_bytes);
	spdk_json_write_named_uint64(w, "vslm_spill_write_bytes", stats.vslm_spill_write_bytes);
	spdk_json_write_named_uint64(w, "vslm_spill_read_bytes", stats.vslm_spill_read_bytes);
	spdk_json_write_named_uint64(w, "vslm_fast_tier_bytes", stats.vslm_fast_tier_bytes);
	spdk_json_write_named_uint64(w, "vslm_logical_working_set_bytes",
				     stats.vslm_logical_working_set_bytes);
	spdk_json_write_named_uint64(w, "dirty_resident_pages", stats.dirty_resident_pages);
	spdk_json_write_named_uint64(w, "vslm_resident_bytes_current",
				     stats.vslm_resident_bytes_current);
	spdk_json_write_named_uint64(w, "vslm_resident_bytes_peak",
				     stats.vslm_resident_bytes_peak);
	spdk_json_write_named_uint64(w, "dirty_writeback_bytes", stats.dirty_writeback_bytes);
	spdk_json_write_named_uint64(w, "lease_conflicts", stats.lease_conflicts);
	spdk_json_write_named_uint64(w, "lease_blocked_ns", stats.lease_blocked_ns);
	spdk_json_write_named_uint64(w, "admission_rejects", stats.admission_rejects);
	spdk_json_write_named_uint64(w, "vslm_lease_create_total", stats.vslm_lease_create_total);
	spdk_json_write_named_uint64(w, "vslm_lease_release_total", stats.vslm_lease_release_total);
	spdk_json_write_named_uint64(w, "vslm_host_read_during_execution_total",
				     stats.vslm_host_read_during_execution_total);
	spdk_json_write_named_uint64(w, "vslm_host_write_conflict_total",
				     stats.vslm_host_write_conflict_total);
	spdk_json_write_named_uint64(w, "vslm_host_write_blocked_total",
				     stats.vslm_host_write_blocked_total);
	spdk_json_write_named_uint64(w, "vslm_host_write_blocked_ns_total", stats.lease_blocked_ns);
	spdk_json_write_named_uint64(w, "vslm_host_write_nonconflict_total",
				     stats.vslm_host_write_nonconflict_total);
	spdk_json_write_named_uint64(w, "vslm_publish_total", stats.vslm_publish_total);
	spdk_json_write_named_uint64(w, "vslm_discard_total", stats.vslm_discard_total);
	spdk_json_write_named_uint64(w, "vslm_visibility_violation_total",
				     stats.vslm_visibility_violation_total);
	spdk_json_write_named_uint64(w, "perf_fault_in_io_ns_total",
				     stats.perf_fault_in_io_ns_total);
	spdk_json_write_named_uint64(w, "perf_fault_in_io_ns_max",
				     stats.perf_fault_in_io_ns_max);
	spdk_json_write_named_uint64(w, "perf_mmu_lock_acquire_total",
				     stats.perf_mmu_lock_acquire_total);
	spdk_json_write_named_uint64(w, "perf_mmu_lock_hold_ns_total",
				     stats.perf_mmu_lock_hold_ns_total);
	spdk_json_write_named_uint64(w, "perf_mmu_lock_hold_ns_max",
				     stats.perf_mmu_lock_hold_ns_max);
	spdk_json_write_named_uint64(w, "perf_sync_base_io_total",
				     stats.perf_sync_base_io_total);
	spdk_json_write_named_uint64(w, "perf_sync_base_io_ns_total",
				     stats.perf_sync_base_io_ns_total);
	spdk_json_write_named_uint64(w, "perf_sync_base_io_ns_max",
				     stats.perf_sync_base_io_ns_max);
	spdk_json_write_named_uint64(w, "perf_sync_read_desc_io_total",
				     stats.perf_sync_read_desc_io_total);
	spdk_json_write_named_uint64(w, "perf_sync_read_desc_io_ns_total",
				     stats.perf_sync_read_desc_io_ns_total);
	spdk_json_write_named_uint64(w, "perf_sync_read_desc_io_ns_max",
				     stats.perf_sync_read_desc_io_ns_max);
	spdk_json_write_named_uint64(w, "perf_fault_batch_attempt_total",
				     stats.perf_fault_batch_attempt_total);
	spdk_json_write_named_uint64(w, "perf_fault_batch_fallback_total",
				     stats.perf_fault_batch_fallback_total);
	spdk_json_write_named_uint64(w, "perf_prefetch_async_wait_total",
				     stats.perf_prefetch_async_wait_total);
	spdk_json_write_named_uint64(w, "perf_prefetch_async_wait_ns_total",
				     stats.perf_prefetch_async_wait_ns_total);
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

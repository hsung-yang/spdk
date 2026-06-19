/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/bdev_slm.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct rpc_bdev_slm_create {
	char *name;
	uint32_t nsid;
	uint64_t size_mb;
	uint32_t granularity;
};

static void
free_rpc_bdev_slm_create(struct rpc_bdev_slm_create *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_slm_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_slm_create, name), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_bdev_slm_create, nsid), spdk_json_decode_uint32},
	{"size_mb", offsetof(struct rpc_bdev_slm_create, size_mb), spdk_json_decode_uint64},
	{"granularity", offsetof(struct rpc_bdev_slm_create, granularity), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_slm_create(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_slm_create req = {0};
	int rc;

	/* Set default granularity */
	req.granularity = 4;

	if (spdk_json_decode_object(params, rpc_bdev_slm_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_slm_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_slm_create(req.name, req.nsid, req.size_mb, req.granularity);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_slm_create(&req);
}
SPDK_RPC_REGISTER("bdev_slm_create", rpc_bdev_slm_create, SPDK_RPC_RUNTIME)

struct rpc_bdev_slm_delete {
	char *name;
};

static void
free_rpc_bdev_slm_delete(struct rpc_bdev_slm_delete *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_slm_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_slm_delete, name), spdk_json_decode_string},
};

static void
_rpc_bdev_slm_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_slm_delete(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_slm_delete req = {NULL};

	if (spdk_json_decode_object(params, rpc_bdev_slm_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_slm_delete_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_slm_delete(req.name, _rpc_bdev_slm_delete_cb, request);

cleanup:
	free_rpc_bdev_slm_delete(&req);
}
SPDK_RPC_REGISTER("bdev_slm_delete", rpc_bdev_slm_delete, SPDK_RPC_RUNTIME)

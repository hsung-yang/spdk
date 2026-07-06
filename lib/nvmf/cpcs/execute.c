/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

#include "execute.h"
#include "program.h"
#include "runtime.h"
#include "spdk/nvme_cpcs_spec.h"
#include "spdk/bdev_slm.h"
#include "spdk/log.h"
#include "spdk/nvmf_cmd.h"
#include "spdk/nvmf_transport.h"
#include "spdk/thread.h"

static void cpcs_execute_runtime_done(void *cb_arg, int status, uint64_t return_value);

struct cpcs_execute_wait_ctx {
	bool done;
	int status;
	uint64_t return_value;
};

static void
cpcs_execute_runtime_wait_done(void *cb_arg, int status, uint64_t return_value)
{
	struct cpcs_execute_wait_ctx *wait_ctx = cb_arg;

	wait_ctx->done = true;
	wait_ctx->status = status;
	wait_ctx->return_value = return_value;
}

static int
cpcs_execute_acquire_slm_range(struct cpcs_exec_context *ctx,
			       struct spdk_bdev *bdev,
			       uint64_t starting_byte, uint64_t length)
{
	int rc;

	rc = bdev_slm_lease_acquire_by_bdev(ctx->slm_lease_id, bdev, starting_byte, length);
	if (rc == -ENOENT || rc == -ENOTSUP) {
		/* Namespace provider has no lease support. */
		return 0;
	}
	if (rc == -EAGAIN) {
		return -SPDK_NVME_CPCS_SC_MEMORY_RANGE_SET_IN_USE;
	}
	if (rc != 0) {
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	ctx->slm_lease_acquired = true;
	return 0;
}

static int
cpcs_execute_acquire_slm_leases(struct cpcs_exec_context *ctx)
{
	const struct cpcs_exec_resolved_range *range;
	uint32_t i;
	int rc;

	if (ctx == NULL) {
		return -EINVAL;
	}

	ctx->slm_lease_id = (uint64_t)(uintptr_t)ctx;
	ctx->slm_lease_acquired = false;

	for (i = 0; i < ctx->resolved_range_count; i++) {
		range = &ctx->resolved_ranges[i];
		rc = cpcs_execute_acquire_slm_range(ctx, range->bdev, range->starting_byte, range->length);
		if (rc != 0) {
			bdev_slm_lease_release(ctx->slm_lease_id);
			ctx->slm_lease_acquired = false;
			return rc;
		}
	}

	return 0;
}

static int
cpcs_execute_finalize_slm_lease(struct cpcs_exec_context *ctx, int status)
{
	int rc;
	int release_rc;

	if (ctx == NULL || !ctx->slm_lease_acquired) {
		return status;
	}

	if (status == SPDK_NVME_SC_SUCCESS) {
		rc = bdev_slm_exec_publish_lease(ctx->slm_lease_id);
		if (rc != 0) {
			SPDK_ERRLOG("SLM publish failed for lease=%" PRIu64 " rc=%d\n",
				    ctx->slm_lease_id, rc);
			status = -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	} else {
		rc = bdev_slm_exec_discard_lease(ctx->slm_lease_id);
		if (rc != 0) {
			SPDK_ERRLOG("SLM discard failed for lease=%" PRIu64 " rc=%d\n",
				    ctx->slm_lease_id, rc);
			status = -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}

	release_rc = bdev_slm_lease_release(ctx->slm_lease_id);
	ctx->slm_lease_acquired = false;
	if (release_rc != 0 && release_rc != -ENOENT) {
		SPDK_ERRLOG("SLM lease release failed for lease=%" PRIu64 " rc=%d\n",
			    ctx->slm_lease_id, release_rc);
		status = -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	return status;
}

static int
cpcs_execute_resolve_ranges(struct cpcs_exec_context *ctx)
{
	const struct cpcs_memory_range *ranges;
	struct cpcs_exec_resolved_range *resolved_ranges;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	uint32_t range_count;
	uint32_t i;
	int rc;

	if (ctx->mrs != NULL) {
		range_count = ctx->mrs->range_count;
		ranges = ctx->mrs->ranges;
	} else if (ctx->inline_ranges != NULL && ctx->inline_range_count != 0) {
		range_count = ctx->inline_range_count;
		ranges = ctx->inline_ranges;
	} else {
		ctx->resolved_ranges = NULL;
		ctx->resolved_range_count = 0;
		return 0;
	}

	resolved_ranges = calloc(range_count, sizeof(*resolved_ranges));
	if (resolved_ranges == NULL) {
		return -ENOMEM;
	}

	for (i = 0; i < range_count; i++) {
		rc = spdk_nvmf_request_get_bdev(ranges[i].mnsid, ctx->req, &bdev, &desc, &ch);
		if (rc != 0 || bdev == NULL) {
			free(resolved_ranges);
			return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
		}

		resolved_ranges[i].bdev = bdev;
		resolved_ranges[i].mnsid = ranges[i].mnsid;
		resolved_ranges[i].starting_byte = ranges[i].starting_byte;
		resolved_ranges[i].length = ranges[i].length;
	}

	ctx->resolved_ranges = resolved_ranges;
	ctx->resolved_range_count = range_count;
	return 0;
}

/*
 * Decrement the program's in-flight execution count under ns->lock, matching
 * the increment in cpcs_execute_run(). Keeping both sides under the same lock
 * means cpcs_program_unload()/deactivate() (which read exec_count under
 * ns->lock) never see a torn or stale value.
 */
static void
cpcs_execute_dec_exec_count(struct cpcs_exec_context *ctx)
{
	if (ctx == NULL || ctx->program == NULL) {
		return;
	}

	/*
	 * Mirror the increment in cpcs_execute_run(): use ns->lock when an ns is
	 * present, otherwise (test-harness path) fall back to an atomic decrement.
	 */
	if (ctx->ns != NULL) {
		pthread_mutex_lock(&ctx->ns->lock);
		if (ctx->program->exec_count > 0) {
			ctx->program->exec_count--;
		}
		pthread_mutex_unlock(&ctx->ns->lock);
	} else {
		if (ctx->program->exec_count > 0) {
			__atomic_sub_fetch(&ctx->program->exec_count, 1, __ATOMIC_SEQ_CST);
		}
	}
}

int
cpcs_execute_program_cmd(struct spdk_nvmf_request *req)
{
	struct cpcs_exec_context *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->req = req;
	ctx->completion_done = false;
	ctx->runtime_pending = false;

	/* Parse command */
	rc = cpcs_execute_parse_cmd(req, ctx);
	if (rc != 0) {
		goto error;
	}

	/* Setup memory access */
	rc = cpcs_execute_setup_memory(ctx);
	if (rc != 0) {
		goto error;
	}

	rc = cpcs_execute_acquire_slm_leases(ctx);
	if (rc != 0) {
		goto error;
	}

	/* Run program */
	rc = cpcs_execute_run(ctx);
	if (rc != 0) {
		goto error;
	}
	return 0;

error:
	cpcs_execute_complete(ctx, rc);
	return rc;
}

int
cpcs_execute_parse_cmd(struct spdk_nvmf_request *req,
		       struct cpcs_exec_context *ctx)
{
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
	struct spdk_nvmf_subsystem *subsystem = spdk_nvmf_request_get_subsystem(req);
	struct spdk_nvmf_cpcs_ns *ns;
	struct cpcs_program *prog;
	uint16_t pind, rsid;
	uint32_t numr, dlen;
	union spdk_nvme_cmd_cdw2 cdw2 = {};
	union spdk_nvme_cmd_cdw3 cdw3 = {};
	union spdk_nvme_cmd_cdw4 cdw4 = {};
	size_t copied;

	/* Extract fields from command (based on spdk_nvme_cpcs_cmd_execute_program) */
	cdw2.raw = cmd->rsvd2;
	cdw3.raw = cmd->rsvd3;
	cdw4.raw = (uint32_t)cmd->mptr;

	pind = cdw2.cpcs_execute_program.pind;
	rsid = cdw2.cpcs_execute_program.rsid;
	numr = cdw3.cpcs_execute_program.numr;
	dlen = cdw4.cpcs_execute_program.dlen;

	/* Get namespace */
	ns = spdk_nvmf_cpcs_ns_get_by_nsid(subsystem, cmd->nsid);
	if (!ns) {
		SPDK_ERRLOG("CPCS namespace not found for NSID %u\n", cmd->nsid);
		return -SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
	}
	ctx->ns = ns;

	/* Get program */
	prog = cpcs_program_get(ns, pind);
	if (!prog) {
		SPDK_ERRLOG("Program %u not found\n", pind);
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}

	SPDK_NOTICELOG("CPCS execute parse: cmd_nsid=%u pind=%u rsid=%u numr=%u dlen=%u activated=%s\n",
		       cmd->nsid, pind, rsid, numr, dlen, prog->activated ? "yes" : "no");

	if (!prog->activated) {
		SPDK_ERRLOG("Program %u not activated\n", pind);
		return -SPDK_NVME_CPCS_SC_PROGRAM_NOT_ACTIVATED;
	}
	ctx->program = prog;

	/* Validate RSID and NUMR combination */
	if (rsid != 0 && numr != 0) {
		SPDK_ERRLOG("Invalid RSID/NUMR combination: RSID=%u NUMR=%u\n", rsid, numr);
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	ctx->rsid = rsid;
	ctx->cparam1 = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
	ctx->cparam2 = ((uint64_t)cmd->cdw13 << 32) | cmd->cdw12;
	ctx->data_len = dlen;
	if (dlen > 0) {
		if (req->length < dlen) {
			SPDK_ERRLOG("Execute Program data length too small: %u < %u\n",
				    req->length, dlen);
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		if (req->iovcnt == 1 && req->iov[0].iov_len >= dlen) {
			ctx->data_buffer = req->iov[0].iov_base;
			ctx->data_buffer_owned = false;
			/* Diagnostic: show raw bytes received from host to catch nsid corruption */
			if (dlen >= 8) {
				const uint8_t *raw = (const uint8_t *)ctx->data_buffer;
				SPDK_NOTICELOG("cpcs_execute_parse_cmd: raw data_buffer[0..7]:"
					       " %02x %02x %02x %02x %02x %02x %02x %02x"
					       " (nsid_field=%u) iovcnt=%d iov_len=%zu dlen=%u\n",
					       raw[0], raw[1], raw[2], raw[3],
					       raw[4], raw[5], raw[6], raw[7],
					       *(const uint32_t *)raw,
					       req->iovcnt, req->iov[0].iov_len, dlen);
			}
		} else if (req->iovcnt == 0) {
			SPDK_ERRLOG("Execute Program: no IOVs in request (dlen=%u req->length=%u)\n",
				    dlen, req->length);
			return -SPDK_NVME_SC_INVALID_FIELD;
		} else {
			ctx->data_buffer = malloc(dlen);
			if (!ctx->data_buffer) {
				return -ENOMEM;
			}
			ctx->data_buffer_base = ctx->data_buffer;
			ctx->data_buffer_owned = true;

			copied = spdk_nvmf_request_copy_to_buf(req, ctx->data_buffer, dlen);
			if (copied != dlen) {
				SPDK_ERRLOG("Failed to copy Execute Program data: %zu != %u\n",
					    copied, dlen);
				free(ctx->data_buffer);
				ctx->data_buffer = NULL;
				ctx->data_buffer_owned = false;
				return -SPDK_NVME_SC_INVALID_FIELD;
			}
		}
	}

	/* Handle inline Memory Ranges */
	if (rsid == 0 && numr > 0) {
		if (ns->max_ranges_per_mrs > 0 && numr > ns->max_ranges_per_mrs) {
			SPDK_ERRLOG("Too many inline ranges: %u (max %u)\n",
				    numr, ns->max_ranges_per_mrs);
			return -SPDK_NVME_CPCS_SC_MAX_MEMORY_RANGES_EXCEEDED;
		}
		ctx->inline_range_count = numr;
		/* Inline ranges will be parsed from data buffer */
	}

	SPDK_DEBUGLOG(nvmf_cpcs, "Execute Program: PIND=%u RSID=%u NUMR=%u DLEN=%u\n",
		      pind, rsid, numr, dlen);

	return 0;
}

int
cpcs_execute_setup_memory(struct cpcs_exec_context *ctx)
{
	int rc;
	size_t descriptor_size;
	size_t required_size;
	uint32_t i;
	struct spdk_nvme_cpcs_memory_range_descriptor *descriptors;

	if (ctx->rsid != 0) {
		/*
		 * Use pre-created Memory Range Set. Look up and acquire the
		 * reference atomically under ns->lock so a concurrent delete
		 * cannot free the set between the lookup and the acquire.
		 */
		ctx->mrs = cpcs_mrs_get_and_acquire(ctx->ns, ctx->rsid);
		if (!ctx->mrs) {
			SPDK_ERRLOG("MRS %u not found\n", ctx->rsid);
			return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_RANGE_SET_ID;
		}

		SPDK_DEBUGLOG(nvmf_cpcs, "Acquired MRS %u for execution\n", ctx->rsid);
	} else if (ctx->inline_range_count > 0) {
		/* Parse inline Memory Ranges from data buffer */
		descriptor_size = sizeof(struct spdk_nvme_cpcs_memory_range_descriptor);
		required_size = ctx->inline_range_count * descriptor_size;

		if (!ctx->data_buffer) {
			SPDK_ERRLOG("No data buffer provided for inline ranges\n");
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		/* Validate data buffer size */
		if (ctx->data_len < required_size) {
			SPDK_ERRLOG("Data buffer too small for inline ranges: %u < %zu\n",
				    ctx->data_len, required_size);
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		/* Allocate inline ranges array */
		ctx->inline_ranges = calloc(ctx->inline_range_count, sizeof(struct cpcs_memory_range));
		if (!ctx->inline_ranges) {
			return -ENOMEM;
		}

		/* Parse descriptors from data buffer */
		descriptors = (struct spdk_nvme_cpcs_memory_range_descriptor *)ctx->data_buffer;

		for (i = 0; i < ctx->inline_range_count; i++) {
			ctx->inline_ranges[i].mnsid = descriptors[i].mnsid;
			ctx->inline_ranges[i].starting_byte = descriptors[i].starting_byte;
			ctx->inline_ranges[i].length = descriptors[i].length;
		}

		/* Validate inline ranges */
		rc = cpcs_mrs_validate(ctx->ns,
				       (const struct spdk_nvme_cpcs_memory_range_descriptor *)descriptors,
				       ctx->inline_range_count);
		if (rc != 0) {
			SPDK_ERRLOG("Inline range validation failed: %d\n", rc);
			free(ctx->inline_ranges);
			ctx->inline_ranges = NULL;
			return rc;
		}

		/* Advance data_buffer past the range descriptors so builtins see their own descriptor */
		ctx->data_buffer = (uint8_t *)ctx->data_buffer + required_size;
		ctx->data_len -= (uint32_t)required_size;

		SPDK_DEBUGLOG(nvmf_cpcs, "Parsed %u inline memory ranges\n", ctx->inline_range_count);
	}

	rc = cpcs_execute_resolve_ranges(ctx);
	return rc;
}

int
cpcs_execute_run(struct cpcs_exec_context *ctx)
{
	const struct cpcs_runtime_ops *runtime;
	struct cpcs_execute_wait_ctx wait_ctx = {};
	struct spdk_thread *thread;
	int rc;

	/* Get runtime for program type */
	runtime = cpcs_runtime_get(ctx->program->ptype);
	if (!runtime) {
		SPDK_ERRLOG("No runtime for program type %u\n", ctx->program->ptype);
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	/*
	 * Re-check that the program is still activated and bump exec_count
	 * atomically under ns->lock. cpcs_program_unload()/deactivate() refuse
	 * while exec_count > 0 (also under ns->lock), so once this increment is
	 * visible the program cannot be freed or deactivated underneath us. This
	 * closes the use-after-free window between the activated-check in
	 * cpcs_execute_parse_cmd() and the start of execution here.
	 *
	 * Some in-process test harnesses call cpcs_execute_run() directly with
	 * ctx->ns == NULL; in that case fall back to an atomic increment.
	 */
	if (ctx->ns != NULL) {
		pthread_mutex_lock(&ctx->ns->lock);
		if (!ctx->program->activated) {
			pthread_mutex_unlock(&ctx->ns->lock);
			SPDK_ERRLOG("Program %u no longer activated\n", ctx->program->pind);
			return -SPDK_NVME_CPCS_SC_PROGRAM_NOT_ACTIVATED;
		}
		ctx->program->exec_count++;
		pthread_mutex_unlock(&ctx->ns->lock);
	} else {
		__atomic_add_fetch(&ctx->program->exec_count, 1, __ATOMIC_SEQ_CST);
	}

	SPDK_DEBUGLOG(nvmf_cpcs, "Executing program %u (type %u)\n",
		      ctx->program->pind, ctx->program->ptype);

	/* Execute program asynchronously. */
	if (runtime->execute_async == NULL) {
		cpcs_execute_dec_exec_count(ctx);
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	/*
	 * Some in-process test harnesses call cpcs_execute_run() directly
	 * without an NVMf request context. Keep that path synchronous.
	 */
	if (ctx->req == NULL) {
		ctx->runtime_pending = true;
		pthread_mutex_lock(&ctx->program->lock);
		rc = runtime->execute_async(ctx->program, ctx,
					    cpcs_execute_runtime_wait_done, &wait_ctx);
		pthread_mutex_unlock(&ctx->program->lock);
		if (rc != 0) {
			ctx->runtime_pending = false;
			cpcs_execute_dec_exec_count(ctx);
			return rc;
		}

		thread = spdk_get_thread();
		while (!wait_ctx.done) {
			if (thread != NULL) {
				spdk_thread_poll(thread, 0, 0);
			} else {
				usleep(1);
			}
		}

		ctx->runtime_pending = false;
		ctx->return_value = wait_ctx.return_value;
		cpcs_execute_dec_exec_count(ctx);
		return wait_ctx.status;
	}

	ctx->runtime_pending = true;
	pthread_mutex_lock(&ctx->program->lock);
	rc = runtime->execute_async(ctx->program, ctx, cpcs_execute_runtime_done, ctx);
	pthread_mutex_unlock(&ctx->program->lock);
	if (rc != 0) {
		ctx->runtime_pending = false;
		SPDK_ERRLOG("Program %u async submission failed: %d\n", ctx->program->pind, rc);
		cpcs_execute_dec_exec_count(ctx);
		return rc;
	}

	return 0;
}

void
cpcs_execute_complete(struct cpcs_exec_context *ctx, int status)
{
	struct spdk_nvme_cpl *cpl = spdk_nvmf_request_get_response(ctx->req);

	if (ctx->completion_done) {
		return;
	}
	ctx->completion_done = true;

	status = cpcs_execute_finalize_slm_lease(ctx, status);

	if (status == SPDK_NVME_SC_SUCCESS) {
		/* Return value in DW0-1 */
		cpl->cdw0 = ctx->return_value & 0xFFFFFFFF;
		cpl->cdw1 = (ctx->return_value >> 32) & 0xFFFFFFFF;
		cpl->status.sct = SPDK_NVME_SCT_GENERIC;
		cpl->status.sc = SPDK_NVME_SC_SUCCESS;
	} else if (status < 0) {
		cpcs_status_from_rc(status, &cpl->status);
	} else {
		cpl->status.sct = SPDK_NVME_SCT_GENERIC;
		cpl->status.sc = (uint16_t)status;
		cpl->status.dnr = 1;
	}

	/* Release MRS if acquired */
	if (ctx->mrs) {
		cpcs_mrs_release(ctx->mrs);
		SPDK_DEBUGLOG(nvmf_cpcs, "Released MRS %u\n", ctx->rsid);
	}

	/* Free inline ranges if allocated */
	if (ctx->inline_ranges) {
		free(ctx->inline_ranges);
	}

	if (ctx->resolved_ranges) {
		free(ctx->resolved_ranges);
	}

	if (ctx->data_buffer_owned) {
		free(ctx->data_buffer_base != NULL ? ctx->data_buffer_base : ctx->data_buffer);
	}

	/* Complete request */
	spdk_nvmf_request_complete(ctx->req);

	SPDK_DEBUGLOG(nvmf_cpcs, "Execute Program completed with status %d\n", status);

	free(ctx);
}

static void
cpcs_execute_runtime_done(void *cb_arg, int status, uint64_t return_value)
{
	struct cpcs_exec_context *ctx = cb_arg;
	struct spdk_nvme_cmd *cmd;

	if (ctx == NULL) {
		return;
	}

	cmd = spdk_nvmf_request_get_cmd(ctx->req);
	ctx->runtime_pending = false;
	ctx->return_value = return_value;

	cpcs_execute_dec_exec_count(ctx);

	if (status == 0) {
		cpcs_execute_complete(ctx, SPDK_NVME_SC_SUCCESS);
		return;
	}

	SPDK_NOTICELOG("CPCS execute async runtime failed: cid=%u nsid=%u rc=%d\n",
		       cmd->cid, cmd->nsid, status);
	cpcs_execute_complete(ctx, status);
}

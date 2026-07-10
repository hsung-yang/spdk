/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

/*
 * Passthrough runtime — see passthrough_runtime.h for architectural rationale
 * and the slide-11 mapping.  This file implements struct cpcs_runtime_ops by
 * forwarding SUM64 execute() requests through a backing SLM bdev to exercise
 * the runtime-indirection routing path end-to-end.  Not a cost proxy for any
 * real CSD substrate (see header for the disclaimer).
 */

#include "passthrough_runtime.h"

#include "builtin_programs.h"
#include "execute.h"
#include "program.h"
#include "runtime.h"

#include "spdk/bdev.h"
#include "spdk/bdev_slm.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk_internal/vbdev_slm.h"

/*
 * Resolved backing-device handle.  Populated lazily on first execute() so
 * registration order vs bdev creation does not matter.
 */
struct cpcs_passthrough_state {
	bool                              resolved;
	bool                              backing_warned;
	struct spdk_bdev                 *backing_bdev;
	const struct spdk_vbdev_slm_ops  *backing_ops;
	uint64_t                          backing_capacity;
};

static struct cpcs_passthrough_state g_passthrough_state;

/*
 * Backing-bdev round-trip uses a single offset (0) so concurrent execute
 * calls on the same backing device would corrupt each other's scratch data.
 * Serialize the write+read pair with a global mutex. PoC scope makes the
 * loss of parallelism acceptable.
 */
static pthread_mutex_t g_passthrough_io_lock = PTHREAD_MUTEX_INITIALIZER;

/* I/O chunk size for the round-trip; matches builtin runtime convention. */
#define CPCS_PASSTHROUGH_IO_CHUNK   (2 * 1024 * 1024)

static void
_passthrough_resolve_backing(void)
{
	const char *name;
	struct spdk_bdev *bdev;

	if (g_passthrough_state.resolved) {
		return;
	}
	g_passthrough_state.resolved = true;

	name = getenv(CPCS_PASSTHROUGH_BACKING_BDEV_ENV);
	if (name == NULL || name[0] == '\0') {
		SPDK_NOTICELOG("Passthrough runtime: %s unset; "
			       "execute will run local-only (no backing-bdev round-trip)\n",
			       CPCS_PASSTHROUGH_BACKING_BDEV_ENV);
		return;
	}

	bdev = spdk_bdev_get_by_name(name);
	if (bdev == NULL) {
		SPDK_WARNLOG("Passthrough runtime: backing bdev '%s' not found; "
			     "execute will run local-only\n", name);
		return;
	}

	g_passthrough_state.backing_bdev = bdev;
	g_passthrough_state.backing_ops = vbdev_slm_lookup_ops_by_bdev(bdev);
	if (g_passthrough_state.backing_ops == NULL) {
		SPDK_WARNLOG("Passthrough runtime: backing bdev '%s' is not SLM-capable; "
			     "execute will fall back to bdev_slm_*_by_bdev which may"
			     " return -ENOTSUP\n", name);
	}

	g_passthrough_state.backing_capacity =
		(uint64_t)spdk_bdev_get_block_size(bdev) *
		spdk_bdev_get_num_blocks(bdev);

	SPDK_NOTICELOG("Passthrough runtime: backing bdev '%s' resolved (capacity=%" PRIu64 " bytes, slm_ops=%s)\n",
		       name, g_passthrough_state.backing_capacity,
		       g_passthrough_state.backing_ops != NULL ? "yes" : "no");
}

/*
 * Forward a chunk of `len` bytes through the backing bdev to exercise the
 * routing path.  Writes the input into the backing namespace at offset 0,
 * then reads it back into a scratch buffer.  Captures the cost of dispatching
 * through this runtime indirection with the configured backing bdev — not a
 * proxy for any specific CSD substrate.
 *
 * Returns 0 on success, negative errno on failure.  When no backing bdev is
 * configured, returns 0 without doing any I/O so the dispatch wiring can
 * still be exercised.
 */
static int
_passthrough_forward_chunk(const void *src, void *scratch, uint64_t len)
{
	struct spdk_bdev *bdev = g_passthrough_state.backing_bdev;
	const struct spdk_vbdev_slm_ops *ops = g_passthrough_state.backing_ops;
	int rc;

	if (bdev == NULL) {
		if (!g_passthrough_state.backing_warned) {
			SPDK_WARNLOG("Passthrough runtime: forwarding without backing bdev "
				     "(local-only mode); set %s to enable the backing-bdev round-trip\n",
				     CPCS_PASSTHROUGH_BACKING_BDEV_ENV);
			g_passthrough_state.backing_warned = true;
		}
		return 0;
	}

	if (len > g_passthrough_state.backing_capacity) {
		SPDK_ERRLOG("Passthrough runtime: chunk len=%" PRIu64
			    " exceeds backing capacity %" PRIu64 "\n",
			    len, g_passthrough_state.backing_capacity);
		return -EINVAL;
	}

	pthread_mutex_lock(&g_passthrough_io_lock);

	if (ops != NULL) {
		rc = ops->write_by_bdev(bdev, 0, len, src);
	} else {
		rc = bdev_slm_write_by_bdev(bdev, 0, len, src);
	}
	if (rc != 0) {
		pthread_mutex_unlock(&g_passthrough_io_lock);
		SPDK_ERRLOG("Passthrough runtime: backing write failed rc=%d\n", rc);
		return rc;
	}

	if (ops != NULL) {
		rc = ops->read_by_bdev(bdev, 0, len, scratch);
	} else {
		rc = bdev_slm_read_by_bdev(bdev, 0, len, scratch);
	}
	pthread_mutex_unlock(&g_passthrough_io_lock);
	if (rc != 0) {
		SPDK_ERRLOG("Passthrough runtime: backing read failed rc=%d\n", rc);
		return rc;
	}

	return 0;
}

/*
 * Read from a resolved memory range.  Equivalent in spirit to
 * _cpcs_exec_read_range_sync() in builtin_runtime.c but accessible here
 * (that one is static to its own translation unit). Only handles
 * ctx->resolved_ranges, already populated by execute.c's
 * cpcs_execute_setup_memory() by the time a runtime's execute_async() runs
 * -- unlike builtin_runtime.c's version, this has no fallback path for
 * in-process test harnesses that call cpcs_execute_run() directly.
 */
static int
_passthrough_read_range(const struct cpcs_exec_context *ctx,
			uint64_t mr_id, uint64_t off, uint64_t len, void *buf)
{
	const struct cpcs_exec_resolved_range *mr;
	uint64_t absolute_offset;

	if (mr_id == 0 || ctx->resolved_ranges == NULL ||
	    ctx->resolved_range_count == 0) {
		return -EINVAL;
	}
	if (mr_id > ctx->resolved_range_count) {
		return -EINVAL;
	}
	mr = &ctx->resolved_ranges[mr_id - 1];
	if (mr->bdev == NULL) {
		return -EINVAL;
	}
	if (off > mr->length || len > mr->length - off) {
		return -EINVAL;
	}

	absolute_offset = mr->starting_byte + off;
	return bdev_slm_read_by_bdev(mr->bdev, absolute_offset, len, buf);
}

/* Descriptor layout matches builtin SUM64: {mr_id(u64), off(u64), len(u64)} */
struct passthrough_sum64_desc {
	uint64_t mr_id;
	uint64_t off;
	uint64_t len;
};

/*
 * Execute SUM64 by forwarding through the backing bdev (modelling the
 * device-side execute) and then reducing locally over the data that came
 * back.  Supports both direct-data and MRS-staged input.
 */
static int
_passthrough_execute_sum64(struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const uint64_t *vals;
	uint8_t *scratch;
	uint8_t *read_buf = NULL;
	uint64_t total;
	uint64_t processed = 0;
	uint64_t chunk;
	uint64_t sum = 0;
	uint64_t mr_id = 0, off = 0;
	bool use_mrs = false;
	size_t i;
	int rc;

	if (ctx == NULL || return_value == NULL) {
		return -EINVAL;
	}

	if (ctx->resolved_range_count > 0 && ctx->data_buffer != NULL &&
	    ctx->data_len >= sizeof(struct passthrough_sum64_desc)) {
		/* MRS-staged path: parse descriptor (little-endian wire format) */
		const struct passthrough_sum64_desc *desc =
			(const struct passthrough_sum64_desc *)ctx->data_buffer;
		mr_id = from_le64(&desc->mr_id);
		off   = from_le64(&desc->off);
		total = from_le64(&desc->len);
		use_mrs = true;
		if (total == 0 || (total % sizeof(uint64_t)) != 0) {
			return -EINVAL;
		}
	} else if (ctx->data_buffer != NULL && ctx->data_len > 0) {
		/* Direct-data path */
		total = ctx->data_len;
		if (total % sizeof(uint64_t) != 0) {
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	scratch = malloc(CPCS_PASSTHROUGH_IO_CHUNK);
	if (scratch == NULL) {
		return -ENOMEM;
	}
	if (use_mrs) {
		read_buf = malloc(CPCS_PASSTHROUGH_IO_CHUNK);
		if (read_buf == NULL) {
			free(scratch);
			return -ENOMEM;
		}
	}

	while (processed < total) {
		const void *src;

		chunk = total - processed;
		if (chunk > CPCS_PASSTHROUGH_IO_CHUNK) {
			chunk = CPCS_PASSTHROUGH_IO_CHUNK;
			chunk -= chunk % sizeof(uint64_t);
		}

		if (use_mrs) {
			rc = _passthrough_read_range(ctx, mr_id, off + processed,
						     chunk, read_buf);
			if (rc != 0) {
				free(scratch);
				free(read_buf);
				return rc;
			}
			src = read_buf;
		} else {
			src = (const uint8_t *)ctx->data_buffer + processed;
		}

		rc = _passthrough_forward_chunk(src, scratch, chunk);
		if (rc != 0) {
			free(scratch);
			free(read_buf);
			return rc;
		}

		/*
		 * In a real on-device runtime the device would return only the
		 * 64-bit aggregate.  We compute locally over the round-tripped
		 * data so the result is bit-identical to the built-in path,
		 * letting the bench compare apples to apples.
		 */
		vals = (const uint64_t *)scratch;
		for (i = 0; i < (chunk / sizeof(uint64_t)); i++) {
			sum += vals[i];
		}
		processed += chunk;
	}

	free(scratch);
	free(read_buf);
	*return_value = sum;
	return 0;
}

static int
passthrough_init(struct cpcs_program *prog)
{
	if (prog == NULL) {
		return -EINVAL;
	}
	/* No per-program state for the PoC; runtime context stays NULL. */
	prog->runtime = NULL;
	SPDK_DEBUGLOG(nvmf_cpcs, "Passthrough runtime initialized for program %u\n", prog->pind);
	return 0;
}

static int
passthrough_validate(struct cpcs_program *prog)
{
	if (prog == NULL) {
		return -EINVAL;
	}
	/*
	 * Passthrough programs carry no host-supplied bytecode (the workload
	 * identity is the PIND).  Accept any payload state — the host loader
	 * may still call program_load with zero or stub bytes.
	 */
	return 0;
}

static int
passthrough_activate(struct cpcs_program *prog)
{
	if (prog == NULL) {
		return -EINVAL;
	}
	SPDK_NOTICELOG("Passthrough runtime activated program %u (pind=%u)\n",
		       prog->pind, prog->pind);
	return 0;
}

/*
 * Per-execution context for the deferred completion below.
 *
 * Structural adaptation from github/e2e_benchmark: that branch's
 * cpcs_runtime_ops has a synchronous `.execute(prog, ctx, *return_value)`
 * member and cpcs_execute_run() calls it directly inline. This branch's
 * cpcs_runtime_ops only has `.execute_async(prog, ctx, done_cb, cb_arg)`,
 * and execute.c's cpcs_execute_run() holds ctx->program->lock across the
 * execute_async() call, only unlocking (via ctx->program->lock, i.e. through
 * ctx) *after* it returns. Invoking done_cb synchronously from within
 * execute_async() — the direct equivalent of the old sync `.execute()` body
 * — would let done_cb's cpcs_execute_complete() free ctx before that unlock
 * runs, a use-after-free. Deferring the actual work via spdk_thread_send_msg()
 * (the same technique builtin_runtime.c's _cpcs_builtin_execute_extended()
 * uses for its synchronous builtins) guarantees execute_async() itself always
 * returns before done_cb fires.
 */
struct cpcs_passthrough_exec_ctx {
	struct cpcs_program *prog;
	struct cpcs_exec_context *exec_ctx;
	cpcs_runtime_execute_done_cb done_cb;
	void *done_arg;
};

static void
passthrough_execute_msg(void *arg)
{
	struct cpcs_passthrough_exec_ctx *pctx = arg;
	uint64_t return_value = 0;
	int status;

	_passthrough_resolve_backing();

	/*
	 * PoC: all passthrough PINDs route to SUM64 forwarding. The PIND value
	 * selects the runtime (builtin vs passthrough) but the workload is
	 * always SUM64 -- this is the architectural demonstration.
	 */
	(void)pctx->prog->pind;
	status = _passthrough_execute_sum64(pctx->exec_ctx, &return_value);

	pctx->done_cb(pctx->done_arg, status, return_value);
	free(pctx);
}

static int
passthrough_execute_async(struct cpcs_program *prog,
			  struct cpcs_exec_context *ctx,
			  cpcs_runtime_execute_done_cb done_cb,
			  void *cb_arg)
{
	struct cpcs_passthrough_exec_ctx *pctx;
	struct spdk_thread *thread;
	int rc;

	if (prog == NULL || ctx == NULL || done_cb == NULL) {
		return -EINVAL;
	}

	pctx = calloc(1, sizeof(*pctx));
	if (pctx == NULL) {
		return -ENOMEM;
	}
	pctx->prog = prog;
	pctx->exec_ctx = ctx;
	pctx->done_cb = done_cb;
	pctx->done_arg = cb_arg;

	thread = spdk_get_thread();
	if (thread == NULL) {
		free(pctx);
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	rc = spdk_thread_send_msg(thread, passthrough_execute_msg, pctx);
	if (rc != 0) {
		free(pctx);
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	return 0;
}

static void
passthrough_deactivate(struct cpcs_program *prog)
{
	if (prog == NULL) {
		return;
	}
	SPDK_DEBUGLOG(nvmf_cpcs, "Passthrough runtime deactivated program %u\n", prog->pind);
}

static void
passthrough_fini(struct cpcs_program *prog)
{
	if (prog == NULL) {
		return;
	}
	prog->runtime = NULL;
	SPDK_DEBUGLOG(nvmf_cpcs, "Passthrough runtime cleaned up program %u\n", prog->pind);
}

static const struct cpcs_runtime_ops g_passthrough_runtime = {
	.name          = "passthrough",
	.ptype         = CPCS_PTYPE_PASSTHROUGH,
	.init          = passthrough_init,
	.validate      = passthrough_validate,
	.activate      = passthrough_activate,
	.execute_async = passthrough_execute_async,
	.deactivate    = passthrough_deactivate,
	.fini          = passthrough_fini,
};

int
cpcs_passthrough_runtime_register(void)
{
	int rc;

	rc = cpcs_runtime_register(&g_passthrough_runtime);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register passthrough runtime: %d\n", rc);
		return rc;
	}

	SPDK_NOTICELOG("Passthrough runtime registered (ptype=0x%02x); "
		       "set %s to a backing SLM bdev to enable the backing-bdev round-trip\n",
		       CPCS_PTYPE_PASSTHROUGH, CPCS_PASSTHROUGH_BACKING_BDEV_ENV);
	return 0;
}

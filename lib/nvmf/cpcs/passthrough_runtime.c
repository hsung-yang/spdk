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
#include "spdk/env.h"
#include "spdk/log.h"
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
			       "execute() will run local-only (no backing-bdev round-trip)\n",
			       CPCS_PASSTHROUGH_BACKING_BDEV_ENV);
		return;
	}

	bdev = spdk_bdev_get_by_name(name);
	if (bdev == NULL) {
		SPDK_WARNLOG("Passthrough runtime: backing bdev '%s' not found; "
			     "execute() will run local-only\n", name);
		return;
	}

	g_passthrough_state.backing_bdev = bdev;
	g_passthrough_state.backing_ops = vbdev_slm_lookup_ops(bdev);
	if (g_passthrough_state.backing_ops == NULL) {
		SPDK_WARNLOG("Passthrough runtime: backing bdev '%s' is not SLM-capable; "
			     "execute() will fall back to bdev_slm_*_by_bdev which may"
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

	if (ops != NULL) {
		rc = ops->write_by_bdev(bdev, 0, len, src);
	} else {
		rc = bdev_slm_write_by_bdev(bdev, 0, len, src);
	}
	if (rc != 0) {
		SPDK_ERRLOG("Passthrough runtime: backing write failed rc=%d\n", rc);
		return rc;
	}

	if (ops != NULL) {
		rc = ops->read_by_bdev(bdev, 0, len, scratch);
	} else {
		rc = bdev_slm_read_by_bdev(bdev, 0, len, scratch);
	}
	if (rc != 0) {
		SPDK_ERRLOG("Passthrough runtime: backing read failed rc=%d\n", rc);
		return rc;
	}

	return 0;
}

/*
 * Execute SUM64 by forwarding through the backing bdev (modelling the
 * device-side execute) and then reducing locally over the data that came
 * back.  Mirrors the direct-data branch of _builtin_execute_sum64.
 */
static int
_passthrough_execute_sum64(struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const uint64_t *vals;
	uint8_t *scratch;
	uint64_t total;
	uint64_t processed = 0;
	uint64_t chunk;
	uint64_t sum = 0;
	size_t i;
	int rc;

	if (ctx == NULL || ctx->data_buffer == NULL || return_value == NULL) {
		return -EINVAL;
	}

	/*
	 * PoC scope: this runtime expects the host to pass the input inline via
	 * the direct-data path (resolved_range_count == 0), the same convention
	 * used by the direct-data branch of the built-in SUM64 handler.  MRS-
	 * staged input is not implemented for the PoC.
	 */
	if (ctx->resolved_range_count != 0) {
		SPDK_WARNLOG("Passthrough runtime: MRS-staged input not supported in PoC\n");
		return -ENOTSUP;
	}

	if (ctx->data_len == 0 || (ctx->data_len % sizeof(uint64_t)) != 0) {
		return -EINVAL;
	}

	total = ctx->data_len;
	scratch = malloc(CPCS_PASSTHROUGH_IO_CHUNK);
	if (scratch == NULL) {
		return -ENOMEM;
	}

	while (processed < total) {
		chunk = total - processed;
		if (chunk > CPCS_PASSTHROUGH_IO_CHUNK) {
			chunk = CPCS_PASSTHROUGH_IO_CHUNK;
			chunk -= chunk % sizeof(uint64_t);
		}

		rc = _passthrough_forward_chunk((const uint8_t *)ctx->data_buffer + processed,
						scratch, chunk);
		if (rc != 0) {
			free(scratch);
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

static int
passthrough_execute(struct cpcs_program *prog,
		    struct cpcs_exec_context *ctx,
		    uint64_t *return_value)
{
	if (prog == NULL || ctx == NULL || return_value == NULL) {
		return -EINVAL;
	}

	_passthrough_resolve_backing();

	switch (prog->pind) {
	case CPCS_BUILTIN_PIND_SUM64:
		return _passthrough_execute_sum64(ctx, return_value);
	default:
		SPDK_WARNLOG("Passthrough runtime: PIND %u not supported in PoC "
			     "(only SUM64 forwarding is implemented)\n", prog->pind);
		return -ENOTSUP;
	}
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
	.name       = "passthrough",
	.ptype      = CPCS_PTYPE_PASSTHROUGH,
	.init       = passthrough_init,
	.validate   = passthrough_validate,
	.activate   = passthrough_activate,
	.execute    = passthrough_execute,
	.deactivate = passthrough_deactivate,
	.fini       = passthrough_fini,
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

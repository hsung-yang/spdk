/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * Stub runtime implementation for CPCS programs
 *
 * This provides a minimal runtime framework that can be replaced
 * with full eBPF/uBPF integration in the future.
 */

#include "runtime.h"
#include "builtin_runtime.h"
#include "ebpf_runtime.h"
#include "passthrough_runtime.h"
#include "program.h"
#include "spdk/log.h"

/* Maximum number of registered runtimes */
#define MAX_RUNTIMES 16

/* Registered runtimes */
static const struct cpcs_runtime_ops *g_runtimes[MAX_RUNTIMES];
static uint32_t g_runtime_count = 0;

/* Stub runtime context */
struct cpcs_stub_runtime_ctx {
	uint8_t ptype;
	bool validated;
	bool activated;
};

/* Stub runtime operations */

static int
stub_init(struct cpcs_program *prog)
{
	struct cpcs_stub_runtime_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->ptype = prog->ptype;
	ctx->validated = false;
	ctx->activated = false;

	prog->runtime = (struct cpcs_runtime_ctx *)ctx;
	return 0;
}

static int
stub_validate(struct cpcs_program *prog)
{
	struct cpcs_stub_runtime_ctx *ctx = (struct cpcs_stub_runtime_ctx *)prog->runtime;

	if (!ctx) {
		return -EINVAL;
	}

	/* Stub: Just check that program data exists */
	if (!prog->data || prog->loaded_bytes != prog->total_size) {
		return -EINVAL;
	}

	ctx->validated = true;
	SPDK_DEBUGLOG(nvmf_cpcs, "Stub runtime validated program %u (type %u)\n",
		      prog->pind, prog->ptype);
	return 0;
}

static int
stub_activate(struct cpcs_program *prog)
{
	struct cpcs_stub_runtime_ctx *ctx = (struct cpcs_stub_runtime_ctx *)prog->runtime;

	if (!ctx || !ctx->validated) {
		return -EINVAL;
	}

	ctx->activated = true;
	SPDK_NOTICELOG("Stub runtime activated program %u (type %u)\n",
		       prog->pind, prog->ptype);
	return 0;
}

static int
stub_execute(struct cpcs_program *prog,
	     struct cpcs_exec_context *ctx,
	     cpcs_runtime_execute_done_cb done_cb,
	     void *cb_arg)
{
	struct cpcs_stub_runtime_ctx *rt_ctx = (struct cpcs_stub_runtime_ctx *)prog->runtime;

	if (!rt_ctx || !rt_ctx->activated) {
		return -EINVAL;
	}
	if (done_cb == NULL) {
		return -EINVAL;
	}

	/*
	 * This runtime is only ever reached as the wildcard (ptype=0xFF)
	 * fallback for program types with no real runtime registered (see
	 * g_stub_runtime below and cpcs_runtime_get()'s wildcard search) --
	 * no legitimate program type is expected to execute here. Faking a
	 * SUCCESS/0 completion would be indistinguishable from the device
	 * having actually computed 0, which violates this project's
	 * benchmark fairness rule. Fail with the CPCS "invalid program
	 * type" status instead of fabricating a result.
	 */
	SPDK_ERRLOG("Stub runtime has no real implementation for program %u (type %u); "
		    "refusing to fake a successful execution\n",
		    prog->pind, rt_ctx->ptype);
	done_cb(cb_arg, -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_TYPE, 0);
	return 0;
}

static void
stub_deactivate(struct cpcs_program *prog)
{
	struct cpcs_stub_runtime_ctx *ctx = (struct cpcs_stub_runtime_ctx *)prog->runtime;

	if (ctx) {
		ctx->activated = false;
		SPDK_DEBUGLOG(nvmf_cpcs, "Stub runtime deactivated program %u\n", prog->pind);
	}
}

static void
stub_fini(struct cpcs_program *prog)
{
	struct cpcs_stub_runtime_ctx *ctx = (struct cpcs_stub_runtime_ctx *)prog->runtime;

	if (ctx) {
		free(ctx);
		prog->runtime = NULL;
		SPDK_DEBUGLOG(nvmf_cpcs, "Stub runtime cleaned up program %u\n", prog->pind);
	}
}

/* Default stub runtime for all program types */
static const struct cpcs_runtime_ops g_stub_runtime = {
	.name = "stub",
	.ptype = 0xFF,  /* Match all types */
	.init = stub_init,
	.validate = stub_validate,
	.activate = stub_activate,
	.execute_async = stub_execute,
	.deactivate = stub_deactivate,
	.fini = stub_fini,
};

int
cpcs_runtime_register(const struct cpcs_runtime_ops *ops)
{
	if (!ops) {
		return -EINVAL;
	}
	if (g_runtime_count >= MAX_RUNTIMES) {
		SPDK_ERRLOG("Cannot register CPCS runtime '%s': MAX_RUNTIMES (%u) reached\n",
			    ops->name, MAX_RUNTIMES);
		return -ENOSPC;
	}

	g_runtimes[g_runtime_count++] = ops;
	SPDK_NOTICELOG("Registered CPCS runtime: %s (ptype=%u)\n",
		       ops->name, ops->ptype);
	return 0;
}

const struct cpcs_runtime_ops *
cpcs_runtime_get(uint8_t ptype)
{
	uint32_t i;

	/* Search for exact match */
	for (i = 0; i < g_runtime_count; i++) {
		if (g_runtimes[i]->ptype == ptype) {
			return g_runtimes[i];
		}
	}

	/* Search for wildcard match */
	for (i = 0; i < g_runtime_count; i++) {
		if (g_runtimes[i]->ptype == 0xFF) {
			return g_runtimes[i];
		}
	}

	return NULL;
}

int
cpcs_runtime_init_all(void)
{
	int rc;

	/* Register built-in (device-defined) runtime. */
	rc = cpcs_builtin_runtime_register();
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register built-in runtime\n");
		return rc;
	}

	/* Register eBPF runtime */
	rc = cpcs_ebpf_runtime_register();
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register eBPF runtime\n");
		return rc;
	}

	/* Register passthrough runtime (Z1 PoC: hardware-independent dispatch). */
	rc = cpcs_passthrough_runtime_register();
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register passthrough runtime\n");
		return rc;
	}

	/* Register stub runtime as fallback for non-eBPF program types */
	rc = cpcs_runtime_register(&g_stub_runtime);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register stub runtime\n");
		return rc;
	}

	return 0;
}

void
cpcs_runtime_fini_all(void)
{
	g_runtime_count = 0;
	SPDK_NOTICELOG("CPCS runtimes cleaned up\n");
}

/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

/**
 * \file
 * CPCS Passthrough Runtime — hardware-independent dispatch PoC
 *
 * Architectural significance (SDC 2026 talk, slide 11):
 *   The SPDK CPCS dispatch layer is hardware-independent by design.  The same
 *   wire protocol (NVMe Execute Program command) can route to:
 *     - target-CPU built-in execution (today, JBOF substrate)
 *     - eBPF programmable execution on the same target CPU
 *     - **another NVMe device via this passthrough runtime** — exercising
 *       the runtime-indirection interface end-to-end without requiring a
 *       hardware CSD
 *
 *   Registering this runtime under a distinct ptype demonstrates that the
 *   dispatch architecture treats execution location as a routing decision,
 *   not a hard-coded property of the target.  The talk's claim
 *   "hardware-independent by design — DEMONSTRATED" rests on this PoC.
 *
 *   This runtime is NOT a cost proxy for any real CSD substrate.  The
 *   backing-bdev round-trip captures only the cost of dispatching through
 *   this particular runtime path; absolute latency reflects the chosen
 *   backing bdev (plain NVMe, malloc, null, ...) and does not approximate
 *   in-package CSD silicon, fabric-attached CSD, or any specific device.
 *   Treat the measured delta as a property of the bench configuration, not
 *   a hardware estimate.
 *
 * Forward path:
 *   host issues Execute Program (PIND, ptype=PASSTHROUGH)
 *     -> nvmf target receives via TCP/RDMA
 *       -> cpcs_execute_run() looks up runtime by ptype
 *         -> cpcs_passthrough_runtime.execute() forwards the workload to a
 *            backing namespace (configured at registration time)
 *           -> backing namespace executes (in this PoC: data is round-tripped
 *              through a backing SLM bdev to exercise the routing path,
 *              then SUM64 is computed locally)
 *         <- 64-bit aggregate result
 *       <- cpcs_execute_complete() packs into NVMe completion DW0/DW1
 *     <- host driver receives completion
 *
 * Backing-device configuration:
 *   At init time, the runtime reads the environment variable
 *   CPCS_PASSTHROUGH_BACKING_BDEV.  The named bdev must already exist in the
 *   SPDK target's bdev registry and (for this PoC) must be an SLM-capable
 *   bdev so the synchronous bdev_slm_*_by_bdev fast path is usable from the
 *   reactor without async-completion plumbing.  If unset or not found,
 *   passthrough degenerates to a local-only path with a warning at first
 *   execute() — useful for smoke-testing the dispatch wiring without a
 *   configured backing namespace.
 *
 * PoC limitations (intentional, documented in talk):
 *   - Only PIND CPCS_BUILTIN_PIND_SUM64 is forwarded; other PINDs return
 *     -ENOTSUP.  Extending coverage is mechanical (mirror builtin_runtime.c
 *     dispatch) but adds no architectural insight beyond what SUM64 shows.
 *   - Round-trip through the backing bdev is synchronous; no batching, no
 *     async I/O coalescing, no parallel forwarding.  The measured overhead
 *     is therefore an upper bound on per-call dispatch cost.
 *   - The "execute on backing" step is currently a data round-trip plus
 *     local compute.  A production implementation would issue a real NVMe
 *     Execute Program via spdk_bdev_nvme_io_passthru against a CPCS-capable
 *     backing device.  The substitution does not affect the architectural
 *     claim (dispatch is hardware-independent); it does keep the PoC runnable
 *     on commodity hardware.
 */

#ifndef SPDK_NVMF_CPCS_PASSTHROUGH_RUNTIME_H
#define SPDK_NVMF_CPCS_PASSTHROUGH_RUNTIME_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Passthrough program type — vendor-specific range (0xC0..0xFF).
 * 0xC0 is taken by CPCS_PTYPE_EBPF; this PoC takes 0xC2.
 */
#define CPCS_PTYPE_PASSTHROUGH		0xC2

/* Env var consulted at registration time for the backing bdev name. */
#define CPCS_PASSTHROUGH_BACKING_BDEV_ENV	"CPCS_PASSTHROUGH_BACKING_BDEV"

/**
 * Register the passthrough runtime with the dispatch layer.
 *
 * Called from cpcs_runtime_init_all() in runtime_stub.c.  Returns 0 on
 * success or a negative errno on failure.  Registration succeeds even when
 * the backing bdev is not (yet) present; the lookup is deferred to first
 * execute().
 */
int cpcs_passthrough_runtime_register(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_PASSTHROUGH_RUNTIME_H */

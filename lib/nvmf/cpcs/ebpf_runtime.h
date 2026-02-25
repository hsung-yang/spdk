/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * \file
 * eBPF Runtime for CPCS Programs
 */

#ifndef SPDK_NVMF_CPCS_EBPF_RUNTIME_H
#define SPDK_NVMF_CPCS_EBPF_RUNTIME_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* eBPF program type (vendor-specific range) */
#define CPCS_PTYPE_EBPF     0xC0

/**
 * eBPF runtime context
 */
struct cpcs_ebpf_ctx {
	void           *vm;         /* uBPF VM instance */
	void           *jit_func;   /* JIT compiled function */
	bool            jit_enabled;
	uint32_t        mem_size;   /* VM memory size */
};

/**
 * eBPF helper function IDs
 */
enum cpcs_ebpf_helper_id {
	CPCS_EBPF_HELPER_SLM_READ   = 1,
	CPCS_EBPF_HELPER_SLM_WRITE  = 2,
	CPCS_EBPF_HELPER_GET_PARAM  = 3,
	CPCS_EBPF_HELPER_LOG        = 4,
};

/**
 * Register eBPF runtime
 */
int cpcs_ebpf_runtime_register(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_EBPF_RUNTIME_H */

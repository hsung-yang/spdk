/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#ifndef SPDK_NVMF_CPCS_BUILTIN_PROGRAMS_H
#define SPDK_NVMF_CPCS_BUILTIN_PROGRAMS_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_nvmf_cpcs_ns;

/*
 * CPCS built-in (device-defined) programs
 *
 * These programs occupy fixed PIND slots and are not downloadable by design.
 * They exist to make experiments reproducible without requiring an external
 * host-side program loader.
 */

enum cpcs_builtin_program {
	CPCS_BUILTIN_MEMCPY = 0,
	CPCS_BUILTIN_MEMFILL = 1,
	CPCS_BUILTIN_SUM64 = 2,
	CPCS_BUILTIN_MAX64 = 3,
	CPCS_BUILTIN_MIN64 = 4,
	CPCS_BUILTIN_MAX,
};

/* Fixed PIND assignments (device-defined program slots). */
#define CPCS_BUILTIN_PIND_MEMCPY  0u
#define CPCS_BUILTIN_PIND_MEMFILL 1u
#define CPCS_BUILTIN_PIND_SUM64   2u
#define CPCS_BUILTIN_PIND_MAX64   3u
#define CPCS_BUILTIN_PIND_MIN64   4u

/* PUID values are a stable ABI for experiments (host can refer to them). */
#define CPCS_BUILTIN_PUID_MEMCPY  0x0000000000000001ull
#define CPCS_BUILTIN_PUID_MEMFILL 0x0000000000000002ull
#define CPCS_BUILTIN_PUID_SUM64   0x0000000000000003ull
#define CPCS_BUILTIN_PUID_MAX64   0x0000000000000004ull
#define CPCS_BUILTIN_PUID_MIN64   0x0000000000000005ull

bool cpcs_program_index_is_builtin(uint16_t pind);
int cpcs_program_install_builtins(struct spdk_nvmf_cpcs_ns *ns);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_BUILTIN_PROGRAMS_H */

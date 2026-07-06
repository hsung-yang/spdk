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
	CPCS_BUILTIN_FILTER_AGG = 5,
	CPCS_BUILTIN_FILTERED_TOPK_EXACT = 6,
	CPCS_BUILTIN_KV_PACK_STORE = 7,
	CPCS_BUILTIN_KV_UNPACK_LOAD = 8,
	CPCS_BUILTIN_KV_LAYOUT_REPACK = 9,
	CPCS_BUILTIN_KV_BLOCK_SELECT = 10,
	CPCS_BUILTIN_KV_PREFIX_LOOKUP = 11,
	CPCS_BUILTIN_KV_BATCH_READ = 12,
	CPCS_BUILTIN_DOT_PRODUCT = 13,
	CPCS_BUILTIN_FILTER_GT = 14,
	CPCS_BUILTIN_MEMCPY_INLINE = 15,
	CPCS_BUILTIN_RLE_COMPRESS = 16,
	CPCS_BUILTIN_MULTI_AGG64 = 17,
	CPCS_BUILTIN_L2_DISTANCE_SQ = 18,
	CPCS_BUILTIN_COSINE_SIMILARITY = 19,
	CPCS_BUILTIN_MAX,
};

/* Fixed PIND assignments (device-defined program slots). */
#define CPCS_BUILTIN_PIND_MEMCPY  0u
#define CPCS_BUILTIN_PIND_MEMFILL 1u
#define CPCS_BUILTIN_PIND_SUM64   2u
#define CPCS_BUILTIN_PIND_MAX64   3u
#define CPCS_BUILTIN_PIND_MIN64   4u
#define CPCS_BUILTIN_PIND_FILTER_AGG 5u
#define CPCS_BUILTIN_PIND_FILTERED_TOPK_EXACT 6u
#define CPCS_BUILTIN_PIND_KV_PACK_STORE 7u
#define CPCS_BUILTIN_PIND_KV_UNPACK_LOAD 8u
#define CPCS_BUILTIN_PIND_KV_LAYOUT_REPACK 9u
#define CPCS_BUILTIN_PIND_KV_BLOCK_SELECT 10u
#define CPCS_BUILTIN_PIND_KV_PREFIX_LOOKUP 11u
#define CPCS_BUILTIN_PIND_KV_BATCH_READ 12u
/*
 * PIND 13-16: ported from github/e2e_benchmark, which had assigned these
 * builtins to PIND 5-8 — colliding with FILTER_AGG/FILTERED_TOPK_EXACT/
 * KV_PACK_STORE/KV_UNPACK_LOAD above. Renumbered to the next free slots.
 */
#define CPCS_BUILTIN_PIND_DOT_PRODUCT   13u
#define CPCS_BUILTIN_PIND_FILTER_GT     14u
#define CPCS_BUILTIN_PIND_MEMCPY_INLINE 15u
#define CPCS_BUILTIN_PIND_RLE_COMPRESS  16u
/*
 * PIND 17-19: ported from github/e2e_benchmark, which had assigned these to
 * PIND 9-11 -- colliding with KV_LAYOUT_REPACK/KV_BLOCK_SELECT/KV_PREFIX_LOOKUP
 * above. Renumbered to the next free slots, same rationale as PIND 13-16.
 */
#define CPCS_BUILTIN_PIND_MULTI_AGG64        17u
#define CPCS_BUILTIN_PIND_L2_DISTANCE_SQ     18u
#define CPCS_BUILTIN_PIND_COSINE_SIMILARITY  19u

/* PUID values are a stable ABI for experiments (host can refer to them). */
#define CPCS_BUILTIN_PUID_MEMCPY  0x0000000000000001ull
#define CPCS_BUILTIN_PUID_MEMFILL 0x0000000000000002ull
#define CPCS_BUILTIN_PUID_SUM64   0x0000000000000003ull
#define CPCS_BUILTIN_PUID_MAX64   0x0000000000000004ull
#define CPCS_BUILTIN_PUID_MIN64   0x0000000000000005ull
#define CPCS_BUILTIN_PUID_FILTER_AGG 0x0000000000000006ull
#define CPCS_BUILTIN_PUID_FILTERED_TOPK_EXACT 0x0000000000000007ull
#define CPCS_BUILTIN_PUID_KV_PACK_STORE 0x0000000000000008ull
#define CPCS_BUILTIN_PUID_KV_UNPACK_LOAD 0x0000000000000009ull
#define CPCS_BUILTIN_PUID_KV_LAYOUT_REPACK 0x000000000000000Aull
#define CPCS_BUILTIN_PUID_KV_BLOCK_SELECT 0x000000000000000Bull
#define CPCS_BUILTIN_PUID_KV_PREFIX_LOOKUP 0x000000000000000Cull
#define CPCS_BUILTIN_PUID_KV_BATCH_READ 0x000000000000000Dull
#define CPCS_BUILTIN_PUID_DOT_PRODUCT    0x000000000000000Eull
#define CPCS_BUILTIN_PUID_FILTER_GT      0x000000000000000Full
#define CPCS_BUILTIN_PUID_MEMCPY_INLINE  0x0000000000000010ull
#define CPCS_BUILTIN_PUID_RLE_COMPRESS   0x0000000000000011ull
#define CPCS_BUILTIN_PUID_MULTI_AGG64        0x0000000000000012ull
#define CPCS_BUILTIN_PUID_L2_DISTANCE_SQ     0x0000000000000013ull
#define CPCS_BUILTIN_PUID_COSINE_SIMILARITY  0x0000000000000014ull

bool cpcs_program_index_is_builtin(uint16_t pind);
int cpcs_program_install_builtins(struct spdk_nvmf_cpcs_ns *ns);

struct cpcs_builtin_runtime_stats {
	uint64_t sync_wait_total;
};

void cpcs_builtin_runtime_get_stats(struct cpcs_builtin_runtime_stats *stats);
void cpcs_builtin_runtime_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_BUILTIN_PROGRAMS_H */

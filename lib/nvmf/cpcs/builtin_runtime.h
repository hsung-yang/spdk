/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#ifndef SPDK_NVMF_CPCS_BUILTIN_RUNTIME_H
#define SPDK_NVMF_CPCS_BUILTIN_RUNTIME_H

#include "spdk/cpuset.h"

#ifdef __cplusplus
extern "C" {
#endif

int cpcs_builtin_runtime_register(void);
int cpcs_builtin_runtime_set_compute_core_mask(const struct spdk_cpuset *mask);
int cpcs_builtin_runtime_start_compute_threads(void);
void cpcs_builtin_runtime_stop_compute_threads(void);
const struct spdk_cpuset *cpcs_builtin_runtime_get_compute_core_mask(void);
bool cpcs_builtin_runtime_is_compute_core(uint32_t lcore);
uint32_t cpcs_builtin_runtime_get_compute_core_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVMF_CPCS_BUILTIN_RUNTIME_H */

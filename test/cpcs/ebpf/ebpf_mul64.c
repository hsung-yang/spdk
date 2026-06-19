/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2026 Samsung Electronics.
 * All rights reserved.
 *
 * Simple CPCS eBPF program:
 * - Reads two uint64 values from MRS #1 offset 0
 * - Writes their product to MRS #1 offset 16
 * - Returns the product in r0
 */

#define SEC(NAME) __attribute__((section(NAME), used))

typedef unsigned long long __u64;

/* Helper IDs must match enum cpcs_ebpf_helper_id in ebpf_runtime.h */
static long (*slm_read)(void *ctx, __u64 mr_id, __u64 offset, __u64 len, void *buf) = (void *)1;
static long (*slm_write)(void *ctx, __u64 mr_id, __u64 offset, __u64 len,
			 const void *buf) = (void *)2;

SEC("cpcs")
__u64
cpcs_mul64(void *ctx)
{
	__u64 vals[2];
	__u64 prod;

	if (slm_read(ctx, 1, 0, sizeof(vals), vals) != 0) {
		return 0;
	}

	prod = vals[0] * vals[1];
	(void)slm_write(ctx, 1, 16, sizeof(prod), &prod);

	return prod;
}

char _license[] SEC("license") = "GPL";

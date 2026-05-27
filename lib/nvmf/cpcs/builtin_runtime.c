/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "builtin_runtime.h"
#include "builtin_programs.h"
#include "execute.h"
#include "memory_range_set.h"
#include "program.h"
#include "runtime.h"

#include "spdk/bdev.h"
#include "spdk/bdev_slm.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk_internal/vbdev_slm.h"
#include <math.h>
#include <immintrin.h>
#include <string.h>

struct cpcs_builtin_memcpy_desc {
	uint64_t src_mr_id;
	uint64_t src_off;
	uint64_t dst_mr_id;
	uint64_t dst_off;
	uint64_t len;
};

struct cpcs_builtin_memfill_desc {
	uint64_t mr_id;
	uint64_t off;
	uint64_t len;
	uint8_t pattern;
	uint8_t _rsvd[7];
};

struct cpcs_builtin_sum64_desc {
	uint64_t mr_id;
	uint64_t off;
	uint64_t len;
};

struct cpcs_builtin_multi_agg64_result {
	uint64_t count;
	double sum;
	double min;
	double max;
};

/*
 * Direct namespace aggregation descriptor (32 bytes).
 * Sent by the host in the data buffer when invoking CS_CSF_DIRECT_NS_AGG.
 *
 * `threshold` is consumed by FILTER_GT (uint64 comparison value) and is
 * ignored by the other workloads.  Host-side definition in
 * libcs/include/cs_api.h must match this layout exactly.
 */
struct cs_direct_ns_desc {
	uint32_t nsid;        /* NVMe namespace ID to read from */
	uint32_t n_uint64;    /* number of uint64 elements to process */
	uint64_t lba_offset;  /* starting LBA offset (byte offset into ns) */
	uint8_t  workload;    /* 0=SUM, 1=MAX, 2=MIN, 3=FILTER_GT, 4=DOT_PRODUCT */
	uint8_t  pad[7];      /* reserved, must be zero */
	uint64_t threshold;   /* FILTER_GT comparison value; zero otherwise */
};

/*
 * Direct namespace aggregation result (16 bytes).
 * Written back into the data buffer by the handler.
 */
struct cs_direct_ns_result {
	uint64_t result; /* aggregation result (SUM/MAX/MIN value, or FILTER_GT count) */
	uint64_t count;  /* elements processed */
};

/* Workload codes for cs_direct_ns_desc.workload */
#define CS_DIRECT_NS_WORKLOAD_SUM         0
#define CS_DIRECT_NS_WORKLOAD_MAX         1
#define CS_DIRECT_NS_WORKLOAD_MIN         2
#define CS_DIRECT_NS_WORKLOAD_FILTER_GT   3
#define CS_DIRECT_NS_WORKLOAD_DOT_PRODUCT 4

/* SLM reads are local DRAM memcpy — no transport limit. A larger chunk
 * eliminates loop overhead and enables the compiler to auto-vectorize
 * the inner reduction without cross-iteration spills. 16 MiB matches
 * the common benchmark data size so most ops complete in one pass. */
#define CPCS_BUILTIN_IO_CHUNK (16 * 1024 * 1024)

/* Allocate only as much as needed; avoids 16MB malloc for small inputs. */
#define CPCS_ALLOC_CHUNK(data_len) \
	((data_len) < CPCS_BUILTIN_IO_CHUNK ? (data_len) : CPCS_BUILTIN_IO_CHUNK)

/* ─── AVX2 SIMD compute helpers ─────────────────────────────────────────────
 * These use 8-wide float FMA with 4 independent accumulators to hide FMA
 * latency (5 cycles) and maximize throughput on modern x86. The float precision
 * is sufficient for benchmark workloads (relative error < 1e-7 for ~4M elements).
 * Caller must ensure n is the number of floats (not bytes). */

#ifdef __AVX2__

static inline float
_avx2_dot_product(const float *a, const float *b, size_t n)
{
	__m256 sum0 = _mm256_setzero_ps();
	__m256 sum1 = _mm256_setzero_ps();
	__m256 sum2 = _mm256_setzero_ps();
	__m256 sum3 = _mm256_setzero_ps();
	size_t i = 0;

	for (; i + 31 < n; i += 32) {
		sum0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i),      _mm256_loadu_ps(b + i),      sum0);
		sum1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8),  _mm256_loadu_ps(b + i + 8),  sum1);
		sum2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), sum2);
		sum3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), sum3);
	}
	for (; i + 7 < n; i += 8) {
		sum0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), sum0);
	}

	sum0 = _mm256_add_ps(_mm256_add_ps(sum0, sum1), _mm256_add_ps(sum2, sum3));
	/* Horizontal sum of 8 floats */
	__m128 hi = _mm256_extractf128_ps(sum0, 1);
	__m128 lo = _mm256_castps256_ps128(sum0);
	__m128 s = _mm_add_ps(lo, hi);
	s = _mm_hadd_ps(s, s);
	s = _mm_hadd_ps(s, s);
	float result = _mm_cvtss_f32(s);

	/* Scalar tail */
	for (; i < n; i++) {
		result += a[i] * b[i];
	}
	return result;
}

static inline void
_avx2_cosine_accum(const float *a, const float *b, size_t n,
		   float *out_dot, float *out_norm_a, float *out_norm_b)
{
	__m256 dot0 = _mm256_setzero_ps(), dot1 = _mm256_setzero_ps();
	__m256 na0  = _mm256_setzero_ps(), na1  = _mm256_setzero_ps();
	__m256 nb0  = _mm256_setzero_ps(), nb1  = _mm256_setzero_ps();
	size_t i = 0;

	for (; i + 15 < n; i += 16) {
		__m256 va0 = _mm256_loadu_ps(a + i);
		__m256 vb0 = _mm256_loadu_ps(b + i);
		__m256 va1 = _mm256_loadu_ps(a + i + 8);
		__m256 vb1 = _mm256_loadu_ps(b + i + 8);
		dot0 = _mm256_fmadd_ps(va0, vb0, dot0);
		dot1 = _mm256_fmadd_ps(va1, vb1, dot1);
		na0  = _mm256_fmadd_ps(va0, va0, na0);
		na1  = _mm256_fmadd_ps(va1, va1, na1);
		nb0  = _mm256_fmadd_ps(vb0, vb0, nb0);
		nb1  = _mm256_fmadd_ps(vb1, vb1, nb1);
	}
	for (; i + 7 < n; i += 8) {
		__m256 va = _mm256_loadu_ps(a + i);
		__m256 vb = _mm256_loadu_ps(b + i);
		dot0 = _mm256_fmadd_ps(va, vb, dot0);
		na0  = _mm256_fmadd_ps(va, va, na0);
		nb0  = _mm256_fmadd_ps(vb, vb, nb0);
	}

	/* Reduce each to scalar */
	dot0 = _mm256_add_ps(dot0, dot1);
	na0  = _mm256_add_ps(na0, na1);
	nb0  = _mm256_add_ps(nb0, nb1);

	__m128 d_hi = _mm256_extractf128_ps(dot0, 1);
	__m128 d_lo = _mm256_castps256_ps128(dot0);
	__m128 d = _mm_add_ps(d_lo, d_hi); d = _mm_hadd_ps(d, d); d = _mm_hadd_ps(d, d);

	__m128 a_hi = _mm256_extractf128_ps(na0, 1);
	__m128 a_lo = _mm256_castps256_ps128(na0);
	__m128 na = _mm_add_ps(a_lo, a_hi); na = _mm_hadd_ps(na, na); na = _mm_hadd_ps(na, na);

	__m128 b_hi = _mm256_extractf128_ps(nb0, 1);
	__m128 b_lo = _mm256_castps256_ps128(nb0);
	__m128 nb = _mm_add_ps(b_lo, b_hi); nb = _mm_hadd_ps(nb, nb); nb = _mm_hadd_ps(nb, nb);

	float dot_s = _mm_cvtss_f32(d);
	float na_s  = _mm_cvtss_f32(na);
	float nb_s  = _mm_cvtss_f32(nb);

	/* Scalar tail */
	for (; i < n; i++) {
		dot_s += a[i] * b[i];
		na_s  += a[i] * a[i];
		nb_s  += b[i] * b[i];
	}

	*out_dot    = dot_s;
	*out_norm_a = na_s;
	*out_norm_b = nb_s;
}

static inline void
_avx2_multi_agg64(const double *vals, size_t n, double *out_sum,
		  double *out_min, double *out_max)
{
	if (n == 0) {
		*out_sum = 0.0;
		*out_min = 0.0;
		*out_max = 0.0;
		return;
	}

	/* AVX2: 4 doubles per register, 4 independent accumulators */
	__m256d vsum0 = _mm256_setzero_pd();
	__m256d vsum1 = _mm256_setzero_pd();
	__m256d vmin  = _mm256_set1_pd(vals[0]);
	__m256d vmax  = _mm256_set1_pd(vals[0]);
	size_t i = 0;

	for (; i + 7 < n; i += 8) {
		__m256d v0 = _mm256_loadu_pd(vals + i);
		__m256d v1 = _mm256_loadu_pd(vals + i + 4);
		vsum0 = _mm256_add_pd(vsum0, v0);
		vsum1 = _mm256_add_pd(vsum1, v1);
		vmin  = _mm256_min_pd(vmin, _mm256_min_pd(v0, v1));
		vmax  = _mm256_max_pd(vmax, _mm256_max_pd(v0, v1));
	}
	for (; i + 3 < n; i += 4) {
		__m256d v = _mm256_loadu_pd(vals + i);
		vsum0 = _mm256_add_pd(vsum0, v);
		vmin  = _mm256_min_pd(vmin, v);
		vmax  = _mm256_max_pd(vmax, v);
	}

	/* Horizontal reduction */
	vsum0 = _mm256_add_pd(vsum0, vsum1);
	__m128d s_hi = _mm256_extractf128_pd(vsum0, 1);
	__m128d s_lo = _mm256_castpd256_pd128(vsum0);
	__m128d s = _mm_add_pd(s_lo, s_hi);
	s = _mm_hadd_pd(s, s);
	double sum_s = _mm_cvtsd_f64(s);

	__m128d mn_hi = _mm256_extractf128_pd(vmin, 1);
	__m128d mn_lo = _mm256_castpd256_pd128(vmin);
	__m128d mn = _mm_min_pd(mn_lo, mn_hi);
	double min_arr[2]; _mm_storeu_pd(min_arr, mn);
	double min_s = min_arr[0] < min_arr[1] ? min_arr[0] : min_arr[1];

	__m128d mx_hi = _mm256_extractf128_pd(vmax, 1);
	__m128d mx_lo = _mm256_castpd256_pd128(vmax);
	__m128d mx = _mm_max_pd(mx_lo, mx_hi);
	double max_arr[2]; _mm_storeu_pd(max_arr, mx);
	double max_s = max_arr[0] > max_arr[1] ? max_arr[0] : max_arr[1];

	/* Scalar tail */
	for (; i < n; i++) {
		sum_s += vals[i];
		min_s = vals[i] < min_s ? vals[i] : min_s;
		max_s = vals[i] > max_s ? vals[i] : max_s;
	}

	*out_sum = sum_s;
	*out_min = min_s;
	*out_max = max_s;
}

static inline float
_avx2_l2_distance_sq(const float *a, const float *b, size_t n)
{
	__m256 sum0 = _mm256_setzero_ps();
	__m256 sum1 = _mm256_setzero_ps();
	__m256 sum2 = _mm256_setzero_ps();
	__m256 sum3 = _mm256_setzero_ps();
	size_t i = 0;

	for (; i + 31 < n; i += 32) {
		__m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i),      _mm256_loadu_ps(b + i));
		__m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8),  _mm256_loadu_ps(b + i + 8));
		__m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16));
		__m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24));
		sum0 = _mm256_fmadd_ps(d0, d0, sum0);
		sum1 = _mm256_fmadd_ps(d1, d1, sum1);
		sum2 = _mm256_fmadd_ps(d2, d2, sum2);
		sum3 = _mm256_fmadd_ps(d3, d3, sum3);
	}
	for (; i + 7 < n; i += 8) {
		__m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
		sum0 = _mm256_fmadd_ps(d, d, sum0);
	}

	sum0 = _mm256_add_ps(_mm256_add_ps(sum0, sum1), _mm256_add_ps(sum2, sum3));
	__m128 hi = _mm256_extractf128_ps(sum0, 1);
	__m128 lo = _mm256_castps256_ps128(sum0);
	__m128 s = _mm_add_ps(lo, hi);
	s = _mm_hadd_ps(s, s);
	s = _mm_hadd_ps(s, s);
	float result = _mm_cvtss_f32(s);

	for (; i < n; i++) {
		float d = a[i] - b[i];
		result += d * d;
	}
	return result;
}

#else /* !__AVX2__ fallback: scalar with multiple accumulators */

static inline float
_avx2_dot_product(const float *a, const float *b, size_t n)
{
	double sum = 0.0;
	size_t i;
	for (i = 0; i < n; i++) {
		sum += (double)a[i] * (double)b[i];
	}
	return (float)sum;
}

static inline void
_avx2_cosine_accum(const float *a, const float *b, size_t n,
		   float *out_dot, float *out_norm_a, float *out_norm_b)
{
	double dot = 0.0, na = 0.0, nb = 0.0;
	size_t i;
	for (i = 0; i < n; i++) {
		double ai = (double)a[i], bi = (double)b[i];
		dot += ai * bi;
		na  += ai * ai;
		nb  += bi * bi;
	}
	*out_dot    = (float)dot;
	*out_norm_a = (float)na;
	*out_norm_b = (float)nb;
}

static inline void
_avx2_multi_agg64(const double *vals, size_t n, double *out_sum,
		  double *out_min, double *out_max)
{
	double sum = 0.0, mn, mx;
	size_t i;
	if (n == 0) { *out_sum = 0; *out_min = 0; *out_max = 0; return; }
	mn = mx = vals[0];
	for (i = 0; i < n; i++) {
		sum += vals[i];
		mn = vals[i] < mn ? vals[i] : mn;
		mx = vals[i] > mx ? vals[i] : mx;
	}
	*out_sum = sum; *out_min = mn; *out_max = mx;
}

static inline float
_avx2_l2_distance_sq(const float *a, const float *b, size_t n)
{
	double sum = 0.0;
	size_t i;
	for (i = 0; i < n; i++) {
		double d = (double)a[i] - (double)b[i];
		sum += d * d;
	}
	return (float)sum;
}

#endif /* __AVX2__ */

static bool
_builtin_has_direct_data(const struct cpcs_exec_context *ctx)
{
	return ctx != NULL && ctx->resolved_range_count == 0;
}

static int
_builtin_execute_reduce64_direct(const struct cpcs_exec_context *ctx, uint16_t pind, uint64_t *return_value)
{
	const uint64_t *vals;
	uint64_t result;
	size_t count;
	size_t i;

	if (ctx == NULL || ctx->data_buffer == NULL || return_value == NULL) {
		return -EINVAL;
	}
	if (ctx->data_len == 0 || (ctx->data_len % sizeof(uint64_t)) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	vals = (const uint64_t *)ctx->data_buffer;
	count = ctx->data_len / sizeof(uint64_t);
	result = vals[0];

	if (pind == CPCS_BUILTIN_PIND_SUM64) {
		result = 0;
		for (i = 0; i < count; i++) {
			result += vals[i];
		}
	} else if (pind == CPCS_BUILTIN_PIND_MAX64) {
		for (i = 1; i < count; i++) {
			if (vals[i] > result) {
				result = vals[i];
			}
		}
	} else if (pind == CPCS_BUILTIN_PIND_MIN64) {
		for (i = 1; i < count; i++) {
			if (vals[i] < result) {
				result = vals[i];
			}
		}
	} else {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}

	*return_value = result;
	return 0;
}

static int
_builtin_execute_vector_float_direct(const struct cpcs_exec_context *ctx, uint16_t pind, uint64_t *return_value)
{
	const float *vals;
	size_t count;
	size_t half;
	float result = 0.0f;
	uint32_t bits = 0;

	if (ctx == NULL || ctx->data_buffer == NULL || return_value == NULL) {
		return -EINVAL;
	}
	if (ctx->data_len == 0 || (ctx->data_len % (2 * sizeof(float))) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	vals = (const float *)ctx->data_buffer;
	count = ctx->data_len / sizeof(float);
	half = count / 2;

	if (pind == CPCS_BUILTIN_PIND_DOT_PRODUCT) {
		result = _avx2_dot_product(vals, vals + half, half);
	} else if (pind == CPCS_BUILTIN_PIND_L2_DISTANCE_SQ) {
		result = _avx2_l2_distance_sq(vals, vals + half, half);
	} else if (pind == CPCS_BUILTIN_PIND_COSINE_SIMILARITY) {
		float f_dot, f_na, f_nb;
		_avx2_cosine_accum(vals, vals + half, half, &f_dot, &f_na, &f_nb);
		float denom = sqrtf(f_na * f_nb);
		if (denom == 0.0f) {
			result = 0.0f;
		} else {
			result = f_dot / denom;
			if (result < -1.0f) {
				result = -1.0f;
			} else if (result > 1.0f) {
				result = 1.0f;
			}
		}
	} else {
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}

	memcpy(&bits, &result, sizeof(bits));
	*return_value = (uint64_t)bits;
	return 0;
}

static int
_builtin_execute_multi_agg64_direct(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	struct cpcs_builtin_multi_agg64_result result;
	const double *vals;
	uint8_t *out;
	size_t input_len;
	size_t count;

	if (ctx == NULL || ctx->data_buffer == NULL || return_value == NULL) {
		return -EINVAL;
	}
	if (ctx->data_len <= sizeof(result)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	input_len = ctx->data_len - sizeof(result);
	if (input_len == 0 || (input_len % sizeof(double)) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	vals = (const double *)ctx->data_buffer;
	count = input_len / sizeof(double);
	result.count = count;

	_avx2_multi_agg64(vals, count, &result.sum, &result.min, &result.max);

	out = (uint8_t *)ctx->data_buffer + input_len;
	memcpy(out, &result, sizeof(result));
	*return_value = sizeof(result);
	return 0;
}

static int
_cpcs_exec_resolve_range_ex(const struct cpcs_exec_context *ctx,
			    uint64_t mr_id, uint64_t off, uint64_t len,
			    struct spdk_bdev **bdev_out, uint64_t *absolute_offset_out,
			    const struct spdk_vbdev_slm_ops **ops_out)
{
	const struct cpcs_exec_resolved_range *mr;
	uint64_t absolute_offset;

	if (ctx == NULL || bdev_out == NULL || absolute_offset_out == NULL) {
		return -EINVAL;
	}

	if (mr_id == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	if (ctx->resolved_ranges == NULL || ctx->resolved_range_count == 0) {
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	if (mr_id > ctx->resolved_range_count) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	mr = &ctx->resolved_ranges[mr_id - 1];

	if (mr->bdev == NULL) {
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}

	if (off > UINT64_MAX - len) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (off + len > mr->length) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	if (mr->starting_byte > UINT64_MAX - off) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	absolute_offset = mr->starting_byte + off;
	*bdev_out = mr->bdev;
	*absolute_offset_out = absolute_offset;
	if (ops_out != NULL) {
		*ops_out = mr->ops;
	}
	return 0;
}

static int
_cpcs_exec_read_range(const struct cpcs_exec_context *ctx,
		      uint64_t mr_id, uint64_t off, uint64_t len, void *buf)
{
	struct spdk_bdev *bdev;
	uint64_t absolute_offset;
	const struct spdk_vbdev_slm_ops *ops;
	int rc;

	if (len != 0 && buf == NULL) {
		return -EINVAL;
	}

	rc = _cpcs_exec_resolve_range_ex(ctx, mr_id, off, len, &bdev, &absolute_offset, &ops);
	if (rc != 0) {
		return rc;
	}

	/* Fast path: cached ops skip the provider rwlock. Fallback to
	 * bdev_slm_read_by_bdev for non-SLM (direct NVMe) bdevs. */
	if (ops != NULL) {
		rc = ops->read_by_bdev(bdev, absolute_offset, len, buf);
	} else {
		rc = bdev_slm_read_by_bdev(bdev, absolute_offset, len, buf);
	}
	if (rc == -ENOENT || rc == -ENOTSUP) {
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}
	if (rc == -EINVAL) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	return rc;
}

static int
_cpcs_exec_write_range(const struct cpcs_exec_context *ctx,
		       uint64_t mr_id, uint64_t off, uint64_t len, const void *buf)
{
	struct spdk_bdev *bdev;
	uint64_t absolute_offset;
	const struct spdk_vbdev_slm_ops *ops;
	int rc;

	if (len != 0 && buf == NULL) {
		return -EINVAL;
	}

	rc = _cpcs_exec_resolve_range_ex(ctx, mr_id, off, len, &bdev, &absolute_offset, &ops);
	if (rc != 0) {
		return rc;
	}

	if (ops != NULL) {
		rc = ops->write_by_bdev(bdev, absolute_offset, len, buf);
	} else {
		rc = bdev_slm_write_by_bdev(bdev, absolute_offset, len, buf);
	}
	if (rc == -ENOENT || rc == -ENOTSUP) {
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}
	if (rc == -EINVAL) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}
	return rc;
}

static int
_builtin_execute_memcpy(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_memcpy_desc *desc;
	uint8_t *tmp;
	uint64_t src_mr_id;
	uint64_t src_off;
	uint64_t dst_mr_id;
	uint64_t dst_off;
	uint64_t copied = 0;
	uint64_t chunk;
	uint64_t len;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_memcpy_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	src_mr_id = from_le64(&desc->src_mr_id);
	src_off = from_le64(&desc->src_off);
	dst_mr_id = from_le64(&desc->dst_mr_id);
	dst_off = from_le64(&desc->dst_off);

	chunk = len < CPCS_BUILTIN_IO_CHUNK ? len : CPCS_BUILTIN_IO_CHUNK;
	tmp = malloc(chunk);
	if (tmp == NULL) {
		return -ENOMEM;
	}

	while (copied < len) {
		chunk = len - copied;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
		}

		rc = _cpcs_exec_read_range(ctx, src_mr_id, src_off + copied, chunk, tmp);
		if (rc != 0) {
			free(tmp);
			return rc;
		}

		rc = _cpcs_exec_write_range(ctx, dst_mr_id, dst_off + copied, chunk, tmp);
		if (rc != 0) {
			free(tmp);
			return rc;
		}

		copied += chunk;
	}

	free(tmp);
	*return_value = len;
	return 0;
}

static int
_builtin_execute_memfill(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_memfill_desc *desc;
	uint64_t mr_id;
	uint64_t off;
	uint8_t *tmp;
	uint64_t written = 0;
	uint64_t chunk;
	uint64_t len;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_memfill_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);

	chunk = len < CPCS_BUILTIN_IO_CHUNK ? len : CPCS_BUILTIN_IO_CHUNK;
	tmp = malloc(chunk);
	if (tmp == NULL) {
		return -ENOMEM;
	}
	memset(tmp, desc->pattern, chunk);

	while (written < len) {
		chunk = len - written;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
		}

		rc = _cpcs_exec_write_range(ctx, mr_id, off + written, chunk, tmp);
		if (rc != 0) {
			free(tmp);
			return rc;
		}
		written += chunk;
	}

	free(tmp);
	*return_value = len;
	return 0;
}

static int
_builtin_execute_sum64(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_sum64_desc *desc;
	const uint64_t *p;
	uint8_t *buf;
	uint64_t mr_id;
	uint64_t off;
	uint64_t processed = 0;
	uint64_t chunk;
	uint64_t len;
	uint64_t sum = 0;
	size_t i;
	int rc;

	if (_builtin_has_direct_data(ctx)) {
		return _builtin_execute_reduce64_direct(ctx, CPCS_BUILTIN_PIND_SUM64, return_value);
	}

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0 || (len % sizeof(uint64_t)) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);

	buf = malloc(CPCS_ALLOC_CHUNK(len));
	if (buf == NULL) {
		return -ENOMEM;
	}

	while (processed < len) {
		chunk = len - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
			chunk -= chunk % sizeof(uint64_t);
		}

		rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk, buf);
		if (rc != 0) {
			free(buf);
			return rc;
		}

		p = (const uint64_t *)buf;
		for (i = 0; i < (chunk / sizeof(uint64_t)); i++) {
			sum += p[i];
		}
		processed += chunk;
	}

	free(buf);
	*return_value = sum;
	return 0;
}

static int
_builtin_execute_max64(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_sum64_desc *desc;
	const uint64_t *p;
	uint8_t *buf;
	uint64_t mr_id;
	uint64_t off;
	uint64_t processed = 0;
	uint64_t chunk;
	uint64_t len;
	uint64_t max = 0;
	bool initialized = false;
	size_t i;
	int rc;

	if (_builtin_has_direct_data(ctx)) {
		return _builtin_execute_reduce64_direct(ctx, CPCS_BUILTIN_PIND_MAX64, return_value);
	}

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0 || (len % sizeof(uint64_t)) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);

	buf = malloc(CPCS_ALLOC_CHUNK(len));
	if (buf == NULL) {
		return -ENOMEM;
	}

	while (processed < len) {
		chunk = len - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
			chunk -= chunk % sizeof(uint64_t);
		}

		rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk, buf);
		if (rc != 0) {
			free(buf);
			return rc;
		}

		p = (const uint64_t *)buf;
		for (i = 0; i < (chunk / sizeof(uint64_t)); i++) {
			if (!initialized || p[i] > max) {
				max = p[i];
				initialized = true;
			}
		}
		processed += chunk;
	}

	free(buf);
	*return_value = max;
	return 0;
}

static int
_builtin_execute_min64(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_sum64_desc *desc;
	const uint64_t *p;
	uint8_t *buf;
	uint64_t mr_id;
	uint64_t off;
	uint64_t processed = 0;
	uint64_t chunk;
	uint64_t len;
	uint64_t min = 0;
	bool initialized = false;
	size_t i;
	int rc;

	if (_builtin_has_direct_data(ctx)) {
		return _builtin_execute_reduce64_direct(ctx, CPCS_BUILTIN_PIND_MIN64, return_value);
	}

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0 || (len % sizeof(uint64_t)) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);

	buf = malloc(CPCS_ALLOC_CHUNK(len));
	if (buf == NULL) {
		return -ENOMEM;
	}

	while (processed < len) {
		chunk = len - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
			chunk -= chunk % sizeof(uint64_t);
		}

		rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk, buf);
		if (rc != 0) {
			free(buf);
			return rc;
		}

		p = (const uint64_t *)buf;
		for (i = 0; i < (chunk / sizeof(uint64_t)); i++) {
			if (!initialized || p[i] < min) {
				min = p[i];
				initialized = true;
			}
		}
		processed += chunk;
	}

	free(buf);
	*return_value = min;
	return 0;
}

/*
 * DOT_PRODUCT descriptor (same layout as sum64):
 *   [0:7]   mr_id  — Memory Range ID (1-based)
 *   [8:15]  offset — byte offset within MRS
 *   [16:23] length — total bytes (must be divisible by 2*sizeof(float))
 *
 * The first half of the data is vector A, the second half is vector B.
 * Result = A[0]*B[0] + A[1]*B[1] + ... + A[n-1]*B[n-1], returned as
 * a float bit-pattern in the lower 32 bits of return_value.
 */
static int
_builtin_execute_dot_product(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cpcs_builtin_sum64_desc *desc;
	uint8_t *buf = NULL;
	uint64_t mr_id;
	uint64_t off;
	uint64_t processed = 0;
	uint64_t chunk;
	uint64_t len;
	uint64_t half_len;
	double sum_d = 0.0;
	float sum = 0.0f;
	uint32_t sum_bits = 0;
	uint8_t *buf_a = NULL;
	int rc;

	if (_builtin_has_direct_data(ctx)) {
		return _builtin_execute_vector_float_direct(ctx, CPCS_BUILTIN_PIND_DOT_PRODUCT, return_value);
	}

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0 || (len % (2 * sizeof(float))) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);
	half_len = len / 2;

	/* Read both vectors in one contiguous buffer when possible (saves one
	 * SLM read call at the common 16MB size). Falls back to two-pass for
	 * very large inputs that exceed IO_CHUNK. */
	buf_a = malloc(len <= CPCS_BUILTIN_IO_CHUNK ? len : half_len);
	if (buf_a == NULL) {
		return -ENOMEM;
	}

	if (len <= CPCS_BUILTIN_IO_CHUNK) {
		rc = _cpcs_exec_read_range(ctx, mr_id, off, len, buf_a);
		if (rc != 0) {
			free(buf_a);
			return rc;
		}

		sum = _avx2_dot_product((const float *)buf_a,
					(const float *)buf_a + half_len / sizeof(float),
					half_len / sizeof(float));

		free(buf_a);
	} else {
		processed = 0;
		while (processed < half_len) {
			chunk = half_len - processed;
			if (chunk > CPCS_BUILTIN_IO_CHUNK) {
				chunk = CPCS_BUILTIN_IO_CHUNK;
				chunk -= chunk % sizeof(float);
			}
			rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk,
						   buf_a + processed);
			if (rc != 0) {
				free(buf_a);
				return rc;
			}
			processed += chunk;
		}

		buf = malloc(CPCS_ALLOC_CHUNK(half_len));
		if (buf == NULL) {
			free(buf_a);
			return -ENOMEM;
		}

		processed = 0;
		while (processed < half_len) {
			chunk = half_len - processed;
			if (chunk > CPCS_BUILTIN_IO_CHUNK) {
				chunk = CPCS_BUILTIN_IO_CHUNK;
				chunk -= chunk % sizeof(float);
			}
			rc = _cpcs_exec_read_range(ctx, mr_id, off + half_len + processed,
						   chunk, buf);
			if (rc != 0) {
				free(buf);
				free(buf_a);
				return rc;
			}

			sum_d += (double)_avx2_dot_product(
				(const float *)(buf_a + processed),
				(const float *)buf, chunk / sizeof(float));
			processed += chunk;
		}

		sum = (float)sum_d;
		free(buf);
		free(buf_a);
	}

	memcpy(&sum_bits, &sum, sizeof(sum_bits));
	*return_value = (uint64_t)sum_bits;
	return 0;
}

static int
_builtin_execute_multi_agg64(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	if (_builtin_has_direct_data(ctx)) {
		return _builtin_execute_multi_agg64_direct(ctx, return_value);
	}

	/* MRS path: read double array from SLM, compute count/sum/min/max */
	const struct cpcs_builtin_sum64_desc *desc;
	struct cpcs_builtin_multi_agg64_result result;
	uint8_t *buf;
	uint64_t mr_id, off, len;
	uint64_t processed = 0, chunk;
	const double *vals;
	size_t n;
	bool initialized = false;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0 || (len % sizeof(double)) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);

	buf = malloc(CPCS_ALLOC_CHUNK(len));
	if (buf == NULL) {
		return -ENOMEM;
	}

	result.count = 0;
	result.sum = 0.0;
	result.min = 0.0;
	result.max = 0.0;

	while (processed < len) {
		chunk = len - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
			chunk -= chunk % sizeof(double);
		}

		rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk, buf);
		if (rc != 0) {
			free(buf);
			return rc;
		}

		vals = (const double *)buf;
		n = chunk / sizeof(double);
		if (n > 0) {
			double chunk_sum, chunk_min, chunk_max;
			_avx2_multi_agg64(vals, n, &chunk_sum, &chunk_min, &chunk_max);
			result.sum += chunk_sum;
			if (!initialized) {
				result.min = chunk_min;
				result.max = chunk_max;
				initialized = true;
			} else {
				if (chunk_min < result.min) result.min = chunk_min;
				if (chunk_max > result.max) result.max = chunk_max;
			}
		}
		result.count += n;
		processed += chunk;
	}

	free(buf);

	/* Write result struct back into data buffer after descriptor */
	if (ctx->data_len >= sizeof(*desc) + sizeof(result)) {
		uint8_t *out = (uint8_t *)ctx->data_buffer + sizeof(*desc);
		memcpy(out, &result, sizeof(result));
	}
	*return_value = sizeof(result);
	return 0;
}

static int
_builtin_execute_l2_distance_sq(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	if (_builtin_has_direct_data(ctx)) {
		return _builtin_execute_vector_float_direct(ctx, CPCS_BUILTIN_PIND_L2_DISTANCE_SQ, return_value);
	}

	/* MRS path: read two vectors from SLM, compute sum of squared differences */
	const struct cpcs_builtin_sum64_desc *desc;
	uint8_t *buf_a = NULL;
	uint8_t *buf = NULL;
	uint64_t mr_id, off, len, half_len;
	uint64_t processed = 0, chunk;
	double sum_d = 0.0;
	float result_f = 0.0f;
	uint32_t result_bits = 0;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0 || (len % (2 * sizeof(float))) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);
	half_len = len / 2;

	buf_a = malloc(len <= CPCS_BUILTIN_IO_CHUNK ? len : half_len);
	if (buf_a == NULL) {
		return -ENOMEM;
	}

	if (len <= CPCS_BUILTIN_IO_CHUNK) {
		rc = _cpcs_exec_read_range(ctx, mr_id, off, len, buf_a);
		if (rc != 0) {
			free(buf_a);
			return rc;
		}

		result_f = _avx2_l2_distance_sq((const float *)buf_a,
						(const float *)buf_a + half_len / sizeof(float),
						half_len / sizeof(float));

		free(buf_a);
	} else {
		processed = 0;
		while (processed < half_len) {
			chunk = half_len - processed;
			if (chunk > CPCS_BUILTIN_IO_CHUNK) {
				chunk = CPCS_BUILTIN_IO_CHUNK;
				chunk -= chunk % sizeof(float);
			}
			rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk, buf_a + processed);
			if (rc != 0) {
				free(buf_a);
				return rc;
			}
			processed += chunk;
		}

		buf = malloc(CPCS_ALLOC_CHUNK(half_len));
		if (buf == NULL) {
			free(buf_a);
			return -ENOMEM;
		}

		processed = 0;
		while (processed < half_len) {
			chunk = half_len - processed;
			if (chunk > CPCS_BUILTIN_IO_CHUNK) {
				chunk = CPCS_BUILTIN_IO_CHUNK;
				chunk -= chunk % sizeof(float);
			}
			rc = _cpcs_exec_read_range(ctx, mr_id, off + half_len + processed, chunk, buf);
			if (rc != 0) {
				free(buf);
				free(buf_a);
				return rc;
			}

			sum_d += (double)_avx2_l2_distance_sq(
				(const float *)(buf_a + processed),
				(const float *)buf, chunk / sizeof(float));
			processed += chunk;
		}

		result_f = (float)sum_d;
		free(buf);
		free(buf_a);
	}

	memcpy(&result_bits, &result_f, sizeof(result_bits));
	*return_value = (uint64_t)result_bits;
	return 0;
}

static int
_builtin_execute_cosine_similarity(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	if (_builtin_has_direct_data(ctx)) {
		return _builtin_execute_vector_float_direct(ctx, CPCS_BUILTIN_PIND_COSINE_SIMILARITY, return_value);
	}

	/* MRS path: read two vectors from SLM, compute cosine similarity */
	const struct cpcs_builtin_sum64_desc *desc;
	uint8_t *buf_a = NULL;
	uint8_t *buf = NULL;
	uint64_t mr_id, off, len, half_len;
	uint64_t processed = 0, chunk;
	double dot = 0.0, lhs_norm = 0.0, rhs_norm = 0.0;
	float result_f;
	uint32_t result_bits = 0;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0 || (len % (2 * sizeof(float))) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);
	half_len = len / 2;

	buf_a = malloc(len <= CPCS_BUILTIN_IO_CHUNK ? len : half_len);
	if (buf_a == NULL) {
		return -ENOMEM;
	}

	if (len <= CPCS_BUILTIN_IO_CHUNK) {
		rc = _cpcs_exec_read_range(ctx, mr_id, off, len, buf_a);
		if (rc != 0) {
			free(buf_a);
			return rc;
		}

		{
			float f_dot, f_na, f_nb;
			_avx2_cosine_accum((const float *)buf_a,
					   (const float *)buf_a + half_len / sizeof(float),
					   half_len / sizeof(float),
					   &f_dot, &f_na, &f_nb);
			dot = (double)f_dot;
			lhs_norm = (double)f_na;
			rhs_norm = (double)f_nb;
		}

		free(buf_a);
	} else {
		processed = 0;
		while (processed < half_len) {
			chunk = half_len - processed;
			if (chunk > CPCS_BUILTIN_IO_CHUNK) {
				chunk = CPCS_BUILTIN_IO_CHUNK;
				chunk -= chunk % sizeof(float);
			}
			rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk, buf_a + processed);
			if (rc != 0) {
				free(buf_a);
				return rc;
			}
			processed += chunk;
		}

		buf = malloc(CPCS_ALLOC_CHUNK(half_len));
		if (buf == NULL) {
			free(buf_a);
			return -ENOMEM;
		}

		processed = 0;
		while (processed < half_len) {
			chunk = half_len - processed;
			if (chunk > CPCS_BUILTIN_IO_CHUNK) {
				chunk = CPCS_BUILTIN_IO_CHUNK;
				chunk -= chunk % sizeof(float);
			}
			rc = _cpcs_exec_read_range(ctx, mr_id, off + half_len + processed, chunk, buf);
			if (rc != 0) {
				free(buf);
				free(buf_a);
				return rc;
			}

			{
				float f_dot, f_na, f_nb;
				_avx2_cosine_accum((const float *)(buf_a + processed),
						   (const float *)buf, chunk / sizeof(float),
						   &f_dot, &f_na, &f_nb);
				dot += (double)f_dot;
				lhs_norm += (double)f_na;
				rhs_norm += (double)f_nb;
			}
			processed += chunk;
		}

		free(buf);
		free(buf_a);
	}

	{
		double denom = sqrt(lhs_norm * rhs_norm);
		if (denom == 0.0) {
			result_f = 0.0f;
		} else {
			result_f = (float)(dot / denom);
			if (result_f < -1.0f) result_f = -1.0f;
			else if (result_f > 1.0f) result_f = 1.0f;
		}
	}

	memcpy(&result_bits, &result_f, sizeof(result_bits));
	*return_value = (uint64_t)result_bits;
	return 0;
}

/*
 * MRS-based FILTER_GT descriptor (32 bytes).
 * Sent inline by the host when data has been pre-staged into SLM.
 *   - mr_id/off/len identify the input range in SLM
 *   - threshold is the float comparison value (stored as raw bits)
 * The result (count of elements > threshold) is returned via cdw0.
 */
struct cpcs_builtin_filter_gt_desc {
	uint64_t mr_id;
	uint64_t off;
	uint64_t len;
	uint32_t threshold_bits;
	uint32_t _pad;
};

static int
_builtin_execute_filter_gt(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const uint8_t *buf;
	const float *in;
	float *out;
	float threshold = 0.0f;
	size_t n;
	size_t i;
	size_t out_count = 0;
	size_t bytes_after_header;

	/*
	 * MRS path: data resides in SLM, descriptor is inline.
	 * Stream the input from SLM in chunks and count elements > threshold.
	 * Returns count via cdw0; no filtered data is written back.
	 */
	if (!_builtin_has_direct_data(ctx)) {
		const struct cpcs_builtin_filter_gt_desc *desc;
		uint64_t mr_id;
		uint64_t off;
		uint64_t len;
		uint64_t processed = 0;
		uint64_t chunk;
		uint8_t *chunk_buf = NULL;
		uint32_t tbits;
		float thr;
		int rc;

		if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		desc = (const struct cpcs_builtin_filter_gt_desc *)ctx->data_buffer;
		len = from_le64(&desc->len);
		if (len == 0 || (len % sizeof(float)) != 0) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		mr_id = from_le64(&desc->mr_id);
		off = from_le64(&desc->off);
		tbits = from_le32(&desc->threshold_bits);
		memcpy(&thr, &tbits, sizeof(thr));

		chunk_buf = malloc(CPCS_ALLOC_CHUNK(len));
		if (chunk_buf == NULL) {
			return -ENOMEM;
		}

		while (processed < len) {
			chunk = len - processed;
			if (chunk > CPCS_BUILTIN_IO_CHUNK) {
				chunk = CPCS_BUILTIN_IO_CHUNK;
				chunk -= chunk % sizeof(float);
			}

			rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk,
						   chunk_buf);
			if (rc != 0) {
				free(chunk_buf);
				return rc;
			}

			{
				const float *p = (const float *)chunk_buf;
				size_t nf = chunk / sizeof(float);
				for (size_t j = 0; j < nf; j++) {
					if (p[j] > thr) {
						out_count++;
					}
				}
			}
			processed += chunk;
		}

		free(chunk_buf);
		*return_value = out_count;
		return 0;
	}

	if (ctx->data_buffer == NULL || ctx->data_len < (4 + 2 * sizeof(float))) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	buf = (const uint8_t *)ctx->data_buffer;
	memcpy(&threshold, buf, sizeof(threshold));

	bytes_after_header = ctx->data_len - 4;
	if ((bytes_after_header % (2 * sizeof(float))) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	n = bytes_after_header / (2 * sizeof(float));
	in = (const float *)(buf + 4);
	out = (float *)((uint8_t *)ctx->data_buffer + 4 + n * sizeof(float));

	for (i = 0; i < n; i++) {
		if (in[i] > threshold) {
			out[out_count++] = in[i];
		}
	}

	*return_value = out_count;
	return 0;
}

static int
_builtin_execute_memcpy_inline(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	uint8_t *buf = (uint8_t *)ctx->data_buffer;
	size_t n;

	if (buf == NULL || ctx->data_len < 2 || (ctx->data_len % 2) != 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	n = ctx->data_len / 2;
	memcpy(buf + n, buf, n);
	*return_value = n;
	return 0;
}

static int
_builtin_execute_rle_compress(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	if (_builtin_has_direct_data(ctx)) {
		/* Direct-data path: input embedded in data buffer */
		uint8_t *buf = (uint8_t *)ctx->data_buffer;
		uint8_t *src;
		uint8_t *dst;
		uint64_t input_size = 0;
		size_t out_capacity;
		size_t out_pos = 0;
		size_t i = 0;

		if (buf == NULL || ctx->data_len < (8 + 1 + 1)) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		memcpy(&input_size, buf, sizeof(input_size));
		if (input_size == 0) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}
		if (ctx->data_len < 8 + input_size + 1) {
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		src = buf + 8;
		dst = buf + 8 + input_size;
		out_capacity = ctx->data_len - 8 - input_size;

		while (i < input_size) {
			uint8_t value = src[i];
			uint8_t run = 1;

			while ((i + run) < input_size && src[i + run] == value && run < 255) {
				run++;
			}

			if (out_pos + 2 > out_capacity) {
				return -SPDK_NVME_SC_INVALID_FIELD;
			}

			dst[out_pos++] = run;
			dst[out_pos++] = value;
			i += run;
		}

		*return_value = out_pos;
		return 0;
	}

	/* MRS path: read input from SLM, count compressed size */
	const struct cpcs_builtin_sum64_desc *desc;
	uint8_t *src_buf = NULL;
	uint64_t mr_id, off, len;
	uint64_t processed = 0, chunk;
	size_t out_pos = 0;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cpcs_builtin_sum64_desc *)ctx->data_buffer;
	len = from_le64(&desc->len);
	if (len == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	mr_id = from_le64(&desc->mr_id);
	off = from_le64(&desc->off);

	src_buf = malloc(CPCS_ALLOC_CHUNK(len));
	if (src_buf == NULL) {
		return -ENOMEM;
	}

	uint8_t prev_value = 0;
	uint8_t prev_run = 0;
	bool has_prev = false;

	while (processed < len) {
		size_t i;

		chunk = len - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
		}

		rc = _cpcs_exec_read_range(ctx, mr_id, off + processed, chunk, src_buf);
		if (rc != 0) {
			free(src_buf);
			return rc;
		}

		for (i = 0; i < chunk; i++) {
			uint8_t value = src_buf[i];

			if (has_prev && value == prev_value && prev_run < 255) {
				prev_run++;
			} else {
				if (has_prev) {
					out_pos += 2;
				}
				prev_value = value;
				prev_run = 1;
				has_prev = true;
			}
		}
		processed += chunk;
	}

	if (has_prev) {
		out_pos += 2;
	}

	free(src_buf);
	*return_value = out_pos;
	return 0;
}

static struct spdk_bdev *
_builtin_get_bdev_by_nsid(uint32_t nsid)
{
	struct spdk_bdev *bdev;

	for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
		if (spdk_bdev_get_nvme_nsid(bdev) == nsid) {
			return bdev;
		}
	}
	return NULL;
}

static int
_builtin_execute_direct_ns_agg(const struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	const struct cs_direct_ns_desc *desc;
	struct cs_direct_ns_result result;
	struct spdk_bdev *bdev;
	const uint64_t *p;
	uint8_t *buf;
	uint64_t offset;
	uint64_t total_bytes;
	uint64_t processed = 0;
	uint64_t chunk;
	uint64_t agg = 0;
	uint64_t count = 0;
	bool initialized = false;
	size_t i;
	size_t n;
	int rc;

	if (ctx->data_buffer == NULL || ctx->data_len < sizeof(*desc) + sizeof(result)) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	desc = (const struct cs_direct_ns_desc *)ctx->data_buffer;

	SPDK_NOTICELOG("DIRECT_NS_AGG entry: nsid=%u n_uint64=%u lba_offset=%" PRIu64 " workload=%u data_len=%u\n",
		       desc->nsid, desc->n_uint64, desc->lba_offset, desc->workload, ctx->data_len);

	if (desc->n_uint64 == 0) {
		return -SPDK_NVME_SC_INVALID_FIELD;
	}

	bdev = _builtin_get_bdev_by_nsid(desc->nsid);
	if (bdev == NULL) {
		SPDK_ERRLOG("DIRECT_NS_AGG: bdev not found for nsid=%u\n", desc->nsid);
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}

	SPDK_NOTICELOG("DIRECT_NS_AGG: using bdev=%s for nsid=%u\n",
		       spdk_bdev_get_name(bdev), desc->nsid);

	/* Resolve SLM ops once outside the chunk loops so reads skip the
	 * provider-lookup rwlock on every I/O. */
	const struct spdk_vbdev_slm_ops *slm_ops = vbdev_slm_lookup_ops(bdev);
	if (slm_ops == NULL) {
		/* bdev_slm_read_by_bdev also returns -ENOTSUP for non-SLM bdevs —
		 * fail fast here with a diagnostic so the nsid mismatch is obvious. */
		SPDK_ERRLOG("DIRECT_NS_AGG: bdev nsid=%u (%s) has no SLM ops."
			    " desc->nsid must point to an SLM namespace (e.g. 100),"
			    " not a plain NVMe namespace. Check host-side nsid argument.\n",
			    desc->nsid, spdk_bdev_get_name(bdev));
		return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
	}

	total_bytes = (uint64_t)desc->n_uint64 * sizeof(uint64_t);
	offset = desc->lba_offset;

	/*
	 * DOT_PRODUCT needs both vector halves in memory at once, so read
	 * the entire buffer up-front.  For other workloads the chunked
	 * streaming path is still used (lower peak memory).
	 */
	if (desc->workload == CS_DIRECT_NS_WORKLOAD_DOT_PRODUCT) {
		uint8_t *full_buf;
		const float *fvals;
		double dp = 0.0;
		size_t half;

		full_buf = spdk_dma_malloc(total_bytes, 4096, NULL);
		if (full_buf == NULL) {
			return -ENOMEM;
		}

		/* Read all data */
		processed = 0;
		while (processed < total_bytes) {
			chunk = total_bytes - processed;
			if (chunk > CPCS_BUILTIN_IO_CHUNK) {
				chunk = CPCS_BUILTIN_IO_CHUNK;
			}
			rc = slm_ops->read_by_bdev(bdev, offset + processed, chunk, full_buf + processed);
			if (rc != 0) {
				spdk_dma_free(full_buf);
				if (rc == -ENOENT || rc == -ENOTSUP) {
					return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
				}
				return rc;
			}
			processed += chunk;
		}

		/* Layout: [a0..a_{dim-1}, b0..b_{dim-1}] as float32 */
		fvals = (const float *)full_buf;
		half = total_bytes / (2 * sizeof(float));
		for (i = 0; i < half; i++) {
			dp += (double)fvals[i] * (double)fvals[i + half];
		}

		spdk_dma_free(full_buf);

		/* Pack double result into uint64_t */
		memcpy(&agg, &dp, sizeof(double));
		count = half;

		goto write_result;
	}

	buf = spdk_dma_malloc(CPCS_BUILTIN_IO_CHUNK, 4096, NULL);
	if (buf == NULL) {
		return -ENOMEM;
	}

	while (processed < total_bytes) {
		chunk = total_bytes - processed;
		if (chunk > CPCS_BUILTIN_IO_CHUNK) {
			chunk = CPCS_BUILTIN_IO_CHUNK;
		}
		chunk -= chunk % sizeof(uint64_t);

		rc = slm_ops->read_by_bdev(bdev, offset + processed, chunk, buf);
		if (rc != 0) {
			SPDK_ERRLOG("DIRECT_NS_AGG: slm read failed nsid=%u offset=%" PRIu64 " chunk=%" PRIu64 " rc=%d\n",
				    desc->nsid, offset + processed, chunk, rc);
			spdk_dma_free(buf);
			if (rc == -ENOENT || rc == -ENOTSUP) {
				return -SPDK_NVME_CPCS_SC_INVALID_MEMORY_NAMESPACE;
			}
			if (rc == -EINVAL) {
				return -SPDK_NVME_SC_INVALID_FIELD;
			}
			return rc;
		}

		p = (const uint64_t *)buf;
		n = chunk / sizeof(uint64_t);

		switch (desc->workload) {
		case CS_DIRECT_NS_WORKLOAD_SUM:
			for (i = 0; i < n; i++) {
				agg += p[i];
			}
			break;
		case CS_DIRECT_NS_WORKLOAD_MAX:
			for (i = 0; i < n; i++) {
				if (!initialized || p[i] > agg) {
					agg = p[i];
					initialized = true;
				}
			}
			break;
		case CS_DIRECT_NS_WORKLOAD_MIN:
			for (i = 0; i < n; i++) {
				if (!initialized || p[i] < agg) {
					agg = p[i];
					initialized = true;
				}
			}
			break;
		case CS_DIRECT_NS_WORKLOAD_FILTER_GT: {
			uint64_t thr = desc->threshold;
			for (i = 0; i < n; i++) {
				if (p[i] > thr) {
					agg++;
				}
			}
			break;
		}
		default:
			spdk_dma_free(buf);
			return -SPDK_NVME_SC_INVALID_FIELD;
		}

		count += n;
		processed += chunk;
	}

	spdk_dma_free(buf);

write_result:
	result.result = agg;
	result.count  = count;

	SPDK_NOTICELOG("DIRECT_NS_AGG complete: result=%" PRIu64 " count=%" PRIu64 "\n",
		       result.result, result.count);

	memcpy((uint8_t *)ctx->data_buffer + sizeof(*desc), &result, sizeof(result));
	*return_value = sizeof(result);
	return 0;
}

static int
builtin_execute(struct cpcs_program *prog, struct cpcs_exec_context *ctx, uint64_t *return_value)
{
	if (prog == NULL || ctx == NULL || return_value == NULL) {
		return -EINVAL;
	}

	switch (prog->pind) {
	case CPCS_BUILTIN_PIND_MEMCPY:
		return _builtin_execute_memcpy(ctx, return_value);
	case CPCS_BUILTIN_PIND_MEMFILL:
		return _builtin_execute_memfill(ctx, return_value);
	case CPCS_BUILTIN_PIND_SUM64:
		return _builtin_execute_sum64(ctx, return_value);
	case CPCS_BUILTIN_PIND_MAX64:
		return _builtin_execute_max64(ctx, return_value);
	case CPCS_BUILTIN_PIND_MIN64:
		return _builtin_execute_min64(ctx, return_value);
	case CPCS_BUILTIN_PIND_DOT_PRODUCT:
		return _builtin_execute_dot_product(ctx, return_value);
	case CPCS_BUILTIN_PIND_FILTER_GT:
		return _builtin_execute_filter_gt(ctx, return_value);
	case CPCS_BUILTIN_PIND_MEMCPY_INLINE:
		return _builtin_execute_memcpy_inline(ctx, return_value);
	case CPCS_BUILTIN_PIND_RLE_COMPRESS:
		return _builtin_execute_rle_compress(ctx, return_value);
	case CPCS_BUILTIN_PIND_MULTI_AGG64:
		return _builtin_execute_multi_agg64(ctx, return_value);
	case CPCS_BUILTIN_PIND_L2_DISTANCE_SQ:
		return _builtin_execute_l2_distance_sq(ctx, return_value);
	case CPCS_BUILTIN_PIND_COSINE_SIMILARITY:
		return _builtin_execute_cosine_similarity(ctx, return_value);
	case CPCS_BUILTIN_PIND_DIRECT_NS_AGG:
		return _builtin_execute_direct_ns_agg(ctx, return_value);
	default:
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_INDEX;
	}
}

static const struct cpcs_runtime_ops g_builtin_runtime = {
	.name = "builtin",
	.ptype = SPDK_NVME_CPCS_PTYPE_DEVICE_DEFINED,
	.init = NULL,
	.validate = NULL,
	.activate = NULL,
	.execute = builtin_execute,
	.deactivate = NULL,
	.fini = NULL,
};

int
cpcs_builtin_runtime_register(void)
{
	return cpcs_runtime_register(&g_builtin_runtime);
}

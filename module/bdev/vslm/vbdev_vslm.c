/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 CPCS Implementation Team. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/bdev_vslm.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/nvme_spec.h"
#include "spdk/util.h"
#include "spdk_internal/vbdev_slm.h"

#define VSLM_PAGE_SIZE  4096
#define VSLM_PAGE_SHIFT 12
#define VSLM_DEFAULT_READAHEAD_PAGES 4
#define VSLM_MAX_READAHEAD_PAGES 64
#define VSLM_DEFAULT_FAULT_BATCH_PAGES 512
#define VSLM_DEFAULT_FAULT_BATCH_MAX_BYTES (2 * 1024 * 1024)
#define VSLM_DEFAULT_PREFETCH_BATCH_PAGES 64
#define VSLM_DEFAULT_PREFETCH_QUEUE_DEPTH 4
#define VSLM_DEFAULT_CLEANER_MAX_PAGES_PER_POLL 16
#define VSLM_DEFAULT_STREAMING_IO_PAGES 256
#define VSLM_ASYNC_EXEC_READ_CHUNK_BYTES (256 * 1024)

/* SLM command set opcodes (CSI 0x03) */
#define SPDK_NVME_SLM_OPC_MEMORY_READ  0x02
#define SPDK_NVME_SLM_OPC_MEMORY_FILL  0x04
#define SPDK_NVME_SLM_OPC_MEMORY_WRITE 0x05

static const struct spdk_bdev_fn_table vbdev_vslm_fn_table;
static bool vbdev_vslm_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);

static TAILQ_HEAD(, vbdev_vslm) g_vslm_bdevs = TAILQ_HEAD_INITIALIZER(g_vslm_bdevs);

/*
 * Guards every insert/remove/traverse of g_vslm_bdevs. The list is mutated by
 * create/delete on the management thread and walked by NSID/name lookups,
 * including CPCS lease ops invoked from I/O threads. The lock is only ever held
 * for the duration of a list walk (releasing before any per-bdev shard/lease
 * lock is taken), so it never participates in lock-ordering with those locks.
 */
static pthread_mutex_t g_vslm_bdevs_lock = PTHREAD_MUTEX_INITIALIZER;

enum vslm_page_location {
	VSLM_PAGE_LOC_NONE = 0,
	VSLM_PAGE_LOC_FREE,
	VSLM_PAGE_LOC_LRU,
};

enum vslm_page_load_state {
	VSLM_PAGE_LOAD_FREE = 0,
	VSLM_PAGE_LOAD_LOADING,
	VSLM_PAGE_LOAD_RESIDENT,
	VSLM_PAGE_LOAD_EVICTING,
};

struct vslm_page;

typedef void (*vslm_page_waiter_cb)(void *cb_arg, int status,
				    struct vslm_page *page);

struct vslm_page_waiter {
	struct vbdev_vslm *vslm;
	struct spdk_thread *thread;
	vslm_page_waiter_cb cb_fn;
	void *cb_arg;
	uint64_t queued_ticks;
	int status;
	struct vslm_page *page;
	TAILQ_ENTRY(vslm_page_waiter) link;
};

TAILQ_HEAD(vslm_page_waiter_list, vslm_page_waiter);

struct vslm_page {
	uint64_t vpn;
	uint64_t ppn;
	uint16_t shard_id;	/* shard that owns this frame (fixed at init) */
	bool dirty;
	bool is_busy;
	enum vslm_page_location location;
	bool on_hash;
	enum vslm_page_load_state load_state;
	int load_status;
	bool prefetched;
	uint32_t pin_count;
	struct vslm_page_waiter_list waiters;
	TAILQ_ENTRY(vslm_page) lru_link;
	LIST_ENTRY(vslm_page) hash_link;
};

struct vslm_lpage {
	/*
	 * Absence of vslm_lpage metadata means the logical page is a default
	 * clean backing page:
	 *
	 *   source_type         = SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING
	 *   source_bdev         = vslm->base_bdev
	 *   source_offset_bytes = vpn * VSLM_PAGE_SIZE
	 *   pending_publish     = false
	 *   private state       = none
	 *
	 * Allocate vslm_lpage only for dirty/private, alias, spilled, pending
	 * publish, superseded-source, or other non-default page states.
	 */
	uint64_t vpn;
	enum spdk_bdev_vslm_lpage_state state;
	enum spdk_bdev_vslm_page_source source_type;
	struct spdk_bdev *source_bdev;
	uint64_t source_offset_bytes;
	bool has_superseded_source;
	enum spdk_bdev_vslm_page_source superseded_source_type;
	struct spdk_bdev *superseded_source_bdev;
	uint64_t superseded_source_offset_bytes;
	bool pending_publish;
	bool private_authoritative;
	LIST_ENTRY(vslm_lpage) hash_link;
};

struct vslm_lease {
	uint64_t lease_id;
	uint64_t offset;
	uint64_t length;
	TAILQ_ENTRY(vslm_lease) link;
};

struct vslm_blocked_cmd {
	struct spdk_bdev_io *bdev_io;
	uint64_t queued_ticks;
	TAILQ_ENTRY(vslm_blocked_cmd) link;
};

struct vslm_lease_range {
	uint64_t offset;
	uint64_t length;
};

struct vslm_fault_batch_page {
	struct vslm_page *page;
	struct vslm_lpage *lpage;	/* non-NULL for a clean-alias page (see batch commit) */
	uint64_t vpn;
	bool inserted_hash;
	bool inserted_lru;
};

struct vslm_fault_batch_ctx {
	struct vbdev_vslm *vslm;
	struct spdk_io_channel *base_ch;
	uint64_t start_vpn;
	uint32_t nr_pages;
	struct vslm_fault_batch_page *pages;
	struct iovec *iov;
	void *bounce_buf;
	bool used_bounce;
};

struct vslm_prefetch_batch_ctx {
	struct vbdev_vslm *vslm;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	uint64_t start_vpn;
	uint32_t nr_pages;
	struct vslm_page **pages;
	struct iovec *iov;
	uint64_t submit_ticks;
};

struct vslm_single_fault_ctx {
	uint64_t vpn;
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	enum spdk_bdev_vslm_lpage_state pre_fault_state;
	bool skip_fault_in;
	bool needs_fault_in;
};

struct vslm_perf_stats {
	uint64_t resolve_total;
	uint64_t resolve_hit_total;
	uint64_t resolve_miss_total;
	uint64_t resolve_evict_total;
	uint64_t resolve_skip_fault_in_total;
	uint64_t resolve_ns_total;
	uint64_t resolve_ns_max;

	uint64_t lpage_lookup_total;
	uint64_t lpage_alloc_total;
	uint64_t lpage_alloc_clean_backing_total;
	uint64_t lpage_free_total;

	uint64_t page_hash_lookup_total;
	uint64_t page_hash_hit_total;
	uint64_t page_hash_miss_total;
	uint64_t page_hash_insert_total;
	uint64_t page_hash_remove_total;

	uint64_t lru_touch_total;
	uint64_t lru_touch_move_total;
	uint64_t lru_touch_insert_total;
	uint64_t lru_touch_walk_steps;

	uint64_t free_list_pop_total;
	uint64_t free_list_push_total;

	uint64_t victim_pick_total;
	uint64_t victim_skip_busy_total;
	uint64_t victim_skip_pinned_total;
	uint64_t victim_skip_loading_total;
	uint64_t victim_skip_lease_total;

	uint64_t fault_in_total;
	uint64_t fault_in_4k_total;
	uint64_t fault_in_4k_bytes;
	uint64_t fault_in_io_ns_total;
	uint64_t fault_in_io_ns_max;

	uint64_t prefetch_call_total;
	uint64_t prefetch_page_attempt_total;
	uint64_t prefetch_page_loaded_total;
	uint64_t prefetch_page_skipped_resident_total;
	uint64_t prefetch_page_skipped_pending_publish_total;
	uint64_t prefetch_page_skipped_no_free_page_total;
	uint64_t prefetch_ns_total;
	uint64_t prefetch_ns_max;

	uint64_t mmu_lock_acquire_total;
	uint64_t mmu_lock_hold_ns_total;
	uint64_t mmu_lock_hold_ns_max;

	uint64_t memcpy_total;
	uint64_t memcpy_bytes_total;
	uint64_t memcpy_ns_total;
	uint64_t memcpy_ns_max;

	uint64_t sync_base_io_total;
	uint64_t sync_base_io_ns_total;
	uint64_t sync_base_io_ns_max;
	uint64_t sync_read_desc_io_total;
	uint64_t sync_read_desc_io_ns_total;
	uint64_t sync_read_desc_io_ns_max;
	uint64_t fault_in_batched_total;
	uint64_t fault_in_batched_pages;
	uint64_t fault_in_batched_bytes;
	uint64_t fault_in_batched_failed_total;
	uint64_t fault_in_batched_fallback_total;
	uint64_t fault_in_batched_readv_total;
	uint64_t fault_batch_attempt_total;
	uint64_t fault_batch_fallback_total;
	uint64_t fault_in_batched_bounce_total;
	uint64_t fault_in_batched_bounce_bytes;
	uint64_t fault_in_batched_bounce_memcpy_bytes;
	uint64_t fault_in_batched_bounce_memcpy_ns_total;
	uint64_t prefetch_async_submit_total;
	uint64_t prefetch_async_complete_total;
	uint64_t prefetch_async_failed_total;
	uint64_t prefetch_async_io_ns_total;
	uint64_t prefetch_async_io_ns_max;
	uint64_t prefetch_async_page_loaded_total;
	uint64_t prefetch_async_page_failed_total;
	uint64_t prefetch_async_page_skipped_resident_total;
	uint64_t prefetch_async_page_skipped_lpage_total;
	uint64_t prefetch_async_page_skipped_no_free_total;
	uint64_t prefetch_async_hit_total;
	uint64_t prefetch_async_wait_total;
	uint64_t prefetch_async_wait_ns_total;
	uint64_t prefetch_clean_default_total;
	uint64_t dirty_page_missing_lpage_total;
	uint64_t cleaner_poll_total;
	uint64_t cleaner_writeback_submit_total;
	uint64_t cleaner_writeback_complete_total;
	uint64_t cleaner_pages_cleaned_total;
	uint64_t cleaner_skipped_pinned_total;
	uint64_t cleaner_skipped_busy_total;
	uint64_t cleaner_skipped_lease_total;
};

/*
 * Shared-nothing partitioned MMU (paper section 4.3).
 *
 * Logical page mapping structures are partitioned into independent shards by a
 * Virtual Page Number (VPN) hashing strategy. Each shard owns a private lock, a
 * private page/lpage hash table, a private LRU chain, a private free list, and a
 * disjoint slice of SRAM frames. A VPN maps to exactly one shard, so resolving,
 * faulting, evicting, and reclaiming a page only ever touches that shard's
 * structures under that shard's lock. This eliminates the cross-core contention
 * of a single global MMU lock during active page resolution.
 *
 * A block-cyclic VPN->shard mapping (runs of VSLM_MMU_SHARD_STRIDE_PAGES
 * consecutive VPNs share a shard) preserves the spatial locality needed by the
 * clean-backing fault batching and strided prefetch paths while still spreading
 * a large working set across every shard.
 *
 * Invariant: at most one shard lock is held at a time on every code path, so no
 * lock-ordering deadlock is possible. Whole-instance operations (teardown, debug
 * validation) walk shards one at a time.
 */
#define VSLM_MMU_SHARD_STRIDE_PAGES 512u
#define VSLM_MMU_MIN_FRAMES_PER_SHARD 1024u
#define VSLM_MMU_MAX_SHARDS 32u

/*
 * Static DMA ring-buffer pool for the vector-fallback fault-in path (paper
 * section 4.5). A fixed pool of controller-visible, DMA-aligned buffers is
 * established at initialization so the fallback never pays a dynamic
 * runtime allocation penalty on the I/O path.
 */
#define VSLM_DMA_RING_BUF_COUNT 4u
#define VSLM_DMA_RING_BUF_BYTES (256u * 1024u)

struct vslm_mmu_shard {
	pthread_mutex_t lock;
	LIST_HEAD(, vslm_page) *hash_table;
	LIST_HEAD(, vslm_lpage) *lpage_hash_table;
	uint64_t num_buckets;
	TAILQ_HEAD(, vslm_page) lru_list;
	TAILQ_HEAD(, vslm_page) free_list;
	uint64_t base_ppn;
	uint64_t num_frames;
	/*
	 * O(1) running count of frames on free_list, maintained at every free-list
	 * push/pop (all funneled through vslm_free_list_insert_tail/vslm_get_free_page
	 * plus the init-time seed below) under shard->lock. Lets the background
	 * cleaner's clean/free watermark check skip an O(frames) free-list rescan per
	 * resolve-miss. Guarded by an assert against a debug rescan, see
	 * vslm_count_clean_or_free_pages_locked().
	 */
	uint32_t free_frame_count;
};

struct vbdev_vslm {
	/* Backing store (NAND) state */
	struct spdk_bdev *base_bdev;
	struct spdk_bdev_desc *base_desc;
	char *base_bdev_name;
	uint64_t virtual_size_bytes;

	/*
	 * Isolated backing tier (paper 3.2 / 4.4). When enabled, evicted private
	 * pages spill to a dedicated reserved LBA region of the base namespace
	 * starting at backing_region_offset_bytes, leaving the clean source data
	 * (at offset vpn*page) untouched. When disabled the region offset is 0 and
	 * spills overwrite the page's own backing slot (legacy behavior).
	 */
	bool backing_region_enabled;
	uint64_t backing_region_offset_bytes;

	/* SRAM cache state */
	uint8_t *sram_buffer;
	uint64_t sram_size_bytes;
	uint64_t num_sram_pages;

	/* Static DMA ring-buffer pool (vector-fallback fault-in, see paper 4.5) */
	uint8_t *dma_ring_bufs[VSLM_DMA_RING_BUF_COUNT];
	bool dma_ring_in_use[VSLM_DMA_RING_BUF_COUNT];
	pthread_mutex_t dma_ring_lock;
	bool dma_ring_initialized;

	/* MMU structures (shared-nothing partitioned, see struct vslm_mmu_shard) */
	struct vslm_page *page_array;
	struct vslm_mmu_shard *shards;
	uint32_t num_shards;

	/* Lease semantics */
	pthread_mutex_t lease_lock;
	TAILQ_HEAD(, vslm_lease) leases;
	TAILQ_HEAD(, vslm_blocked_cmd) blocked_cmds;

	/* Policy runtime state */
	pthread_mutex_t policy_lock;
	uint64_t admission_window_start_ticks;
	uint64_t admission_faults_in_window;
	bool fault_batch_enabled;
	uint32_t fault_batch_pages;
	uint32_t fault_batch_max_bytes;
	bool prefetch_batch_enabled;
	uint32_t prefetch_batch_pages;
	uint32_t prefetch_queue_depth;
	uint32_t prefetch_inflight_batches;
	uint64_t last_exec_read_vpn;
	uint32_t sequential_exec_read_count;
	int64_t last_exec_read_stride;		/* learned range-local stride (pages) */
	uint32_t strided_exec_read_count;	/* consecutive demands matching the stride */
	uint64_t prefetch_until_vpn;
	uint64_t last_batch_start_vpn;
	uint32_t last_batch_pages;
	bool background_cleaner_enabled;
	uint32_t clean_free_low_watermark_pages;
	uint32_t clean_free_high_watermark_pages;
	uint32_t cleaner_max_pages_per_poll;
	bool streaming_mode_enabled;
	uint32_t streaming_tile_pages;
	uint32_t streaming_io_pages;
	bool async_exec_enabled;

	/*
	 * Ablation overrides for mechanisms that are normally auto-selected with no
	 * runtime knob (paper Sec. 5 mechanism ablation). Defaults preserve normal
	 * behavior. force_dma_fallback forces the static DMA ring-buffer fallback
	 * (paper 4.5) on the batched fault-in path even when the backing supports a
	 * native block-vector readv. disable_cow_bypass forces a full backing read
	 * on a full-page compute/host overwrite instead of skipping it.
	 */
	bool force_dma_fallback;
	bool disable_cow_bypass;

	/* Endurance/FDP policy and telemetry */
	struct spdk_bdev_vslm_policy policy;
	bool fdp_mode_enabled;
	uint16_t fdp_dspec;
	bool base_fdp_supported;
	bool trace_events_enabled;
	char *trace_output_path;
	char *trace_run_id;
	FILE *trace_fp;
	pthread_mutex_t trace_lock;
	struct spdk_bdev_vslm_stats stats;
	struct vslm_perf_stats perf_stats;

	/* Async lifetime protection */
	pthread_mutex_t inflight_lock;
	pthread_cond_t inflight_cond;
	uint32_t inflight_io_count;
	bool destructing;

	/* SPDK integration */
	struct spdk_bdev vbdev;
	TAILQ_ENTRY(vbdev_vslm) link;
};

struct vslm_channel {
	struct spdk_io_channel *base_ch;
};

static int vbdev_vslm_module_init(void);
static void vbdev_vslm_module_fini(void);

static struct spdk_bdev_module vslm_if = {
	.name = "vslm",
	.module_init = vbdev_vslm_module_init,
	.module_fini = vbdev_vslm_module_fini,
};

static int vbdev_vslm_ch_create_cb(void *io_device, void *ctx_buf);
static void vbdev_vslm_ch_destroy_cb(void *io_device, void *ctx_buf);
static struct spdk_io_channel *vbdev_vslm_get_io_channel(void *ctx);
static void vbdev_vslm_submit_request(struct spdk_io_channel *ch,
				      struct spdk_bdev_io *bdev_io);
static void vbdev_vslm_submit_nvme_passthru(struct vbdev_vslm *vslm,
		struct spdk_io_channel *base_ch,
		struct spdk_bdev_io *bdev_io);
static bool vslm_range_overlap(uint64_t offset_a, uint64_t length_a,
			       uint64_t offset_b, uint64_t length_b);
static bool vslm_has_lease_conflict(struct vbdev_vslm *vslm, uint64_t offset,
				    uint64_t length);
static int vslm_queue_blocked_cmd(struct vbdev_vslm *vslm,
				  struct spdk_bdev_io *bdev_io,
				  uint64_t offset, uint64_t length);
static void vslm_process_blocked_cmds(struct vbdev_vslm *vslm);
static bool vslm_admission_allow_execute(struct vbdev_vslm *vslm);
static void vslm_note_page_fault(struct vbdev_vslm *vslm, bool private_refault);
static void vslm_note_visibility_violation(struct vbdev_vslm *vslm, uint64_t vpn,
		const char *reason);
static void vslm_note_resident_add(struct vbdev_vslm *vslm);
static void vslm_note_resident_remove(struct vbdev_vslm *vslm);
static void vslm_trace_emitf(struct vbdev_vslm *vslm, const char *event,
			     const char *fmt, ...);
static int vslm_writeback_page(struct vbdev_vslm *vslm,
			       struct spdk_io_channel *base_ch,
			       struct vslm_page *page,
			       bool evict_after_writeback);
static void vslm_page_mark_dirty(struct vbdev_vslm *vslm, struct vslm_page *page);
static void vslm_lru_touch(struct vbdev_vslm *vslm, struct vslm_page *page);

#define VSLM_PERF_INC(vslm, field) \
	__atomic_add_fetch(&(vslm)->perf_stats.field, 1, __ATOMIC_RELAXED)

#define VSLM_PERF_ADD(vslm, field, value) \
	__atomic_add_fetch(&(vslm)->perf_stats.field, (value), __ATOMIC_RELAXED)

static inline void
vslm_perf_max_u64(uint64_t *field, uint64_t value)
{
	uint64_t old = __atomic_load_n(field, __ATOMIC_RELAXED);

	while (value > old &&
	       !__atomic_compare_exchange_n(field, &old, value, false,
					    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

static inline uint64_t
vslm_ticks_delta_to_ns(uint64_t start_ticks, uint64_t end_ticks)
{
	uint64_t hz;

	hz = spdk_get_ticks_hz();
	if (hz == 0 || end_ticks < start_ticks) {
		return 0;
	}

	return ((end_ticks - start_ticks) * SPDK_SEC_TO_NSEC) / hz;
}

/*
 * memcpy on the page-copy hot path, recording the memcpy latency only when
 * trace/telemetry is enabled. Each timed copy otherwise costs two extra
 * spdk_get_ticks()/spdk_get_ticks_hz() reads and a 64-bit divide per chunk.
 */
static inline void
vslm_timed_memcpy(struct vbdev_vslm *vslm, void *dst, const void *src, size_t len)
{
	uint64_t memcpy_start_ticks;
	uint64_t memcpy_ns;

	if (vslm->trace_events_enabled) {
		memcpy_start_ticks = spdk_get_ticks();
		memcpy(dst, src, len);
		memcpy_ns = vslm_ticks_delta_to_ns(memcpy_start_ticks, spdk_get_ticks());
		VSLM_PERF_ADD(vslm, memcpy_ns_total, memcpy_ns);
		vslm_perf_max_u64(&vslm->perf_stats.memcpy_ns_max, memcpy_ns);
	} else {
		memcpy(dst, src, len);
	}
	VSLM_PERF_INC(vslm, memcpy_total);
	VSLM_PERF_ADD(vslm, memcpy_bytes_total, len);
}

static bool
vslm_try_get_io_ref(struct vbdev_vslm *vslm)
{
	bool ok = false;

	pthread_mutex_lock(&vslm->inflight_lock);
	if (!vslm->destructing) {
		vslm->inflight_io_count++;
		ok = true;
	}
	pthread_mutex_unlock(&vslm->inflight_lock);

	return ok;
}

static void
vslm_put_io_ref(struct vbdev_vslm *vslm)
{
	pthread_mutex_lock(&vslm->inflight_lock);
	assert(vslm->inflight_io_count > 0);
	vslm->inflight_io_count--;
	if (vslm->destructing && vslm->inflight_io_count == 0) {
		pthread_cond_signal(&vslm->inflight_cond);
	}
	pthread_mutex_unlock(&vslm->inflight_lock);
}

static void
vbdev_vslm_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			 void *event_ctx)
{
	(void)type;
	(void)bdev;
	(void)event_ctx;
}

static struct vbdev_vslm *
vbdev_vslm_get_by_nsid(uint32_t nsid)
{
	struct vbdev_vslm *vslm;
	struct vbdev_vslm *found = NULL;

	pthread_mutex_lock(&g_vslm_bdevs_lock);
	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (vslm->vbdev.nsid == nsid) {
			found = vslm;
			break;
		}
	}
	pthread_mutex_unlock(&g_vslm_bdevs_lock);

	return found;
}

/* Look up a vSLM bdev by name under g_vslm_bdevs_lock (released before return). */
static struct vbdev_vslm *
vbdev_vslm_get_by_name(const char *name)
{
	struct vbdev_vslm *vslm;
	struct vbdev_vslm *found = NULL;

	if (name == NULL) {
		return NULL;
	}

	pthread_mutex_lock(&g_vslm_bdevs_lock);
	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (strcmp(vslm->vbdev.name, name) == 0) {
			found = vslm;
			break;
		}
	}
	pthread_mutex_unlock(&g_vslm_bdevs_lock);

	return found;
}

/*
 * Snapshot the current set of vSLM bdev pointers into a freshly allocated array
 * under g_vslm_bdevs_lock. Callers that must iterate every bdev while taking
 * per-bdev locks (which cannot be nested under the global list lock) walk the
 * snapshot instead of the live list, so a concurrent create/delete cannot
 * corrupt the traversal. Returns the count via *count_out; the caller frees the
 * returned array. Returns NULL with *count_out == 0 when the list is empty or on
 * allocation failure (treated as "no bdevs" by the callers).
 */
static struct vbdev_vslm **
vbdev_vslm_snapshot_bdevs(size_t *count_out)
{
	struct vbdev_vslm *vslm;
	struct vbdev_vslm **arr = NULL;
	size_t count = 0;
	size_t i = 0;

	*count_out = 0;

	pthread_mutex_lock(&g_vslm_bdevs_lock);
	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		count++;
	}
	if (count == 0) {
		pthread_mutex_unlock(&g_vslm_bdevs_lock);
		return NULL;
	}
	arr = calloc(count, sizeof(*arr));
	if (arr == NULL) {
		pthread_mutex_unlock(&g_vslm_bdevs_lock);
		return NULL;
	}
	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		arr[i++] = vslm;
	}
	pthread_mutex_unlock(&g_vslm_bdevs_lock);

	*count_out = count;
	return arr;
}

static struct vbdev_vslm *
vbdev_vslm_get_by_bdev(struct spdk_bdev *bdev)
{
	struct vbdev_vslm *vslm;

	if (bdev == NULL || bdev->module != &vslm_if) {
		return NULL;
	}

	vslm = bdev->ctxt;
	if (vslm == NULL || &vslm->vbdev != bdev) {
		return NULL;
	}

	return vslm;
}

static bool
vbdev_vslm_mem_owns_bdev(struct spdk_bdev *bdev)
{
	return vbdev_vslm_get_by_bdev(bdev) != NULL;
}

static int vbdev_vslm_mem_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
		uint64_t length, void **ptr);
static int vbdev_vslm_mem_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
		uint64_t length, bool for_write,
		struct spdk_bdev_slm_sg_entry *entries,
		uint32_t max_entries, uint32_t *entry_count);
static int vbdev_vslm_mem_try_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
		uint64_t length, bool for_write,
		struct spdk_bdev_slm_sg_entry *entries,
		uint32_t max_entries, uint32_t *entry_count);
static int vbdev_vslm_mem_unpin_range_by_bdev(struct spdk_bdev *bdev,
		const struct spdk_bdev_slm_sg_entry *entries,
		uint32_t entry_count, bool dirtied);
static int vbdev_vslm_mem_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				       uint64_t length, void *buf);
static int vbdev_vslm_mem_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
		uint64_t length, void *buf,
		spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);
static int vbdev_vslm_mem_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
					uint64_t length, const void *buf);
static int vbdev_vslm_mem_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
		uint64_t length, const void *buf,
		spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg);
static int vbdev_vslm_mem_copy_by_bdev(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
				       struct spdk_bdev *src_bdev, uint64_t src_offset,
				       uint64_t length);
static int vbdev_vslm_mem_copy_by_bdev_async(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
		struct spdk_bdev *src_bdev, uint64_t src_offset,
		uint64_t length, spdk_bdev_slm_io_completion_cb cb_fn,
		void *cb_arg);
static int vbdev_vslm_mem_exec_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
		uint64_t length, void *buf);
static int vbdev_vslm_mem_exec_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
		uint64_t length, void *buf,
		spdk_bdev_slm_io_completion_cb cb_fn,
		void *cb_arg);
static int vbdev_vslm_mem_exec_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
		uint64_t length, const void *buf);
static int vbdev_vslm_mem_exec_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset,
		uint64_t length, const void *buf,
		spdk_bdev_slm_io_completion_cb cb_fn,
		void *cb_arg);
static int vbdev_vslm_mem_lease_acquire_by_bdev(uint64_t lease_id, struct spdk_bdev *bdev,
		uint64_t offset, uint64_t length);
static int vbdev_vslm_mem_exec_publish_lease(uint64_t lease_id);
static int vbdev_vslm_mem_exec_discard_lease(uint64_t lease_id);
static int vbdev_vslm_mem_lease_release(uint64_t lease_id);

static const struct spdk_vbdev_slm_ops g_vslm_mem_ops = {
	.name = "vslm",
	.owns_bdev = vbdev_vslm_mem_owns_bdev,
	.get_buffer_ptr_by_bdev = vbdev_vslm_mem_get_buffer_ptr_by_bdev,
	.pin_range_by_bdev = vbdev_vslm_mem_pin_range_by_bdev,
	.try_pin_range_by_bdev = vbdev_vslm_mem_try_pin_range_by_bdev,
	.unpin_range_by_bdev = vbdev_vslm_mem_unpin_range_by_bdev,
	.read_by_bdev = vbdev_vslm_mem_read_by_bdev,
	.read_by_bdev_async = vbdev_vslm_mem_read_by_bdev_async,
	.write_by_bdev = vbdev_vslm_mem_write_by_bdev,
	.write_by_bdev_async = vbdev_vslm_mem_write_by_bdev_async,
	.copy_by_bdev = vbdev_vslm_mem_copy_by_bdev,
	.copy_by_bdev_async = vbdev_vslm_mem_copy_by_bdev_async,
	.exec_read_by_bdev = vbdev_vslm_mem_exec_read_by_bdev,
	.exec_read_by_bdev_async = vbdev_vslm_mem_exec_read_by_bdev_async,
	.exec_write_by_bdev = vbdev_vslm_mem_exec_write_by_bdev,
	.exec_write_by_bdev_async = vbdev_vslm_mem_exec_write_by_bdev_async,
	.lease_acquire_by_bdev = vbdev_vslm_mem_lease_acquire_by_bdev,
	.exec_publish_lease = vbdev_vslm_mem_exec_publish_lease,
	.exec_discard_lease = vbdev_vslm_mem_exec_discard_lease,
	.lease_release = vbdev_vslm_mem_lease_release,
};

static int
vbdev_vslm_module_init(void)
{
	return vbdev_slm_register_ops(&g_vslm_mem_ops);
}

static void
vbdev_vslm_module_fini(void)
{
	vbdev_slm_unregister_ops(&g_vslm_mem_ops);
}

/*
 * Block-cyclic VPN->shard mapping. Runs of VSLM_MMU_SHARD_STRIDE_PAGES
 * consecutive VPNs land in the same shard (preserving locality for fault
 * batching and prefetch), and successive runs round-robin across shards.
 */
static inline uint32_t
vslm_vpn_to_shard(const struct vbdev_vslm *vslm, uint64_t vpn)
{
	if (vslm->num_shards <= 1) {
		return 0;
	}
	return (uint32_t)((vpn / VSLM_MMU_SHARD_STRIDE_PAGES) % vslm->num_shards);
}

static inline struct vslm_mmu_shard *
vslm_shard_for_vpn(struct vbdev_vslm *vslm, uint64_t vpn)
{
	return &vslm->shards[vslm_vpn_to_shard(vslm, vpn)];
}

static inline struct vslm_mmu_shard *
vslm_shard_for_page(struct vbdev_vslm *vslm, const struct vslm_page *page)
{
	return &vslm->shards[page->shard_id];
}

static void
vslm_hash_insert_lpage(struct vbdev_vslm *vslm, struct vslm_lpage *lpage)
{
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, lpage->vpn);
	uint64_t idx;

	idx = lpage->vpn % shard->num_buckets;
	LIST_INSERT_HEAD(&shard->lpage_hash_table[idx], lpage, hash_link);
}

static void
vslm_hash_remove_lpage(struct vslm_lpage *lpage)
{
	LIST_REMOVE(lpage, hash_link);
}

static struct vslm_lpage *
vslm_lookup_lpage(struct vbdev_vslm *vslm, uint64_t vpn)
{
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, vpn);
	struct vslm_lpage *lpage;
	uint64_t idx;

	VSLM_PERF_INC(vslm, lpage_lookup_total);

	idx = vpn % shard->num_buckets;
	LIST_FOREACH(lpage, &shard->lpage_hash_table[idx], hash_link) {
		if (lpage->vpn == vpn) {
			return lpage;
		}
	}

	return NULL;
}

static inline bool
vslm_lpage_is_default_clean_backing(const struct vslm_lpage *lpage)
{
	return lpage == NULL;
}

static inline void
vslm_default_backing_source(struct vbdev_vslm *vslm, uint64_t vpn,
			    struct spdk_bdev **source_bdev,
			    uint64_t *source_offset_bytes)
{
	*source_bdev = vslm->base_bdev;
	*source_offset_bytes = vpn * VSLM_PAGE_SIZE;
}

/*
 * Source describing where an evicted (spilled) private page lives: the isolated
 * backing region of the base namespace. With the region disabled
 * (backing_region_offset_bytes == 0) this is identical to the default backing
 * source, preserving legacy spill-in-place behavior.
 */
static void
vslm_spilled_backing_source(struct vbdev_vslm *vslm, uint64_t vpn,
			    struct spdk_bdev **source_bdev,
			    uint64_t *source_offset_bytes)
{
	*source_bdev = vslm->base_bdev;
	*source_offset_bytes = vslm->backing_region_offset_bytes + vpn * VSLM_PAGE_SIZE;
}

/* Block offset where an evicted private page is written/read in the backing region. */
static uint64_t
vslm_spill_offset_blocks(struct vbdev_vslm *vslm, uint64_t vpn, uint64_t page_blocks)
{
	uint64_t block_size = spdk_bdev_get_block_size(vslm->base_bdev);

	return (vslm->backing_region_offset_bytes / block_size) + vpn * page_blocks;
}

static bool
vslm_lpage_can_collapse_to_default_backing(struct vbdev_vslm *vslm,
		const struct vslm_lpage *lpage)
{
	if (vslm_lpage_is_default_clean_backing(lpage)) {
		return false;
	}

	if (lpage->state != SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT) {
		return false;
	}

	if (lpage->source_type != SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING) {
		return false;
	}

	if (lpage->source_bdev != vslm->base_bdev) {
		return false;
	}

	if (lpage->source_offset_bytes != lpage->vpn * VSLM_PAGE_SIZE) {
		return false;
	}

	if (lpage->has_superseded_source || lpage->pending_publish || lpage->private_authoritative) {
		return false;
	}

	return true;
}

static struct vslm_lpage *
vslm_lpage_alloc_base(struct vbdev_vslm *vslm, uint64_t vpn)
{
	struct vslm_lpage *lpage;

	lpage = calloc(1, sizeof(*lpage));
	if (lpage == NULL) {
		return NULL;
	}

	lpage->vpn = vpn;
	lpage->state = SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT;
	lpage->source_type = SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING;
	lpage->source_bdev = vslm->base_bdev;
	lpage->source_offset_bytes = vpn * VSLM_PAGE_SIZE;
	lpage->has_superseded_source = false;
	lpage->superseded_source_type = SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING;
	lpage->superseded_source_bdev = NULL;
	lpage->superseded_source_offset_bytes = 0;
	lpage->pending_publish = false;
	lpage->private_authoritative = false;
	vslm_hash_insert_lpage(vslm, lpage);
	VSLM_PERF_INC(vslm, lpage_alloc_total);
	return lpage;
}

static void
vslm_lpage_delete(struct vbdev_vslm *vslm, struct vslm_lpage *lpage)
{
	if (lpage == NULL) {
		return;
	}

	vslm_hash_remove_lpage(lpage);
	VSLM_PERF_INC(vslm, lpage_free_total);
	free(lpage);
}

static struct vslm_lpage *
vslm_lpage_alloc_private_dirty(struct vbdev_vslm *vslm, uint64_t vpn)
{
	struct vslm_lpage *lpage;

	lpage = vslm_lpage_alloc_base(vslm, vpn);
	if (lpage == NULL) {
		return NULL;
	}

	lpage->state = SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT;
	lpage->source_bdev = NULL;
	lpage->source_offset_bytes = 0;
	lpage->private_authoritative = true;
	return lpage;
}

static struct vslm_lpage *
vslm_lpage_alloc_alias(struct vbdev_vslm *vslm, uint64_t vpn,
		       struct spdk_bdev *src_bdev, uint64_t src_offset)
{
	struct vslm_lpage *lpage;

	lpage = vslm_lpage_alloc_base(vslm, vpn);
	if (lpage == NULL) {
		return NULL;
	}

	lpage->state = SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS;
	lpage->source_type = SPDK_BDEV_VSLM_PAGE_SOURCE_ALIAS_BDEV;
	lpage->source_bdev = src_bdev;
	lpage->source_offset_bytes = src_offset;
	lpage->pending_publish = false;
	lpage->private_authoritative = false;
	lpage->has_superseded_source = false;
	lpage->superseded_source_type = SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING;
	lpage->superseded_source_bdev = NULL;
	lpage->superseded_source_offset_bytes = 0;
	return lpage;
}

static struct vslm_lpage *
vslm_lpage_get_or_create_for_dirty(struct vbdev_vslm *vslm, uint64_t vpn)
{
	struct vslm_lpage *lpage;

	lpage = vslm_lookup_lpage(vslm, vpn);
	if (lpage != NULL) {
		return lpage;
	}

	/*
	 * Dirty transition must preserve the previous committed source, which
	 * defaults to base backing for pages without metadata.
	 */
	return vslm_lpage_alloc_base(vslm, vpn);
}

static bool
vslm_lpage_free_if_default_backing(struct vbdev_vslm *vslm, struct vslm_lpage *lpage)
{
	if (!vslm_lpage_can_collapse_to_default_backing(vslm, lpage)) {
		return false;
	}

	vslm_lpage_delete(vslm, lpage);
	return true;
}

static void
vslm_lpage_set_backing_source(struct vbdev_vslm *vslm, struct vslm_lpage *lpage)
{
	vslm_default_backing_source(vslm, lpage->vpn, &lpage->source_bdev,
				    &lpage->source_offset_bytes);
	lpage->source_type = SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING;
}

/* Point an evicted private page at its slot in the isolated backing region. */
static void
vslm_lpage_set_spilled_source(struct vbdev_vslm *vslm, struct vslm_lpage *lpage)
{
	vslm_spilled_backing_source(vslm, lpage->vpn, &lpage->source_bdev,
				    &lpage->source_offset_bytes);
	lpage->source_type = SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING;
}

static void
vslm_lpage_clear_superseded_source(struct vslm_lpage *lpage)
{
	lpage->has_superseded_source = false;
	lpage->superseded_source_type = SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING;
	lpage->superseded_source_bdev = NULL;
	lpage->superseded_source_offset_bytes = 0;
}

static void
vslm_lpage_snapshot_source(struct vslm_lpage *lpage)
{
	if (lpage->source_bdev == NULL) {
		return;
	}

	lpage->has_superseded_source = true;
	lpage->superseded_source_type = lpage->source_type;
	lpage->superseded_source_bdev = lpage->source_bdev;
	lpage->superseded_source_offset_bytes = lpage->source_offset_bytes;
}

static void
vslm_lpage_invalidate_active_source(struct vslm_lpage *lpage)
{
	vslm_lpage_snapshot_source(lpage);
	lpage->source_bdev = NULL;
	lpage->source_offset_bytes = 0;
}

static bool
vslm_vpn_has_active_lease(struct vbdev_vslm *vslm, uint64_t vpn)
{
	struct vslm_lease *lease;
	uint64_t page_start;
	uint64_t page_length;
	bool has_lease = false;

	page_start = vpn * VSLM_PAGE_SIZE;
	page_length = VSLM_PAGE_SIZE;

	pthread_mutex_lock(&vslm->lease_lock);
	TAILQ_FOREACH(lease, &vslm->leases, link) {
		if (vslm_range_overlap(page_start, page_length, lease->offset, lease->length)) {
			has_lease = true;
			break;
		}
	}
	pthread_mutex_unlock(&vslm->lease_lock);

	return has_lease;
}

static bool
vslm_lpage_state_is_dirty_resident(enum spdk_bdev_vslm_lpage_state state)
{
	return state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT;
}

static const char *
vslm_lpage_state_to_string(enum spdk_bdev_vslm_lpage_state state)
{
	switch (state) {
	case SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS:
		return "clean_alias";
	case SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT:
		return "clean_resident";
	case SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT:
		return "private_dirty_resident";
	case SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE:
		return "spilled_private";
	default:
		return "unknown";
	}
}

static void
vslm_lpage_set_state(struct vbdev_vslm *vslm, struct vslm_page *page,
		     struct vslm_lpage *lpage,
		     enum spdk_bdev_vslm_lpage_state new_state)
{
	enum spdk_bdev_vslm_lpage_state old_state;
	bool old_dirty;
	bool new_dirty;

	old_state = lpage->state;
	old_dirty = vslm_lpage_state_is_dirty_resident(old_state);
	new_dirty = vslm_lpage_state_is_dirty_resident(new_state);
	lpage->state = new_state;

	if (!old_dirty && new_dirty) {
		__atomic_add_fetch(&vslm->stats.dirty_resident_pages, 1, __ATOMIC_RELAXED);
	} else if (old_dirty && !new_dirty) {
		__atomic_sub_fetch(&vslm->stats.dirty_resident_pages, 1, __ATOMIC_RELAXED);
	}

	if (page != NULL) {
		page->dirty = new_dirty;
	}
}

static void
vslm_page_mark_dirty(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	struct vslm_lpage *lpage;
	bool has_active_lease;
	enum spdk_bdev_vslm_lpage_state old_state;

	lpage = vslm_lpage_get_or_create_for_dirty(vslm, page->vpn);
	if (lpage == NULL) {
		return;
	}

	/*
	 * Fast path for a re-dirty of an already-private page that is already
	 * publishing: the state, private_authoritative flag, and pending_publish are
	 * all already maximal and mark_dirty never downgrades them, so re-running the
	 * full path (lease_lock scan + source invalidate + trace) would be a no-op.
	 */
	if (lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT &&
	    lpage->private_authoritative && lpage->pending_publish) {
		page->dirty = true;
		return;
	}

	has_active_lease = vslm_vpn_has_active_lease(vslm, page->vpn);
	old_state = lpage->state;
	if (old_state == SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS ||
	    old_state == SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT ||
	    old_state == SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE) {
		vslm_lpage_invalidate_active_source(lpage);
	}

	lpage->private_authoritative = true;
	if (has_active_lease) {
		lpage->pending_publish = true;
	}
	vslm_lpage_set_state(vslm, page, lpage, SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT);
	if (old_state != SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT) {
		vslm_trace_emitf(vslm, "cow_private",
				 ",\"page_id\":%" PRIu64 ",\"state_before\":\"%s\",\"state_after\":\"%s\"",
				 page->vpn, vslm_lpage_state_to_string(old_state),
				 vslm_lpage_state_to_string(SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT));
	}

#ifdef VSLM_DEBUG_VALIDATE
	assert(vslm_lookup_lpage(vslm, page->vpn) != NULL);
#endif
}

static void
vslm_page_mark_clean_if_lpage_exists(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	struct vslm_lpage *lpage;

	lpage = vslm_lookup_lpage(vslm, page->vpn);
	if (lpage == NULL) {
		page->dirty = false;
		return;
	}

	lpage->pending_publish = false;
	vslm_lpage_set_state(vslm, page, lpage, SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT);
	(void)vslm_lpage_free_if_default_backing(vslm, lpage);
}

static void
vslm_lpage_free_all(struct vbdev_vslm *vslm)
{
	struct vslm_lpage *lpage, *tmp;
	uint64_t i;
	uint32_t s;

	if (vslm->shards == NULL) {
		return;
	}

	for (s = 0; s < vslm->num_shards; s++) {
		struct vslm_mmu_shard *shard = &vslm->shards[s];

		if (shard->lpage_hash_table == NULL) {
			continue;
		}
		for (i = 0; i < shard->num_buckets; i++) {
			LIST_FOREACH_SAFE(lpage, &shard->lpage_hash_table[i], hash_link, tmp) {
				vslm_lpage_delete(vslm, lpage);
			}
		}
	}
}

static void
vslm_note_resident_add(struct vbdev_vslm *vslm)
{
	uint64_t current;
	uint64_t peak;

	current = __atomic_add_fetch(&vslm->stats.vslm_resident_bytes_current,
				     VSLM_PAGE_SIZE, __ATOMIC_RELAXED);
	peak = __atomic_load_n(&vslm->stats.vslm_resident_bytes_peak, __ATOMIC_RELAXED);
	while (current > peak &&
	       !__atomic_compare_exchange_n(&vslm->stats.vslm_resident_bytes_peak, &peak,
					    current, false, __ATOMIC_RELAXED,
					    __ATOMIC_RELAXED)) {
	}
}

static void
vslm_note_resident_remove(struct vbdev_vslm *vslm)
{
	__atomic_sub_fetch(&vslm->stats.vslm_resident_bytes_current,
			   VSLM_PAGE_SIZE, __ATOMIC_RELAXED);
}

static uint64_t
vslm_trace_now_ns(void)
{
	uint64_t hz = spdk_get_ticks_hz();
	uint64_t ticks = spdk_get_ticks();

	if (hz == 0) {
		return 0;
	}

	return (ticks * SPDK_SEC_TO_NSEC) / hz;
}

static const char *
vslm_trace_get_run_id(struct vbdev_vslm *vslm)
{
	if (vslm->trace_run_id != NULL && vslm->trace_run_id[0] != '\0') {
		return vslm->trace_run_id;
	}

	if (vslm->vbdev.name != NULL && vslm->vbdev.name[0] != '\0') {
		return vslm->vbdev.name;
	}

	return "vslm";
}

static void
vslm_trace_emitf(struct vbdev_vslm *vslm, const char *event, const char *fmt, ...)
{
	va_list args;
	const char *run_id;
	uint64_t ts_ns;

	if (vslm == NULL || !vslm->trace_events_enabled || vslm->trace_fp == NULL || event == NULL) {
		return;
	}

	ts_ns = vslm_trace_now_ns();
	run_id = vslm_trace_get_run_id(vslm);

	pthread_mutex_lock(&vslm->trace_lock);
	fprintf(vslm->trace_fp, "{\"ts_ns\":%" PRIu64 ",\"run_id\":\"%s\",\"event\":\"%s\"",
		ts_ns, run_id, event);
	if (fmt != NULL) {
		va_start(args, fmt);
		vfprintf(vslm->trace_fp, fmt, args);
		va_end(args);
	}
	fputs("}\n", vslm->trace_fp);
	fflush(vslm->trace_fp);
	pthread_mutex_unlock(&vslm->trace_lock);
}

static void
vslm_note_page_fault(struct vbdev_vslm *vslm, bool private_refault)
{
	uint64_t now_ticks;
	uint64_t hz;

	__atomic_add_fetch(&vslm->stats.page_faults, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&vslm->stats.page_fault_bytes, VSLM_PAGE_SIZE, __ATOMIC_RELAXED);
	if (private_refault) {
		__atomic_add_fetch(&vslm->stats.vslm_fault_private_total, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&vslm->stats.vslm_spill_read_bytes,
				   VSLM_PAGE_SIZE, __ATOMIC_RELAXED);
		vslm_trace_emitf(vslm, "fault_private", ",\"bytes\":%u", VSLM_PAGE_SIZE);
		vslm_trace_emitf(vslm, "refault_private", ",\"bytes\":%u", VSLM_PAGE_SIZE);
	} else {
		__atomic_add_fetch(&vslm->stats.vslm_fault_clean_total, 1, __ATOMIC_RELAXED);
		vslm_trace_emitf(vslm, "fault_clean", ",\"bytes\":%u", VSLM_PAGE_SIZE);
	}

	now_ticks = spdk_get_ticks();
	hz = spdk_get_ticks_hz();

	pthread_mutex_lock(&vslm->policy_lock);
	if (vslm->admission_window_start_ticks == 0 ||
	    hz == 0 ||
	    now_ticks < vslm->admission_window_start_ticks ||
	    (now_ticks - vslm->admission_window_start_ticks) >= hz) {
		vslm->admission_window_start_ticks = now_ticks;
		vslm->admission_faults_in_window = 0;
	}
	vslm->admission_faults_in_window++;
	pthread_mutex_unlock(&vslm->policy_lock);
}

static void
vslm_note_visibility_violation(struct vbdev_vslm *vslm, uint64_t vpn, const char *reason)
{
	__atomic_add_fetch(&vslm->stats.vslm_visibility_violation_total, 1, __ATOMIC_RELAXED);
	vslm_trace_emitf(vslm, "visibility_violation",
			 ",\"page_id\":%" PRIu64 ",\"result\":\"%s\"",
			 vpn, reason == NULL ? "unknown" : reason);
	SPDK_WARNLOG("vSLM visibility violation bdev=%s vpn=%" PRIu64 " reason=%s\n",
		     vslm->vbdev.name, vpn, reason);
}

static bool
vslm_admission_allow_execute(struct vbdev_vslm *vslm)
{
	bool allow = true;
	uint64_t now_ticks;
	uint64_t hz;

	now_ticks = spdk_get_ticks();
	hz = spdk_get_ticks_hz();

	pthread_mutex_lock(&vslm->policy_lock);
	if (!vslm->policy.admission_enabled) {
		goto out;
	}

	if (vslm->admission_window_start_ticks == 0 ||
	    hz == 0 ||
	    now_ticks < vslm->admission_window_start_ticks ||
	    (now_ticks - vslm->admission_window_start_ticks) >= hz) {
		vslm->admission_window_start_ticks = now_ticks;
		vslm->admission_faults_in_window = 0;
		goto out;
	}

	if (vslm->admission_faults_in_window >=
	    vslm->policy.admission_faults_per_sec_threshold) {
		allow = false;
		__atomic_add_fetch(&vslm->stats.admission_rejects, 1, __ATOMIC_RELAXED);
	}

out:
	pthread_mutex_unlock(&vslm->policy_lock);
	return allow;
}

static bool
vslm_range_overlap(uint64_t offset_a, uint64_t length_a,
		   uint64_t offset_b, uint64_t length_b)
{
	uint64_t end_a;
	uint64_t end_b;

	if (length_a == 0 || length_b == 0) {
		return false;
	}

	end_a = offset_a + length_a;
	end_b = offset_b + length_b;
	if (end_a < offset_a || end_b < offset_b) {
		return true;
	}

	return !(end_a <= offset_b || end_b <= offset_a);
}

static bool
vslm_has_lease_conflict(struct vbdev_vslm *vslm, uint64_t offset, uint64_t length)
{
	struct vslm_lease *lease;
	uint64_t req_page_start;
	uint64_t req_page_end;
	uint64_t lease_page_start;
	uint64_t lease_page_end;
	bool conflict = false;

	req_page_start = (offset / VSLM_PAGE_SIZE) * VSLM_PAGE_SIZE;
	req_page_end = ((offset + length - 1) / VSLM_PAGE_SIZE + 1) * VSLM_PAGE_SIZE;

	pthread_mutex_lock(&vslm->lease_lock);
	TAILQ_FOREACH(lease, &vslm->leases, link) {
		lease_page_start = (lease->offset / VSLM_PAGE_SIZE) * VSLM_PAGE_SIZE;
		lease_page_end = ((lease->offset + lease->length - 1) / VSLM_PAGE_SIZE + 1) *
				 VSLM_PAGE_SIZE;
		if (vslm_range_overlap(req_page_start, req_page_end - req_page_start,
				       lease_page_start, lease_page_end - lease_page_start)) {
			conflict = true;
			break;
		}
	}
	pthread_mutex_unlock(&vslm->lease_lock);

	return conflict;
}

static int
vslm_queue_blocked_cmd(struct vbdev_vslm *vslm,
		       struct spdk_bdev_io *bdev_io,
		       uint64_t offset, uint64_t length)
{
	struct vslm_blocked_cmd *blocked;
	struct vslm_lease *lease;
	uint64_t req_page_start;
	uint64_t req_page_end;
	uint64_t lease_page_start;
	uint64_t lease_page_end;
	bool conflict = false;
	int rc = 0;

	blocked = calloc(1, sizeof(*blocked));
	if (blocked == NULL) {
		return -ENOMEM;
	}

	req_page_start = (offset / VSLM_PAGE_SIZE) * VSLM_PAGE_SIZE;
	req_page_end = ((offset + length - 1) / VSLM_PAGE_SIZE + 1) * VSLM_PAGE_SIZE;

	pthread_mutex_lock(&vslm->lease_lock);
	TAILQ_FOREACH(lease, &vslm->leases, link) {
		lease_page_start = (lease->offset / VSLM_PAGE_SIZE) * VSLM_PAGE_SIZE;
		lease_page_end = ((lease->offset + lease->length - 1) / VSLM_PAGE_SIZE + 1) *
				 VSLM_PAGE_SIZE;
		if (vslm_range_overlap(req_page_start, req_page_end - req_page_start,
				       lease_page_start, lease_page_end - lease_page_start)) {
			conflict = true;
			break;
		}
	}
	if (!conflict) {
		rc = -EAGAIN;
		goto out_unlock;
	}

	blocked->bdev_io = bdev_io;
	blocked->queued_ticks = spdk_get_ticks();
	TAILQ_INSERT_TAIL(&vslm->blocked_cmds, blocked, link);
	__atomic_add_fetch(&vslm->stats.lease_conflicts, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&vslm->stats.vslm_host_write_conflict_total, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&vslm->stats.vslm_host_write_blocked_total, 1, __ATOMIC_RELAXED);
	vslm_trace_emitf(vslm, "lease_conflict",
			 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"blocked\"",
			 offset, length);
	vslm_trace_emitf(vslm, "host_write_conflict",
			 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"blocked\"",
			 offset, length);
	vslm_trace_emitf(vslm, "lease_block_start",
			 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"queued\"",
			 offset, length);
	SPDK_DEBUGLOG(vslm, "vSLM queued conflicting command nsid=%u off=%" PRIu64 " len=%" PRIu64 "\n",
		      vslm->vbdev.nsid, offset, length);
	pthread_mutex_unlock(&vslm->lease_lock);
	return 0;

out_unlock:
	pthread_mutex_unlock(&vslm->lease_lock);
	free(blocked);
	return rc;
}

static void
vslm_process_blocked_cmds(struct vbdev_vslm *vslm)
{
	struct vslm_blocked_cmd *blocked, *tmp;
	struct spdk_io_channel *io_ch;
	struct vslm_channel *vch;
	TAILQ_HEAD(, vslm_blocked_cmd) pending =
		TAILQ_HEAD_INITIALIZER(pending);

	pthread_mutex_lock(&vslm->lease_lock);
	TAILQ_FOREACH_SAFE(blocked, &vslm->blocked_cmds, link, tmp) {
		TAILQ_REMOVE(&vslm->blocked_cmds, blocked, link);
		TAILQ_INSERT_TAIL(&pending, blocked, link);
	}
	pthread_mutex_unlock(&vslm->lease_lock);

	TAILQ_FOREACH_SAFE(blocked, &pending, link, tmp) {
		uint64_t now_ticks = spdk_get_ticks();
		uint64_t hz = spdk_get_ticks_hz();
		uint64_t blocked_ns = 0;

		TAILQ_REMOVE(&pending, blocked, link);
		if (hz != 0 && now_ticks > blocked->queued_ticks) {
			blocked_ns = ((now_ticks - blocked->queued_ticks) * SPDK_SEC_TO_NSEC) / hz;
			__atomic_add_fetch(&vslm->stats.lease_blocked_ns,
					   blocked_ns, __ATOMIC_RELAXED);
			/*
			 * Blocked commands are queued only by the host write/fill path
			 * (vslm_queue_blocked_cmd), so this is the host-write-blocked time.
			 */
			__atomic_add_fetch(&vslm->stats.vslm_host_write_blocked_ns_total,
					   blocked_ns, __ATOMIC_RELAXED);
		}
		io_ch = spdk_bdev_io_get_io_channel(blocked->bdev_io);
		if (io_ch == NULL) {
			vslm_trace_emitf(vslm, "lease_block_end",
					 ",\"latency_ns\":%" PRIu64 ",\"result\":\"failed_no_channel\"",
					 blocked_ns);
			spdk_bdev_io_complete(blocked->bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			free(blocked);
			continue;
		}

		vch = spdk_io_channel_get_ctx(io_ch);
		vslm_trace_emitf(vslm, "lease_block_end",
				 ",\"latency_ns\":%" PRIu64 ",\"result\":\"resubmitted\"",
				 blocked_ns);
		vbdev_vslm_submit_nvme_passthru(vslm, vch->base_ch, blocked->bdev_io);
		free(blocked);
	}
}

#ifdef VSLM_DEBUG_VALIDATE
static void
vslm_debug_validate_page_lists(struct vbdev_vslm *vslm)
{
	uint64_t i;
	uint64_t free_count = 0;
	uint64_t lru_count = 0;
	uint64_t max_vpn;
	struct vslm_page *page;
	struct vslm_lpage *lpage;

	uint32_t s;

	max_vpn = vslm->virtual_size_bytes / VSLM_PAGE_SIZE;

	for (s = 0; s < vslm->num_shards; s++) {
		struct vslm_mmu_shard *shard = &vslm->shards[s];

		TAILQ_FOREACH(page, &shard->free_list, lru_link) {
			assert(page >= &vslm->page_array[0]);
			assert(page < &vslm->page_array[vslm->num_sram_pages]);
			assert(page->shard_id == s);
			assert(page->location == VSLM_PAGE_LOC_FREE);
			assert(page->vpn == UINT64_MAX);
			assert(!page->dirty);
			assert(!page->is_busy);
			assert(!page->on_hash);
			assert(page->load_state == VSLM_PAGE_LOAD_FREE);
			assert(page->pin_count == 0);
			free_count++;
		}

		TAILQ_FOREACH(page, &shard->lru_list, lru_link) {
			assert(page >= &vslm->page_array[0]);
			assert(page < &vslm->page_array[vslm->num_sram_pages]);
			assert(page->shard_id == s);
			assert(page->location == VSLM_PAGE_LOC_LRU);
			assert(page->vpn != UINT64_MAX);
			assert(page->on_hash);
			assert(page->load_state == VSLM_PAGE_LOAD_RESIDENT);
			lru_count++;
		}
	}

	for (i = 0; i < vslm->num_sram_pages; i++) {
		page = &vslm->page_array[i];
		switch (page->location) {
		case VSLM_PAGE_LOC_FREE:
			assert(page->vpn == UINT64_MAX);
			assert(!page->dirty);
			assert(!page->is_busy);
			assert(!page->on_hash);
			assert(page->load_state == VSLM_PAGE_LOAD_FREE);
			assert(page->pin_count == 0);
			break;
		case VSLM_PAGE_LOC_LRU:
			assert(page->vpn != UINT64_MAX);
			assert(page->on_hash);
			assert(page->load_state == VSLM_PAGE_LOAD_RESIDENT);
			if (page->dirty) {
				assert(vslm_lookup_lpage(vslm, page->vpn) != NULL);
			}
			break;
		case VSLM_PAGE_LOC_NONE:
			break;
		default:
			assert(false);
		}
	}

	for (s = 0; s < vslm->num_shards; s++) {
		struct vslm_mmu_shard *shard = &vslm->shards[s];

		for (i = 0; i < shard->num_buckets; i++) {
			LIST_FOREACH(lpage, &shard->lpage_hash_table[i], hash_link) {
				assert(lpage->vpn < max_vpn);
				assert(vslm_vpn_to_shard(vslm, lpage->vpn) == s);
				if (lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT) {
					assert(lpage->private_authoritative);
				}
			}
		}
	}

	assert(free_count + lru_count <= vslm->num_sram_pages);
}
#else
#define vslm_debug_validate_page_lists(vslm) do { } while (0)
#endif

static void
vslm_page_prepare_free(struct vslm_page *page)
{
	assert(TAILQ_EMPTY(&page->waiters));

	page->vpn = UINT64_MAX;
	page->dirty = false;
	page->is_busy = false;
	page->on_hash = false;
	page->location = VSLM_PAGE_LOC_NONE;
	page->load_state = VSLM_PAGE_LOAD_FREE;
	page->load_status = 0;
	page->prefetched = false;
	page->pin_count = 0;
	TAILQ_INIT(&page->waiters);
}

static void
vslm_free_list_insert_tail(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	struct vslm_mmu_shard *shard = vslm_shard_for_page(vslm, page);

	assert(page != NULL);
	assert(page->location == VSLM_PAGE_LOC_NONE);
	assert(!page->on_hash);
	assert(page->vpn == UINT64_MAX);
	assert(!page->dirty);
	assert(!page->is_busy);

	TAILQ_INSERT_TAIL(&shard->free_list, page, lru_link);
	page->location = VSLM_PAGE_LOC_FREE;
	shard->free_frame_count++;
	VSLM_PERF_INC(vslm, free_list_push_total);
}

static void
vslm_mmu_unlock_measured(struct vbdev_vslm *vslm, struct vslm_mmu_shard *shard,
			 uint64_t lock_start_ticks)
{
	uint64_t lock_hold_ns;

	/*
	 * The two spdk_get_ticks()/spdk_get_ticks_hz() reads plus the 64-bit divide
	 * needed to measure the hold time run on every shard-lock release. Only pay
	 * for them when trace/telemetry is enabled.
	 */
	if (vslm->trace_events_enabled) {
		lock_hold_ns = vslm_ticks_delta_to_ns(lock_start_ticks, spdk_get_ticks());
		VSLM_PERF_ADD(vslm, mmu_lock_hold_ns_total, lock_hold_ns);
		vslm_perf_max_u64(&vslm->perf_stats.mmu_lock_hold_ns_max, lock_hold_ns);
	}
	pthread_mutex_unlock(&shard->lock);
}


static void
vslm_page_waiter_complete_msg(void *arg)
{
	struct vslm_page_waiter *waiter = arg;
	uint64_t wait_ns;

	wait_ns = vslm_ticks_delta_to_ns(waiter->queued_ticks, spdk_get_ticks());
	VSLM_PERF_ADD(waiter->vslm, prefetch_async_wait_ns_total, wait_ns);
	waiter->cb_fn(waiter->cb_arg, waiter->status, waiter->page);
	free(waiter);
}

static int
vslm_page_add_waiter_locked(struct vbdev_vslm *vslm, struct vslm_page *page,
			    struct spdk_thread *thread, vslm_page_waiter_cb cb_fn,
			    void *cb_arg)
{
	struct vslm_page_waiter *waiter;

	if (vslm == NULL || page == NULL || thread == NULL || cb_fn == NULL) {
		return -EINVAL;
	}

	if (page->load_state != VSLM_PAGE_LOAD_LOADING &&
	    page->load_state != VSLM_PAGE_LOAD_EVICTING) {
		return -EINVAL;
	}

	waiter = calloc(1, sizeof(*waiter));
	if (waiter == NULL) {
		return -ENOMEM;
	}

	waiter->vslm = vslm;
	waiter->thread = thread;
	waiter->cb_fn = cb_fn;
	waiter->cb_arg = cb_arg;
	waiter->queued_ticks = spdk_get_ticks();
	TAILQ_INSERT_TAIL(&page->waiters, waiter, link);
	VSLM_PERF_INC(vslm, prefetch_async_wait_total);
	return 0;
}

static void
vslm_page_take_waiters_locked(struct vslm_page *page, int status,
			      struct vslm_page *ready_page,
			      struct vslm_page_waiter_list *waiters)
{
	struct vslm_page_waiter *waiter;

	while ((waiter = TAILQ_FIRST(&page->waiters)) != NULL) {
		TAILQ_REMOVE(&page->waiters, waiter, link);
		waiter->status = status;
		waiter->page = status == 0 ? ready_page : NULL;
		TAILQ_INSERT_TAIL(waiters, waiter, link);
	}
}

static void
vslm_page_complete_waiters(struct vslm_page_waiter_list *waiters)
{
	struct vslm_page_waiter *waiter;
	int rc;

	while ((waiter = TAILQ_FIRST(waiters)) != NULL) {
		TAILQ_REMOVE(waiters, waiter, link);
		rc = spdk_thread_send_msg(waiter->thread, vslm_page_waiter_complete_msg,
					  waiter);
		if (rc != 0) {
			waiter->status = rc;
			waiter->page = NULL;
			waiter->cb_fn(waiter->cb_arg, waiter->status, waiter->page);
			free(waiter);
		}
	}
}

static int
vslm_wait_for_loading_page(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	struct vslm_mmu_shard *shard = vslm_shard_for_page(vslm, page);
	struct spdk_thread *thread;
	uint64_t wait_start_ticks = 0;
	uint64_t wait_ns;
	bool waited = false;

	thread = spdk_get_thread();
	if (thread == NULL) {
		return -EINVAL;
	}

	while (page->load_state == VSLM_PAGE_LOAD_LOADING ||
	       page->load_state == VSLM_PAGE_LOAD_EVICTING) {
		if (!waited) {
			wait_start_ticks = spdk_get_ticks();
			waited = true;
			VSLM_PERF_INC(vslm, prefetch_async_wait_total);
		}
		pthread_mutex_unlock(&shard->lock);
		spdk_thread_poll(thread, 0, 0);
		pthread_mutex_lock(&shard->lock);
	}

	if (waited) {
		wait_ns = vslm_ticks_delta_to_ns(wait_start_ticks, spdk_get_ticks());
		VSLM_PERF_ADD(vslm, prefetch_async_wait_ns_total, wait_ns);
	}

	if (page->load_state != VSLM_PAGE_LOAD_RESIDENT) {
		return page->load_status != 0 ? page->load_status : -EIO;
	}

	return 0;
}

static void
vslm_page_pin_locked(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	(void)vslm;

	assert(page != NULL);
	assert(page->vpn != UINT64_MAX);
	assert(page->load_state == VSLM_PAGE_LOAD_RESIDENT);
	page->pin_count++;
}

static void
vslm_page_unpin_locked(struct vbdev_vslm *vslm,
		       struct vslm_page *page,
		       bool dirtied,
		       bool touch_lru)
{
	assert(page != NULL);
	assert(page->pin_count > 0);

	if (dirtied) {
		vslm_page_mark_dirty(vslm, page);
	}

	if (touch_lru) {
		vslm_lru_touch(vslm, page);
	}

	page->pin_count--;
}

static uint32_t
vslm_compute_num_shards(uint64_t num_frames)
{
	uint64_t shards;

	if (num_frames < 2 * VSLM_MMU_MIN_FRAMES_PER_SHARD) {
		return 1;
	}

	shards = num_frames / VSLM_MMU_MIN_FRAMES_PER_SHARD;
	if (shards > VSLM_MMU_MAX_SHARDS) {
		shards = VSLM_MMU_MAX_SHARDS;
	}
	if (shards < 1) {
		shards = 1;
	}
	return (uint32_t)shards;
}

static void
vslm_free_shards(struct vbdev_vslm *vslm)
{
	uint32_t s;

	if (vslm->shards == NULL) {
		return;
	}

	for (s = 0; s < vslm->num_shards; s++) {
		struct vslm_mmu_shard *shard = &vslm->shards[s];

		free(shard->hash_table);
		shard->hash_table = NULL;
		free(shard->lpage_hash_table);
		shard->lpage_hash_table = NULL;
		pthread_mutex_destroy(&shard->lock);
	}

	free(vslm->shards);
	vslm->shards = NULL;
	vslm->num_shards = 0;
}

/* Allocate the fixed DMA ring-buffer pool used by the vector-fallback path. */
static void
vslm_init_dma_ring_pool(struct vbdev_vslm *vslm)
{
	uint32_t i;

	if (pthread_mutex_init(&vslm->dma_ring_lock, NULL) != 0) {
		return;
	}
	vslm->dma_ring_initialized = true;

	for (i = 0; i < VSLM_DMA_RING_BUF_COUNT; i++) {
		vslm->dma_ring_bufs[i] = spdk_dma_malloc(VSLM_DMA_RING_BUF_BYTES,
					 VSLM_PAGE_SIZE, NULL);
		vslm->dma_ring_in_use[i] = false;
		/* A NULL slot simply falls back to a one-shot allocation at use time. */
	}
}

static void
vslm_free_dma_ring_pool(struct vbdev_vslm *vslm)
{
	uint32_t i;

	if (!vslm->dma_ring_initialized) {
		return;
	}
	for (i = 0; i < VSLM_DMA_RING_BUF_COUNT; i++) {
		if (vslm->dma_ring_bufs[i] != NULL) {
			spdk_dma_free(vslm->dma_ring_bufs[i]);
			vslm->dma_ring_bufs[i] = NULL;
		}
	}
	pthread_mutex_destroy(&vslm->dma_ring_lock);
	vslm->dma_ring_initialized = false;
}

/* Borrow a ring buffer from the pool; returns NULL if none is free/available. */
static uint8_t *
vslm_dma_ring_acquire(struct vbdev_vslm *vslm, uint64_t need_bytes, int *out_idx)
{
	uint32_t i;

	*out_idx = -1;
	if (!vslm->dma_ring_initialized || need_bytes > VSLM_DMA_RING_BUF_BYTES) {
		return NULL;
	}

	pthread_mutex_lock(&vslm->dma_ring_lock);
	for (i = 0; i < VSLM_DMA_RING_BUF_COUNT; i++) {
		if (vslm->dma_ring_bufs[i] != NULL && !vslm->dma_ring_in_use[i]) {
			vslm->dma_ring_in_use[i] = true;
			pthread_mutex_unlock(&vslm->dma_ring_lock);
			*out_idx = (int)i;
			return vslm->dma_ring_bufs[i];
		}
	}
	pthread_mutex_unlock(&vslm->dma_ring_lock);
	return NULL;
}

static void
vslm_dma_ring_release(struct vbdev_vslm *vslm, int idx)
{
	if (idx < 0) {
		return;
	}
	pthread_mutex_lock(&vslm->dma_ring_lock);
	vslm->dma_ring_in_use[idx] = false;
	pthread_mutex_unlock(&vslm->dma_ring_lock);
}

/*
 * Build (or rebuild) the partitioned MMU shards over the already-allocated
 * page_array, partitioning num_sram_pages frames across num_shards. Expects
 * vslm->shards == NULL on entry (callers free it first). On failure the shards
 * are torn down (page_array/sram are owned by the caller). Factored out of
 * vslm_init_mmu so the debug/benchmark path can re-shard an idle MMU.
 */
static int
vslm_build_shards(struct vbdev_vslm *vslm, uint32_t num_shards)
{
	uint64_t i;
	uint32_t s;
	uint64_t assigned;
	struct vslm_page *page;

	if (num_shards < 1) {
		num_shards = 1;
	}

	vslm->num_shards = num_shards;
	vslm->shards = calloc(num_shards, sizeof(*vslm->shards));
	if (!vslm->shards) {
		vslm->num_shards = 0;
		return -1;
	}

	/* Partition frames into disjoint per-shard slices and per-shard hash tables. */
	assigned = 0;
	for (s = 0; s < num_shards; s++) {
		struct vslm_mmu_shard *shard = &vslm->shards[s];
		uint64_t frames = vslm->num_sram_pages / num_shards;

		/* Hand any remainder frames to the final shard. */
		if (s == num_shards - 1) {
			frames = vslm->num_sram_pages - assigned;
		}

		shard->base_ppn = assigned;
		shard->num_frames = frames;
		shard->num_buckets = frames > 0 ? frames : 1;
		assigned += frames;

		if (pthread_mutex_init(&shard->lock, NULL) != 0) {
			vslm->num_shards = s; /* only [0, s) are initialized */
			vslm_free_shards(vslm);
			return -1;
		}

		shard->hash_table = calloc(shard->num_buckets, sizeof(*shard->hash_table));
		shard->lpage_hash_table = calloc(shard->num_buckets,
						 sizeof(*shard->lpage_hash_table));
		if (shard->hash_table == NULL || shard->lpage_hash_table == NULL) {
			vslm->num_shards = s + 1; /* free [0, s] */
			vslm_free_shards(vslm);
			return -1;
		}

		TAILQ_INIT(&shard->lru_list);
		TAILQ_INIT(&shard->free_list);
		shard->free_frame_count = 0;
		for (i = 0; i < shard->num_buckets; i++) {
			LIST_INIT(&shard->hash_table[i]);
			LIST_INIT(&shard->lpage_hash_table[i]);
		}
	}

	for (i = 0; i < vslm->num_sram_pages; i++) {
		page = &vslm->page_array[i];
		page->ppn = i;
		page->vpn = UINT64_MAX;
		page->dirty = false;
		page->is_busy = false;
		page->location = VSLM_PAGE_LOC_FREE;
		page->on_hash = false;
		page->load_state = VSLM_PAGE_LOAD_FREE;
		page->load_status = 0;
		page->prefetched = false;
		page->pin_count = 0;
		TAILQ_INIT(&page->waiters);

		/* Map this frame to the shard that owns its ppn slice. */
		for (s = vslm->num_shards; s > 0; s--) {
			if (i >= vslm->shards[s - 1].base_ppn) {
				page->shard_id = (uint16_t)(s - 1);
				break;
			}
		}
		TAILQ_INSERT_TAIL(&vslm->shards[page->shard_id].free_list, page, lru_link);
		vslm->shards[page->shard_id].free_frame_count++;
	}

	return 0;
}

static int
vslm_init_mmu(struct vbdev_vslm *vslm)
{
	vslm->num_sram_pages = vslm->sram_size_bytes / VSLM_PAGE_SIZE;

	vslm->sram_buffer = spdk_dma_malloc(vslm->sram_size_bytes, VSLM_PAGE_SIZE, NULL);
	if (!vslm->sram_buffer) {
		return -1;
	}

	vslm->page_array = calloc(vslm->num_sram_pages, sizeof(*vslm->page_array));
	if (!vslm->page_array) {
		spdk_dma_free(vslm->sram_buffer);
		vslm->sram_buffer = NULL;
		return -1;
	}

	if (vslm_build_shards(vslm, vslm_compute_num_shards(vslm->num_sram_pages)) != 0) {
		free(vslm->page_array);
		vslm->page_array = NULL;
		spdk_dma_free(vslm->sram_buffer);
		vslm->sram_buffer = NULL;
		return -1;
	}

	vslm_init_dma_ring_pool(vslm);

	vslm->async_exec_enabled = true;
	vslm->force_dma_fallback = false;
	vslm->disable_cow_bypass = false;
	vslm_debug_validate_page_lists(vslm);
	return 0;
}

static struct vslm_page *
vslm_lookup_page(struct vbdev_vslm *vslm, uint64_t vpn)
{
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, vpn);
	struct vslm_page *page;
	uint64_t idx;

	VSLM_PERF_INC(vslm, page_hash_lookup_total);

	idx = vpn % shard->num_buckets;
	LIST_FOREACH(page, &shard->hash_table[idx], hash_link) {
		if (page->vpn == vpn) {
			VSLM_PERF_INC(vslm, page_hash_hit_total);
			return page;
		}
	}

	VSLM_PERF_INC(vslm, page_hash_miss_total);
	return NULL;
}

static void
vslm_hash_insert(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	struct vslm_mmu_shard *shard = vslm_shard_for_page(vslm, page);
	uint64_t idx;

	assert(page != NULL);
	assert(!page->on_hash);
	assert(page->vpn != UINT64_MAX);
	assert(vslm_vpn_to_shard(vslm, page->vpn) == page->shard_id);

	idx = page->vpn % shard->num_buckets;
	LIST_INSERT_HEAD(&shard->hash_table[idx], page, hash_link);
	page->on_hash = true;
	VSLM_PERF_INC(vslm, page_hash_insert_total);
}

static void
vslm_hash_remove(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	assert(page != NULL);
	assert(page->on_hash);

	LIST_REMOVE(page, hash_link);
	page->on_hash = false;
	VSLM_PERF_INC(vslm, page_hash_remove_total);
}

static void
vslm_lru_touch(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	struct vslm_mmu_shard *shard = vslm_shard_for_page(vslm, page);

	assert(page != NULL);
	assert(page->vpn != UINT64_MAX);
	assert(page->location == VSLM_PAGE_LOC_LRU ||
	       page->location == VSLM_PAGE_LOC_NONE);
	assert(page->load_state != VSLM_PAGE_LOAD_FREE);

	VSLM_PERF_INC(vslm, lru_touch_total);

	if (page->location == VSLM_PAGE_LOC_LRU) {
		TAILQ_REMOVE(&shard->lru_list, page, lru_link);
		VSLM_PERF_INC(vslm, lru_touch_move_total);
	} else {
		VSLM_PERF_INC(vslm, lru_touch_insert_total);
	}

	TAILQ_INSERT_TAIL(&shard->lru_list, page, lru_link);
	page->location = VSLM_PAGE_LOC_LRU;
}

static void
vslm_lru_remove(struct vbdev_vslm *vslm, struct vslm_page *page)
{
	struct vslm_mmu_shard *shard = vslm_shard_for_page(vslm, page);

	assert(page != NULL);
	assert(page->location == VSLM_PAGE_LOC_LRU);

	TAILQ_REMOVE(&shard->lru_list, page, lru_link);
	page->location = VSLM_PAGE_LOC_NONE;
}

/* Pop a free frame from a specific shard's core-local free list. */
static struct vslm_page *
vslm_get_free_page(struct vbdev_vslm *vslm, struct vslm_mmu_shard *shard)
{
	struct vslm_page *page;

	page = TAILQ_FIRST(&shard->free_list);
	if (page != NULL) {
		assert(page->location == VSLM_PAGE_LOC_FREE);
		assert(page->vpn == UINT64_MAX);
		assert(!page->on_hash);
		assert(page->load_state == VSLM_PAGE_LOAD_FREE);

		TAILQ_REMOVE(&shard->free_list, page, lru_link);
		page->location = VSLM_PAGE_LOC_NONE;
		assert(shard->free_frame_count > 0);
		shard->free_frame_count--;
		VSLM_PERF_INC(vslm, free_list_pop_total);
	}

	return page;
}

static struct vslm_page *
vslm_pick_victim_pass(struct vbdev_vslm *vslm, struct vslm_mmu_shard *shard,
		      bool clean_only, bool count_skips, bool any_lease)
{
	struct vslm_page *page;
	struct vslm_lpage *lpage;

	TAILQ_FOREACH(page, &shard->lru_list, lru_link) {
		assert(page->location == VSLM_PAGE_LOC_LRU);

		if (page->is_busy) {
			if (count_skips) {
				VSLM_PERF_INC(vslm, victim_skip_busy_total);
			}
			continue;
		}
		if (page->pin_count > 0) {
			if (count_skips) {
				VSLM_PERF_INC(vslm, victim_skip_pinned_total);
			}
			continue;
		}
		if (page->load_state != VSLM_PAGE_LOAD_RESIDENT) {
			if (count_skips) {
				VSLM_PERF_INC(vslm, victim_skip_loading_total);
			}
			continue;
		}

		/*
		 * Lease pins are never evicted. any_lease is a snapshot of "the
		 * instance has at least one active lease" taken once for this scan:
		 * when it is false the lease list is empty, so no vpn can be leased
		 * and the per-candidate vslm_vpn_has_active_lease() lock+scan is
		 * skipped. When it is true keep the exact per-candidate check.
		 */
		if (any_lease) {
			lpage = vslm_lookup_lpage(vslm, page->vpn);
			if (lpage != NULL &&
			    lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT &&
			    lpage->pending_publish &&
			    vslm_vpn_has_active_lease(vslm, page->vpn)) {
				if (count_skips) {
					VSLM_PERF_INC(vslm, victim_skip_lease_total);
				}
				continue;
			}
		}
		if (clean_only && page->dirty) {
			continue;
		}

		TAILQ_REMOVE(&shard->lru_list, page, lru_link);
		page->location = VSLM_PAGE_LOC_NONE;
		return page;
	}

	return NULL;
}

/* Pick an eviction victim from a specific shard (clean-first, then LRU dirty). */
static struct vslm_page *
vslm_pick_victim(struct vbdev_vslm *vslm, struct vslm_mmu_shard *shard)
{
	struct vslm_page *page;
	bool any_lease;

	VSLM_PERF_INC(vslm, victim_pick_total);

	/*
	 * Snapshot once whether any lease exists. The caller already holds
	 * shard->lock, and vslm_vpn_has_active_lease() also takes lease_lock while
	 * the shard lock is held, so taking lease_lock here preserves the existing
	 * shard->lock -> lease_lock ordering (no new lock-order edge).
	 */
	pthread_mutex_lock(&vslm->lease_lock);
	any_lease = !TAILQ_EMPTY(&vslm->leases);
	pthread_mutex_unlock(&vslm->lease_lock);

	page = vslm_pick_victim_pass(vslm, shard, true, true, any_lease);
	if (page != NULL) {
		return page;
	}

	return vslm_pick_victim_pass(vslm, shard, false, false, any_lease);
}

/* Count clean/free frames in a single shard (caller holds shard->lock). */
static uint32_t
vslm_count_clean_or_free_pages_locked(struct vbdev_vslm *vslm,
				      struct vslm_mmu_shard *shard)
{
	struct vslm_page *page;
	uint32_t count;

	(void)vslm;

	/*
	 * Free frames use the O(1) running counter (vslm_mmu_shard.free_frame_count)
	 * maintained at every free-list push/pop, removing the per-call O(frames)
	 * free-list rescan. In debug builds, cross-check the counter against a real
	 * rescan so a missed maintenance site is caught immediately.
	 */
	count = shard->free_frame_count;

#ifndef NDEBUG
	{
		uint32_t scanned_free = 0;

		TAILQ_FOREACH(page, &shard->free_list, lru_link) {
			scanned_free++;
		}
		assert(scanned_free == shard->free_frame_count);
	}
#endif

	TAILQ_FOREACH(page, &shard->lru_list, lru_link) {
		if (!page->dirty &&
		    !page->is_busy &&
		    page->pin_count == 0 &&
		    page->load_state == VSLM_PAGE_LOAD_RESIDENT) {
			count++;
		}
	}

	return count;
}

/* Opportunistically write back dirty frames within one shard (holds shard->lock). */
static void
vslm_background_clean_pages_locked(struct vbdev_vslm *vslm,
				   struct vslm_mmu_shard *shard,
				   struct spdk_io_channel *base_ch)
{
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	uint32_t cleaned = 0;
	uint32_t clean_or_free_pages;
	int rc;

	if (!vslm->background_cleaner_enabled ||
	    base_ch == NULL ||
	    vslm->cleaner_max_pages_per_poll == 0) {
		return;
	}

	clean_or_free_pages = vslm_count_clean_or_free_pages_locked(vslm, shard);
	if (clean_or_free_pages >= vslm->clean_free_low_watermark_pages) {
		return;
	}

	VSLM_PERF_INC(vslm, cleaner_poll_total);
	TAILQ_FOREACH(page, &shard->lru_list, lru_link) {
		if (cleaned >= vslm->cleaner_max_pages_per_poll) {
			break;
		}
		if (!page->dirty) {
			continue;
		}
		if (page->is_busy) {
			VSLM_PERF_INC(vslm, cleaner_skipped_busy_total);
			continue;
		}
		if (page->pin_count > 0) {
			VSLM_PERF_INC(vslm, cleaner_skipped_pinned_total);
			continue;
		}
		if (page->load_state != VSLM_PAGE_LOAD_RESIDENT) {
			continue;
		}

		lpage = vslm_lookup_lpage(vslm, page->vpn);
		if (lpage != NULL &&
		    lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT &&
		    lpage->pending_publish &&
		    vslm_vpn_has_active_lease(vslm, page->vpn)) {
			VSLM_PERF_INC(vslm, cleaner_skipped_lease_total);
			continue;
		}

		page->is_busy = true;
		VSLM_PERF_INC(vslm, cleaner_writeback_submit_total);
		rc = vslm_writeback_page(vslm, base_ch, page, false);
		page->is_busy = false;
		if (rc != 0) {
			break;
		}

		VSLM_PERF_INC(vslm, cleaner_writeback_complete_total);
		VSLM_PERF_INC(vslm, cleaner_pages_cleaned_total);
		cleaned++;

		clean_or_free_pages = vslm_count_clean_or_free_pages_locked(vslm, shard);
		if (clean_or_free_pages >= vslm->clean_free_high_watermark_pages) {
			break;
		}
	}
}

struct vslm_sync_io_ctx {
	bool done;
	int status;
};

static void
vslm_sync_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct vslm_sync_io_ctx *ctx = cb_arg;

	spdk_bdev_free_io(bdev_io);
	ctx->status = success ? 0 : -EIO;
	ctx->done = true;
}

static int
vslm_submit_sync_base_io(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
			 struct vslm_mmu_shard *held_shard,
			 void *buf, uint64_t offset_blocks, uint64_t num_blocks, bool is_write)
{
	uint64_t start_ticks;
	uint64_t elapsed_ns;
	struct vslm_sync_io_ctx ctx = {};
	struct spdk_bdev_ext_io_opts io_opts = {};
	struct iovec iov = {};
	struct spdk_thread *thread;
	uint64_t block_size;
	uint64_t bytes;
	bool tagged = false;
	bool submitted = false;
	int rc = 0;

	start_ticks = spdk_get_ticks();
	VSLM_PERF_INC(vslm, sync_base_io_total);

	if (num_blocks == 0) {
		goto out;
	}

	thread = spdk_get_thread();
	if (thread == NULL) {
		rc = -EINVAL;
		goto out;
	}

	block_size = spdk_bdev_get_block_size(vslm->base_bdev);
	bytes = num_blocks * block_size;

	if (is_write && vslm->fdp_mode_enabled) {
		iov.iov_base = buf;
		iov.iov_len = bytes;
		io_opts.size = sizeof(io_opts);
		io_opts.nvme_cdw12.write.dtype = SPDK_NVME_DIRECTIVE_TYPE_DATA_PLACEMENT;
		io_opts.nvme_cdw13.write.dspec = vslm->fdp_dspec;

		rc = spdk_bdev_writev_blocks_ext(vslm->base_desc, base_ch,
						 &iov, 1, offset_blocks, num_blocks,
						 vslm_sync_io_completion_cb, &ctx, &io_opts);
		if (rc == 0) {
			tagged = true;
			submitted = true;
		} else if (rc == -ENOTSUP) {
			/* Fall back to mixed-placement write path. */
			rc = 0;
		}
	}

	if (!submitted) {
		if (is_write) {
			rc = spdk_bdev_write_blocks(vslm->base_desc, base_ch, buf, offset_blocks,
						    num_blocks, vslm_sync_io_completion_cb, &ctx);
		} else {
			rc = spdk_bdev_read_blocks(vslm->base_desc, base_ch, buf, offset_blocks,
						   num_blocks, vslm_sync_io_completion_cb, &ctx);
		}
		submitted = (rc == 0);
	}
	if (rc != 0) {
		goto out;
	}

	/*
	 * Never busy-poll the reactor while holding a shard lock: spdk_thread_poll()
	 * runs other vSLM I/O on this same thread which would re-enter the same
	 * (non-recursive) shard lock and self-deadlock. Drop the held shard lock
	 * around the poll loop and re-acquire it afterward, exactly like
	 * vslm_wait_for_loading_page(). The page being faulted/evicted/written back
	 * is already protected (LOADING/EVICTING/is_busy) so it cannot be stolen
	 * during the unlocked window.
	 */
	if (held_shard != NULL) {
		pthread_mutex_unlock(&held_shard->lock);
	}
	while (!ctx.done) {
		spdk_thread_poll(thread, 0, 0);
	}
	if (held_shard != NULL) {
		pthread_mutex_lock(&held_shard->lock);
	}

	if (is_write && submitted && ctx.status == 0) {
		__atomic_add_fetch(&vslm->stats.backing_write_ops, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&vslm->stats.backing_write_bytes, bytes, __ATOMIC_RELAXED);
		if (tagged) {
			__atomic_add_fetch(&vslm->stats.backing_write_tagged_bytes, bytes, __ATOMIC_RELAXED);
		} else {
			__atomic_add_fetch(&vslm->stats.backing_write_untagged_bytes, bytes, __ATOMIC_RELAXED);
		}
	}

	rc = ctx.status;

out:
	elapsed_ns = vslm_ticks_delta_to_ns(start_ticks, spdk_get_ticks());
	VSLM_PERF_ADD(vslm, sync_base_io_ns_total, elapsed_ns);
	vslm_perf_max_u64(&vslm->perf_stats.sync_base_io_ns_max, elapsed_ns);
	return rc;
}

static int
vslm_submit_sync_readv_desc(struct vbdev_vslm *vslm, struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch,
			    struct vslm_mmu_shard *held_shard,
			    struct iovec *iov, int iovcnt,
			    uint64_t offset_blocks, uint64_t num_blocks)
{
	struct vslm_sync_io_ctx ctx = {};
	struct spdk_thread *thread;
	int rc;

	if (num_blocks == 0) {
		return 0;
	}

	if (iov == NULL || iovcnt <= 0) {
		return -EINVAL;
	}

	thread = spdk_get_thread();
	if (thread == NULL) {
		return -EINVAL;
	}

	/*
	 * Ablation override: pretend the backing cannot do a native block-vector
	 * readv so the caller takes the static DMA ring-buffer fallback (paper 4.5).
	 * Read atomically: the knob is published lock-free from vslm_apply_debug.
	 */
	if (__atomic_load_n(&vslm->force_dma_fallback, __ATOMIC_RELAXED)) {
		return -ENOTSUP;
	}

	rc = spdk_bdev_readv_blocks(desc, ch,
				    iov, iovcnt,
				    offset_blocks, num_blocks,
				    vslm_sync_io_completion_cb, &ctx);
	if (rc != 0) {
		return rc;
	}

	/* See vslm_submit_sync_base_io(): drop any held shard lock across the poll. */
	if (held_shard != NULL) {
		pthread_mutex_unlock(&held_shard->lock);
	}
	while (!ctx.done) {
		spdk_thread_poll(thread, 0, 0);
	}
	if (held_shard != NULL) {
		pthread_mutex_lock(&held_shard->lock);
	}

	return ctx.status;
}

static int
vslm_submit_sync_read_desc_io(struct vbdev_vslm *vslm,
			      struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      struct vslm_mmu_shard *held_shard,
			      void *buf, uint64_t offset_blocks, uint64_t num_blocks)
{
	uint64_t start_ticks;
	uint64_t elapsed_ns;
	struct vslm_sync_io_ctx ctx = {};
	struct spdk_thread *thread;
	int rc = 0;

	start_ticks = spdk_get_ticks();
	VSLM_PERF_INC(vslm, sync_read_desc_io_total);

	if (num_blocks == 0) {
		goto out;
	}

	thread = spdk_get_thread();
	if (thread == NULL) {
		rc = -EINVAL;
		goto out;
	}

	rc = spdk_bdev_read_blocks(desc, ch, buf, offset_blocks, num_blocks,
				   vslm_sync_io_completion_cb, &ctx);
	if (rc != 0) {
		goto out;
	}

	/* See vslm_submit_sync_base_io(): drop any held shard lock across the poll. */
	if (held_shard != NULL) {
		pthread_mutex_unlock(&held_shard->lock);
	}
	while (!ctx.done) {
		spdk_thread_poll(thread, 0, 0);
	}
	if (held_shard != NULL) {
		pthread_mutex_lock(&held_shard->lock);
	}

	rc = ctx.status;

out:
	elapsed_ns = vslm_ticks_delta_to_ns(start_ticks, spdk_get_ticks());
	VSLM_PERF_ADD(vslm, sync_read_desc_io_ns_total, elapsed_ns);
	vslm_perf_max_u64(&vslm->perf_stats.sync_read_desc_io_ns_max, elapsed_ns);
	return rc;
}

static int
vslm_read_page_from_source(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
			   struct spdk_bdev *source_bdev, uint64_t source_offset_bytes,
			   uint8_t *page_buf)
{
	struct spdk_bdev_desc *source_desc = NULL;
	struct spdk_io_channel *source_ch = NULL;
	const char *source_name;
	uint64_t source_block_size;
	uint64_t source_page_blocks;
	uint64_t source_offset_blocks;
	uint64_t source_num_blocks;
	bool opened_source_desc = false;
	int rc;

	if (source_bdev == NULL) {
		source_bdev = vslm->base_bdev;
	}

	source_block_size = spdk_bdev_get_block_size(source_bdev);
	if (source_block_size == 0 ||
	    (VSLM_PAGE_SIZE % source_block_size) != 0 ||
	    (source_offset_bytes % source_block_size) != 0) {
		return -EINVAL;
	}

	source_page_blocks = VSLM_PAGE_SIZE / source_block_size;
	source_offset_blocks = source_offset_bytes / source_block_size;
	source_num_blocks = spdk_bdev_get_num_blocks(source_bdev);
	if (source_offset_blocks > source_num_blocks ||
	    source_page_blocks > (source_num_blocks - source_offset_blocks)) {
		return -EINVAL;
	}

	if (source_bdev == vslm->base_bdev) {
		source_desc = vslm->base_desc;
		source_ch = base_ch;
		rc = 0;
	} else {
		source_name = source_bdev->name;
		if (source_name == NULL) {
			return -EINVAL;
		}

		rc = spdk_bdev_open_ext(source_name, false, vbdev_vslm_bdev_event_cb, vslm, &source_desc);
		if (rc != 0) {
			return rc;
		}
		opened_source_desc = true;
		source_ch = spdk_bdev_get_io_channel(source_desc);
		if (source_ch == NULL) {
			spdk_bdev_close(source_desc);
			return -ENOMEM;
		}
	}

	/* Callers (vslm_read_committed_range) drop the shard lock before this. */
	rc = vslm_submit_sync_read_desc_io(vslm, source_desc, source_ch, NULL, page_buf,
					   source_offset_blocks, source_page_blocks);

	if (opened_source_desc) {
		spdk_put_io_channel(source_ch);
		spdk_bdev_close(source_desc);
	}

	return rc;
}

static void
vslm_get_committed_source(struct vbdev_vslm *vslm, uint64_t vpn,
			  struct spdk_bdev **source_bdev,
			  uint64_t *source_offset_bytes)
{
	struct vslm_lpage *lpage;

	vslm_default_backing_source(vslm, vpn, source_bdev, source_offset_bytes);

	lpage = vslm_lookup_lpage(vslm, vpn);
	if (lpage == NULL) {
		return;
	}

	if (lpage->pending_publish &&
	    lpage->has_superseded_source &&
	    lpage->superseded_source_bdev != NULL) {
		*source_bdev = lpage->superseded_source_bdev;
		*source_offset_bytes = lpage->superseded_source_offset_bytes;
		return;
	}

	if (lpage->source_bdev != NULL) {
		*source_bdev = lpage->source_bdev;
		*source_offset_bytes = lpage->source_offset_bytes;
		return;
	}

	if (lpage->has_superseded_source && lpage->superseded_source_bdev != NULL) {
		*source_bdev = lpage->superseded_source_bdev;
		*source_offset_bytes = lpage->superseded_source_offset_bytes;
	}
}

static int
vslm_read_committed_range(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
			  uint64_t starting_byte, uint32_t length, uint8_t *buf)
{
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	struct vslm_mmu_shard *shard;
	struct spdk_bdev *source_bdev;
	uint64_t source_offset_bytes;
	uint64_t offset = 0;
	uint64_t absolute;
	uint64_t vpn;
	uint64_t page_offset;
	uint64_t chunk;
	uint8_t *page_buf = NULL;
	int rc;

	if (length == 0) {
		return 0;
	}

	if (starting_byte >= vslm->virtual_size_bytes ||
	    length > vslm->virtual_size_bytes - starting_byte) {
		return -EINVAL;
	}

	while (offset < length) {
		absolute = starting_byte + offset;
		vpn = absolute / VSLM_PAGE_SIZE;
		page_offset = absolute % VSLM_PAGE_SIZE;
		chunk = spdk_min((uint64_t)(VSLM_PAGE_SIZE - page_offset), (uint64_t)length - offset);
		shard = vslm_shard_for_vpn(vslm, vpn);

		pthread_mutex_lock(&shard->lock);
		page = vslm_lookup_page(vslm, vpn);
		lpage = vslm_lookup_lpage(vslm, vpn);
		if (page != NULL &&
		    (page->load_state == VSLM_PAGE_LOAD_LOADING ||
		     page->load_state == VSLM_PAGE_LOAD_EVICTING)) {
			rc = vslm_wait_for_loading_page(vslm, page);
			if (rc != 0) {
				pthread_mutex_unlock(&shard->lock);
				return rc;
			}
		}
		if (page != NULL && page->load_state != VSLM_PAGE_LOAD_RESIDENT) {
			page = NULL;
		}

		if (page != NULL && lpage == NULL && !page->dirty) {
			memcpy(buf + offset,
			       vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset,
			       chunk);
			pthread_mutex_unlock(&shard->lock);
			offset += chunk;
			continue;
		}

		if (page != NULL && lpage != NULL && !page->dirty &&
		    vslm_lpage_can_collapse_to_default_backing(vslm, lpage)) {
			memcpy(buf + offset,
			       vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset,
			       chunk);
			pthread_mutex_unlock(&shard->lock);
			offset += chunk;
			continue;
		}

		if (page != NULL && lpage != NULL &&
		    lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT &&
		    !lpage->pending_publish) {
			if (vslm_vpn_has_active_lease(vslm, vpn)) {
				vslm_note_visibility_violation(vslm, vpn,
							       "host_read_private_dirty_with_active_lease");
			}
			memcpy(buf + offset,
			       vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset,
			       chunk);
			pthread_mutex_unlock(&shard->lock);
			offset += chunk;
			continue;
		}

		vslm_get_committed_source(vslm, vpn, &source_bdev, &source_offset_bytes);
		pthread_mutex_unlock(&shard->lock);

		if (page_buf == NULL) {
			page_buf = spdk_dma_malloc(VSLM_PAGE_SIZE, VSLM_PAGE_SIZE, NULL);
			if (page_buf == NULL) {
				return -ENOMEM;
			}
		}

		rc = vslm_read_page_from_source(vslm, base_ch, source_bdev, source_offset_bytes, page_buf);
		if (rc != 0) {
			spdk_dma_free(page_buf);
			return rc;
		}

		memcpy(buf + offset, page_buf + page_offset, chunk);
		offset += chunk;
	}

	if (page_buf != NULL) {
		spdk_dma_free(page_buf);
	}

	return 0;
}

static int
vslm_writeback_page(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		    struct vslm_page *page, bool evict_after_writeback)
{
	struct vslm_lpage *lpage;
	uint64_t block_size;
	uint64_t page_blocks;
	uint64_t offset_blocks;
	uint8_t *ptr;
	int post_state = -1;
	int rc;

	lpage = vslm_lookup_lpage(vslm, page->vpn);
	if (lpage == NULL && page->dirty) {
		VSLM_PERF_INC(vslm, dirty_page_missing_lpage_total);
		lpage = vslm_lpage_alloc_private_dirty(vslm, page->vpn);
		if (lpage == NULL) {
			return -ENOMEM;
		}
		page->dirty = true;
	}

	if (lpage == NULL ||
	    lpage->state != SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT) {
		return 0;
	}
	SPDK_DEBUGLOG(vslm, "vSLM writeback start bdev=%s vpn=%" PRIu64 " ppn=%" PRIu64
		      " evict=%d\n", vslm->vbdev.name, page->vpn, page->ppn, evict_after_writeback);

	block_size = spdk_bdev_get_block_size(vslm->base_bdev);
	page_blocks = VSLM_PAGE_SIZE / block_size;
	/*
	 * Spill goes to the isolated backing region (preserving the clean source);
	 * publish writes the committed data back to the page's own backing slot.
	 */
	if (evict_after_writeback) {
		offset_blocks = vslm_spill_offset_blocks(vslm, page->vpn, page_blocks);
	} else {
		offset_blocks = page->vpn * page_blocks;
	}
	ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE);

	/*
	 * The owning shard lock is held by every caller (evict/cleaner/publish).
	 * Hand it to the sync I/O so the poll loop runs with the lock dropped; the
	 * victim page is protected during that window (EVICTING / is_busy).
	 */
	rc = vslm_submit_sync_base_io(vslm, base_ch, vslm_shard_for_page(vslm, page),
				      ptr, offset_blocks, page_blocks, true);
	if (rc == 0) {
		lpage->pending_publish = false;
		vslm_lpage_clear_superseded_source(lpage);
		if (evict_after_writeback) {
			vslm_lpage_set_spilled_source(vslm, lpage);
			lpage->private_authoritative = true;
			vslm_lpage_set_state(vslm, page, lpage,
					     SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE);
		} else {
			vslm_lpage_set_backing_source(vslm, lpage);
			lpage->private_authoritative = false;
			vslm_lpage_set_state(vslm, page, lpage,
					     SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT);
			if (vslm_lpage_free_if_default_backing(vslm, lpage)) {
				lpage = NULL;
			}
		}
		__atomic_add_fetch(&vslm->stats.page_writebacks, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&vslm->stats.page_writeback_bytes, VSLM_PAGE_SIZE, __ATOMIC_RELAXED);
		__atomic_add_fetch(&vslm->stats.dirty_writeback_bytes, VSLM_PAGE_SIZE, __ATOMIC_RELAXED);
		if (evict_after_writeback) {
			__atomic_add_fetch(&vslm->stats.vslm_spill_write_bytes,
					   VSLM_PAGE_SIZE, __ATOMIC_RELAXED);
		}
		post_state = lpage == NULL ? -1 : (int)lpage->state;
		SPDK_DEBUGLOG(vslm, "vSLM writeback done bdev=%s vpn=%" PRIu64 " state=%d\n",
			      vslm->vbdev.name, page->vpn, post_state);
	} else {
		SPDK_DEBUGLOG(vslm, "vSLM writeback failed bdev=%s vpn=%" PRIu64 " rc=%d\n",
			      vslm->vbdev.name, page->vpn, rc);
	}
	(void)post_state;

	return rc;
}

/*
 * held_shard is the shard lock the caller is holding (the page's owning shard)
 * or NULL when the caller has already dropped it (the split reserve/fault path).
 * It is threaded into the sync backing reads so they drop it across their poll
 * loop. The faulting page is LOADING/busy throughout, so it cannot be stolen
 * during the unlocked window.
 */
/*
 * Per-execute source-I/O holder. A clean page's source backing (an ALIAS_BDEV,
 * e.g. the dataset namespace staged by SLM Copy) is generally a different bdev
 * than the vSLM backing namespace, so its fault-in must read through that source
 * bdev's own descriptor + I/O channel. Opening and closing that descriptor and
 * channel on every single 4 KiB page fault is enormously expensive (it creates
 * and tears down an NVMe qpair per page). Instead the copy/exec path holds one
 * source_io across the whole call: the descriptor + per-thread channel are opened
 * on the first source fault and reused for every later page in that call, then
 * released once. A NULL holder falls back to the old open-once-per-fault path, so
 * callers that do not thread one are unchanged.
 */
struct vslm_source_io {
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	bool opened;
};

static int
vslm_source_io_acquire(struct vbdev_vslm *vslm, struct vslm_source_io *src,
		       struct spdk_io_channel *base_ch, struct spdk_bdev *source_bdev,
		       struct spdk_bdev_desc **out_desc, struct spdk_io_channel **out_ch)
{
	const char *source_name;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ch = NULL;
	int rc;

	if (source_bdev == vslm->base_bdev) {
		*out_desc = vslm->base_desc;
		*out_ch = base_ch;
		return 0;
	}
	if (src != NULL && src->opened && src->bdev == source_bdev) {
		*out_desc = src->desc;
		*out_ch = src->ch;
		return 0;
	}

	source_name = source_bdev->name;
	if (source_name == NULL) {
		return -EINVAL;
	}
	rc = spdk_bdev_open_ext(source_name, false, vbdev_vslm_bdev_event_cb, vslm, &desc);
	if (rc != 0) {
		return rc;
	}
	ch = spdk_bdev_get_io_channel(desc);
	if (ch == NULL) {
		spdk_bdev_close(desc);
		return -ENOMEM;
	}

	if (src != NULL) {
		/* Cache for reuse; release any previously-held different source first. */
		if (src->opened) {
			spdk_put_io_channel(src->ch);
			spdk_bdev_close(src->desc);
		}
		src->bdev = source_bdev;
		src->desc = desc;
		src->ch = ch;
		src->opened = true;
	}
	*out_desc = desc;
	*out_ch = ch;
	return 0;
}

static void
vslm_source_io_release(struct vslm_source_io *src)
{
	if (src == NULL || !src->opened) {
		return;
	}
	spdk_put_io_channel(src->ch);
	spdk_bdev_close(src->desc);
	src->opened = false;
	src->bdev = NULL;
	src->desc = NULL;
	src->ch = NULL;
}

static int
vslm_fault_in_page(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		   struct vslm_source_io *src, struct vslm_mmu_shard *held_shard,
		   struct vslm_page *page, struct vslm_lpage *lpage, uint64_t vpn)
{
	struct spdk_bdev *source_bdev = NULL;
	struct spdk_bdev_desc *source_desc = NULL;
	struct spdk_io_channel *source_ch = NULL;
	uint64_t source_block_size;
	uint64_t source_page_blocks;
	uint64_t source_offset_blocks;
	uint64_t source_num_blocks;
	uint64_t block_size;
	uint64_t page_blocks;
	uint64_t offset_blocks;
	uint64_t fault_start_ticks;
	uint64_t fault_ns;
	uint8_t *ptr;
	int rc = 0;

	fault_start_ticks = spdk_get_ticks();
	VSLM_PERF_INC(vslm, fault_in_total);

	ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE);
	SPDK_DEBUGLOG(vslm, "vSLM fault-in start bdev=%s vpn=%" PRIu64 " ppn=%" PRIu64
		      " source_type=%d\n", vslm->vbdev.name, vpn, page->ppn,
		      lpage == NULL ? -1 : (int)lpage->source_type);

	if (lpage == NULL) {
		block_size = spdk_bdev_get_block_size(vslm->base_bdev);
		if (block_size == 0 || (VSLM_PAGE_SIZE % block_size) != 0) {
			rc = -EINVAL;
			goto out;
		}

		page_blocks = VSLM_PAGE_SIZE / block_size;
		offset_blocks = vpn * page_blocks;
		SPDK_DEBUGLOG(vslm, "vSLM fault-in default backing-read bdev=%s vpn=%" PRIu64
			      " offset_blocks=%" PRIu64 " blocks=%" PRIu64 "\n",
			      vslm->vbdev.name, vpn, offset_blocks, page_blocks);
		rc = vslm_submit_sync_base_io(vslm, base_ch, held_shard, ptr, offset_blocks,
					      page_blocks, false);
		goto out;
	}

	if (lpage != NULL &&
	    (lpage->source_type == SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING ||
	     lpage->source_type == SPDK_BDEV_VSLM_PAGE_SOURCE_ALIAS_BDEV) &&
	    lpage->source_bdev != NULL) {
		source_bdev = lpage->source_bdev;
		source_block_size = spdk_bdev_get_block_size(source_bdev);
		if ((source_block_size != 0) &&
		    (VSLM_PAGE_SIZE % source_block_size) == 0 &&
		    (lpage->source_offset_bytes % source_block_size) == 0) {
			source_page_blocks = VSLM_PAGE_SIZE / source_block_size;
			source_offset_blocks = lpage->source_offset_bytes / source_block_size;
			source_num_blocks = spdk_bdev_get_num_blocks(source_bdev);
			if (source_offset_blocks <= source_num_blocks &&
			    source_page_blocks <= (source_num_blocks - source_offset_blocks)) {
				struct vslm_source_io local_src = {0};
				struct vslm_source_io *use_src = (src != NULL) ? src : &local_src;

				rc = vslm_source_io_acquire(vslm, use_src, base_ch, source_bdev,
							    &source_desc, &source_ch);
				if (rc == 0) {
					rc = vslm_submit_sync_read_desc_io(vslm, source_desc, source_ch,
									   held_shard, ptr,
									   source_offset_blocks, source_page_blocks);
				}

				/* A throwaway holder (no per-call src threaded) is released now;
				 * a real src keeps the desc+channel cached for later faults. */
				if (use_src == &local_src) {
					vslm_source_io_release(&local_src);
				}

				if (rc == 0) {
					SPDK_DEBUGLOG(vslm, "vSLM fault-in source-read bdev=%s vpn=%" PRIu64
						      " source=%s offset=%" PRIu64 " bytes=%u\n",
						      vslm->vbdev.name, vpn,
						      source_bdev == NULL ? "none" : source_bdev->name,
						      lpage->source_offset_bytes, VSLM_PAGE_SIZE);
					goto out;
				}

				SPDK_WARNLOG("vSLM source read fault-in failed, falling back to backing: "
					     "vpn=%" PRIu64 " source_type=%d source=%s rc=%d\n",
					     vpn, (int)lpage->source_type,
					     source_bdev == NULL ? "none" : source_bdev->name, rc);
			}
		}
	}

	block_size = spdk_bdev_get_block_size(vslm->base_bdev);
	page_blocks = VSLM_PAGE_SIZE / block_size;
	offset_blocks = vpn * page_blocks;
	SPDK_DEBUGLOG(vslm, "vSLM fault-in fallback backing-read bdev=%s vpn=%" PRIu64
		      " offset_blocks=%" PRIu64 " blocks=%" PRIu64 "\n",
		      vslm->vbdev.name, vpn, offset_blocks, page_blocks);
	rc = vslm_submit_sync_base_io(vslm, base_ch, held_shard, ptr, offset_blocks,
				      page_blocks, false);

out:
	fault_ns = vslm_ticks_delta_to_ns(fault_start_ticks, spdk_get_ticks());
	VSLM_PERF_ADD(vslm, fault_in_io_ns_total, fault_ns);
	vslm_perf_max_u64(&vslm->perf_stats.fault_in_io_ns_max, fault_ns);
	if (rc == 0) {
		VSLM_PERF_INC(vslm, fault_in_4k_total);
		VSLM_PERF_ADD(vslm, fault_in_4k_bytes, VSLM_PAGE_SIZE);
	}
	return rc;
}

static int
vslm_evict_page(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		struct vslm_page *page)
{
	struct vslm_lpage *lpage;
	uint64_t victim_vpn;
	bool evicted_clean = false;
	bool evicted_private = false;
	int final_state = -1;
	int rc;

	victim_vpn = page->vpn;
	lpage = vslm_lookup_lpage(vslm, victim_vpn);
	SPDK_DEBUGLOG(vslm, "vSLM evict start bdev=%s vpn=%" PRIu64 " ppn=%" PRIu64
		      " dirty=%d state=%d\n", vslm->vbdev.name, victim_vpn, page->ppn,
		      page->dirty, lpage == NULL ? -1 : (int)lpage->state);
	if (lpage == NULL) {
		if (!page->dirty) {
			page->load_state = VSLM_PAGE_LOAD_EVICTING;
			page->load_status = 0;
			if (page->on_hash) {
				vslm_hash_remove(vslm, page);
			}
			evicted_clean = true;
			goto evict_done;
		}

		VSLM_PERF_INC(vslm, dirty_page_missing_lpage_total);
		lpage = vslm_lpage_alloc_private_dirty(vslm, victim_vpn);
		if (lpage == NULL) {
			return -ENOMEM;
		}
		page->dirty = true;
	}

	page->load_state = VSLM_PAGE_LOAD_EVICTING;
	page->load_status = 0;

	if (page->on_hash) {
		vslm_hash_remove(vslm, page);
	}

	if (lpage != NULL) {
		switch (lpage->state) {
		case SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT:
			if (lpage->pending_publish && vslm_vpn_has_active_lease(vslm, victim_vpn)) {
				SPDK_DEBUGLOG(vslm, "vSLM evict blocked by active lease bdev=%s vpn=%" PRIu64
					      " pending_publish=1\n", vslm->vbdev.name, victim_vpn);
				rc = -EBUSY;
				goto restore_victim;
			}

			rc = vslm_writeback_page(vslm, base_ch, page, true);
			if (rc != 0) {
				goto restore_victim;
			}
			evicted_private = true;
			break;
		case SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT:
			if (lpage->private_authoritative) {
				vslm_lpage_set_spilled_source(vslm, lpage);
				vslm_lpage_set_state(vslm, page, lpage,
						     SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE);
			} else if (lpage->source_type == SPDK_BDEV_VSLM_PAGE_SOURCE_ALIAS_BDEV) {
				vslm_lpage_set_state(vslm, page, lpage,
						     SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS);
			} else {
				vslm_lpage_delete(vslm, lpage);
				lpage = NULL;
			}
			evicted_clean = true;
			break;
		case SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS:
		case SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE:
		default:
			evicted_clean = true;
			break;
		}
	}

evict_done:
	if (lpage != NULL) {
		final_state = (int)lpage->state;
	}

	if (evicted_private) {
		__atomic_add_fetch(&vslm->stats.vslm_eviction_private_total, 1, __ATOMIC_RELAXED);
		vslm_trace_emitf(vslm, "spill_private",
				 ",\"page_id\":%" PRIu64 ",\"bytes\":%u,\"result\":\"evicted\"",
				 victim_vpn, VSLM_PAGE_SIZE);
	} else if (evicted_clean) {
		__atomic_add_fetch(&vslm->stats.vslm_eviction_clean_total, 1, __ATOMIC_RELAXED);
		vslm_trace_emitf(vslm, "evict_clean",
				 ",\"page_id\":%" PRIu64 ",\"bytes\":%u,\"result\":\"evicted\"",
				 victim_vpn, VSLM_PAGE_SIZE);
	}
	vslm_note_resident_remove(vslm);

	vslm_page_prepare_free(page);
	SPDK_DEBUGLOG(vslm, "vSLM evict done bdev=%s victim_vpn=%" PRIu64 " final_state=%d\n",
		      vslm->vbdev.name, victim_vpn, final_state);
	vslm_debug_validate_page_lists(vslm);
	(void)final_state;
	return 0;

restore_victim:
	if (!page->on_hash) {
		vslm_hash_insert(vslm, page);
	}
	page->load_state = VSLM_PAGE_LOAD_RESIDENT;
	page->load_status = 0;
	if (page->location == VSLM_PAGE_LOC_NONE) {
		vslm_lru_touch(vslm, page);
	}
	vslm_debug_validate_page_lists(vslm);
	return rc;
}

static int
vslm_resolve_page_ex(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		     struct vslm_source_io *src, uint64_t vpn, struct vslm_page **out_page,
		     bool count_fault_stats, bool skip_fault_in)
{
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, vpn);
	uint64_t start_ticks;
	uint64_t elapsed_ns;
	struct vslm_lpage *lpage = NULL;
	enum spdk_bdev_vslm_lpage_state pre_fault_state;
	bool private_refault = false;
	struct vslm_page *page = NULL;
	uint64_t max_vpn;
	int rc = 0;

	start_ticks = spdk_get_ticks();
	VSLM_PERF_INC(vslm, resolve_total);
	*out_page = NULL;

	/*
	 * CoW full-overwrite media-bypass ablation override: never skip the backing
	 * read on a full-page overwrite when disable_cow_bypass is set. Read
	 * atomically: the knob is published lock-free from vslm_apply_debug.
	 */
	if (__atomic_load_n(&vslm->disable_cow_bypass, __ATOMIC_RELAXED)) {
		skip_fault_in = false;
	}

	max_vpn = vslm->virtual_size_bytes / VSLM_PAGE_SIZE;
	if (vpn >= max_vpn) {
		rc = -EINVAL;
		goto out;
	}

	page = vslm_lookup_page(vslm, vpn);
	if (page != NULL) {
		VSLM_PERF_INC(vslm, resolve_hit_total);

		if (page->load_state == VSLM_PAGE_LOAD_LOADING ||
		    page->load_state == VSLM_PAGE_LOAD_EVICTING) {
			rc = vslm_wait_for_loading_page(vslm, page);
			if (rc != 0) {
				goto out;
			}
		}

		if (page->load_state != VSLM_PAGE_LOAD_RESIDENT) {
			rc = page->load_status != 0 ? page->load_status : -EIO;
			goto out;
		}
		if (page->prefetched) {
			VSLM_PERF_INC(vslm, prefetch_async_hit_total);
			page->prefetched = false;
		}

		if (page->dirty) {
			vslm_page_mark_dirty(vslm, page);
		} else {
			vslm_page_mark_clean_if_lpage_exists(vslm, page);
		}
		vslm_lru_touch(vslm, page);
		*out_page = page;
		rc = 0;
		goto out;
	}

	VSLM_PERF_INC(vslm, resolve_miss_total);
	page = vslm_get_free_page(vslm, shard);
	if (page == NULL) {
		vslm_background_clean_pages_locked(vslm, shard, base_ch);
		page = vslm_pick_victim(vslm, shard);
		if (page == NULL) {
			rc = -ENOMEM;
			goto out;
		}
		VSLM_PERF_INC(vslm, resolve_evict_total);
		SPDK_DEBUGLOG(vslm, "vSLM resolve miss bdev=%s vpn=%" PRIu64
			      " no free page, evicting ppn=%" PRIu64 " victim_vpn=%" PRIu64 "\n",
			      vslm->vbdev.name, vpn, page->ppn, page->vpn);

		__atomic_add_fetch(&vslm->stats.page_evictions, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&vslm->stats.page_eviction_bytes, VSLM_PAGE_SIZE, __ATOMIC_RELAXED);
		rc = vslm_evict_page(vslm, base_ch, page);
		if (rc != 0) {
			goto out;
		}
	}

	lpage = vslm_lookup_lpage(vslm, vpn);

	page->vpn = vpn;
	page->dirty = false;
	page->is_busy = false;
	page->load_state = VSLM_PAGE_LOAD_LOADING;
	page->load_status = 0;
	page->prefetched = false;
	vslm_hash_insert(vslm, page);
	vslm_lru_touch(vslm, page);
	SPDK_DEBUGLOG(vslm, "vSLM resolve map bdev=%s vpn=%" PRIu64 " ppn=%" PRIu64
		      " skip_fault_in=%d has_lpage=%d\n",
		      vslm->vbdev.name, vpn, page->ppn, skip_fault_in, lpage != NULL);
	pre_fault_state = lpage != NULL ? lpage->state : SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT;

	if (!skip_fault_in) {
		/*
		 * shard->lock is held here; hand it to the fault-in so its sync
		 * backing read does not busy-poll the reactor under the lock. The
		 * page is LOADING and on the hash/LRU, so it is not an eviction
		 * candidate during the unlocked window.
		 */
		rc = vslm_fault_in_page(vslm, base_ch, src, shard, page, lpage, vpn);
		if (rc != 0) {
			goto rollback_page;
		}

		if (lpage != NULL) {
			if (lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT) {
				lpage->private_authoritative = true;
				vslm_lpage_set_backing_source(vslm, lpage);
			}
			vslm_lpage_set_state(vslm, page, lpage, SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT);
			if (vslm_lpage_free_if_default_backing(vslm, lpage)) {
				lpage = NULL;
			}
		} else {
			page->dirty = false;
		}
		page->load_state = VSLM_PAGE_LOAD_RESIDENT;
		page->load_status = 0;

		private_refault = (lpage != NULL &&
				   pre_fault_state == SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE);
		if (count_fault_stats) {
			vslm_note_page_fault(vslm, private_refault);
		}
	} else {
		VSLM_PERF_INC(vslm, resolve_skip_fault_in_total);
		page->load_state = VSLM_PAGE_LOAD_RESIDENT;
		page->load_status = 0;
		SPDK_DEBUGLOG(vslm, "vSLM resolve skipped fault-in bdev=%s vpn=%" PRIu64
			      " full-page overwrite\n", vslm->vbdev.name, vpn);
	}
	vslm_note_resident_add(vslm);
	*out_page = page;
	SPDK_DEBUGLOG(vslm, "vSLM resolve done bdev=%s vpn=%" PRIu64 " ppn=%" PRIu64
		      " state=%d\n", vslm->vbdev.name, vpn, page->ppn,
		      lpage == NULL ? -1 : (int)lpage->state);
	vslm_debug_validate_page_lists(vslm);
	rc = 0;
	goto out;

rollback_page:
	if (page != NULL) {
		if (page->location == VSLM_PAGE_LOC_LRU) {
			vslm_lru_remove(vslm, page);
		}
		if (page->on_hash) {
			vslm_hash_remove(vslm, page);
		}
		vslm_page_prepare_free(page);
		vslm_free_list_insert_tail(vslm, page);
	}
	vslm_debug_validate_page_lists(vslm);

out:
	elapsed_ns = vslm_ticks_delta_to_ns(start_ticks, spdk_get_ticks());
	VSLM_PERF_ADD(vslm, resolve_ns_total, elapsed_ns);
	vslm_perf_max_u64(&vslm->perf_stats.resolve_ns_max, elapsed_ns);
	return rc;
}

static int
vslm_resolve_page(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		  uint64_t vpn, struct vslm_page **out_page)
{
	return vslm_resolve_page_ex(vslm, base_ch, NULL, vpn, out_page, true, false);
}

static int
vslm_reserve_page_for_fault_locked(struct vbdev_vslm *vslm,
				   uint64_t vpn,
				   bool for_write,
				   bool full_page_overwrite,
				   struct vslm_single_fault_ctx *fault_ctx)
{
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, vpn);
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	uint64_t max_vpn;

	memset(fault_ctx, 0, sizeof(*fault_ctx));
	fault_ctx->vpn = vpn;

	max_vpn = vslm->virtual_size_bytes / VSLM_PAGE_SIZE;
	if (vpn >= max_vpn) {
		return -EINVAL;
	}

	if (vslm_lookup_page(vslm, vpn) != NULL) {
		return -EEXIST;
	}

	page = vslm_get_free_page(vslm, shard);
	if (page == NULL) {
		return -ENOSPC;
	}

	lpage = vslm_lookup_lpage(vslm, vpn);

	page->vpn = vpn;
	page->dirty = false;
	page->is_busy = true;
	page->load_state = VSLM_PAGE_LOAD_LOADING;
	page->load_status = 0;
	page->prefetched = false;
	page->pin_count = 0;
	vslm_hash_insert(vslm, page);

	fault_ctx->page = page;
	fault_ctx->lpage = lpage;
	fault_ctx->pre_fault_state = lpage != NULL ? lpage->state :
				     SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT;
	/*
	 * CoW full-overwrite media-bypass: a full-page overwrite skips the backing
	 * read and prepares a blank frame. The disable_cow_bypass ablation override
	 * forces the read so the page is always faulted in first. Read atomically:
	 * the knob is published lock-free from vslm_apply_debug.
	 */
	fault_ctx->skip_fault_in = for_write && full_page_overwrite &&
				   !__atomic_load_n(&vslm->disable_cow_bypass, __ATOMIC_RELAXED);
	fault_ctx->needs_fault_in = !fault_ctx->skip_fault_in;
	return 0;
}

static int
vslm_fault_in_reserved_page_unlocked(struct vbdev_vslm *vslm,
				     struct spdk_io_channel *base_ch,
				     struct vslm_source_io *src,
				     struct vslm_single_fault_ctx *fault_ctx)
{
	if (!fault_ctx->needs_fault_in) {
		return 0;
	}

	/* The caller (split path) has already dropped the shard lock: pass NULL shard. */
	return vslm_fault_in_page(vslm, base_ch, src, NULL, fault_ctx->page,
				  fault_ctx->lpage, fault_ctx->vpn);
}

static int
vslm_commit_reserved_page_locked(struct vbdev_vslm *vslm,
				 struct vslm_single_fault_ctx *fault_ctx,
				 bool count_fault_stats)
{
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	bool private_refault = false;

	page = fault_ctx->page;
	lpage = fault_ctx->lpage;
	if (page == NULL) {
		return -EINVAL;
	}

	if (lpage != NULL) {
		if (lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT) {
			lpage->private_authoritative = true;
			vslm_lpage_set_backing_source(vslm, lpage);
		}
		vslm_lpage_set_state(vslm, page, lpage, SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT);
		if (vslm_lpage_free_if_default_backing(vslm, lpage)) {
			lpage = NULL;
		}
	} else {
		page->dirty = false;
	}

	page->is_busy = false;
	page->load_state = VSLM_PAGE_LOAD_RESIDENT;
	page->load_status = 0;
	vslm_lru_touch(vslm, page);
	vslm_note_resident_add(vslm);

	if (fault_ctx->needs_fault_in) {
		private_refault = (lpage != NULL &&
				   fault_ctx->pre_fault_state ==
				   SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE);
		if (count_fault_stats) {
			vslm_note_page_fault(vslm, private_refault);
		}
	} else {
		VSLM_PERF_INC(vslm, resolve_skip_fault_in_total);
	}

	return 0;
}

static void
vslm_rollback_reserved_page_locked(struct vbdev_vslm *vslm,
				   struct vslm_single_fault_ctx *fault_ctx)
{
	struct vslm_page *page;

	page = fault_ctx->page;
	if (page == NULL) {
		return;
	}

	if (page->location == VSLM_PAGE_LOC_LRU) {
		vslm_lru_remove(vslm, page);
	}
	if (page->on_hash) {
		vslm_hash_remove(vslm, page);
	}
	vslm_page_prepare_free(page);
	vslm_free_list_insert_tail(vslm, page);
	fault_ctx->page = NULL;
}

static int
vslm_resolve_and_pin_page_locked(struct vbdev_vslm *vslm,
				 struct spdk_io_channel *base_ch,
				 struct vslm_source_io *src,
				 uint64_t vpn,
				 bool for_write,
				 bool full_page_overwrite,
				 struct vslm_page **out_page)
{
	struct vslm_single_fault_ctx fault_ctx = {};
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, vpn);
	struct vslm_page *page;
	uint64_t start_ticks;
	uint64_t elapsed_ns;
	bool use_split = false;
	int rc;

	page = vslm_lookup_page(vslm, vpn);
	if (page == NULL) {
		rc = vslm_reserve_page_for_fault_locked(vslm, vpn, for_write,
							full_page_overwrite, &fault_ctx);
		if (rc == 0) {
			use_split = true;
		} else if (rc == -ENOSPC || rc == -EEXIST) {
			rc = vslm_resolve_page_ex(vslm, base_ch, src, vpn, &page, true,
						  for_write && full_page_overwrite);
			if (rc != 0) {
				return rc;
			}
		} else {
			return rc;
		}
	} else {
		rc = vslm_resolve_page_ex(vslm, base_ch, src, vpn, &page, true,
					  for_write && full_page_overwrite);
		if (rc != 0) {
			return rc;
		}
	}

	if (use_split) {
		start_ticks = spdk_get_ticks();
		VSLM_PERF_INC(vslm, resolve_total);
		VSLM_PERF_INC(vslm, resolve_miss_total);

		if (fault_ctx.needs_fault_in) {
			pthread_mutex_unlock(&shard->lock);
			rc = vslm_fault_in_reserved_page_unlocked(vslm, base_ch, src, &fault_ctx);
			pthread_mutex_lock(&shard->lock);
		} else {
			rc = 0;
		}

		if (rc != 0) {
			vslm_rollback_reserved_page_locked(vslm, &fault_ctx);
			vslm_debug_validate_page_lists(vslm);
		} else {
			rc = vslm_commit_reserved_page_locked(vslm, &fault_ctx, true);
			if (rc != 0) {
				vslm_rollback_reserved_page_locked(vslm, &fault_ctx);
				vslm_debug_validate_page_lists(vslm);
			} else {
				page = fault_ctx.page;
				vslm_debug_validate_page_lists(vslm);
			}
		}

		elapsed_ns = vslm_ticks_delta_to_ns(start_ticks, spdk_get_ticks());
		VSLM_PERF_ADD(vslm, resolve_ns_total, elapsed_ns);
		vslm_perf_max_u64(&vslm->perf_stats.resolve_ns_max, elapsed_ns);
		if (rc != 0) {
			return rc;
		}
	}

	if (page->load_state == VSLM_PAGE_LOAD_LOADING ||
	    page->load_state == VSLM_PAGE_LOAD_EVICTING) {
		rc = vslm_wait_for_loading_page(vslm, page);
		if (rc != 0) {
			return rc;
		}
	}

	if (page->load_state != VSLM_PAGE_LOAD_RESIDENT) {
		return page->load_status != 0 ? page->load_status : -EIO;
	}

	vslm_page_pin_locked(vslm, page);
	*out_page = page;
	return 0;
}

static bool
vslm_vpn_batch_clean_source(struct vbdev_vslm *vslm, uint64_t vpn,
			    struct spdk_bdev **src_bdev, uint64_t *src_off,
			    struct vslm_lpage **out_lpage)
{
	struct vslm_lpage *lpage;
	uint64_t max_vpn;

	max_vpn = vslm->virtual_size_bytes / VSLM_PAGE_SIZE;
	if (vpn >= max_vpn) {
		return false;
	}
	if (vslm_lookup_page(vslm, vpn) != NULL) {
		return false;	/* already resident */
	}

	lpage = vslm_lookup_lpage(vslm, vpn);
	if (lpage == NULL) {
		/* Default clean backing: the vSLM backing namespace at the linear offset. */
		*src_bdev = vslm->base_bdev;
		*src_off = vpn * VSLM_PAGE_SIZE;
		*out_lpage = NULL;
		return true;
	}
	/*
	 * Clean alias (e.g. an SLM-Copy-staged dataset range): a contiguous run of
	 * aliases over the same source bdev with contiguous source offsets is bulk-read
	 * in one readv from that source instead of one read (and, pre-fix, one source
	 * descriptor open/close) per page.
	 */
	if (lpage->state == SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS &&
	    lpage->source_bdev != NULL && !lpage->pending_publish) {
		*src_bdev = lpage->source_bdev;
		*src_off = lpage->source_offset_bytes;
		*out_lpage = lpage;
		return true;
	}
	return false;
}

/*
 * Longest run starting at start_vpn that can be filled by ONE source readv: every
 * page must be a clean batch candidate over the SAME source bdev with source
 * offsets contiguous (off == run_off + i*PAGE), and its shard must have a free
 * frame. Returns the run length and the run's source bdev/offset (from the first
 * page). A run never mixes default-backing and alias pages, nor two sources.
 */
static uint32_t
vslm_find_clean_batch_run(struct vbdev_vslm *vslm, uint64_t start_vpn, uint32_t max_pages,
			  struct spdk_bdev **run_src_bdev, uint64_t *run_src_off)
{
	uint64_t max_vpn;
	struct spdk_bdev *first_bdev = NULL;
	uint64_t first_off = 0;
	uint32_t nr_pages = 0;
	uint64_t vpn;

	max_vpn = vslm->virtual_size_bytes / VSLM_PAGE_SIZE;
	for (vpn = start_vpn; vpn < max_vpn && nr_pages < max_pages; vpn++, nr_pages++) {
		struct spdk_bdev *sbdev = NULL;
		uint64_t soff = 0;
		struct vslm_lpage *lp = NULL;

		if (!vslm_vpn_batch_clean_source(vslm, vpn, &sbdev, &soff, &lp)) {
			break;
		}
		if (nr_pages == 0) {
			first_bdev = sbdev;
			first_off = soff;
		} else if (sbdev != first_bdev ||
			   soff != first_off + (uint64_t)nr_pages * VSLM_PAGE_SIZE) {
			break;	/* source not contiguous with the run */
		}
		if (TAILQ_FIRST(&vslm_shard_for_vpn(vslm, vpn)->free_list) == NULL) {
			break;
		}
	}

	if (nr_pages > 0) {
		*run_src_bdev = first_bdev;
		*run_src_off = first_off;
	}
	return nr_pages;
}

static uint32_t
vslm_clamp_batch_pages_to_range(struct vbdev_vslm *vslm,
				uint64_t start_vpn,
				uint32_t requested_pages)
{
	uint64_t max_vpn;

	max_vpn = vslm->virtual_size_bytes / VSLM_PAGE_SIZE;
	if (start_vpn >= max_vpn) {
		return 0;
	}

	if (requested_pages > (max_vpn - start_vpn)) {
		return (uint32_t)(max_vpn - start_vpn);
	}

	return requested_pages;
}

static int
vslm_batch_reserve_free_pages(struct vbdev_vslm *vslm,
			      struct vslm_mmu_shard *shard,
			      uint64_t start_vpn,
			      uint32_t requested_pages,
			      struct vslm_fault_batch_ctx *batch)
{
	uint32_t i;
	uint64_t vpn;
	struct vslm_page *page;

	batch->nr_pages = 0;
	for (i = 0; i < requested_pages; i++) {
		struct spdk_bdev *sbdev = NULL;
		uint64_t soff = 0;
		struct vslm_lpage *lp = NULL;

		vpn = start_vpn + i;
		if (!vslm_vpn_batch_clean_source(vslm, vpn, &sbdev, &soff, &lp)) {
			break;
		}
		assert(vslm_vpn_to_shard(vslm, vpn) == (uint32_t)(shard - vslm->shards));

		page = vslm_get_free_page(vslm, shard);
		if (page == NULL) {
			break;
		}

		page->vpn = vpn;
		page->dirty = false;
		page->is_busy = true;
		page->load_state = VSLM_PAGE_LOAD_LOADING;
		page->load_status = 0;
		page->prefetched = false;
		vslm_hash_insert(vslm, page);

		batch->pages[i].page = page;
		batch->pages[i].lpage = lp;
		batch->pages[i].vpn = vpn;
		batch->pages[i].inserted_hash = true;
		batch->pages[i].inserted_lru = false;
		batch->nr_pages++;
	}

	if (batch->nr_pages == 0) {
		return -ENOSPC;
	}

	return 0;
}

static void
vslm_batch_rollback_reserved_pages(struct vbdev_vslm *vslm,
				   struct vslm_fault_batch_ctx *batch)
{
	uint32_t i;
	struct vslm_page *page;

	for (i = 0; i < batch->nr_pages; i++) {
		page = batch->pages[i].page;
		if (page == NULL) {
			continue;
		}

		if (page->on_hash) {
			vslm_hash_remove(vslm, page);
		}
		if (page->location == VSLM_PAGE_LOC_LRU) {
			vslm_lru_remove(vslm, page);
		}

		vslm_page_prepare_free(page);
		vslm_free_list_insert_tail(vslm, page);
		batch->pages[i].page = NULL;
	}

	batch->nr_pages = 0;
}

static void
vslm_batch_commit_pages(struct vbdev_vslm *vslm,
			struct vslm_fault_batch_ctx *batch,
			bool count_fault_stats)
{
	uint32_t i;
	struct vslm_page *page;

	for (i = 0; i < batch->nr_pages; i++) {
		struct vslm_lpage *lpage = batch->pages[i].lpage;

		page = batch->pages[i].page;
		if (page == NULL) {
			continue;
		}

		if (lpage != NULL) {
			/*
			 * Clean-alias page: link the lpage to this resident frame
			 * (mirroring vslm_commit_reserved_page_locked) so eviction
			 * reverts it to its alias source rather than the backing namespace.
			 */
			vslm_lpage_set_state(vslm, page, lpage, SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT);
			(void)vslm_lpage_free_if_default_backing(vslm, lpage);
		}

		page->dirty = false;
		page->is_busy = false;
		page->load_state = VSLM_PAGE_LOAD_RESIDENT;
		page->load_status = 0;
		page->prefetched = false;
		vslm_lru_touch(vslm, page);
		batch->pages[i].inserted_lru = true;
		vslm_note_resident_add(vslm);
		if (count_fault_stats) {
			vslm_note_page_fault(vslm, false);
		}

		VSLM_PERF_INC(vslm, fault_in_batched_pages);
	}

	VSLM_PERF_INC(vslm, fault_in_batched_total);
	VSLM_PERF_ADD(vslm, fault_in_batched_bytes, batch->nr_pages * VSLM_PAGE_SIZE);
}

static void
vslm_batch_build_iov(struct vbdev_vslm *vslm, struct vslm_fault_batch_ctx *batch)
{
	uint32_t i;
	struct vslm_page *page;

	for (i = 0; i < batch->nr_pages; i++) {
		page = batch->pages[i].page;
		batch->iov[i].iov_base = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE);
		batch->iov[i].iov_len = VSLM_PAGE_SIZE;
	}
}

static int
vslm_fault_in_clean_backing_batch_sync(struct vbdev_vslm *vslm,
				       struct spdk_io_channel *base_ch,
				       struct vslm_source_io *src,
				       struct spdk_bdev *src_bdev,
				       uint64_t src_off_bytes,
				       uint64_t start_vpn,
				       uint32_t requested_pages,
				       bool count_fault_stats,
				       uint32_t *pages_loaded)
{
	struct spdk_bdev_desc *src_desc = NULL;
	struct spdk_io_channel *src_ch = NULL;
	struct vslm_fault_batch_ctx batch = {};
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, start_vpn);
	/*
	 * requested_pages is clamped below to VSLM_MMU_SHARD_STRIDE_PAGES (a single
	 * shard's stride run) and to the configured fault-batch page budget, both of
	 * which are <= VSLM_MMU_SHARD_STRIDE_PAGES (512). Back the per-batch page and
	 * iov scratch with fixed on-stack arrays sized to that bound instead of a
	 * per-call calloc/free pair on the hot fault-in path.
	 */
	struct vslm_fault_batch_page stack_pages[VSLM_MMU_SHARD_STRIDE_PAGES];
	struct iovec stack_iov[VSLM_MMU_SHARD_STRIDE_PAGES];
	uint64_t block_size;
	uint64_t page_blocks;
	uint64_t offset_blocks;
	uint64_t num_blocks;
	uint64_t io_start_ticks;
	uint64_t io_ns;
	uint64_t lock_start_ticks;
	uint64_t bounce_memcpy_start_ticks;
	uint64_t bounce_memcpy_ns = 0;
	uint64_t bounce_bytes;
	uint32_t max_by_bytes;
	int rc;

	if (pages_loaded == NULL) {
		return -EINVAL;
	}
	*pages_loaded = 0;

	if (requested_pages == 0) {
		return 0;
	}

	block_size = spdk_bdev_get_block_size(src_bdev);
	if (block_size == 0 || (VSLM_PAGE_SIZE % block_size) != 0 ||
	    (src_off_bytes % block_size) != 0) {
		return -EINVAL;
	}

	page_blocks = VSLM_PAGE_SIZE / block_size;
	max_by_bytes = vslm->fault_batch_max_bytes / VSLM_PAGE_SIZE;
	if (max_by_bytes == 0) {
		max_by_bytes = 1;
	}
	requested_pages = spdk_min(requested_pages, max_by_bytes);
	requested_pages = vslm_clamp_batch_pages_to_range(vslm, start_vpn, requested_pages);
	/*
	 * Keep the whole batch within one MMU shard so a single shard lock covers
	 * it. A batch never spans more than the bytes remaining in the current
	 * stride run, which also bounds it to VSLM_MMU_SHARD_STRIDE_PAGES and lets
	 * the fixed stack scratch arrays above suffice.
	 */
	{
		uint64_t shard_room = VSLM_MMU_SHARD_STRIDE_PAGES -
				      (start_vpn % VSLM_MMU_SHARD_STRIDE_PAGES);
		if (requested_pages > shard_room) {
			requested_pages = (uint32_t)shard_room;
		}
	}
	if (requested_pages > VSLM_MMU_SHARD_STRIDE_PAGES) {
		requested_pages = VSLM_MMU_SHARD_STRIDE_PAGES;
	}
	if (requested_pages == 0) {
		return -ENOSPC;
	}

	batch.vslm = vslm;
	batch.base_ch = base_ch;
	batch.start_vpn = start_vpn;
	batch.pages = stack_pages;
	batch.iov = stack_iov;
	memset(stack_pages, 0, requested_pages * sizeof(stack_pages[0]));
	memset(stack_iov, 0, requested_pages * sizeof(stack_iov[0]));

	VSLM_PERF_INC(vslm, fault_batch_attempt_total);
	VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
	pthread_mutex_lock(&shard->lock);
	lock_start_ticks = spdk_get_ticks();
	rc = vslm_batch_reserve_free_pages(vslm, shard, start_vpn, requested_pages, &batch);
	if (rc == 0) {
		vslm_batch_build_iov(vslm, &batch);
	}
	vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
	if (rc != 0) {
		goto out_free;
	}

	offset_blocks = src_off_bytes / block_size;
	num_blocks = batch.nr_pages * page_blocks;
	io_start_ticks = spdk_get_ticks();
	VSLM_PERF_INC(vslm, fault_in_batched_readv_total);

	/*
	 * Resolve the source channel once for the whole run (base namespace for a
	 * default-backing run, or the held alias source for an alias run) and bulk-read
	 * the entire contiguous run in one readv.
	 */
	rc = vslm_source_io_acquire(vslm, src, base_ch, src_bdev, &src_desc, &src_ch);
	if (rc == 0) {
		/* The shard lock is already dropped here (released above), so pass NULL. */
		rc = vslm_submit_sync_readv_desc(vslm, src_desc, src_ch, NULL, batch.iov,
						 (int)batch.nr_pages, offset_blocks, num_blocks);
	}
	if (rc == -ENOTSUP) {
		bool contiguous = true;
		uint8_t *bounce = NULL;
		int ring_idx = -1;
		uint32_t ci;

		VSLM_PERF_INC(vslm, fault_batch_fallback_total);
		VSLM_PERF_INC(vslm, fault_in_batched_fallback_total);
		bounce_bytes = batch.nr_pages * VSLM_PAGE_SIZE;

		/*
		 * Zero-copy pointer remapping: when the reserved frames are
		 * physically contiguous in SRAM, slice the single contiguous
		 * fallback read directly into the page frames, with no bounce
		 * buffer and no CPU copy.
		 */
		for (ci = 1; ci < batch.nr_pages; ci++) {
			if ((uint8_t *)batch.iov[ci].iov_base !=
			    (uint8_t *)batch.iov[0].iov_base + (uint64_t)ci * VSLM_PAGE_SIZE) {
				contiguous = false;
				break;
			}
		}

		if (contiguous) {
			rc = vslm_submit_sync_read_desc_io(vslm, src_desc, src_ch, NULL,
							       batch.iov[0].iov_base,
							       offset_blocks, num_blocks);
		} else {
			/* Borrow a pre-allocated DMA ring buffer; malloc only if the pool is full. */
			bounce = vslm_dma_ring_acquire(vslm, bounce_bytes, &ring_idx);
			if (bounce == NULL) {
				batch.bounce_buf = spdk_dma_malloc(bounce_bytes, VSLM_PAGE_SIZE, NULL);
				bounce = batch.bounce_buf;
			}
			if (bounce == NULL) {
				rc = -ENOMEM;
			} else {
				batch.used_bounce = true;
				rc = vslm_submit_sync_read_desc_io(vslm, src_desc, src_ch, NULL, bounce,
								       offset_blocks, num_blocks);
				if (rc == 0) {
					bounce_memcpy_start_ticks = spdk_get_ticks();
					/*
					 * The bounce buffer is contiguous (page i lives at
					 * bounce + i*PAGE), so a run of destination frames that
					 * are physically contiguous in SRAM maps to one
					 * contiguous source slice. Coalesce the scatter into one
					 * memcpy per maximal contiguous destination run instead of
					 * one per page; the data movement is identical.
					 */
					for (uint32_t i = 0; i < batch.nr_pages;) {
						uint32_t j = i + 1;

						while (j < batch.nr_pages &&
						       (uint8_t *)batch.iov[j].iov_base ==
						       (uint8_t *)batch.iov[i].iov_base +
						       (uint64_t)(j - i) * VSLM_PAGE_SIZE) {
							j++;
						}
						memcpy(batch.iov[i].iov_base,
						       bounce + ((uint64_t)i * VSLM_PAGE_SIZE),
						       (size_t)(j - i) * VSLM_PAGE_SIZE);
						i = j;
					}
					bounce_memcpy_ns = vslm_ticks_delta_to_ns(bounce_memcpy_start_ticks,
							   spdk_get_ticks());
				}
			}
			vslm_dma_ring_release(vslm, ring_idx);
		}
	}
	io_ns = vslm_ticks_delta_to_ns(io_start_ticks, spdk_get_ticks());
	VSLM_PERF_ADD(vslm, fault_in_io_ns_total, io_ns);
	vslm_perf_max_u64(&vslm->perf_stats.fault_in_io_ns_max, io_ns);

	VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
	pthread_mutex_lock(&shard->lock);
	lock_start_ticks = spdk_get_ticks();
	if (rc == 0) {
		vslm_batch_commit_pages(vslm, &batch, count_fault_stats);
		/* last_batch_* are read lock-free by the prefetch path; publish atomically. */
		__atomic_store_n(&vslm->last_batch_start_vpn, start_vpn, __ATOMIC_RELAXED);
		__atomic_store_n(&vslm->last_batch_pages, batch.nr_pages, __ATOMIC_RELAXED);
		*pages_loaded = batch.nr_pages;
	} else {
		__atomic_store_n(&vslm->last_batch_start_vpn, UINT64_MAX, __ATOMIC_RELAXED);
		__atomic_store_n(&vslm->last_batch_pages, 0, __ATOMIC_RELAXED);
		vslm_batch_rollback_reserved_pages(vslm, &batch);
	}
	vslm_debug_validate_page_lists(vslm);
	vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);

	if (batch.used_bounce && rc == 0) {
		VSLM_PERF_INC(vslm, fault_in_batched_bounce_total);
		VSLM_PERF_ADD(vslm, fault_in_batched_bounce_bytes,
			      batch.nr_pages * VSLM_PAGE_SIZE);
		VSLM_PERF_ADD(vslm, fault_in_batched_bounce_memcpy_bytes,
			      batch.nr_pages * VSLM_PAGE_SIZE);
		VSLM_PERF_ADD(vslm, fault_in_batched_bounce_memcpy_ns_total, bounce_memcpy_ns);
	}
	if (rc != 0) {
		VSLM_PERF_INC(vslm, fault_in_batched_failed_total);
	}

out_free:
	if (batch.bounce_buf != NULL) {
		spdk_dma_free(batch.bounce_buf);
	}
	/* batch.pages/batch.iov point at on-stack scratch; nothing to free. */
	return rc;
}

static bool
vslm_range_covered_by_last_batch(struct vbdev_vslm *vslm, uint64_t vpn)
{
	uint64_t start;
	uint32_t pages;
	uint64_t end;

	/* last_batch_* are written lock-free under various shard locks; load atomically. */
	start = __atomic_load_n(&vslm->last_batch_start_vpn, __ATOMIC_RELAXED);
	pages = __atomic_load_n(&vslm->last_batch_pages, __ATOMIC_RELAXED);
	if (start == UINT64_MAX || pages == 0) {
		return false;
	}

	end = start + pages;
	if (end < start) {
		return false;
	}

	return vpn >= start && vpn < end;
}

/*
 * Generalized range-local access-trend detector. Learns an arbitrary stride
 * (the page delta between consecutive demand faults), of which pure sequential
 * access is the special case stride == +1. Returns true once the same stride
 * has repeated enough to be confident, and reports it via *out_stride so the
 * issuing path can stage forthcoming non-contiguous blocks along the trend.
 */
static bool
vslm_prefetch_policy_should_submit(struct vbdev_vslm *vslm, uint64_t demand_vpn,
				   int64_t *out_stride)
{
	bool should = false;
	int64_t stride = 1;
	int64_t delta;

	pthread_mutex_lock(&vslm->policy_lock);
	if (vslm->last_exec_read_vpn == UINT64_MAX) {
		/* First demand of a new run: no trend yet. */
		vslm->sequential_exec_read_count = 1;
		vslm->strided_exec_read_count = 0;
		vslm->last_exec_read_stride = 0;
		vslm->prefetch_until_vpn = 0;
	} else {
		delta = (int64_t)demand_vpn - (int64_t)vslm->last_exec_read_vpn;

		/* Sequential sub-detector (kept for stride==1 fast path/stats). */
		if (delta == 1) {
			vslm->sequential_exec_read_count++;
		} else {
			vslm->sequential_exec_read_count = 1;
		}

		/* Generalized stride sub-detector. */
		if (delta != 0 && delta == vslm->last_exec_read_stride) {
			vslm->strided_exec_read_count++;
		} else {
			vslm->last_exec_read_stride = delta;
			vslm->strided_exec_read_count = (delta != 0) ? 1 : 0;
			vslm->prefetch_until_vpn = 0;
		}
	}
	vslm->last_exec_read_vpn = demand_vpn;

	if (vslm->sequential_exec_read_count >= 2 &&
	    demand_vpn >= vslm->prefetch_until_vpn) {
		should = true;
		stride = 1;
	} else if (vslm->strided_exec_read_count >= 2 &&
		   demand_vpn >= vslm->prefetch_until_vpn) {
		should = true;
		stride = vslm->last_exec_read_stride;
	}
	pthread_mutex_unlock(&vslm->policy_lock);

	if (out_stride != NULL) {
		*out_stride = stride;
	}
	return should;
}

static int
vslm_prefetch_reserve_pages_locked(struct vbdev_vslm *vslm,
				   struct vslm_prefetch_batch_ctx *ctx,
				   uint32_t requested_pages)
{
	uint32_t i;
	uint64_t max_vpn;
	uint64_t vpn;
	struct vslm_page *page;
	struct vslm_lpage *lpage;

	max_vpn = vslm->virtual_size_bytes / VSLM_PAGE_SIZE;
	ctx->nr_pages = 0;
	for (i = 0; i < requested_pages; i++) {
		vpn = ctx->start_vpn + i;
		if (vpn >= max_vpn) {
			break;
		}

		page = vslm_lookup_page(vslm, vpn);
		if (page != NULL) {
			VSLM_PERF_INC(vslm, prefetch_async_page_skipped_resident_total);
			break;
		}

		lpage = vslm_lookup_lpage(vslm, vpn);
		if (lpage != NULL) {
			VSLM_PERF_INC(vslm, prefetch_async_page_skipped_lpage_total);
			break;
		}

		if (TAILQ_FIRST(&vslm_shard_for_vpn(vslm, vpn)->free_list) == NULL) {
			VSLM_PERF_INC(vslm, prefetch_async_page_skipped_no_free_total);
			break;
		}

		page = vslm_get_free_page(vslm, vslm_shard_for_vpn(vslm, vpn));
		if (page == NULL) {
			VSLM_PERF_INC(vslm, prefetch_async_page_skipped_no_free_total);
			break;
		}

		page->vpn = vpn;
		page->dirty = false;
		page->is_busy = true;
		page->load_state = VSLM_PAGE_LOAD_LOADING;
		page->load_status = 0;
		vslm_hash_insert(vslm, page);
		ctx->pages[i] = page;
		ctx->iov[i].iov_base = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE);
		ctx->iov[i].iov_len = VSLM_PAGE_SIZE;
		ctx->nr_pages++;
	}

	if (ctx->nr_pages == 0) {
		return -ENOSPC;
	}

	return 0;
}

static void
vslm_prefetch_rollback_locked(struct vbdev_vslm *vslm,
			      struct vslm_prefetch_batch_ctx *ctx,
			      struct vslm_page_waiter_list *waiters)
{
	uint32_t i;
	struct vslm_page *page;

	for (i = 0; i < ctx->nr_pages; i++) {
		page = ctx->pages[i];
		if (page == NULL) {
			continue;
		}

		vslm_page_take_waiters_locked(page, -EIO, NULL, waiters);
		if (page->on_hash) {
			vslm_hash_remove(vslm, page);
		}
		if (page->location == VSLM_PAGE_LOC_LRU) {
			vslm_lru_remove(vslm, page);
		}

		vslm_page_prepare_free(page);
		vslm_free_list_insert_tail(vslm, page);
		ctx->pages[i] = NULL;
	}

	ctx->nr_pages = 0;
}

static void
vslm_prefetch_batch_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct vslm_prefetch_batch_ctx *ctx = cb_arg;
	struct vbdev_vslm *vslm = ctx->vslm;
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, ctx->start_vpn);
	struct vslm_page_waiter_list waiters;
	uint64_t elapsed_ns;
	uint32_t i;
	struct vslm_page *page;

	TAILQ_INIT(&waiters);
	spdk_bdev_free_io(bdev_io);
	elapsed_ns = vslm_ticks_delta_to_ns(ctx->submit_ticks, spdk_get_ticks());
	VSLM_PERF_ADD(vslm, prefetch_async_io_ns_total, elapsed_ns);
	vslm_perf_max_u64(&vslm->perf_stats.prefetch_async_io_ns_max, elapsed_ns);
	VSLM_PERF_INC(vslm, prefetch_async_complete_total);

	pthread_mutex_lock(&shard->lock);
	if (success) {
		/* last_batch_* are read lock-free by the prefetch path; publish atomically. */
		__atomic_store_n(&vslm->last_batch_start_vpn, ctx->start_vpn, __ATOMIC_RELAXED);
		__atomic_store_n(&vslm->last_batch_pages, ctx->nr_pages, __ATOMIC_RELAXED);
	}
	for (i = 0; i < ctx->nr_pages; i++) {
		page = ctx->pages[i];
		if (page == NULL) {
			continue;
		}

		if (success) {
			page->dirty = false;
			page->is_busy = false;
			page->load_state = VSLM_PAGE_LOAD_RESIDENT;
			page->load_status = 0;
			page->prefetched = true;
			vslm_lru_touch(vslm, page);
			vslm_note_resident_add(vslm);
			vslm_page_take_waiters_locked(page, 0, page, &waiters);
			VSLM_PERF_INC(vslm, prefetch_async_page_loaded_total);
		} else {
			vslm_page_take_waiters_locked(page, -EIO, NULL, &waiters);
			if (page->on_hash) {
				vslm_hash_remove(vslm, page);
			}
			vslm_page_prepare_free(page);
			vslm_free_list_insert_tail(vslm, page);
			VSLM_PERF_INC(vslm, prefetch_async_page_failed_total);
		}
	}
	if (!success) {
		VSLM_PERF_INC(vslm, prefetch_async_failed_total);
	}
	vslm_debug_validate_page_lists(vslm);
	pthread_mutex_unlock(&shard->lock);
	vslm_page_complete_waiters(&waiters);

	if (ctx->ch != NULL) {
		spdk_put_io_channel(ctx->ch);
	}
	__atomic_sub_fetch(&vslm->prefetch_inflight_batches, 1, __ATOMIC_RELAXED);
	vslm_put_io_ref(vslm);
	free(ctx->iov);
	free(ctx->pages);
	free(ctx);
}

static int
vslm_prefetch_clean_backing_batch_submit(struct vbdev_vslm *vslm,
		uint64_t start_vpn,
		uint32_t requested_pages)
{
	struct vslm_prefetch_batch_ctx *ctx;
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, start_vpn);
	uint64_t lock_start_ticks;
	uint64_t block_size;
	uint64_t page_blocks;
	uint64_t offset_blocks;
	uint64_t num_blocks;
	struct vslm_page_waiter_list waiters;
	int rc;

	TAILQ_INIT(&waiters);
	if (requested_pages == 0) {
		return 0;
	}
	/* Keep the whole prefetch batch within one MMU shard. */
	if (vslm->num_shards > 1) {
		uint64_t shard_room = VSLM_MMU_SHARD_STRIDE_PAGES -
				      (start_vpn % VSLM_MMU_SHARD_STRIDE_PAGES);
		if (requested_pages > shard_room) {
			requested_pages = (uint32_t)shard_room;
		}
	}
	if (!vslm_try_get_io_ref(vslm)) {
		return -ESHUTDOWN;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		rc = -ENOMEM;
		goto err_put_ref;
	}

	ctx->vslm = vslm;
	ctx->desc = vslm->base_desc;
	ctx->start_vpn = start_vpn;
	ctx->pages = calloc(requested_pages, sizeof(*ctx->pages));
	ctx->iov = calloc(requested_pages, sizeof(*ctx->iov));
	if (ctx->pages == NULL || ctx->iov == NULL) {
		rc = -ENOMEM;
		goto err_free_ctx;
	}

	ctx->ch = spdk_bdev_get_io_channel(vslm->base_desc);
	if (ctx->ch == NULL) {
		rc = -ENOMEM;
		goto err_free_ctx;
	}

	VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
	pthread_mutex_lock(&shard->lock);
	lock_start_ticks = spdk_get_ticks();
	rc = vslm_prefetch_reserve_pages_locked(vslm, ctx, requested_pages);
	vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
	if (rc != 0) {
		goto err_free_ctx;
	}

	block_size = spdk_bdev_get_block_size(vslm->base_bdev);
	if (block_size == 0 || (VSLM_PAGE_SIZE % block_size) != 0) {
		rc = -EINVAL;
		goto rollback_and_free;
	}

	page_blocks = VSLM_PAGE_SIZE / block_size;
	offset_blocks = start_vpn * page_blocks;
	num_blocks = ctx->nr_pages * page_blocks;
	ctx->submit_ticks = spdk_get_ticks();

	rc = spdk_bdev_readv_blocks(vslm->base_desc, ctx->ch, ctx->iov, (int)ctx->nr_pages,
				    offset_blocks, num_blocks,
				    vslm_prefetch_batch_complete, ctx);
	if (rc != 0) {
		goto rollback_and_free;
	}

	VSLM_PERF_INC(vslm, prefetch_async_submit_total);
	__atomic_add_fetch(&vslm->prefetch_inflight_batches, 1, __ATOMIC_RELAXED);
	return 0;

rollback_and_free:
	VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
	pthread_mutex_lock(&shard->lock);
	lock_start_ticks = spdk_get_ticks();
	vslm_prefetch_rollback_locked(vslm, ctx, &waiters);
	vslm_debug_validate_page_lists(vslm);
	vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
	vslm_page_complete_waiters(&waiters);

err_free_ctx:
	if (ctx->ch != NULL) {
		spdk_put_io_channel(ctx->ch);
	}
	free(ctx->iov);
	free(ctx->pages);
	free(ctx);
err_put_ref:
	vslm_put_io_ref(vslm);
	return rc;
}

/* Cap how far a single learned stride may jump, to bound speculative work. */
#define VSLM_PREFETCH_MAX_STRIDE_PAGES 4096

/*
 * Stage forthcoming non-contiguous blocks along a learned stride. Each strided
 * target is submitted as an independent single-page async readv batch (the
 * source blocks are non-contiguous, so they cannot share one vector command),
 * reusing the same asynchronous clean-backing batch machinery as the
 * contiguous path and respecting the prefetch queue depth.
 */
static void
vslm_prefetch_strided_submit(struct vbdev_vslm *vslm, uint64_t demand_vpn,
			     int64_t stride, uint32_t readahead_pages)
{
	uint64_t max_vpn = vslm->virtual_size_bytes / VSLM_PAGE_SIZE;
	uint64_t furthest = demand_vpn;
	uint32_t submitted = 0;
	uint32_t k;

	for (k = 1; k <= readahead_pages; k++) {
		int64_t target = (int64_t)demand_vpn + (int64_t)k * stride;
		int rc;

		if (target < 0 || (uint64_t)target >= max_vpn) {
			break;
		}
		if (__atomic_load_n(&vslm->prefetch_inflight_batches, __ATOMIC_RELAXED) >=
		    vslm->prefetch_queue_depth) {
			break;
		}

		rc = vslm_prefetch_clean_backing_batch_submit(vslm, (uint64_t)target, 1);
		if (rc != 0) {
			/* Non-candidate (resident/lpage) or transient failure: keep walking. */
			continue;
		}
		VSLM_PERF_INC(vslm, prefetch_page_attempt_total);
		submitted++;
		furthest = (uint64_t)target;
	}

	if (submitted > 0) {
		pthread_mutex_lock(&vslm->policy_lock);
		vslm->prefetch_until_vpn = furthest + 1;
		pthread_mutex_unlock(&vslm->policy_lock);
	}
}

static void
vslm_prefetch_next_pages(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
			 uint64_t demand_vpn, uint32_t readahead_pages, int64_t stride)
{
	uint64_t start_ticks;
	uint64_t elapsed_ns;
	uint64_t start_vpn;
	struct vslm_page *prefetch_page;
	struct vslm_lpage *lpage;
	uint64_t max_vpn;
	uint64_t vpn;
	int64_t target;
	uint32_t i;
	uint32_t requested_pages;
	int rc;

	start_ticks = spdk_get_ticks();
	VSLM_PERF_INC(vslm, prefetch_call_total);

	if (readahead_pages == 0) {
		goto out;
	}
	if (stride == 0 || stride > VSLM_PREFETCH_MAX_STRIDE_PAGES ||
	    stride < -VSLM_PREFETCH_MAX_STRIDE_PAGES) {
		goto out;
	}

	if (vslm->fault_batch_enabled) {
		if (!vslm->prefetch_batch_enabled) {
			goto out;
		}

		if (__atomic_load_n(&vslm->prefetch_inflight_batches, __ATOMIC_RELAXED) >=
		    vslm->prefetch_queue_depth) {
			goto out;
		}

		/* Non-unit stride: stage non-contiguous blocks along the trend. */
		if (stride != 1) {
			vslm_prefetch_strided_submit(vslm, demand_vpn, stride, readahead_pages);
			goto out;
		}

		start_vpn = demand_vpn + 1;
		if (vslm_range_covered_by_last_batch(vslm, start_vpn)) {
			/* last_batch_* are written lock-free under shard locks; load atomically. */
			start_vpn = __atomic_load_n(&vslm->last_batch_start_vpn, __ATOMIC_RELAXED) +
				    __atomic_load_n(&vslm->last_batch_pages, __ATOMIC_RELAXED);
		}

		requested_pages = spdk_min(readahead_pages, vslm->prefetch_batch_pages);
		requested_pages = vslm_clamp_batch_pages_to_range(vslm, start_vpn, requested_pages);
		if (requested_pages == 0) {
			goto out;
		}

		rc = vslm_prefetch_clean_backing_batch_submit(vslm, start_vpn, requested_pages);
		if (rc == 0) {
			VSLM_PERF_ADD(vslm, prefetch_page_attempt_total, requested_pages);
			pthread_mutex_lock(&vslm->policy_lock);
			vslm->prefetch_until_vpn = start_vpn + requested_pages;
			pthread_mutex_unlock(&vslm->policy_lock);
		}
		goto out;
	}

	max_vpn = vslm->virtual_size_bytes / VSLM_PAGE_SIZE;
	for (i = 1; i <= readahead_pages; i++) {
		struct vslm_mmu_shard *shard;
		bool no_free = false;
		bool skip = false;
		bool stop = false;

		target = (int64_t)demand_vpn + (int64_t)i * stride;
		if (target < 0 || (uint64_t)target >= max_vpn) {
			break;
		}
		vpn = (uint64_t)target;
		VSLM_PERF_INC(vslm, prefetch_page_attempt_total);

		/*
		 * The lookup/free-list test and the page reservation done by
		 * vslm_resolve_page_ex() all mutate the owning shard's hash/LRU/free
		 * lists and must run under that shard's lock. (resolve_page_ex drops
		 * the lock itself across the actual backing read via held_shard.)
		 */
		shard = vslm_shard_for_vpn(vslm, vpn);
		pthread_mutex_lock(&shard->lock);

		if (vslm_lookup_page(vslm, vpn) != NULL) {
			VSLM_PERF_INC(vslm, prefetch_page_skipped_resident_total);
			skip = true;
		} else {
			lpage = vslm_lookup_lpage(vslm, vpn);
			if (lpage != NULL && lpage->pending_publish) {
				VSLM_PERF_INC(vslm, prefetch_page_skipped_pending_publish_total);
				skip = true;
			} else if (TAILQ_FIRST(&shard->free_list) == NULL) {
				VSLM_PERF_INC(vslm, prefetch_page_skipped_no_free_page_total);
				no_free = true;
			}
		}

		if (skip) {
			pthread_mutex_unlock(&shard->lock);
			continue;
		}
		if (no_free) {
			pthread_mutex_unlock(&shard->lock);
			break;
		}

		rc = vslm_resolve_page_ex(vslm, base_ch, NULL, vpn, &prefetch_page, false, false);
		if (rc != 0 || prefetch_page == NULL) {
			stop = true;
		} else {
			if (vslm_lookup_lpage(vslm, vpn) == NULL) {
				VSLM_PERF_INC(vslm, prefetch_clean_default_total);
			}
			VSLM_PERF_INC(vslm, prefetch_page_loaded_total);
		}
		pthread_mutex_unlock(&shard->lock);

		if (stop) {
			break;
		}
	}

out:
	elapsed_ns = vslm_ticks_delta_to_ns(start_ticks, spdk_get_ticks());
	VSLM_PERF_ADD(vslm, prefetch_ns_total, elapsed_ns);
	vslm_perf_max_u64(&vslm->perf_stats.prefetch_ns_max, elapsed_ns);
}

static int
vslm_copy_range_exec_read_batched(struct vbdev_vslm *vslm,
				  struct spdk_io_channel *base_ch,
				  struct vslm_source_io *src,
				  uint64_t starting_byte,
				  uint32_t length,
				  uint8_t *buf,
				  bool readahead_enabled,
				  uint32_t readahead_pages)
{
	struct vslm_page *page;
	struct vslm_mmu_shard *shard;
	struct spdk_bdev *run_src_bdev = NULL;
	uint64_t run_src_off = 0;
	uint64_t offset = 0;
	uint64_t absolute;
	uint64_t vpn;
	uint64_t page_offset;
	uint64_t chunk;
	uint64_t lock_start_ticks;
	uint8_t *ptr;
	uint32_t run_len;
	uint32_t pages_loaded;
	bool demand_miss;
	bool page_pinned;
	bool issue_prefetch;
	uint64_t prefetch_vpn;
	int64_t prefetch_stride = 1;
	int rc;

	if (length == 0) {
		return 0;
	}

	if (starting_byte >= vslm->virtual_size_bytes ||
	    length > vslm->virtual_size_bytes - starting_byte) {
		return -EINVAL;
	}

	SPDK_DEBUGLOG(vslm, "vSLM batched exec-read start bdev=%s start=%" PRIu64 " len=%u\n",
		      vslm->vbdev.name, starting_byte, length);

	while (offset < length) {
		absolute = starting_byte + offset;
		vpn = absolute / VSLM_PAGE_SIZE;
		page_offset = absolute % VSLM_PAGE_SIZE;
		chunk = spdk_min((uint64_t)(VSLM_PAGE_SIZE - page_offset), (uint64_t)length - offset);
		shard = vslm_shard_for_vpn(vslm, vpn);
		run_len = 0;
		pages_loaded = 0;
		page_pinned = false;
		issue_prefetch = false;
		prefetch_vpn = 0;

		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		lock_start_ticks = spdk_get_ticks();
		page = vslm_lookup_page(vslm, vpn);
		demand_miss = (page == NULL);
		if (demand_miss && page_offset == 0 &&
		    ((uint64_t)length - offset) >= VSLM_PAGE_SIZE &&
		    vslm->fault_batch_enabled) {
			run_len = vslm_find_clean_batch_run(vslm, vpn, vslm->fault_batch_pages,
							    &run_src_bdev, &run_src_off);
		}
		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);

		if (demand_miss && run_len > 1) {
			rc = vslm_fault_in_clean_backing_batch_sync(vslm, base_ch, src, run_src_bdev,
					run_src_off, vpn, run_len, true, &pages_loaded);
			if (rc != 0 && rc != -ENOSPC) {
				SPDK_DEBUGLOG(vslm, "vSLM batched fault fallback bdev=%s vpn=%" PRIu64
					      " rc=%d\n", vslm->vbdev.name, vpn, rc);
			}
			if (pages_loaded > 0) {
				prefetch_vpn = vpn + pages_loaded - 1;
			}
		}

		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		lock_start_ticks = spdk_get_ticks();

		page = vslm_lookup_page(vslm, vpn);
		if (page == NULL) {
			rc = vslm_resolve_and_pin_page_locked(vslm, base_ch, src, vpn, false, false, &page);
			if (rc != 0) {
				vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
				return rc;
			}
			page_pinned = true;
			demand_miss = true;
		} else if (page->load_state == VSLM_PAGE_LOAD_LOADING ||
			   page->load_state == VSLM_PAGE_LOAD_EVICTING) {
			rc = vslm_wait_for_loading_page(vslm, page);
			if (rc != 0) {
				vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
				return rc;
			}
		}

		if (page->load_state != VSLM_PAGE_LOAD_RESIDENT) {
			rc = page->load_status != 0 ? page->load_status : -EIO;
			vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
			return rc;
		}
		if (!page_pinned) {
			vslm_page_pin_locked(vslm, page);
			page_pinned = true;
		}

		ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset;
		vslm_timed_memcpy(vslm, buf + offset, ptr, chunk);
		if (page_pinned) {
			vslm_page_unpin_locked(vslm, page, false, false);
		}

		if (readahead_enabled && demand_miss &&
		    vslm_prefetch_policy_should_submit(vslm, vpn, &prefetch_stride)) {
			issue_prefetch = true;
			if (pages_loaded == 0) {
				prefetch_vpn = vpn;
			}
		}

		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
		if (issue_prefetch) {
			vslm_prefetch_next_pages(vslm, base_ch, prefetch_vpn, readahead_pages,
						 prefetch_stride);
		}
		offset += chunk;
	}

	SPDK_DEBUGLOG(vslm, "vSLM batched exec-read done bdev=%s start=%" PRIu64 " len=%u\n",
		      vslm->vbdev.name, starting_byte, length);
	return 0;
}

static int
vslm_stream_tile_drop_clean_locked(struct vbdev_vslm *vslm,
				   uint64_t start_vpn,
				   uint32_t nr_pages)
{
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	uint32_t i;
	uint64_t vpn;

	for (i = 0; i < nr_pages; i++) {
		vpn = start_vpn + i;
		page = vslm_lookup_page(vslm, vpn);
		if (page == NULL) {
			continue;
		}
		lpage = vslm_lookup_lpage(vslm, vpn);
		if (lpage != NULL || page->dirty || page->pin_count != 0) {
			return -EINVAL;
		}
		if (page->load_state != VSLM_PAGE_LOAD_RESIDENT) {
			return -EIO;
		}

		if (page->on_hash) {
			vslm_hash_remove(vslm, page);
		}
		if (page->location == VSLM_PAGE_LOC_LRU) {
			vslm_lru_remove(vslm, page);
		}
		vslm_page_prepare_free(page);
		vslm_free_list_insert_tail(vslm, page);
		vslm_note_resident_remove(vslm);
	}

	return 0;
}

static int
vslm_copy_range_exec_read_streaming(struct vbdev_vslm *vslm,
				    struct spdk_io_channel *base_ch,
				    struct vslm_source_io *src,
				    uint64_t starting_byte,
				    uint32_t length,
				    uint8_t *buf,
				    uint32_t tile_pages)
{
	struct vslm_page *page;
	struct vslm_mmu_shard *shard;
	uint64_t lock_start_ticks = 0;
	uint64_t offset = 0;
	uint64_t tile_start_byte;
	uint64_t start_vpn;
	uint64_t tile_bytes;
	uint64_t ptr_offset;
	uint8_t *ptr;
	uint32_t requested_pages;
	uint32_t run_len;
	uint32_t pages_loaded;
	uint32_t i;
	struct spdk_bdev *run_src_bdev = NULL;
	uint64_t run_src_off = 0;
	int rc;

	if ((starting_byte % VSLM_PAGE_SIZE) != 0 ||
	    (length % VSLM_PAGE_SIZE) != 0 ||
	    tile_pages == 0) {
		return -ENOTSUP;
	}

	while (offset < length) {
		tile_start_byte = starting_byte + offset;
		start_vpn = tile_start_byte / VSLM_PAGE_SIZE;
		requested_pages = (uint32_t)spdk_min((uint64_t)tile_pages,
						     ((uint64_t)length - offset) / VSLM_PAGE_SIZE);
		/* fault_in_clean_backing_batch_sync clamps pages_loaded to one shard. */
		shard = vslm_shard_for_vpn(vslm, start_vpn);

		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		lock_start_ticks = spdk_get_ticks();
		run_len = vslm_find_clean_batch_run(vslm, start_vpn, requested_pages,
						    &run_src_bdev, &run_src_off);
		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
		if (run_len == 0) {
			return -ENOTSUP;
		}

		rc = vslm_fault_in_clean_backing_batch_sync(vslm, base_ch, src, run_src_bdev,
				run_src_off, start_vpn, run_len, true, &pages_loaded);
		if (rc != 0) {
			return rc;
		}
		if (pages_loaded == 0) {
			return -ENOTSUP;
		}

		tile_bytes = (uint64_t)pages_loaded * VSLM_PAGE_SIZE;
		for (i = 0; i < pages_loaded; i++) {
			VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
			pthread_mutex_lock(&shard->lock);
			lock_start_ticks = spdk_get_ticks();
			rc = vslm_resolve_and_pin_page_locked(vslm, base_ch, src, start_vpn + i,
							      false, false, &page);
			if (rc != 0) {
				vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
				return rc;
			}
			ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE);
			vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);

			ptr_offset = ((uint64_t)i) * VSLM_PAGE_SIZE;
			vslm_timed_memcpy(vslm, buf + offset + ptr_offset, ptr, VSLM_PAGE_SIZE);

			VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
			pthread_mutex_lock(&shard->lock);
			lock_start_ticks = spdk_get_ticks();
			vslm_page_unpin_locked(vslm, page, false, false);
			vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
		}

		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		lock_start_ticks = spdk_get_ticks();
		rc = vslm_stream_tile_drop_clean_locked(vslm, start_vpn, pages_loaded);
		vslm_debug_validate_page_lists(vslm);
		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
		if (rc != 0) {
			return rc;
		}

		offset += tile_bytes;
	}

	return 0;
}

static int
vslm_copy_range_ex(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		   uint64_t starting_byte, uint32_t length, uint8_t *buf,
		   bool is_write, bool exec_read)
{
	struct vslm_page *page;
	struct vslm_mmu_shard *shard;
	uint64_t lock_start_ticks = 0;
	uint64_t offset = 0;
	uint64_t absolute;
	uint64_t vpn;
	uint64_t page_offset;
	uint64_t chunk;
	uint8_t *ptr;
	bool demand_miss;
	bool full_page_overwrite;
	uint64_t demand_miss_count = 0;
	uint64_t overwrite_miss_count = 0;
	bool readahead_enabled = false;
	bool fault_batch_enabled = false;
	bool streaming_mode_enabled = false;
	uint32_t readahead_pages = 0;
	uint32_t streaming_tile_pages = 0;
	struct vslm_source_io src = {0};
	int rc = 0;

	if (exec_read) {
		pthread_mutex_lock(&vslm->policy_lock);
		readahead_enabled = vslm->policy.readahead_enabled;
		readahead_pages = vslm->policy.readahead_pages;
		fault_batch_enabled = vslm->fault_batch_enabled;
		streaming_mode_enabled = vslm->streaming_mode_enabled;
		streaming_tile_pages = vslm->streaming_tile_pages;
		pthread_mutex_unlock(&vslm->policy_lock);
	}

	if (length == 0) {
		return 0;
	}

	if (starting_byte >= vslm->virtual_size_bytes ||
	    length > vslm->virtual_size_bytes - starting_byte) {
		return -EINVAL;
	}
	SPDK_DEBUGLOG(vslm, "vSLM copy-range start bdev=%s %s start=%" PRIu64
		      " len=%u exec_read=%d\n", vslm->vbdev.name, is_write ? "WRITE" : "READ",
		      starting_byte, length, exec_read);

	if (exec_read && !is_write && streaming_mode_enabled) {
		rc = vslm_copy_range_exec_read_streaming(vslm, base_ch, &src, starting_byte, length, buf,
				streaming_tile_pages);
		if (rc == 0 || rc != -ENOTSUP) {
			goto out;
		}
	}

	if (exec_read && !is_write && fault_batch_enabled) {
		rc = vslm_copy_range_exec_read_batched(vslm, base_ch, &src, starting_byte, length, buf,
				readahead_enabled, readahead_pages);
		goto out;
	}

	while (offset < length) {
		bool issue_prefetch = false;
		uint64_t prefetch_vpn = 0;
		int64_t prefetch_stride = 1;

		absolute = starting_byte + offset;
		vpn = absolute / VSLM_PAGE_SIZE;
		page_offset = absolute % VSLM_PAGE_SIZE;
		chunk = spdk_min((uint64_t)(VSLM_PAGE_SIZE - page_offset), (uint64_t)length - offset);
		shard = vslm_shard_for_vpn(vslm, vpn);

		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		lock_start_ticks = spdk_get_ticks();

		demand_miss = (vslm_lookup_page(vslm, vpn) == NULL);
		full_page_overwrite = is_write && page_offset == 0 && chunk == VSLM_PAGE_SIZE;
		if (demand_miss) {
			demand_miss_count++;
			if (full_page_overwrite) {
				overwrite_miss_count++;
			}
		}

		rc = vslm_resolve_and_pin_page_locked(vslm, base_ch, &src, vpn, is_write,
						      full_page_overwrite, &page);
		if (rc != 0) {
			vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
			SPDK_DEBUGLOG(vslm, "vSLM copy-range failed bdev=%s vpn=%" PRIu64 " rc=%d\n",
				      vslm->vbdev.name, vpn, rc);
			goto out;
		}

		ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset;
		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);

		if (is_write) {
			vslm_timed_memcpy(vslm, ptr, buf + offset, chunk);
		} else {
			vslm_timed_memcpy(vslm, buf + offset, ptr, chunk);
		}

		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		lock_start_ticks = spdk_get_ticks();

		vslm_page_unpin_locked(vslm, page, is_write, false);
		if (exec_read && readahead_enabled && demand_miss &&
		    vslm_prefetch_policy_should_submit(vslm, vpn, &prefetch_stride)) {
			issue_prefetch = true;
			prefetch_vpn = vpn;
		}
		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);

		if (issue_prefetch) {
			vslm_prefetch_next_pages(vslm, base_ch, prefetch_vpn, readahead_pages,
						 prefetch_stride);
		}

		offset += chunk;
	}

	SPDK_DEBUGLOG(vslm, "vSLM copy-range done bdev=%s %s start=%" PRIu64 " len=%u"
		      " misses=%" PRIu64 " overwrite_misses=%" PRIu64 "\n",
		      vslm->vbdev.name, is_write ? "WRITE" : "READ", starting_byte, length,
		      demand_miss_count, overwrite_miss_count);
	(void)demand_miss_count;
	(void)overwrite_miss_count;
	rc = 0;
out:
	vslm_source_io_release(&src);
	return rc;
}

static int
vslm_stage_alias_range(struct vbdev_vslm *vslm, uint64_t dst_offset,
		       uint64_t length, struct spdk_bdev *src_bdev,
		       uint64_t src_offset)
{
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	uint64_t page_count;
	uint64_t i;
	uint64_t vpn;

	if ((dst_offset % VSLM_PAGE_SIZE) != 0 ||
	    (src_offset % VSLM_PAGE_SIZE) != 0 ||
	    (length % VSLM_PAGE_SIZE) != 0) {
		return -EINVAL;
	}

	page_count = length / VSLM_PAGE_SIZE;
	SPDK_DEBUGLOG(vslm, "vSLM alias-stage start bdev=%s dst_off=%" PRIu64 " src_bdev=%s"
		      " src_off=%" PRIu64 " len=%" PRIu64 " pages=%" PRIu64 "\n",
		      vslm->vbdev.name, dst_offset, src_bdev->name, src_offset, length, page_count);

	for (i = 0; i < page_count; i++) {
		struct vslm_mmu_shard *shard;

		vpn = (dst_offset / VSLM_PAGE_SIZE) + i;
		shard = vslm_shard_for_vpn(vslm, vpn);
		pthread_mutex_lock(&shard->lock);
		page = vslm_lookup_page(vslm, vpn);
		if (page != NULL) {
			vslm_hash_remove(vslm, page);
			if (page->location == VSLM_PAGE_LOC_LRU) {
				vslm_lru_remove(vslm, page);
			}
			vslm_page_prepare_free(page);
			vslm_free_list_insert_tail(vslm, page);
			vslm_note_resident_remove(vslm);
		}

		lpage = vslm_lookup_lpage(vslm, vpn);
		if (lpage == NULL) {
			lpage = vslm_lpage_alloc_alias(vslm, vpn, src_bdev,
						       src_offset + (i * VSLM_PAGE_SIZE));
			if (lpage == NULL) {
				pthread_mutex_unlock(&shard->lock);
				return -ENOMEM;
			}
		} else {
			lpage->source_type = SPDK_BDEV_VSLM_PAGE_SOURCE_ALIAS_BDEV;
			lpage->source_bdev = src_bdev;
			lpage->source_offset_bytes = src_offset + (i * VSLM_PAGE_SIZE);
			lpage->pending_publish = false;
			lpage->private_authoritative = false;
			vslm_lpage_clear_superseded_source(lpage);
			vslm_lpage_set_state(vslm, NULL, lpage, SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS);
		}
		pthread_mutex_unlock(&shard->lock);
	}
	SPDK_DEBUGLOG(vslm, "vSLM alias-stage done bdev=%s pages=%" PRIu64 "\n",
		      vslm->vbdev.name, page_count);

	return 0;
}

static int
vslm_copy_range(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		uint64_t starting_byte, uint32_t length, uint8_t *buf, bool is_write)
{
	return vslm_copy_range_ex(vslm, base_ch, starting_byte, length, buf, is_write, false);
}

static int
vslm_fill_range(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
		uint64_t starting_byte, uint32_t length)
{
	struct vslm_page *page;
	uint64_t offset = 0;
	uint64_t absolute;
	uint64_t vpn;
	uint64_t page_offset;
	uint64_t chunk;
	uint8_t *ptr;
	int rc;

	if (length == 0) {
		return 0;
	}

	if (starting_byte >= vslm->virtual_size_bytes ||
	    length > vslm->virtual_size_bytes - starting_byte) {
		return -EINVAL;
	}

	while (offset < length) {
		struct vslm_mmu_shard *shard;

		absolute = starting_byte + offset;
		vpn = absolute / VSLM_PAGE_SIZE;
		page_offset = absolute % VSLM_PAGE_SIZE;
		chunk = spdk_min((uint64_t)(VSLM_PAGE_SIZE - page_offset), (uint64_t)length - offset);
		shard = vslm_shard_for_vpn(vslm, vpn);

		pthread_mutex_lock(&shard->lock);
		rc = vslm_resolve_page(vslm, base_ch, vpn, &page);
		if (rc != 0) {
			pthread_mutex_unlock(&shard->lock);
			return rc;
		}

		ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset;
		memset(ptr, 0, chunk);
		vslm_page_mark_dirty(vslm, page);
		pthread_mutex_unlock(&shard->lock);
		offset += chunk;
	}

	return 0;
}

static int
vbdev_vslm_destruct(void *ctx)
{
	struct vbdev_vslm *vslm = ctx;
	struct vslm_lease *lease, *lease_tmp;
	struct vslm_blocked_cmd *blocked, *blocked_tmp;

	pthread_mutex_lock(&g_vslm_bdevs_lock);
	TAILQ_REMOVE(&g_vslm_bdevs, vslm, link);
	pthread_mutex_unlock(&g_vslm_bdevs_lock);
	pthread_mutex_lock(&vslm->inflight_lock);
	vslm->destructing = true;
	while (vslm->inflight_io_count != 0) {
		pthread_cond_wait(&vslm->inflight_cond, &vslm->inflight_lock);
	}
	pthread_mutex_unlock(&vslm->inflight_lock);

	spdk_io_device_unregister(vslm, NULL);
	if (vslm->base_desc) {
		spdk_bdev_close(vslm->base_desc);
	}

	pthread_mutex_lock(&vslm->lease_lock);
	TAILQ_FOREACH_SAFE(lease, &vslm->leases, link, lease_tmp) {
		TAILQ_REMOVE(&vslm->leases, lease, link);
		free(lease);
	}
	TAILQ_FOREACH_SAFE(blocked, &vslm->blocked_cmds, link, blocked_tmp) {
		TAILQ_REMOVE(&vslm->blocked_cmds, blocked, link);
		spdk_bdev_io_complete(blocked->bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		free(blocked);
	}
	pthread_mutex_unlock(&vslm->lease_lock);

	vslm_debug_validate_page_lists(vslm);
	pthread_mutex_destroy(&vslm->lease_lock);
	pthread_mutex_destroy(&vslm->policy_lock);
	pthread_mutex_destroy(&vslm->trace_lock);
	pthread_cond_destroy(&vslm->inflight_cond);
	pthread_mutex_destroy(&vslm->inflight_lock);

	vslm_lpage_free_all(vslm);
	spdk_dma_free(vslm->sram_buffer);
	free(vslm->page_array);
	vslm_free_shards(vslm);
	vslm_free_dma_ring_pool(vslm);
	if (vslm->trace_fp != NULL) {
		fclose(vslm->trace_fp);
	}
	free(vslm->trace_output_path);
	free(vslm->trace_run_id);
	free(vslm->base_bdev_name);
	free(vslm->vbdev.name);
	free(vslm);
	return 0;
}

void
bdev_vslm_opts_init(struct spdk_bdev_vslm_opts *opts)
{
	if (opts == NULL) {
		return;
	}

	memset(opts, 0, sizeof(*opts));
	opts->policy.semantics_mode = SPDK_BDEV_VSLM_SEMANTICS_LEASE;
	opts->policy.writeback_policy = SPDK_BDEV_VSLM_WRITEBACK_ON_EVICT;
	opts->policy.admission_enabled = false;
	opts->policy.admission_faults_per_sec_threshold = 0;
	opts->policy.readahead_enabled = false;
	opts->policy.readahead_pages = VSLM_DEFAULT_READAHEAD_PAGES;
	opts->fdp_mode_enabled = false;
	opts->fdp_dspec = 0;
	opts->backing_region_enabled = false;
	opts->trace_events_enabled = false;
	opts->trace_output_path = NULL;
	opts->trace_run_id = NULL;
}

static int
vslm_validate_policy(const struct spdk_bdev_vslm_policy *policy)
{
	if (policy == NULL) {
		return -EINVAL;
	}

	if (policy->semantics_mode != SPDK_BDEV_VSLM_SEMANTICS_LEASE) {
		return -ENOTSUP;
	}

	if (policy->writeback_policy != SPDK_BDEV_VSLM_WRITEBACK_ON_EVICT) {
		return -EINVAL;
	}

	if (policy->admission_enabled &&
	    policy->admission_faults_per_sec_threshold == 0) {
		return -EINVAL;
	}

	if (policy->readahead_enabled &&
	    (policy->readahead_pages == 0 ||
	     policy->readahead_pages > VSLM_MAX_READAHEAD_PAGES)) {
		return -EINVAL;
	}

	return 0;
}

static int
vslm_apply_policy(struct vbdev_vslm *vslm, const struct spdk_bdev_vslm_policy *policy)
{
	int rc;

	rc = vslm_validate_policy(policy);
	if (rc != 0) {
		return rc;
	}

	pthread_mutex_lock(&vslm->policy_lock);
	vslm->policy = *policy;
	if (!vslm->policy.admission_enabled) {
		vslm->admission_window_start_ticks = 0;
		vslm->admission_faults_in_window = 0;
	}
	pthread_mutex_unlock(&vslm->policy_lock);
	return 0;
}

static int
vslm_init_internal_batch_policy(struct vbdev_vslm *vslm)
{
	vslm->fault_batch_enabled = true;
	vslm->fault_batch_pages = VSLM_DEFAULT_FAULT_BATCH_PAGES;
	vslm->fault_batch_max_bytes = VSLM_DEFAULT_FAULT_BATCH_MAX_BYTES;
	vslm->prefetch_batch_enabled = false;
	vslm->prefetch_batch_pages = VSLM_DEFAULT_PREFETCH_BATCH_PAGES;
	vslm->prefetch_queue_depth = VSLM_DEFAULT_PREFETCH_QUEUE_DEPTH;

	if (vslm->fault_batch_enabled) {
		/*
		 * A batch is clamped to one shard stride, so that is the natural upper
		 * bound on both the page count and the byte size of a single coalesced
		 * fault-in readv (the on-stack scratch arrays are sized to it).
		 */
		if (vslm->fault_batch_pages == 0 ||
		    vslm->fault_batch_pages > VSLM_MMU_SHARD_STRIDE_PAGES) {
			return -EINVAL;
		}

		if (vslm->fault_batch_max_bytes == 0 ||
		    vslm->fault_batch_max_bytes >
		    (VSLM_MMU_SHARD_STRIDE_PAGES * (uint32_t)VSLM_PAGE_SIZE)) {
			return -EINVAL;
		}

		if (vslm->fault_batch_max_bytes <
		    vslm->fault_batch_pages * VSLM_PAGE_SIZE) {
			return -EINVAL;
		}
	}

	return 0;
}

static int
vslm_init_internal_cleaner_policy(struct vbdev_vslm *vslm)
{
	uint32_t page_count;
	uint32_t low;
	uint32_t high;

	page_count = vslm->num_sram_pages > UINT32_MAX ?
		     UINT32_MAX : (uint32_t)vslm->num_sram_pages;
	if (page_count == 0) {
		return -EINVAL;
	}

	low = page_count / 32;
	if (low == 0) {
		low = 1;
	}
	if (low < 16 && page_count >= 16) {
		low = 16;
	}

	high = page_count / 16;
	if (high < low) {
		high = low;
	}
	if (high < low + 16 && page_count > high) {
		high = spdk_min(page_count, low + 16);
	}

	vslm->background_cleaner_enabled = false;
	vslm->clean_free_low_watermark_pages = low;
	vslm->clean_free_high_watermark_pages = high;
	vslm->cleaner_max_pages_per_poll = VSLM_DEFAULT_CLEANER_MAX_PAGES_PER_POLL;

	if (vslm->clean_free_low_watermark_pages == 0 ||
	    vslm->clean_free_high_watermark_pages == 0 ||
	    vslm->cleaner_max_pages_per_poll == 0) {
		return -EINVAL;
	}
	if (vslm->clean_free_low_watermark_pages >
	    vslm->clean_free_high_watermark_pages) {
		return -EINVAL;
	}
	if (vslm->clean_free_high_watermark_pages > page_count) {
		return -EINVAL;
	}

	return 0;
}

static int
vslm_init_internal_streaming_policy(struct vbdev_vslm *vslm)
{
	uint32_t page_count;

	page_count = vslm->num_sram_pages > UINT32_MAX ?
		     UINT32_MAX : (uint32_t)vslm->num_sram_pages;
	if (page_count == 0) {
		return -EINVAL;
	}

	vslm->streaming_mode_enabled = false;
	vslm->streaming_tile_pages = page_count;
	vslm->streaming_io_pages = spdk_min(page_count, VSLM_DEFAULT_STREAMING_IO_PAGES);

	if (vslm->streaming_tile_pages == 0 ||
	    vslm->streaming_io_pages == 0 ||
	    vslm->streaming_tile_pages > page_count ||
	    vslm->streaming_io_pages > vslm->streaming_tile_pages) {
		return -EINVAL;
	}

	return 0;
}

static int
vslm_create_common(const char *bdev_name, const char *base_bdev_name,
		   uint64_t sram_size_bytes, const struct spdk_bdev_vslm_opts *opts)
{
	struct spdk_bdev_vslm_opts default_opts;
	const struct spdk_bdev_vslm_opts *create_opts;
	struct vbdev_vslm *vslm;
	struct vbdev_vslm *iter;
	bool io_device_registered = false;
	bool lease_lock_initialized = false;
	bool policy_lock_initialized = false;
	bool trace_lock_initialized = false;
	bool inflight_lock_initialized = false;
	bool inflight_cond_initialized = false;
	uint32_t base_block_size;
	int rc;

	if (opts == NULL) {
		bdev_vslm_opts_init(&default_opts);
		create_opts = &default_opts;
	} else {
		create_opts = opts;
	}

	rc = vslm_validate_policy(&create_opts->policy);
	if (rc != 0) {
		return rc;
	}

	if (!bdev_name || !base_bdev_name || sram_size_bytes == 0) {
		SPDK_ERRLOG("Invalid vSLM parameters\n");
		return -EINVAL;
	}

	if (sram_size_bytes < VSLM_PAGE_SIZE || (sram_size_bytes % VSLM_PAGE_SIZE) != 0) {
		SPDK_ERRLOG("sram_size_bytes must be page-aligned and >= %u bytes\n",
			    VSLM_PAGE_SIZE);
		return -EINVAL;
	}

	pthread_mutex_lock(&g_vslm_bdevs_lock);
	TAILQ_FOREACH(iter, &g_vslm_bdevs, link) {
		if (strcmp(iter->vbdev.name, bdev_name) == 0) {
			pthread_mutex_unlock(&g_vslm_bdevs_lock);
			SPDK_ERRLOG("vSLM bdev '%s' already exists\n", bdev_name);
			return -EEXIST;
		}
		if (create_opts->nsid != 0 && iter->vbdev.nsid == create_opts->nsid) {
			pthread_mutex_unlock(&g_vslm_bdevs_lock);
			SPDK_ERRLOG("vSLM nsid %u already in use by bdev '%s'\n",
				    create_opts->nsid, iter->vbdev.name);
			return -EEXIST;
		}
	}
	pthread_mutex_unlock(&g_vslm_bdevs_lock);

	vslm = calloc(1, sizeof(*vslm));
	if (!vslm) {
		return -ENOMEM;
	}
	vslm->last_exec_read_vpn = UINT64_MAX;
	vslm->sequential_exec_read_count = 0;
	vslm->last_exec_read_stride = 0;
	vslm->strided_exec_read_count = 0;
	vslm->prefetch_until_vpn = 0;
	vslm->last_batch_start_vpn = UINT64_MAX;
	vslm->last_batch_pages = 0;

	rc = pthread_mutex_init(&vslm->lease_lock, NULL);
	if (rc != 0) {
		free(vslm);
		return -rc;
	}
	lease_lock_initialized = true;
	TAILQ_INIT(&vslm->leases);
	TAILQ_INIT(&vslm->blocked_cmds);

	/* Per-shard MMU locks are created later in vslm_init_mmu(). */

	rc = pthread_mutex_init(&vslm->policy_lock, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&vslm->lease_lock);
		free(vslm);
		return -rc;
	}
	policy_lock_initialized = true;
	vslm->admission_window_start_ticks = 0;
	vslm->admission_faults_in_window = 0;

	rc = pthread_mutex_init(&vslm->trace_lock, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&vslm->policy_lock);
		pthread_mutex_destroy(&vslm->lease_lock);
		free(vslm);
		return -rc;
	}
	trace_lock_initialized = true;

	vslm->sram_size_bytes = sram_size_bytes;
	vslm->base_bdev_name = strdup(base_bdev_name);
	if (!vslm->base_bdev_name) {
		pthread_mutex_destroy(&vslm->trace_lock);
		pthread_mutex_destroy(&vslm->policy_lock);
		pthread_mutex_destroy(&vslm->lease_lock);
		free(vslm);
		return -ENOMEM;
	}

	rc = pthread_mutex_init(&vslm->inflight_lock, NULL);
	if (rc != 0) {
		free(vslm->base_bdev_name);
		pthread_mutex_destroy(&vslm->trace_lock);
		pthread_mutex_destroy(&vslm->policy_lock);
		pthread_mutex_destroy(&vslm->lease_lock);
		free(vslm);
		return -rc;
	}
	inflight_lock_initialized = true;

	rc = pthread_cond_init(&vslm->inflight_cond, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&vslm->inflight_lock);
		free(vslm->base_bdev_name);
		pthread_mutex_destroy(&vslm->trace_lock);
		pthread_mutex_destroy(&vslm->policy_lock);
		pthread_mutex_destroy(&vslm->lease_lock);
		free(vslm);
		return -rc;
	}
	inflight_cond_initialized = true;
	vslm->inflight_io_count = 0;
	vslm->destructing = false;

	rc = spdk_bdev_open_ext(base_bdev_name, true, vbdev_vslm_bdev_event_cb, vslm, &vslm->base_desc);
	if (rc != 0) {
		goto err;
	}

	vslm->base_bdev = spdk_bdev_desc_get_bdev(vslm->base_desc);
	base_block_size = spdk_bdev_get_block_size(vslm->base_bdev);
	vslm->virtual_size_bytes = spdk_bdev_get_num_blocks(vslm->base_bdev) * (uint64_t)base_block_size;
	vslm->base_fdp_supported = spdk_bdev_get_nvme_ctratt(vslm->base_bdev).bits.fdps;

	/*
	 * Isolated backing tier: reserve the upper half of the base namespace for
	 * spilled private pages so the clean source data (lower half) is never
	 * overwritten by eviction. The exposed virtual size is the lower half.
	 */
	vslm->backing_region_enabled = create_opts->backing_region_enabled;
	if (vslm->backing_region_enabled) {
		uint64_t half = (vslm->virtual_size_bytes / 2) & ~((uint64_t)VSLM_PAGE_SIZE - 1);

		if (half < VSLM_PAGE_SIZE) {
			SPDK_ERRLOG("vSLM backing region requires base bdev >= 2 pages\n");
			rc = -EINVAL;
			goto err;
		}
		vslm->virtual_size_bytes = half;
		vslm->backing_region_offset_bytes = half;
	} else {
		vslm->backing_region_offset_bytes = 0;
	}
	if ((VSLM_PAGE_SIZE % base_block_size) != 0 ||
	    (sram_size_bytes % base_block_size) != 0) {
		SPDK_ERRLOG("vSLM requires block/page alignment: block_size=%u page_size=%u sram=%" PRIu64 "\n",
			    base_block_size, VSLM_PAGE_SIZE, sram_size_bytes);
		rc = -EINVAL;
		goto err;
	}
	if (vslm->virtual_size_bytes < sram_size_bytes) {
		SPDK_ERRLOG("vSLM backing size must be >= SRAM size: backing=%" PRIu64 " sram=%" PRIu64 "\n",
			    vslm->virtual_size_bytes, sram_size_bytes);
		rc = -EINVAL;
		goto err;
	}
	if ((vslm->virtual_size_bytes % VSLM_PAGE_SIZE) != 0) {
		SPDK_ERRLOG("vSLM backing size must be page-aligned: backing=%" PRIu64 " page=%u\n",
			    vslm->virtual_size_bytes, VSLM_PAGE_SIZE);
		rc = -EINVAL;
		goto err;
	}

	rc = vslm_init_mmu(vslm);
	if (rc != 0) {
		goto err;
	}

	vslm->vbdev.name = strdup(bdev_name);
	if (!vslm->vbdev.name) {
		rc = -ENOMEM;
		goto err;
	}

	rc = vslm_apply_policy(vslm, &create_opts->policy);
	if (rc != 0) {
		goto err;
	}

	rc = vslm_init_internal_batch_policy(vslm);
	if (rc != 0) {
		goto err;
	}
	rc = vslm_init_internal_cleaner_policy(vslm);
	if (rc != 0) {
		goto err;
	}
	rc = vslm_init_internal_streaming_policy(vslm);
	if (rc != 0) {
		goto err;
	}

	if (create_opts->fdp_mode_enabled && !vslm->base_fdp_supported) {
		rc = -ENOTSUP;
		goto err;
	}
	vslm->fdp_mode_enabled = create_opts->fdp_mode_enabled;
	vslm->fdp_dspec = create_opts->fdp_dspec;
	vslm->trace_events_enabled = create_opts->trace_events_enabled;
	if (create_opts->trace_output_path != NULL && create_opts->trace_output_path[0] != '\0') {
		vslm->trace_output_path = strdup(create_opts->trace_output_path);
		if (vslm->trace_output_path == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}
	if (create_opts->trace_run_id != NULL && create_opts->trace_run_id[0] != '\0') {
		vslm->trace_run_id = strdup(create_opts->trace_run_id);
		if (vslm->trace_run_id == NULL) {
			rc = -ENOMEM;
			goto err;
		}
	}
	if (vslm->trace_events_enabled) {
		if (vslm->trace_output_path == NULL) {
			SPDK_ERRLOG("vSLM trace_events_enabled requires trace_output_path\n");
			rc = -EINVAL;
			goto err;
		}

		vslm->trace_fp = fopen(vslm->trace_output_path, "a");
		if (vslm->trace_fp == NULL) {
			rc = -errno;
			if (rc == 0) {
				rc = -EIO;
			}
			SPDK_ERRLOG("vSLM failed to open trace_output_path=%s rc=%d\n",
				    vslm->trace_output_path, rc);
			goto err;
		}
		setvbuf(vslm->trace_fp, NULL, _IOLBF, 0);
	}

	vslm->vbdev.product_name = "vSLM Device";
	vslm->vbdev.write_cache = 0;
	vslm->vbdev.blocklen = base_block_size;
	vslm->vbdev.blockcnt = vslm->virtual_size_bytes / base_block_size;
	vslm->vbdev.ctxt = vslm;
	vslm->vbdev.fn_table = &vbdev_vslm_fn_table;
	vslm->vbdev.module = &vslm_if;
	vslm->vbdev.slm = true;
	vslm->vbdev.nsid = create_opts->nsid;

	spdk_io_device_register(vslm, vbdev_vslm_ch_create_cb, vbdev_vslm_ch_destroy_cb,
				sizeof(struct vslm_channel), vslm->vbdev.name);
	io_device_registered = true;

	rc = spdk_bdev_register(&vslm->vbdev);
	if (rc != 0) {
		goto err;
	}

	pthread_mutex_lock(&g_vslm_bdevs_lock);
	TAILQ_INSERT_TAIL(&g_vslm_bdevs, vslm, link);
	pthread_mutex_unlock(&g_vslm_bdevs_lock);
	return 0;

err:
	if (vslm->base_desc) {
		spdk_bdev_close(vslm->base_desc);
	}
	if (io_device_registered) {
		spdk_io_device_unregister(vslm, NULL);
	}
	vslm_lpage_free_all(vslm);
	spdk_dma_free(vslm->sram_buffer);
	free(vslm->page_array);
	vslm_free_shards(vslm);
	vslm_free_dma_ring_pool(vslm);
	if (vslm->trace_fp != NULL) {
		fclose(vslm->trace_fp);
	}
	free(vslm->trace_output_path);
	free(vslm->trace_run_id);
	free(vslm->base_bdev_name);
	free(vslm->vbdev.name);
	if (policy_lock_initialized) {
		pthread_mutex_destroy(&vslm->policy_lock);
	}
	if (trace_lock_initialized) {
		pthread_mutex_destroy(&vslm->trace_lock);
	}
	if (inflight_cond_initialized) {
		pthread_cond_destroy(&vslm->inflight_cond);
	}
	if (inflight_lock_initialized) {
		pthread_mutex_destroy(&vslm->inflight_lock);
	}
	if (lease_lock_initialized) {
		pthread_mutex_destroy(&vslm->lease_lock);
	}
	free(vslm);
	return rc;
}

int
bdev_vslm_create(const char *bdev_name, const char *base_bdev_name,
		 uint64_t sram_size_bytes)
{
	return vslm_create_common(bdev_name, base_bdev_name, sram_size_bytes, NULL);
}

int
bdev_vslm_create_with_opts(const char *bdev_name, const char *base_bdev_name,
			   uint64_t sram_size_bytes,
			   const struct spdk_bdev_vslm_opts *opts)
{
	return vslm_create_common(bdev_name, base_bdev_name, sram_size_bytes, opts);
}

void
bdev_vslm_delete(const char *name,
		 spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct vbdev_vslm *vslm;

	vslm = vbdev_vslm_get_by_name(name);
	if (vslm != NULL) {
		spdk_bdev_unregister(&vslm->vbdev, cb_fn, cb_arg);
		return;
	}

	if (cb_fn) {
		cb_fn(cb_arg, -ENOENT);
	}
}

int
bdev_vslm_set_nsid(const char *name, uint32_t nsid)
{
	struct vbdev_vslm *vslm;
	struct vbdev_vslm *target = NULL;
	int rc = -ENOENT;

	if (!name || nsid == 0) {
		return -EINVAL;
	}

	pthread_mutex_lock(&g_vslm_bdevs_lock);

	/* Reject if another bdev already owns this nsid. */
	TAILQ_FOREACH(vslm, &g_vslm_bdevs, link) {
		if (vslm->vbdev.nsid == nsid && strcmp(vslm->vbdev.name, name) != 0) {
			pthread_mutex_unlock(&g_vslm_bdevs_lock);
			SPDK_ERRLOG("vSLM nsid %u already in use by bdev '%s'\n",
				    nsid, vslm->vbdev.name);
			return -EEXIST;
		}
		if (strcmp(vslm->vbdev.name, name) == 0) {
			target = vslm;
		}
	}

	if (target != NULL) {
		target->vbdev.nsid = nsid;
		rc = 0;
	}
	pthread_mutex_unlock(&g_vslm_bdevs_lock);

	return rc;
}

int
bdev_vslm_get_buffer_ptr_by_nsid(uint32_t nsid, uint64_t offset,
				 uint64_t length, void **ptr)
{
	struct vbdev_vslm *vslm;

	if (!ptr || nsid == 0) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_nsid(nsid);
	if (vslm == NULL) {
		SPDK_ERRLOG("vSLM bdev not found for NSID %u\n", nsid);
		return -ENOENT;
	}

	return vbdev_vslm_mem_get_buffer_ptr_by_bdev(&vslm->vbdev, offset, length, ptr);
}

int
bdev_vslm_pin_range_by_nsid(uint32_t nsid, uint64_t offset,
			    uint64_t length, bool for_write,
			    struct spdk_bdev_slm_sg_entry *entries,
			    uint32_t max_entries, uint32_t *entry_count)
{
	struct vbdev_vslm *vslm;

	if (nsid == 0) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_nsid(nsid);
	if (vslm == NULL) {
		return -ENOENT;
	}

	return vbdev_vslm_mem_pin_range_by_bdev(&vslm->vbdev, offset, length, for_write,
						entries, max_entries, entry_count);
}

int
bdev_vslm_unpin_range_by_nsid(uint32_t nsid,
			      const struct spdk_bdev_slm_sg_entry *entries,
			      uint32_t entry_count, bool dirtied)
{
	struct vbdev_vslm *vslm;

	if (nsid == 0) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_nsid(nsid);
	if (vslm == NULL) {
		return -ENOENT;
	}

	return vbdev_vslm_mem_unpin_range_by_bdev(&vslm->vbdev, entries, entry_count, dirtied);
}

static int
vbdev_vslm_mem_get_buffer_ptr_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				      uint64_t length, void **ptr)
{
	if (!ptr || bdev == NULL) {
		return -EINVAL;
	}

	if (length == 0) {
		*ptr = NULL;
		return 0;
	}

	if (vbdev_vslm_get_by_bdev(bdev) == NULL) {
		return -ENOTSUP;
	}

	(void)offset;
	*ptr = NULL;
	return -ENOTSUP;
}

/*
 * Unpin SG entries that may span multiple shards. Each entry is unpinned under
 * its own shard lock (one lock held at a time), preserving the shared-nothing
 * invariant. Validation is per-entry rather than all-or-nothing because holding
 * every shard lock simultaneously is disallowed.
 */
static int
vslm_unpin_sg_entries(struct vbdev_vslm *vslm,
		      const struct spdk_bdev_slm_sg_entry *entries,
		      uint32_t entry_count,
		      bool dirtied)
{
	struct vslm_page *page;
	struct vslm_mmu_shard *shard;
	uint32_t i;
	bool entry_dirtied;
	int rc = 0;

	for (i = 0; i < entry_count; i++) {
		shard = vslm_shard_for_vpn(vslm, entries[i].vpn);
		pthread_mutex_lock(&shard->lock);
		page = vslm_lookup_page(vslm, entries[i].vpn);
		if (page == NULL ||
		    page->ppn != entries[i].ppn ||
		    page->load_state != VSLM_PAGE_LOAD_RESIDENT ||
		    page->pin_count == 0) {
			pthread_mutex_unlock(&shard->lock);
			rc = -EINVAL;
			continue;
		}
		entry_dirtied = dirtied || entries[i].dirtied;
		vslm_page_unpin_locked(vslm, page, entry_dirtied, true);
		pthread_mutex_unlock(&shard->lock);
	}

	return rc;
}

static int
vbdev_vslm_mem_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				 uint64_t length, bool for_write,
				 struct spdk_bdev_slm_sg_entry *entries,
				 uint32_t max_entries, uint32_t *entry_count)
{
	struct vbdev_vslm *vslm;
	struct spdk_io_channel *base_ch;
	struct vslm_page *page;
	struct vslm_mmu_shard *shard;
	uint64_t lock_start_ticks = 0;
	uint64_t processed = 0;
	uint64_t absolute;
	uint64_t vpn;
	uint64_t page_offset;
	uint64_t chunk;
	uint32_t pinned_count = 0;
	int rc = 0;

	if (bdev == NULL || entries == NULL || entry_count == NULL || max_entries == 0) {
		return -EINVAL;
	}

	*entry_count = 0;
	if (length == 0) {
		return 0;
	}

	vslm = vbdev_vslm_get_by_bdev(bdev);
	if (vslm == NULL) {
		return -ENOTSUP;
	}

	if (offset >= vslm->virtual_size_bytes ||
	    length > vslm->virtual_size_bytes - offset) {
		return -EINVAL;
	}

	base_ch = spdk_bdev_get_io_channel(vslm->base_desc);
	if (base_ch == NULL) {
		return -ENOMEM;
	}

	while (processed < length) {
		if (pinned_count == max_entries) {
			rc = -ENOSPC;
			break;
		}

		absolute = offset + processed;
		vpn = absolute / VSLM_PAGE_SIZE;
		page_offset = absolute % VSLM_PAGE_SIZE;
		chunk = spdk_min((uint64_t)(VSLM_PAGE_SIZE - page_offset), length - processed);
		shard = vslm_shard_for_vpn(vslm, vpn);

		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		lock_start_ticks = spdk_get_ticks();
		rc = vslm_resolve_and_pin_page_locked(vslm, base_ch, NULL, vpn, for_write, false, &page);
		if (rc == 0) {
			entries[pinned_count].addr = vslm->sram_buffer +
						     (page->ppn * VSLM_PAGE_SIZE) + page_offset;
			entries[pinned_count].logical_offset = absolute;
			entries[pinned_count].len = (uint32_t)chunk;
			entries[pinned_count].vpn = vpn;
			entries[pinned_count].ppn = page->ppn;
			entries[pinned_count].dirtied = false;
			pinned_count++;
		}
		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
		if (rc != 0) {
			break;
		}

		processed += chunk;
	}

	if (rc != 0 && pinned_count > 0) {
		(void)vslm_unpin_sg_entries(vslm, entries, pinned_count, false);
		pinned_count = 0;
	}

	spdk_put_io_channel(base_ch);
	*entry_count = pinned_count;
	return rc;
}

static int
vbdev_vslm_mem_try_pin_range_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				     uint64_t length, bool for_write,
				     struct spdk_bdev_slm_sg_entry *entries,
				     uint32_t max_entries, uint32_t *entry_count)
{
	struct vbdev_vslm *vslm;
	struct vslm_page *page;
	uint64_t lock_start_ticks = 0;
	uint64_t processed = 0;
	uint64_t absolute;
	uint64_t vpn;
	uint64_t page_offset;
	uint64_t chunk;
	uint32_t pinned_count = 0;
	int rc = 0;

	if (bdev == NULL || entries == NULL || entry_count == NULL || max_entries == 0) {
		return -EINVAL;
	}

	*entry_count = 0;
	if (length == 0) {
		return 0;
	}

	vslm = vbdev_vslm_get_by_bdev(bdev);
	if (vslm == NULL) {
		return -ENOTSUP;
	}

	if (offset >= vslm->virtual_size_bytes ||
	    length > vslm->virtual_size_bytes - offset) {
		return -EINVAL;
	}

	(void)for_write;
	(void)lock_start_ticks;

	while (processed < length) {
		struct vslm_mmu_shard *shard;

		if (pinned_count == max_entries) {
			rc = -ENOSPC;
			break;
		}

		absolute = offset + processed;
		vpn = absolute / VSLM_PAGE_SIZE;
		page_offset = absolute % VSLM_PAGE_SIZE;
		chunk = spdk_min((uint64_t)(VSLM_PAGE_SIZE - page_offset), length - processed);
		shard = vslm_shard_for_vpn(vslm, vpn);

		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		page = vslm_lookup_page(vslm, vpn);
		if (page == NULL || page->load_state != VSLM_PAGE_LOAD_RESIDENT) {
			pthread_mutex_unlock(&shard->lock);
			rc = -EAGAIN;
			break;
		}

		vslm_page_pin_locked(vslm, page);
		pthread_mutex_unlock(&shard->lock);
		entries[pinned_count].addr = vslm->sram_buffer +
					     (page->ppn * VSLM_PAGE_SIZE) + page_offset;
		entries[pinned_count].logical_offset = absolute;
		entries[pinned_count].len = (uint32_t)chunk;
		entries[pinned_count].vpn = vpn;
		entries[pinned_count].ppn = page->ppn;
		entries[pinned_count].dirtied = false;
		pinned_count++;
		processed += chunk;
	}

	if (rc != 0 && pinned_count > 0) {
		(void)vslm_unpin_sg_entries(vslm, entries, pinned_count, false);
		pinned_count = 0;
	}

	*entry_count = pinned_count;
	return rc;
}

static int
vbdev_vslm_mem_unpin_range_by_bdev(struct spdk_bdev *bdev,
				   const struct spdk_bdev_slm_sg_entry *entries,
				   uint32_t entry_count, bool dirtied)
{
	struct vbdev_vslm *vslm;

	if (bdev == NULL || (entry_count != 0 && entries == NULL)) {
		return -EINVAL;
	}

	if (entry_count == 0) {
		return 0;
	}

	vslm = vbdev_vslm_get_by_bdev(bdev);
	if (vslm == NULL) {
		return -ENOTSUP;
	}

	return vslm_unpin_sg_entries(vslm, entries, entry_count, dirtied);
}

static int
vslm_rw_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf,
		bool is_write, bool exec_view)
{
	struct vbdev_vslm *vslm;
	struct spdk_io_channel *base_ch;
	uint64_t processed = 0;
	uint32_t chunk;
	uint8_t *buf_u8 = buf;
	bool has_conflict;
	int rc;

	if (bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	if (length == 0) {
		return 0;
	}
	SPDK_DEBUGLOG(vslm, "vSLM rw start bdev=%s %s off=%" PRIu64 " len=%" PRIu64
		      " exec_view=%d\n", bdev->name, is_write ? "WRITE" : "READ",
		      offset, length, exec_view);

	vslm = vbdev_vslm_get_by_bdev(bdev);
	if (vslm == NULL) {
		return -ENOTSUP;
	}

	base_ch = spdk_bdev_get_io_channel(vslm->base_desc);
	if (base_ch == NULL) {
		return -ENOMEM;
	}

	if (!exec_view && is_write) {
		has_conflict = vslm_has_lease_conflict(vslm, offset, length);
		if (has_conflict) {
			__atomic_add_fetch(&vslm->stats.vslm_host_write_conflict_total, 1,
					   __ATOMIC_RELAXED);
			vslm_trace_emitf(vslm, "lease_conflict",
					 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"rejected\"",
					 offset, length);
			vslm_trace_emitf(vslm, "host_write_conflict",
					 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"rejected\"",
					 offset, length);
			spdk_put_io_channel(base_ch);
			SPDK_DEBUGLOG(vslm, "vSLM rw blocked by lease conflict bdev=%s off=%" PRIu64
				      " len=%" PRIu64 "\n", bdev->name, offset, length);
			return -EAGAIN;
		}

		__atomic_add_fetch(&vslm->stats.vslm_host_write_nonconflict_total, 1,
				   __ATOMIC_RELAXED);
		vslm_trace_emitf(vslm, "host_write_nonconflict",
				 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"allowed\"",
				 offset, length);
	}

	if (!exec_view && !is_write && vslm_has_lease_conflict(vslm, offset, length)) {
		__atomic_add_fetch(&vslm->stats.vslm_host_read_during_execution_total, 1,
				   __ATOMIC_RELAXED);
		vslm_trace_emitf(vslm, "host_read_committed",
				 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"committed_view\"",
				 offset, length);
	}

	while (processed < length) {
		chunk = (uint32_t)spdk_min(length - processed, (uint64_t)UINT32_MAX);
		if (!exec_view && !is_write) {
			rc = vslm_read_committed_range(vslm, base_ch, offset + processed, chunk,
						       buf_u8 + processed);
		} else {
			rc = vslm_copy_range_ex(vslm, base_ch, offset + processed, chunk,
						buf_u8 + processed, is_write,
						exec_view && !is_write);
		}
		if (rc != 0) {
			break;
		}
		processed += chunk;
	}
	spdk_put_io_channel(base_ch);
	SPDK_DEBUGLOG(vslm, "vSLM rw done bdev=%s %s off=%" PRIu64 " len=%" PRIu64
		      " rc=%d processed=%" PRIu64 "\n", bdev->name, is_write ? "WRITE" : "READ",
		      offset, length, rc, processed);
	return rc;
}

static int
vslm_rw_by_nsid(uint32_t nsid, uint64_t offset, uint64_t length, void *buf, bool is_write,
		bool exec_view)
{
	struct vbdev_vslm *vslm;

	if (nsid == 0 || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_nsid(nsid);
	if (vslm == NULL) {
		return -ENOENT;
	}

	return vslm_rw_by_bdev(&vslm->vbdev, offset, length, buf, is_write, exec_view);
}

enum vslm_mem_async_op {
	VSLM_MEM_ASYNC_READ = 0,
	VSLM_MEM_ASYNC_WRITE,
	VSLM_MEM_ASYNC_COPY,
	VSLM_MEM_ASYNC_EXEC_READ,
	VSLM_MEM_ASYNC_EXEC_WRITE,
};

struct vslm_mem_async_ctx {
	enum vslm_mem_async_op op;
	struct spdk_thread *submit_thread;
	struct spdk_bdev *bdev;
	struct spdk_bdev *dst_bdev;
	struct spdk_bdev *src_bdev;
	uint64_t offset;
	uint64_t length;
	uint64_t dst_offset;
	uint64_t src_offset;
	void *buf;
	spdk_bdev_slm_io_completion_cb cb_fn;
	void *cb_arg;
	int status;
};

struct vslm_exec_rw_async_ctx {
	struct spdk_thread *submit_thread;
	struct spdk_thread *worker_thread;
	struct spdk_bdev *bdev;
	uint64_t offset;
	uint64_t length;
	uint64_t processed;
	uint8_t *buf;
	spdk_bdev_slm_io_completion_cb cb_fn;
	void *cb_arg;
	struct vbdev_vslm *vslm;
	struct spdk_io_channel *base_ch;
	struct spdk_bdev_desc *source_desc;
	struct spdk_io_channel *source_ch;
	struct vslm_single_fault_ctx fault_ctx;
	struct vslm_page *evict_page;
	uint64_t evict_vpn;
	uint64_t fault_start_ticks;
	bool source_desc_opened;
	bool source_ch_owned;
	struct vslm_source_io exec_src;	/* source desc+channel cached for the whole execute */
	/* Bulk read-fault coalescing: a contiguous clean run is faulted in one readv. */
	struct vslm_fault_batch_ctx batch;
	struct vslm_fault_batch_page batch_pages[VSLM_MMU_SHARD_STRIDE_PAGES];
	struct iovec batch_iov[VSLM_MMU_SHARD_STRIDE_PAGES];
	struct spdk_bdev *batch_src_bdev;
	uint64_t batch_src_off;
	bool batch_active;
	bool fault_read_is_default_backing;
	bool fault_in_counted;
	bool is_write;
	int status;
};

static void vslm_exec_rw_async_step(void *arg);
static int vslm_exec_rw_async_submit_backing_fault(struct vslm_exec_rw_async_ctx *ctx);
static int vslm_exec_rw_async_submit_evict_writeback(struct vslm_exec_rw_async_ctx *ctx);

static void
vslm_exec_rw_async_cleanup_source(struct vslm_exec_rw_async_ctx *ctx)
{
	if (ctx->source_ch_owned && ctx->source_ch != NULL) {
		spdk_put_io_channel(ctx->source_ch);
	}
	if (ctx->source_desc_opened && ctx->source_desc != NULL) {
		spdk_bdev_close(ctx->source_desc);
	}

	ctx->source_desc = NULL;
	ctx->source_ch = NULL;
	ctx->source_desc_opened = false;
	ctx->source_ch_owned = false;
}

static void
vslm_exec_rw_async_complete_msg(void *arg)
{
	struct vslm_exec_rw_async_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, ctx->status);
	vslm_exec_rw_async_cleanup_source(ctx);
	if (ctx->base_ch != NULL) {
		spdk_put_io_channel(ctx->base_ch);
	}
	if (ctx->vslm != NULL) {
		vslm_put_io_ref(ctx->vslm);
	}
	vslm_source_io_release(&ctx->exec_src);
	free(ctx);
}

static void
vslm_exec_rw_async_finish(struct vslm_exec_rw_async_ctx *ctx, int status)
{
	int rc;

	ctx->status = status;
	/*
	 * Release the cached source channel here, on the worker thread that created it
	 * (SPDK I/O channels are thread-local), before any cross-thread completion
	 * handoff. The later release sites are then idempotent no-ops.
	 */
	vslm_source_io_release(&ctx->exec_src);
	if (ctx->submit_thread == NULL || ctx->submit_thread == spdk_get_thread()) {
		ctx->cb_fn(ctx->cb_arg, ctx->status);
		vslm_exec_rw_async_cleanup_source(ctx);
		if (ctx->base_ch != NULL) {
			spdk_put_io_channel(ctx->base_ch);
		}
		if (ctx->vslm != NULL) {
			vslm_put_io_ref(ctx->vslm);
		}
		vslm_source_io_release(&ctx->exec_src);
		free(ctx);
		return;
	}

	rc = spdk_thread_send_msg(ctx->submit_thread, vslm_exec_rw_async_complete_msg, ctx);
	if (rc != 0) {
		ctx->cb_fn(ctx->cb_arg, rc);
		vslm_exec_rw_async_cleanup_source(ctx);
		if (ctx->base_ch != NULL) {
			spdk_put_io_channel(ctx->base_ch);
		}
		if (ctx->vslm != NULL) {
			vslm_put_io_ref(ctx->vslm);
		}
		vslm_source_io_release(&ctx->exec_src);
		free(ctx);
	}
}

static void
vslm_exec_rw_async_schedule_step(struct vslm_exec_rw_async_ctx *ctx)
{
	int rc;

	rc = spdk_thread_send_msg(ctx->worker_thread, vslm_exec_rw_async_step, ctx);
	if (rc != 0) {
		vslm_exec_rw_async_finish(ctx, rc);
	}
}

static void
vslm_exec_rw_async_waiter_cb(void *cb_arg, int status, struct vslm_page *page)
{
	struct vslm_exec_rw_async_ctx *ctx = cb_arg;

	(void)page;
	if (status != 0) {
		vslm_exec_rw_async_finish(ctx, status);
		return;
	}

	vslm_exec_rw_async_schedule_step(ctx);
}

static void
vslm_exec_rw_async_legacy_step(struct vslm_exec_rw_async_ctx *ctx)
{
	uint64_t chunk;
	int rc;

	chunk = spdk_min(ctx->length - ctx->processed,
			 (uint64_t)VSLM_ASYNC_EXEC_READ_CHUNK_BYTES);
	chunk = spdk_min(chunk, (uint64_t)UINT32_MAX);
	if (ctx->is_write) {
		rc = vbdev_vslm_mem_exec_write_by_bdev(ctx->bdev,
						       ctx->offset + ctx->processed,
						       chunk,
						       ctx->buf + ctx->processed);
	} else {
		rc = vbdev_vslm_mem_exec_read_by_bdev(ctx->bdev,
						      ctx->offset + ctx->processed,
						      chunk,
						      ctx->buf + ctx->processed);
	}
	if (rc != 0) {
		vslm_exec_rw_async_finish(ctx, rc);
		return;
	}

	ctx->processed += chunk;
	vslm_exec_rw_async_schedule_step(ctx);
}

static void
vslm_exec_rw_async_finish_clean_eviction_locked(struct vbdev_vslm *vslm,
		struct vslm_page *page)
{
	struct vslm_lpage *lpage;
	uint64_t victim_vpn;
	int final_state = -1;

	victim_vpn = page->vpn;
	lpage = vslm_lookup_lpage(vslm, victim_vpn);

	page->load_state = VSLM_PAGE_LOAD_EVICTING;
	page->load_status = 0;
	if (page->on_hash) {
		vslm_hash_remove(vslm, page);
	}

	if (lpage != NULL) {
		switch (lpage->state) {
		case SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT:
			if (lpage->private_authoritative) {
				vslm_lpage_set_spilled_source(vslm, lpage);
				vslm_lpage_set_state(vslm, page, lpage,
						     SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE);
			} else if (lpage->source_type == SPDK_BDEV_VSLM_PAGE_SOURCE_ALIAS_BDEV) {
				vslm_lpage_set_state(vslm, page, lpage,
						     SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS);
			} else {
				vslm_lpage_delete(vslm, lpage);
				lpage = NULL;
			}
			break;
		case SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS:
		case SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE:
		default:
			break;
		}
	}

	if (lpage != NULL) {
		final_state = (int)lpage->state;
	}
	__atomic_add_fetch(&vslm->stats.page_evictions, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&vslm->stats.page_eviction_bytes, VSLM_PAGE_SIZE,
			   __ATOMIC_RELAXED);
	__atomic_add_fetch(&vslm->stats.vslm_eviction_clean_total, 1,
			   __ATOMIC_RELAXED);
	vslm_trace_emitf(vslm, "evict_clean",
			 ",\"page_id\":%" PRIu64 ",\"bytes\":%u,\"result\":\"evicted\"",
			 victim_vpn, VSLM_PAGE_SIZE);
	vslm_note_resident_remove(vslm);
	vslm_page_prepare_free(page);
	vslm_free_list_insert_tail(vslm, page);
	SPDK_DEBUGLOG(vslm, "vSLM async clean evict done bdev=%s victim_vpn=%" PRIu64
		      " final_state=%d\n", vslm->vbdev.name, victim_vpn, final_state);
	(void)final_state;
}

static void
vslm_exec_rw_async_restore_evict_page_locked(struct vbdev_vslm *vslm,
		struct vslm_page *page, int status)
{
	(void)status;

	page->is_busy = false;
	page->load_state = VSLM_PAGE_LOAD_RESIDENT;
	page->load_status = 0;
	if (page->location == VSLM_PAGE_LOC_NONE) {
		vslm_lru_touch(vslm, page);
	}
}

static int
vslm_exec_rw_async_prepare_eviction_locked(struct vslm_exec_rw_async_ctx *ctx,
		bool *submit_writeback)
{
	struct vbdev_vslm *vslm = ctx->vslm;
	uint64_t fault_vpn = (ctx->offset + ctx->processed) / VSLM_PAGE_SIZE;
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, fault_vpn);
	struct vslm_page *page;
	struct vslm_lpage *lpage;

	*submit_writeback = false;
	/* Evict only within the faulting VPN's own shard (shared-nothing). */
	page = vslm_pick_victim(vslm, shard);
	if (page == NULL) {
		return -ENOMEM;
	}

	VSLM_PERF_INC(vslm, resolve_evict_total);
	SPDK_DEBUGLOG(vslm, "vSLM async resolve miss bdev=%s no free page, evicting "
		      "ppn=%" PRIu64 " victim_vpn=%" PRIu64 "\n",
		      vslm->vbdev.name, page->ppn, page->vpn);

	if (!page->dirty) {
		vslm_exec_rw_async_finish_clean_eviction_locked(vslm, page);
		vslm_debug_validate_page_lists(vslm);
		return 0;
	}

	lpage = vslm_lookup_lpage(vslm, page->vpn);
	if (lpage == NULL) {
		VSLM_PERF_INC(vslm, dirty_page_missing_lpage_total);
		lpage = vslm_lpage_alloc_private_dirty(vslm, page->vpn);
		if (lpage == NULL) {
			vslm_exec_rw_async_restore_evict_page_locked(vslm, page, -ENOMEM);
			return -ENOMEM;
		}
		page->dirty = true;
	}

	if (lpage->state != SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT) {
		vslm_exec_rw_async_restore_evict_page_locked(vslm, page, -EINVAL);
		return -EINVAL;
	}

	if (lpage->pending_publish && vslm_vpn_has_active_lease(vslm, page->vpn)) {
		vslm_exec_rw_async_restore_evict_page_locked(vslm, page, -EBUSY);
		return -EBUSY;
	}

	page->is_busy = true;
	page->load_state = VSLM_PAGE_LOAD_EVICTING;
	page->load_status = 0;
	ctx->evict_page = page;
	ctx->evict_vpn = page->vpn;
	*submit_writeback = true;
	vslm_debug_validate_page_lists(vslm);
	return 0;
}

static void
vslm_exec_rw_async_evict_writeback_complete(struct spdk_bdev_io *bdev_io,
		bool success, void *cb_arg)
{
	struct vslm_exec_rw_async_ctx *ctx = cb_arg;
	struct vbdev_vslm *vslm = ctx->vslm;
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, ctx->evict_vpn);
	struct vslm_page_waiter_list waiters;
	struct vslm_page *page = ctx->evict_page;
	struct vslm_lpage *lpage;
	int rc = 0;

	TAILQ_INIT(&waiters);
	spdk_bdev_free_io(bdev_io);
	if (!success) {
		rc = -EIO;
	}

	VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
	pthread_mutex_lock(&shard->lock);
	if (page == NULL || page->load_state != VSLM_PAGE_LOAD_EVICTING) {
		rc = rc != 0 ? rc : -EIO;
	} else if (rc == 0) {
		lpage = vslm_lookup_lpage(vslm, ctx->evict_vpn);
		if (lpage == NULL) {
			rc = -EIO;
		} else {
			lpage->pending_publish = false;
			vslm_lpage_clear_superseded_source(lpage);
			vslm_lpage_set_spilled_source(vslm, lpage);
			lpage->private_authoritative = true;
			vslm_lpage_set_state(vslm, page, lpage,
					     SPDK_BDEV_VSLM_LPAGE_SPILLED_PRIVATE);
			if (page->on_hash) {
				vslm_hash_remove(vslm, page);
			}
			vslm_page_take_waiters_locked(page, 0, NULL, &waiters);
			__atomic_add_fetch(&vslm->stats.page_writebacks, 1, __ATOMIC_RELAXED);
			__atomic_add_fetch(&vslm->stats.page_writeback_bytes, VSLM_PAGE_SIZE,
					   __ATOMIC_RELAXED);
			__atomic_add_fetch(&vslm->stats.dirty_writeback_bytes, VSLM_PAGE_SIZE,
					   __ATOMIC_RELAXED);
			__atomic_add_fetch(&vslm->stats.vslm_spill_write_bytes, VSLM_PAGE_SIZE,
					   __ATOMIC_RELAXED);
			__atomic_add_fetch(&vslm->stats.page_evictions, 1, __ATOMIC_RELAXED);
			__atomic_add_fetch(&vslm->stats.page_eviction_bytes, VSLM_PAGE_SIZE,
					   __ATOMIC_RELAXED);
			__atomic_add_fetch(&vslm->stats.vslm_eviction_private_total, 1,
					   __ATOMIC_RELAXED);
			vslm_trace_emitf(vslm, "spill_private",
					 ",\"page_id\":%" PRIu64 ",\"bytes\":%u,\"result\":\"evicted\"",
					 ctx->evict_vpn, VSLM_PAGE_SIZE);
			vslm_note_resident_remove(vslm);
			vslm_page_prepare_free(page);
			vslm_free_list_insert_tail(vslm, page);
		}
	}

	if (rc != 0 && page != NULL) {
		vslm_exec_rw_async_restore_evict_page_locked(vslm, page, rc);
		vslm_page_take_waiters_locked(page, 0, page, &waiters);
	}
	ctx->evict_page = NULL;
	ctx->evict_vpn = 0;
	vslm_debug_validate_page_lists(vslm);
	pthread_mutex_unlock(&shard->lock);
	vslm_page_complete_waiters(&waiters);

	if (rc != 0) {
		vslm_exec_rw_async_finish(ctx, rc);
		return;
	}

	vslm_exec_rw_async_schedule_step(ctx);
}

static int
vslm_exec_rw_async_submit_evict_writeback(struct vslm_exec_rw_async_ctx *ctx)
{
	struct vbdev_vslm *vslm = ctx->vslm;
	struct vslm_page *page = ctx->evict_page;
	uint64_t block_size;
	uint64_t page_blocks;
	uint64_t offset_blocks;
	uint8_t *ptr;

	if (page == NULL) {
		return -EINVAL;
	}

	block_size = spdk_bdev_get_block_size(vslm->base_bdev);
	if (block_size == 0 || (VSLM_PAGE_SIZE % block_size) != 0) {
		return -EINVAL;
	}

	page_blocks = VSLM_PAGE_SIZE / block_size;
	/* Spill to the isolated backing region, preserving the clean source slot. */
	offset_blocks = vslm_spill_offset_blocks(vslm, ctx->evict_vpn, page_blocks);
	ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE);
	return spdk_bdev_write_blocks(vslm->base_desc, ctx->base_ch, ptr,
				      offset_blocks, page_blocks,
				      vslm_exec_rw_async_evict_writeback_complete, ctx);
}

static void
vslm_exec_rw_async_restore_pending_eviction(struct vslm_exec_rw_async_ctx *ctx,
		int status)
{
	struct vbdev_vslm *vslm = ctx->vslm;
	struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, ctx->evict_vpn);
	struct vslm_page_waiter_list waiters;
	struct vslm_page *page;

	TAILQ_INIT(&waiters);
	VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
	pthread_mutex_lock(&shard->lock);
	page = ctx->evict_page;
	if (page != NULL) {
		vslm_exec_rw_async_restore_evict_page_locked(vslm, page, status);
		vslm_page_take_waiters_locked(page, 0, page, &waiters);
	}
	ctx->evict_page = NULL;
	ctx->evict_vpn = 0;
	vslm_debug_validate_page_lists(vslm);
	pthread_mutex_unlock(&shard->lock);
	vslm_page_complete_waiters(&waiters);
}

static void
vslm_exec_rw_async_fault_complete(struct spdk_bdev_io *bdev_io, bool success,
				  void *cb_arg)
{
	struct vslm_exec_rw_async_ctx *ctx = cb_arg;
	struct vbdev_vslm *vslm = ctx->vslm;
	struct vslm_mmu_shard *shard;
	struct vslm_page_waiter_list waiters;
	struct vslm_page *page = NULL;
	uint64_t absolute;
	uint64_t page_offset;
	uint64_t chunk;
	uint64_t fault_ns;
	uint8_t *ptr = NULL;
	bool was_default_backing;
	bool committed = false;
	int submit_rc;
	int rc;

	TAILQ_INIT(&waiters);
	was_default_backing = ctx->fault_read_is_default_backing;
	spdk_bdev_free_io(bdev_io);
	vslm_exec_rw_async_cleanup_source(ctx);

	if (!success && !was_default_backing) {
		submit_rc = vslm_exec_rw_async_submit_backing_fault(ctx);
		if (submit_rc == 0) {
			return;
		}
		rc = submit_rc;
	} else {
		rc = 0;
	}

	fault_ns = vslm_ticks_delta_to_ns(ctx->fault_start_ticks, spdk_get_ticks());
	ctx->fault_start_ticks = 0;
	ctx->fault_in_counted = false;
	VSLM_PERF_ADD(vslm, fault_in_io_ns_total, fault_ns);
	vslm_perf_max_u64(&vslm->perf_stats.fault_in_io_ns_max, fault_ns);

	absolute = ctx->offset + ctx->processed;
	page_offset = absolute % VSLM_PAGE_SIZE;
	chunk = spdk_min((uint64_t)(VSLM_PAGE_SIZE - page_offset),
			 ctx->length - ctx->processed);

	shard = vslm_shard_for_vpn(vslm, ctx->fault_ctx.vpn);
	VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
	pthread_mutex_lock(&shard->lock);
	if (success) {
		rc = vslm_commit_reserved_page_locked(vslm, &ctx->fault_ctx, true);
		if (rc == 0) {
			VSLM_PERF_INC(vslm, fault_in_4k_total);
			VSLM_PERF_ADD(vslm, fault_in_4k_bytes, VSLM_PAGE_SIZE);
			vslm_page_take_waiters_locked(ctx->fault_ctx.page, 0,
						      ctx->fault_ctx.page, &waiters);
			/*
			 * Pin the just-committed page before releasing the shard lock and
			 * perform this fault's memcpy NOW, holding the pin across it,
			 * instead of leaving the still-clean page resident-and-unpinned
			 * and rescheduling a fresh lookup. Otherwise a concurrent fault's
			 * vslm_pick_victim() can clean-evict (steal) it in the window,
			 * which livelocks under sustained eviction pressure (mirrors the
			 * full-page-overwrite protection in vslm_exec_rw_async_page_step).
			 */
			page = ctx->fault_ctx.page;
			vslm_page_pin_locked(vslm, page);
			ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset;
			committed = true;
		}
	} else if (rc == 0) {
		rc = -EIO;
	}

	if (rc != 0) {
		if (ctx->fault_ctx.page != NULL) {
			vslm_page_take_waiters_locked(ctx->fault_ctx.page, rc, NULL, &waiters);
		}
		vslm_rollback_reserved_page_locked(vslm, &ctx->fault_ctx);
	}
	vslm_debug_validate_page_lists(vslm);
	pthread_mutex_unlock(&shard->lock);
	vslm_page_complete_waiters(&waiters);

	if (rc != 0) {
		vslm_exec_rw_async_finish(ctx, rc);
		return;
	}

	if (committed) {
		if (ctx->is_write) {
			memcpy(ptr, ctx->buf + ctx->processed, chunk);
		} else {
			memcpy(ctx->buf + ctx->processed, ptr, chunk);
		}

		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		vslm_page_unpin_locked(vslm, page, ctx->is_write, false);
		pthread_mutex_unlock(&shard->lock);

		ctx->processed += chunk;
	}

	vslm_exec_rw_async_schedule_step(ctx);
}

static int
vslm_exec_rw_async_submit_fault_io(struct vslm_exec_rw_async_ctx *ctx,
				   struct spdk_bdev_desc *desc,
				   struct spdk_io_channel *ch,
				   uint64_t offset_blocks,
				   uint64_t num_blocks,
				   bool default_backing)
{
	struct vbdev_vslm *vslm = ctx->vslm;
	struct vslm_single_fault_ctx *fault_ctx = &ctx->fault_ctx;
	uint8_t *ptr;

	if (!ctx->fault_in_counted) {
		VSLM_PERF_INC(vslm, fault_in_total);
		ctx->fault_start_ticks = spdk_get_ticks();
		ctx->fault_in_counted = true;
	}

	ptr = vslm->sram_buffer + (fault_ctx->page->ppn * VSLM_PAGE_SIZE);
	ctx->fault_read_is_default_backing = default_backing;
	return spdk_bdev_read_blocks(desc, ch, ptr, offset_blocks, num_blocks,
				     vslm_exec_rw_async_fault_complete, ctx);
}

static int
vslm_exec_rw_async_submit_backing_fault(struct vslm_exec_rw_async_ctx *ctx)
{
	struct vbdev_vslm *vslm = ctx->vslm;
	struct vslm_single_fault_ctx *fault_ctx = &ctx->fault_ctx;
	uint64_t block_size;
	uint64_t page_blocks;
	uint64_t offset_blocks;
	int rc;

	block_size = spdk_bdev_get_block_size(vslm->base_bdev);
	if (block_size == 0 || (VSLM_PAGE_SIZE % block_size) != 0) {
		return -EINVAL;
	}

	page_blocks = VSLM_PAGE_SIZE / block_size;
	offset_blocks = fault_ctx->vpn * page_blocks;

	ctx->source_desc = vslm->base_desc;
	ctx->source_ch = ctx->base_ch;
	ctx->source_desc_opened = false;
	ctx->source_ch_owned = false;
	rc = vslm_exec_rw_async_submit_fault_io(ctx, vslm->base_desc, ctx->base_ch,
						offset_blocks, page_blocks, true);
	if (rc != 0) {
		vslm_exec_rw_async_cleanup_source(ctx);
	}
	return rc;
}

static int
vslm_exec_rw_async_submit_source_fault(struct vslm_exec_rw_async_ctx *ctx)
{
	struct vbdev_vslm *vslm = ctx->vslm;
	struct vslm_single_fault_ctx *fault_ctx = &ctx->fault_ctx;
	struct vslm_lpage *lpage = fault_ctx->lpage;
	struct spdk_bdev *source_bdev;
	struct spdk_bdev_desc *source_desc = NULL;
	struct spdk_io_channel *source_ch = NULL;
	uint64_t source_block_size;
	uint64_t source_page_blocks;
	uint64_t source_offset_blocks;
	uint64_t source_num_blocks;
	int rc;

	if (lpage == NULL ||
	    (lpage->source_type != SPDK_BDEV_VSLM_PAGE_SOURCE_BACKING &&
	     lpage->source_type != SPDK_BDEV_VSLM_PAGE_SOURCE_ALIAS_BDEV) ||
	    lpage->source_bdev == NULL) {
		return vslm_exec_rw_async_submit_backing_fault(ctx);
	}

	source_bdev = lpage->source_bdev;
	source_block_size = spdk_bdev_get_block_size(source_bdev);
	if (source_block_size == 0 ||
	    (VSLM_PAGE_SIZE % source_block_size) != 0 ||
	    (lpage->source_offset_bytes % source_block_size) != 0) {
		return vslm_exec_rw_async_submit_backing_fault(ctx);
	}

	source_page_blocks = VSLM_PAGE_SIZE / source_block_size;
	source_offset_blocks = lpage->source_offset_bytes / source_block_size;
	source_num_blocks = spdk_bdev_get_num_blocks(source_bdev);
	if (source_offset_blocks > source_num_blocks ||
	    source_page_blocks > (source_num_blocks - source_offset_blocks)) {
		return vslm_exec_rw_async_submit_backing_fault(ctx);
	}

	/*
	 * Cache the source desc + per-thread channel on the exec ctx and reuse them
	 * for every fault in this execute, instead of opening a descriptor and creating
	 * (then destroying) an NVMe qpair per 4 KiB page. Falls back to the backing
	 * fault if the source cannot be opened.
	 */
	rc = vslm_source_io_acquire(vslm, &ctx->exec_src, ctx->base_ch, source_bdev,
				    &source_desc, &source_ch);
	if (rc != 0) {
		return vslm_exec_rw_async_submit_backing_fault(ctx);
	}

	/* exec_src owns the cached desc+channel; the per-fault cleanup must not close them. */
	ctx->source_desc = source_desc;
	ctx->source_ch = source_ch;
	ctx->source_desc_opened = false;
	ctx->source_ch_owned = false;
	rc = vslm_exec_rw_async_submit_fault_io(ctx, source_desc, source_ch,
						source_offset_blocks, source_page_blocks,
						false);
	if (rc == 0) {
		return 0;
	}

	vslm_exec_rw_async_cleanup_source(ctx);
	return vslm_exec_rw_async_submit_backing_fault(ctx);
}

/*
 * Completion for a coalesced bulk fault-in readv. Commits the whole run, copies
 * each page out to the caller's buffer (under the shard lock, so the just-committed
 * frames cannot be clean-evicted mid-copy), advances past the run, and continues.
 */
static void
vslm_exec_rw_async_fault_batch_complete(struct spdk_bdev_io *bdev_io, bool success,
					void *cb_arg)
{
	struct vslm_exec_rw_async_ctx *ctx = cb_arg;
	struct vbdev_vslm *vslm = ctx->vslm;
	struct vslm_mmu_shard *shard;
	struct vslm_page_waiter_list waiters;
	uint64_t fault_ns;
	uint32_t i;

	TAILQ_INIT(&waiters);
	spdk_bdev_free_io(bdev_io);

	fault_ns = vslm_ticks_delta_to_ns(ctx->fault_start_ticks, spdk_get_ticks());
	ctx->fault_start_ticks = 0;
	ctx->fault_in_counted = false;
	VSLM_PERF_ADD(vslm, fault_in_io_ns_total, fault_ns);
	vslm_perf_max_u64(&vslm->perf_stats.fault_in_io_ns_max, fault_ns);

	shard = vslm_shard_for_vpn(vslm, ctx->batch.start_vpn);
	VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
	pthread_mutex_lock(&shard->lock);

	if (!success) {
		for (i = 0; i < ctx->batch.nr_pages; i++) {
			struct vslm_page *p = ctx->batch.pages[i].page;

			if (p != NULL) {
				vslm_page_take_waiters_locked(p, -EIO, NULL, &waiters);
			}
		}
		vslm_batch_rollback_reserved_pages(vslm, &ctx->batch);
		vslm_debug_validate_page_lists(vslm);
		pthread_mutex_unlock(&shard->lock);
		vslm_page_complete_waiters(&waiters);
		ctx->batch_active = false;
		vslm_exec_rw_async_finish(ctx, -EIO);
		return;
	}

	vslm_batch_commit_pages(vslm, &ctx->batch, true);
	for (i = 0; i < ctx->batch.nr_pages; i++) {
		struct vslm_page *p = ctx->batch.pages[i].page;
		uint8_t *frame = vslm->sram_buffer + (p->ppn * VSLM_PAGE_SIZE);

		vslm_page_take_waiters_locked(p, 0, p, &waiters);
		memcpy(ctx->buf + ctx->processed + (uint64_t)i * VSLM_PAGE_SIZE,
		       frame, VSLM_PAGE_SIZE);
	}
	vslm_debug_validate_page_lists(vslm);
	pthread_mutex_unlock(&shard->lock);
	vslm_page_complete_waiters(&waiters);

	ctx->processed += (uint64_t)ctx->batch.nr_pages * VSLM_PAGE_SIZE;
	ctx->batch_active = false;
	vslm_exec_rw_async_schedule_step(ctx);
}

/* Submit the coalesced readv from the run's source (cached exec_src channel). */
static int
vslm_exec_rw_async_submit_batch_readv(struct vslm_exec_rw_async_ctx *ctx)
{
	struct vbdev_vslm *vslm = ctx->vslm;
	struct spdk_bdev_desc *desc = NULL;
	struct spdk_io_channel *ch = NULL;
	uint64_t block_size;
	uint64_t page_blocks;
	uint64_t off_blocks;
	uint64_t num_blocks;
	int rc;

	block_size = spdk_bdev_get_block_size(ctx->batch_src_bdev);
	if (block_size == 0 || (VSLM_PAGE_SIZE % block_size) != 0 ||
	    (ctx->batch_src_off % block_size) != 0) {
		return -EINVAL;
	}
	page_blocks = VSLM_PAGE_SIZE / block_size;
	off_blocks = ctx->batch_src_off / block_size;
	num_blocks = (uint64_t)ctx->batch.nr_pages * page_blocks;

	rc = vslm_source_io_acquire(vslm, &ctx->exec_src, ctx->base_ch,
				    ctx->batch_src_bdev, &desc, &ch);
	if (rc != 0) {
		return rc;
	}

	if (!ctx->fault_in_counted) {
		VSLM_PERF_INC(vslm, fault_in_total);
		ctx->fault_start_ticks = spdk_get_ticks();
		ctx->fault_in_counted = true;
	}
	VSLM_PERF_INC(vslm, fault_batch_attempt_total);
	VSLM_PERF_INC(vslm, fault_in_batched_readv_total);

	return spdk_bdev_readv_blocks(desc, ch, ctx->batch.iov, (int)ctx->batch.nr_pages,
				      off_blocks, num_blocks,
				      vslm_exec_rw_async_fault_batch_complete, ctx);
}

static void
vslm_exec_rw_async_page_step(struct vslm_exec_rw_async_ctx *ctx)
{
	struct vbdev_vslm *vslm = ctx->vslm;
	struct vslm_page_waiter_list waiters;
	struct vslm_page *page;
	struct vslm_mmu_shard *shard;
	uint64_t lock_start_ticks = 0;
	uint64_t absolute;
	uint64_t vpn;
	uint64_t page_offset;
	uint64_t chunk;
	uint8_t *ptr;
	bool full_page_overwrite;
	bool submit_evict_writeback;
	int rc;

	TAILQ_INIT(&waiters);
	if (ctx->processed >= ctx->length) {
		vslm_exec_rw_async_finish(ctx, 0);
		return;
	}

	absolute = ctx->offset + ctx->processed;
	vpn = absolute / VSLM_PAGE_SIZE;
	page_offset = absolute % VSLM_PAGE_SIZE;
	chunk = spdk_min((uint64_t)(VSLM_PAGE_SIZE - page_offset),
			 ctx->length - ctx->processed);
	full_page_overwrite = ctx->is_write && page_offset == 0 && chunk == VSLM_PAGE_SIZE;
	shard = vslm_shard_for_vpn(vslm, vpn);

	VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
	pthread_mutex_lock(&shard->lock);
	lock_start_ticks = spdk_get_ticks();

	page = vslm_lookup_page(vslm, vpn);
	if (page != NULL) {
		if (page->load_state == VSLM_PAGE_LOAD_LOADING ||
		    page->load_state == VSLM_PAGE_LOAD_EVICTING) {
			rc = vslm_page_add_waiter_locked(vslm, page, ctx->worker_thread,
							 vslm_exec_rw_async_waiter_cb, ctx);
			vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
			if (rc != 0) {
				vslm_exec_rw_async_finish(ctx, rc);
			}
			return;
		}

		if (page->load_state != VSLM_PAGE_LOAD_RESIDENT) {
			rc = page->load_status != 0 ? page->load_status : -EIO;
			vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
			vslm_exec_rw_async_finish(ctx, rc);
			return;
		}

		vslm_page_pin_locked(vslm, page);
		ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset;
		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);

		if (ctx->is_write) {
			vslm_timed_memcpy(vslm, ptr, ctx->buf + ctx->processed, chunk);
		} else {
			vslm_timed_memcpy(vslm, ctx->buf + ctx->processed, ptr, chunk);
		}

		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		lock_start_ticks = spdk_get_ticks();
		vslm_page_unpin_locked(vslm, page, ctx->is_write, false);
		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);

		ctx->processed += chunk;
		vslm_exec_rw_async_schedule_step(ctx);
		return;
	}

	/*
	 * Bulk read-fault coalescing: a contiguous run of clean-backed pages (default
	 * backing or same-source contiguous aliases) is faulted in ONE readv instead of
	 * one read per page. Read-only, page-aligned, full-page chunks; the run is held
	 * within one MMU shard so the held lock covers reserve and the iov build. On a
	 * run of <= 1 page we roll back and fall through to the single-page path.
	 */
	if (!ctx->is_write && page_offset == 0 && chunk == VSLM_PAGE_SIZE &&
	    (ctx->length - ctx->processed) >= VSLM_PAGE_SIZE &&
	    vslm->fault_batch_enabled) {
		struct spdk_bdev *rsb = NULL;
		uint64_t rso = 0;
		uint64_t rem_pages = (ctx->length - ctx->processed) / VSLM_PAGE_SIZE;
		uint64_t shard_room = VSLM_MMU_SHARD_STRIDE_PAGES -
				      (vpn % VSLM_MMU_SHARD_STRIDE_PAGES);
		uint32_t by_bytes = vslm->fault_batch_max_bytes / VSLM_PAGE_SIZE;
		uint32_t max_run = vslm->fault_batch_pages;
		uint32_t run_len;

		if (by_bytes == 0) {
			by_bytes = 1;
		}
		if (max_run > by_bytes) {
			max_run = by_bytes;
		}
		if ((uint64_t)max_run > rem_pages) {
			max_run = (uint32_t)rem_pages;
		}
		if ((uint64_t)max_run > shard_room) {
			max_run = (uint32_t)shard_room;
		}

		run_len = vslm_find_clean_batch_run(vslm, vpn, max_run, &rsb, &rso);
		if (run_len > 1) {
			ctx->batch.vslm = vslm;
			ctx->batch.base_ch = ctx->base_ch;
			ctx->batch.start_vpn = vpn;
			ctx->batch.pages = ctx->batch_pages;
			ctx->batch.iov = ctx->batch_iov;
			ctx->batch.bounce_buf = NULL;
			ctx->batch.used_bounce = false;
			ctx->batch.nr_pages = 0;
			memset(ctx->batch_pages, 0, run_len * sizeof(ctx->batch_pages[0]));

			rc = vslm_batch_reserve_free_pages(vslm, shard, vpn, run_len, &ctx->batch);
			if (rc == 0 && ctx->batch.nr_pages > 1) {
				vslm_batch_build_iov(vslm, &ctx->batch);
				ctx->batch_src_bdev = rsb;
				ctx->batch_src_off = rso;
				ctx->batch_active = true;
				vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);

				rc = vslm_exec_rw_async_submit_batch_readv(ctx);
				if (rc != 0) {
					uint32_t i;

					VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
					pthread_mutex_lock(&shard->lock);
					lock_start_ticks = spdk_get_ticks();
					for (i = 0; i < ctx->batch.nr_pages; i++) {
						struct vslm_page *p = ctx->batch.pages[i].page;

						if (p != NULL) {
							vslm_page_take_waiters_locked(p, rc, NULL,
										      &waiters);
						}
					}
					vslm_batch_rollback_reserved_pages(vslm, &ctx->batch);
					vslm_debug_validate_page_lists(vslm);
					vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
					vslm_page_complete_waiters(&waiters);
					ctx->batch_active = false;
					vslm_exec_rw_async_finish(ctx, rc);
				}
				return;
			}

			/* Reserved 0 or 1 page: roll back and fall through (lock still held). */
			if (ctx->batch.nr_pages > 0) {
				vslm_batch_rollback_reserved_pages(vslm, &ctx->batch);
			}
			ctx->batch_active = false;
		}
	}

	rc = vslm_reserve_page_for_fault_locked(vslm, vpn, ctx->is_write,
						full_page_overwrite, &ctx->fault_ctx);
	if (rc != 0) {
		if (rc == -ENOSPC) {
			rc = vslm_exec_rw_async_prepare_eviction_locked(ctx,
					&submit_evict_writeback);
			vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
			if (rc != 0) {
				vslm_exec_rw_async_finish(ctx, rc);
				return;
			}
			if (submit_evict_writeback) {
				rc = vslm_exec_rw_async_submit_evict_writeback(ctx);
				if (rc != 0) {
					vslm_exec_rw_async_restore_pending_eviction(ctx, rc);
					vslm_exec_rw_async_finish(ctx, rc);
				}
				return;
			}
			vslm_exec_rw_async_schedule_step(ctx);
			return;
		}

		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
		if (rc == -EEXIST) {
			vslm_exec_rw_async_schedule_step(ctx);
		} else {
			vslm_exec_rw_async_finish(ctx, rc);
		}
		return;
	}

	if (!ctx->fault_ctx.needs_fault_in) {
		rc = vslm_commit_reserved_page_locked(vslm, &ctx->fault_ctx, true);
		if (rc != 0) {
			vslm_page_take_waiters_locked(ctx->fault_ctx.page, rc, NULL, &waiters);
			vslm_rollback_reserved_page_locked(vslm, &ctx->fault_ctx);
			vslm_debug_validate_page_lists(vslm);
			vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
			vslm_page_complete_waiters(&waiters);
			vslm_exec_rw_async_finish(ctx, rc);
			return;
		}

		/*
		 * Full-page overwrite: write the data into the freshly committed page
		 * NOW, holding a pin across the memcpy, instead of committing it clean
		 * and rescheduling the write to a later step. Otherwise the still-clean
		 * page is briefly resident and unpinned, and a concurrent fault's
		 * vslm_pick_victim() can clean-evict (steal) it before it is dirtied --
		 * which livelocks under sustained eviction pressure (peers keep
		 * evicting each other's just-faulted pages, near-zero forward progress).
		 */
		page = ctx->fault_ctx.page;
		vslm_page_take_waiters_locked(page, 0, page, &waiters);
		vslm_page_pin_locked(vslm, page);
		ptr = vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset;
		vslm_debug_validate_page_lists(vslm);
		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
		vslm_page_complete_waiters(&waiters);

		memcpy(ptr, ctx->buf + ctx->processed, chunk);

		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		lock_start_ticks = spdk_get_ticks();
		vslm_page_unpin_locked(vslm, page, true, false);
		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);

		ctx->processed += chunk;
		vslm_exec_rw_async_schedule_step(ctx);
		return;
	}

	vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
	rc = vslm_exec_rw_async_submit_source_fault(ctx);
	if (rc != 0) {
		VSLM_PERF_INC(vslm, mmu_lock_acquire_total);
		pthread_mutex_lock(&shard->lock);
		lock_start_ticks = spdk_get_ticks();
		vslm_page_take_waiters_locked(ctx->fault_ctx.page, rc, NULL, &waiters);
		vslm_rollback_reserved_page_locked(vslm, &ctx->fault_ctx);
		vslm_debug_validate_page_lists(vslm);
		vslm_mmu_unlock_measured(vslm, shard, lock_start_ticks);
		vslm_page_complete_waiters(&waiters);
		vslm_exec_rw_async_finish(ctx, rc);
	}
}

static void
vslm_exec_rw_async_step(void *arg)
{
	struct vslm_exec_rw_async_ctx *ctx = arg;

	if (ctx->processed >= ctx->length) {
		vslm_exec_rw_async_finish(ctx, 0);
		return;
	}

	if (ctx->vslm->async_exec_enabled) {
		vslm_exec_rw_async_page_step(ctx);
		return;
	}

	vslm_exec_rw_async_legacy_step(ctx);
}

static int
vslm_exec_rw_async_submit(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
			  void *buf, bool is_write,
			  spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	struct vslm_exec_rw_async_ctx *ctx;
	struct vbdev_vslm *vslm;
	struct spdk_thread *thread;
	int rc;

	thread = spdk_get_thread();
	if (thread == NULL) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_bdev(bdev);
	if (vslm == NULL) {
		return -ENOTSUP;
	}
	if (offset >= vslm->virtual_size_bytes ||
	    length > vslm->virtual_size_bytes - offset) {
		return -EINVAL;
	}
	if (!vslm_try_get_io_ref(vslm)) {
		return -ESHUTDOWN;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		vslm_put_io_ref(vslm);
		return -ENOMEM;
	}

	ctx->submit_thread = thread;
	ctx->worker_thread = thread;
	ctx->bdev = bdev;
	ctx->offset = offset;
	ctx->length = length;
	ctx->processed = 0;
	ctx->buf = buf;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->vslm = vslm;
	ctx->is_write = is_write;

	if (vslm->async_exec_enabled) {
		ctx->base_ch = spdk_bdev_get_io_channel(vslm->base_desc);
		if (ctx->base_ch == NULL) {
			vslm_put_io_ref(vslm);
			free(ctx);
			return -ENOMEM;
		}
	}

	rc = spdk_thread_send_msg(ctx->worker_thread, vslm_exec_rw_async_step, ctx);
	if (rc != 0) {
		if (ctx->base_ch != NULL) {
			spdk_put_io_channel(ctx->base_ch);
		}
		vslm_put_io_ref(vslm);
		free(ctx);
		return rc;
	}

	return 0;
}

static int
vslm_exec_read_async_submit(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
			    void *buf, spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	return vslm_exec_rw_async_submit(bdev, offset, length, buf, false, cb_fn, cb_arg);
}

static int
vslm_exec_write_async_submit(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
			     const void *buf, spdk_bdev_slm_io_completion_cb cb_fn,
			     void *cb_arg)
{
	return vslm_exec_rw_async_submit(bdev, offset, length, (void *)(uintptr_t)buf, true,
					 cb_fn, cb_arg);
}

static void
vslm_mem_async_complete_msg(void *arg)
{
	struct vslm_mem_async_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, ctx->status);
	free(ctx);
}

static void
vslm_mem_async_finish(struct vslm_mem_async_ctx *ctx, int status)
{
	int rc;

	ctx->status = status;
	if (ctx->submit_thread == NULL || ctx->submit_thread == spdk_get_thread()) {
		ctx->cb_fn(ctx->cb_arg, ctx->status);
		free(ctx);
		return;
	}

	rc = spdk_thread_send_msg(ctx->submit_thread, vslm_mem_async_complete_msg, ctx);
	if (rc != 0) {
		ctx->cb_fn(ctx->cb_arg, rc);
		free(ctx);
	}
}

static void
vslm_mem_async_run(void *arg)
{
	struct vslm_mem_async_ctx *ctx = arg;
	int rc = 0;

	switch (ctx->op) {
	case VSLM_MEM_ASYNC_READ:
		rc = vbdev_vslm_mem_read_by_bdev(ctx->bdev, ctx->offset, ctx->length, ctx->buf);
		break;
	case VSLM_MEM_ASYNC_WRITE:
		rc = vbdev_vslm_mem_write_by_bdev(ctx->bdev, ctx->offset, ctx->length, ctx->buf);
		break;
	case VSLM_MEM_ASYNC_COPY:
		rc = vbdev_vslm_mem_copy_by_bdev(ctx->dst_bdev, ctx->dst_offset,
						 ctx->src_bdev, ctx->src_offset, ctx->length);
		break;
	case VSLM_MEM_ASYNC_EXEC_READ:
		rc = vbdev_vslm_mem_exec_read_by_bdev(ctx->bdev, ctx->offset, ctx->length, ctx->buf);
		break;
	case VSLM_MEM_ASYNC_EXEC_WRITE:
		rc = vbdev_vslm_mem_exec_write_by_bdev(ctx->bdev, ctx->offset, ctx->length, ctx->buf);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	vslm_mem_async_finish(ctx, rc);
}

static int
vslm_mem_async_submit(struct vslm_mem_async_ctx *ctx)
{
	int rc;

	/* Remove after solved problem: bridge sync vSLM internals behind async provider APIs. */
	ctx->submit_thread = spdk_get_thread();
	if (ctx->submit_thread == NULL) {
		free(ctx);
		return -EINVAL;
	}

	rc = spdk_thread_send_msg(ctx->submit_thread, vslm_mem_async_run, ctx);
	if (rc != 0) {
		free(ctx);
		return rc;
	}

	return 0;
}

static int
vbdev_vslm_mem_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
				  void *buf, spdk_bdev_slm_io_completion_cb cb_fn, void *cb_arg)
{
	struct vslm_mem_async_ctx *ctx;

	if (cb_fn == NULL || bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->op = VSLM_MEM_ASYNC_READ;
	ctx->bdev = bdev;
	ctx->offset = offset;
	ctx->length = length;
	ctx->buf = buf;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	return vslm_mem_async_submit(ctx);
}

static int
vbdev_vslm_mem_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset, uint64_t length, void *buf)
{
	return vslm_rw_by_bdev(bdev, offset, length, buf, false, false);
}

static int
vbdev_vslm_mem_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
				   const void *buf, spdk_bdev_slm_io_completion_cb cb_fn,
				   void *cb_arg)
{
	struct vslm_mem_async_ctx *ctx;

	if (cb_fn == NULL || bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->op = VSLM_MEM_ASYNC_WRITE;
	ctx->bdev = bdev;
	ctx->offset = offset;
	ctx->length = length;
	ctx->buf = (void *)(uintptr_t)buf;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	return vslm_mem_async_submit(ctx);
}

static int
vbdev_vslm_mem_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
			     uint64_t length, const void *buf)
{
	return vslm_rw_by_bdev(bdev, offset, length, (void *)(uintptr_t)buf, true, false);
}

static int
vbdev_vslm_mem_copy_by_bdev_async(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
				  struct spdk_bdev *src_bdev, uint64_t src_offset,
				  uint64_t length, spdk_bdev_slm_io_completion_cb cb_fn,
				  void *cb_arg)
{
	struct vslm_mem_async_ctx *ctx;

	if (cb_fn == NULL || dst_bdev == NULL || src_bdev == NULL) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->op = VSLM_MEM_ASYNC_COPY;
	ctx->dst_bdev = dst_bdev;
	ctx->dst_offset = dst_offset;
	ctx->src_bdev = src_bdev;
	ctx->src_offset = src_offset;
	ctx->length = length;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	return vslm_mem_async_submit(ctx);
}

static int
vbdev_vslm_mem_copy_by_bdev(struct spdk_bdev *dst_bdev, uint64_t dst_offset,
			    struct spdk_bdev *src_bdev, uint64_t src_offset,
			    uint64_t length)
{
	struct vbdev_vslm *vslm;
	uint64_t src_block_size;
	uint64_t src_num_blocks;
	uint64_t src_offset_blocks;
	uint64_t src_required_blocks;
	int rc;

	if (dst_bdev == NULL || src_bdev == NULL) {
		return -EINVAL;
	}

	if (length == 0) {
		return 0;
	}
	SPDK_DEBUGLOG(vslm, "vSLM mem-copy req dst_bdev=%s src_bdev=%s dst_off=%" PRIu64
		      " src_off=%" PRIu64 " len=%" PRIu64 "\n",
		      dst_bdev->name, src_bdev->name, dst_offset, src_offset, length);

	if (src_offset > UINT64_MAX - length) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_bdev(dst_bdev);
	if (vslm == NULL) {
		return -ENOTSUP;
	}

	if (dst_offset > vslm->virtual_size_bytes ||
	    length > vslm->virtual_size_bytes - dst_offset) {
		return -EINVAL;
	}

	if (vslm_has_lease_conflict(vslm, dst_offset, length)) {
		__atomic_add_fetch(&vslm->stats.vslm_host_write_conflict_total, 1, __ATOMIC_RELAXED);
		vslm_trace_emitf(vslm, "lease_conflict",
				 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"rejected\"",
				 dst_offset, length);
		vslm_trace_emitf(vslm, "host_write_conflict",
				 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"rejected\"",
				 dst_offset, length);
		return -EAGAIN;
	}
	__atomic_add_fetch(&vslm->stats.vslm_host_write_nonconflict_total, 1, __ATOMIC_RELAXED);
	vslm_trace_emitf(vslm, "host_write_nonconflict",
			 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"allowed\"",
			 dst_offset, length);

	/* Alias staging requires full-page, block-aligned copies. */
	if ((dst_offset % VSLM_PAGE_SIZE) != 0 ||
	    (src_offset % VSLM_PAGE_SIZE) != 0 ||
	    (length % VSLM_PAGE_SIZE) != 0) {
		return -ENOTSUP;
	}

	src_block_size = spdk_bdev_get_block_size(src_bdev);
	if (src_block_size == 0 ||
	    (VSLM_PAGE_SIZE % src_block_size) != 0 ||
	    (src_offset % src_block_size) != 0 ||
	    (length % src_block_size) != 0) {
		return -ENOTSUP;
	}

	src_num_blocks = spdk_bdev_get_num_blocks(src_bdev);
	src_offset_blocks = src_offset / src_block_size;
	src_required_blocks = length / src_block_size;

	if (src_offset_blocks > src_num_blocks ||
	    src_required_blocks > src_num_blocks - src_offset_blocks) {
		return -EINVAL;
	}

	rc = vslm_stage_alias_range(vslm, dst_offset, length, src_bdev, src_offset);
	SPDK_DEBUGLOG(vslm, "vSLM mem-copy result dst_bdev=%s src_bdev=%s rc=%d\n",
		      dst_bdev->name, src_bdev->name, rc);
	return rc;
}

static int
vbdev_vslm_mem_exec_read_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
				       void *buf, spdk_bdev_slm_io_completion_cb cb_fn,
				       void *cb_arg)
{
	if (cb_fn == NULL || bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return 0;
	}

	return vslm_exec_read_async_submit(bdev, offset, length, buf, cb_fn, cb_arg);
}

static int
vbdev_vslm_mem_exec_read_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				 uint64_t length, void *buf)
{
	int rc;

	rc = vslm_rw_by_bdev(bdev, offset, length, buf, false, true);
	return rc;
}

static int
vbdev_vslm_mem_exec_write_by_bdev_async(struct spdk_bdev *bdev, uint64_t offset, uint64_t length,
					const void *buf, spdk_bdev_slm_io_completion_cb cb_fn,
					void *cb_arg)
{
	if (cb_fn == NULL || bdev == NULL || (length != 0 && buf == NULL)) {
		return -EINVAL;
	}

	if (length == 0) {
		cb_fn(cb_arg, 0);
		return 0;
	}

	return vslm_exec_write_async_submit(bdev, offset, length, buf, cb_fn, cb_arg);
}

static int
vbdev_vslm_mem_exec_write_by_bdev(struct spdk_bdev *bdev, uint64_t offset,
				  uint64_t length, const void *buf)
{
	int rc;

	rc = vslm_rw_by_bdev(bdev, offset, length, (void *)(uintptr_t)buf, true, true);
	return rc;
}

int
bdev_vslm_read_by_nsid(uint32_t nsid, uint64_t offset, uint64_t length, void *buf)
{
	return vslm_rw_by_nsid(nsid, offset, length, buf, false, false);
}

int
bdev_vslm_write_by_nsid(uint32_t nsid, uint64_t offset, uint64_t length, const void *buf)
{
	return vslm_rw_by_nsid(nsid, offset, length, (void *)(uintptr_t)buf, true, false);
}

static int
vbdev_vslm_lease_acquire_by_vslm(uint64_t lease_id, struct vbdev_vslm *vslm,
				 uint64_t offset, uint64_t length)
{
	struct vslm_lease *lease, *new_lease;
	uint64_t new_page_start;
	uint64_t new_page_end;
	uint64_t lease_page_start;
	uint64_t lease_page_end;

	if (lease_id == 0 || vslm == NULL || length == 0) {
		return -EINVAL;
	}

	if (offset >= vslm->virtual_size_bytes ||
	    length > vslm->virtual_size_bytes - offset) {
		return -EINVAL;
	}

	new_page_start = (offset / VSLM_PAGE_SIZE) * VSLM_PAGE_SIZE;
	new_page_end = ((offset + length - 1) / VSLM_PAGE_SIZE + 1) * VSLM_PAGE_SIZE;

	pthread_mutex_lock(&vslm->lease_lock);
	TAILQ_FOREACH(lease, &vslm->leases, link) {
		if (lease->lease_id == lease_id &&
		    lease->offset == offset &&
		    lease->length == length) {
			pthread_mutex_unlock(&vslm->lease_lock);
			return 0;
		}

		lease_page_start = (lease->offset / VSLM_PAGE_SIZE) * VSLM_PAGE_SIZE;
		lease_page_end = ((lease->offset + lease->length - 1) / VSLM_PAGE_SIZE + 1) *
				 VSLM_PAGE_SIZE;
		if (lease->lease_id != lease_id &&
		    vslm_range_overlap(new_page_start, new_page_end - new_page_start,
				       lease_page_start, lease_page_end - lease_page_start)) {
			vslm_trace_emitf(vslm, "lease_conflict",
					 ",\"exec_id\":\"%" PRIu64 "\",\"range_start\":%" PRIu64
					 ",\"range_len\":%" PRIu64 ",\"result\":\"acquire_conflict\"",
					 lease_id, offset, length);
			pthread_mutex_unlock(&vslm->lease_lock);
			return -EAGAIN;
		}
	}

	if (!vslm_admission_allow_execute(vslm)) {
		pthread_mutex_unlock(&vslm->lease_lock);
		return -EBUSY;
	}

	new_lease = calloc(1, sizeof(*new_lease));
	if (new_lease == NULL) {
		pthread_mutex_unlock(&vslm->lease_lock);
		return -ENOMEM;
	}

	new_lease->lease_id = lease_id;
	new_lease->offset = offset;
	new_lease->length = length;
	TAILQ_INSERT_TAIL(&vslm->leases, new_lease, link);
	__atomic_add_fetch(&vslm->stats.vslm_lease_create_total, 1, __ATOMIC_RELAXED);
	vslm_trace_emitf(vslm, "lease_create",
			 ",\"exec_id\":\"%" PRIu64 "\",\"range_start\":%" PRIu64
			 ",\"range_len\":%" PRIu64 ",\"result\":\"ok\"",
			 lease_id, offset, length);
	vslm_trace_emitf(vslm, "exec_start",
			 ",\"exec_id\":\"%" PRIu64 "\",\"range_start\":%" PRIu64
			 ",\"range_len\":%" PRIu64 ",\"result\":\"started\"",
			 lease_id, offset, length);
	pthread_mutex_unlock(&vslm->lease_lock);
	return 0;
}

static int
vbdev_vslm_mem_lease_acquire_by_bdev(uint64_t lease_id, struct spdk_bdev *bdev,
				     uint64_t offset, uint64_t length)
{
	struct vbdev_vslm *vslm;

	if (lease_id == 0 || bdev == NULL || length == 0) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_bdev(bdev);
	if (vslm == NULL) {
		return -ENOTSUP;
	}

	return vbdev_vslm_lease_acquire_by_vslm(lease_id, vslm, offset, length);
}

int
bdev_vslm_lease_acquire(uint64_t lease_id, uint32_t nsid,
			uint64_t offset, uint64_t length)
{
	struct vbdev_vslm *vslm;

	if (lease_id == 0 || nsid == 0 || length == 0) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_nsid(nsid);
	if (vslm == NULL) {
		return -ENOENT;
	}

	return vbdev_vslm_lease_acquire_by_vslm(lease_id, vslm, offset, length);
}

static int
vslm_collect_lease_ranges(struct vbdev_vslm *vslm, uint64_t lease_id,
			  struct vslm_lease_range **ranges_out, size_t *count_out)
{
	struct vslm_lease *lease;
	struct vslm_lease_range *ranges;
	size_t i = 0;
	size_t count = 0;

	*ranges_out = NULL;
	*count_out = 0;

	pthread_mutex_lock(&vslm->lease_lock);
	TAILQ_FOREACH(lease, &vslm->leases, link) {
		if (lease->lease_id == lease_id) {
			count++;
		}
	}
	if (count == 0) {
		pthread_mutex_unlock(&vslm->lease_lock);
		return -ENOENT;
	}

	ranges = calloc(count, sizeof(*ranges));
	if (ranges == NULL) {
		pthread_mutex_unlock(&vslm->lease_lock);
		return -ENOMEM;
	}

	TAILQ_FOREACH(lease, &vslm->leases, link) {
		if (lease->lease_id == lease_id) {
			ranges[i].offset = lease->offset;
			ranges[i].length = lease->length;
			i++;
		}
	}
	pthread_mutex_unlock(&vslm->lease_lock);

	*ranges_out = ranges;
	*count_out = count;
	return 0;
}

static int
vslm_publish_lease_ranges(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
			  struct vslm_lease_range *ranges, size_t range_count)
{
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	uint64_t start_vpn, end_vpn;
	size_t i;
	uint64_t vpn;
	int rc = 0;

	for (i = 0; i < range_count; i++) {
		start_vpn = ranges[i].offset / VSLM_PAGE_SIZE;
		end_vpn = (ranges[i].offset + ranges[i].length - 1) / VSLM_PAGE_SIZE;
		for (vpn = start_vpn; vpn <= end_vpn; vpn++) {
			struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, vpn);

			pthread_mutex_lock(&shard->lock);
			lpage = vslm_lookup_lpage(vslm, vpn);
			if (lpage == NULL ||
			    lpage->state != SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT ||
			    !lpage->pending_publish) {
				pthread_mutex_unlock(&shard->lock);
				continue;
			}

			page = vslm_lookup_page(vslm, vpn);
			if (page == NULL || !page->dirty) {
				pthread_mutex_unlock(&shard->lock);
				continue;
			}

			/*
			 * vslm_writeback_page drops shard->lock across its sync
			 * backing write; mark the page busy first so it cannot be
			 * picked as an eviction victim during that window (the
			 * cleaner path relies on the same is_busy guard).
			 */
			page->is_busy = true;
			rc = vslm_writeback_page(vslm, base_ch, page, false);
			page->is_busy = false;
			pthread_mutex_unlock(&shard->lock);
			if (rc != 0) {
				break;
			}
		}
		if (rc != 0) {
			break;
		}
	}

	return rc;
}

static int
vslm_discard_lease_ranges(struct vbdev_vslm *vslm,
			  struct vslm_lease_range *ranges, size_t range_count)
{
	struct vslm_page *page;
	struct vslm_lpage *lpage;
	uint64_t start_vpn, end_vpn;
	size_t i;
	uint64_t vpn;

	for (i = 0; i < range_count; i++) {
		start_vpn = ranges[i].offset / VSLM_PAGE_SIZE;
		end_vpn = (ranges[i].offset + ranges[i].length - 1) / VSLM_PAGE_SIZE;
		for (vpn = start_vpn; vpn <= end_vpn; vpn++) {
			struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, vpn);

			pthread_mutex_lock(&shard->lock);
			lpage = vslm_lookup_lpage(vslm, vpn);
			if (lpage == NULL || !lpage->pending_publish) {
				pthread_mutex_unlock(&shard->lock);
				continue;
			}

			page = vslm_lookup_page(vslm, vpn);
			if (page != NULL) {
				vslm_hash_remove(vslm, page);
				if (page->location == VSLM_PAGE_LOC_LRU) {
					vslm_lru_remove(vslm, page);
				}
				vslm_page_prepare_free(page);
				vslm_free_list_insert_tail(vslm, page);
				vslm_note_resident_remove(vslm);
			}

			if (lpage->has_superseded_source && lpage->superseded_source_bdev != NULL) {
				lpage->source_type = lpage->superseded_source_type;
				lpage->source_bdev = lpage->superseded_source_bdev;
				lpage->source_offset_bytes = lpage->superseded_source_offset_bytes;
			} else {
				vslm_lpage_set_backing_source(vslm, lpage);
			}

			lpage->pending_publish = false;
			lpage->private_authoritative = false;
			vslm_lpage_clear_superseded_source(lpage);

			if (lpage->source_type == SPDK_BDEV_VSLM_PAGE_SOURCE_ALIAS_BDEV) {
				vslm_lpage_set_state(vslm, NULL, lpage, SPDK_BDEV_VSLM_LPAGE_CLEAN_ALIAS);
			} else {
				if (lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT) {
					vslm_lpage_set_state(vslm, NULL, lpage,
							     SPDK_BDEV_VSLM_LPAGE_CLEAN_RESIDENT);
				}
				vslm_lpage_delete(vslm, lpage);
			}
			pthread_mutex_unlock(&shard->lock);
		}
	}

	return 0;
}

static int
vbdev_vslm_exec_lease_op(uint64_t lease_id, bool publish)
{
	struct vbdev_vslm *vslm;
	struct vbdev_vslm **snapshot;
	struct vslm_lease_range *ranges = NULL;
	struct spdk_io_channel *base_ch;
	size_t range_count;
	size_t snap_count;
	size_t snap_i;
	bool found = false;
	int rc, op_rc, first_err = 0;

	if (lease_id == 0) {
		return -EINVAL;
	}

	snapshot = vbdev_vslm_snapshot_bdevs(&snap_count);
	for (snap_i = 0; snap_i < snap_count; snap_i++) {
		vslm = snapshot[snap_i];
		rc = vslm_collect_lease_ranges(vslm, lease_id, &ranges, &range_count);
		if (rc == -ENOENT) {
			continue;
		}
		if (rc != 0) {
			if (first_err == 0) {
				first_err = rc;
			}
			continue;
		}

		found = true;
		if (publish) {
			base_ch = spdk_bdev_get_io_channel(vslm->base_desc);
			if (base_ch == NULL) {
				op_rc = -ENOMEM;
			} else {
				op_rc = vslm_publish_lease_ranges(vslm, base_ch, ranges, range_count);
				spdk_put_io_channel(base_ch);
			}
		} else {
			op_rc = vslm_discard_lease_ranges(vslm, ranges, range_count);
		}

		free(ranges);
		ranges = NULL;
		if (op_rc == 0) {
			if (publish) {
				__atomic_add_fetch(&vslm->stats.vslm_publish_total, 1, __ATOMIC_RELAXED);
				vslm_trace_emitf(vslm, "publish",
						 ",\"exec_id\":\"%" PRIu64 "\",\"result\":\"ok\"",
						 lease_id);
				vslm_trace_emitf(vslm, "exec_success",
						 ",\"exec_id\":\"%" PRIu64 "\",\"result\":\"published\"",
						 lease_id);
			} else {
				__atomic_add_fetch(&vslm->stats.vslm_discard_total, 1, __ATOMIC_RELAXED);
				vslm_trace_emitf(vslm, "discard",
						 ",\"exec_id\":\"%" PRIu64 "\",\"result\":\"ok\"",
						 lease_id);
				vslm_trace_emitf(vslm, "exec_failure",
						 ",\"exec_id\":\"%" PRIu64 "\",\"result\":\"discarded\"",
						 lease_id);
			}
		} else {
			vslm_trace_emitf(vslm, "exec_failure",
					 ",\"exec_id\":\"%" PRIu64 "\",\"result\":\"%s\",\"counters\":{\"rc\":%d}",
					 lease_id, publish ? "publish_error" : "discard_error", op_rc);
		}
		if (op_rc != 0 && first_err == 0) {
			first_err = op_rc;
		}
	}
	free(snapshot);

	if (first_err != 0) {
		return first_err;
	}

	return found ? 0 : -ENOENT;
}

static int
vbdev_vslm_mem_exec_publish_lease(uint64_t lease_id)
{
	return vbdev_vslm_exec_lease_op(lease_id, true);
}

static int
vbdev_vslm_mem_exec_discard_lease(uint64_t lease_id)
{
	return vbdev_vslm_exec_lease_op(lease_id, false);
}

static int
vbdev_vslm_mem_lease_release(uint64_t lease_id)
{
	struct vbdev_vslm *vslm;
	struct vbdev_vslm **snapshot;
	struct vslm_lease *lease, *lease_tmp;
	size_t snap_count;
	size_t snap_i;
	bool found = false;
	bool released;
	uint64_t released_ranges;

	if (lease_id == 0) {
		return -EINVAL;
	}

	snapshot = vbdev_vslm_snapshot_bdevs(&snap_count);
	for (snap_i = 0; snap_i < snap_count; snap_i++) {
		vslm = snapshot[snap_i];
		released = false;
		released_ranges = 0;

		pthread_mutex_lock(&vslm->lease_lock);
		TAILQ_FOREACH_SAFE(lease, &vslm->leases, link, lease_tmp) {
			if (lease->lease_id == lease_id) {
				TAILQ_REMOVE(&vslm->leases, lease, link);
				free(lease);
				released = true;
				released_ranges++;
			}
		}
		pthread_mutex_unlock(&vslm->lease_lock);

		if (released) {
			found = true;
			__atomic_add_fetch(&vslm->stats.vslm_lease_release_total,
					   released_ranges, __ATOMIC_RELAXED);
			vslm_trace_emitf(vslm, "lease_release",
					 ",\"exec_id\":\"%" PRIu64 "\",\"result\":\"released\",\"counters\":{\"ranges\":%" PRIu64 "}",
					 lease_id, released_ranges);
			vslm_process_blocked_cmds(vslm);
		}
	}
	free(snapshot);

	if (!found) {
		return -ENOENT;
	}

	return 0;
}

int
bdev_vslm_lease_release(uint64_t lease_id)
{
	return vbdev_vslm_mem_lease_release(lease_id);
}

int
bdev_vslm_set_policy(const char *name, const struct spdk_bdev_vslm_policy *policy)
{
	struct vbdev_vslm *vslm;
	int rc;

	if (name == NULL || policy == NULL) {
		return -EINVAL;
	}

	rc = vslm_validate_policy(policy);
	if (rc != 0) {
		return rc;
	}

	vslm = vbdev_vslm_get_by_name(name);
	if (vslm == NULL) {
		return -ENOENT;
	}

	return vslm_apply_policy(vslm, policy);
}

/*
 * Re-shard an IDLE MMU to num_shards. Debug/benchmark only: the page_array and
 * SRAM buffer are kept; only the shard partitioning is rebuilt. Requires every
 * frame to be FREE (no resident/private/pinned pages). Caller holds policy_lock
 * and must have verified no active leases.
 */
static int
vslm_reshard_idle(struct vbdev_vslm *vslm, uint32_t num_shards)
{
	struct vslm_mmu_shard *old_shards;
	uint32_t old_num_shards;
	uint64_t i;
	int rc;

	if (num_shards > VSLM_MMU_MAX_SHARDS) {
		num_shards = VSLM_MMU_MAX_SHARDS;
	}
	if (num_shards < 1) {
		num_shards = 1;
	}
	if (num_shards == vslm->num_shards) {
		return 0;
	}

	/*
	 * Refuse to re-shard while any host/compute I/O is in flight: in-flight
	 * operations hold (or are about to take) the existing per-shard locks,
	 * which would be destroyed out from under them. Reuse the destruct drain
	 * counter (incremented by vslm_try_get_io_ref) in addition to the
	 * lease-empty check enforced by the caller.
	 */
	pthread_mutex_lock(&vslm->inflight_lock);
	if (vslm->inflight_io_count != 0) {
		pthread_mutex_unlock(&vslm->inflight_lock);
		return -EBUSY;
	}
	pthread_mutex_unlock(&vslm->inflight_lock);

	for (i = 0; i < vslm->num_sram_pages; i++) {
		struct vslm_page *page = &vslm->page_array[i];

		if (page->load_state != VSLM_PAGE_LOAD_FREE ||
		    page->location != VSLM_PAGE_LOC_FREE ||
		    page->on_hash || page->pin_count != 0) {
			return -EBUSY;
		}
	}

	/*
	 * Build the new shard array into a temporary and only swap it in on
	 * success. vslm_build_shards() builds into vslm->shards, so stash the
	 * live shards aside (NULLing them) before the build, then free them on
	 * success or restore them on failure -- the live bdev is never left with
	 * shards == NULL.
	 */
	old_shards = vslm->shards;
	old_num_shards = vslm->num_shards;
	vslm->shards = NULL;
	vslm->num_shards = 0;

	rc = vslm_build_shards(vslm, num_shards);
	if (rc != 0) {
		/* build failed and tore down its partial state; restore the old shards. */
		vslm->shards = old_shards;
		vslm->num_shards = old_num_shards;
		SPDK_ERRLOG("vslm: re-shard to %u failed; keeping existing %u shards\n",
			    num_shards, old_num_shards);
		return -ENOMEM;
	}

	/* New shards are live; tear down the old array. */
	if (old_shards != NULL) {
		uint32_t s;

		for (s = 0; s < old_num_shards; s++) {
			struct vslm_mmu_shard *shard = &old_shards[s];

			free(shard->hash_table);
			free(shard->lpage_hash_table);
			pthread_mutex_destroy(&shard->lock);
		}
		free(old_shards);
	}
	return 0;
}

static int
vslm_apply_debug(struct vbdev_vslm *vslm, const struct spdk_bdev_vslm_debug *dbg)
{
	int rc = 0;

	pthread_mutex_lock(&vslm->policy_lock);

	if (dbg->num_shards != 0 && dbg->num_shards != vslm->num_shards) {
		bool leases_empty;

		pthread_mutex_lock(&vslm->lease_lock);
		leases_empty = TAILQ_EMPTY(&vslm->leases);
		pthread_mutex_unlock(&vslm->lease_lock);
		if (!leases_empty) {
			rc = -EBUSY;
			goto out;
		}
		rc = vslm_reshard_idle(vslm, dbg->num_shards);
		if (rc != 0) {
			goto out;
		}
	}

	if (dbg->async_exec >= 0) {
		vslm->async_exec_enabled = (dbg->async_exec != 0);
	}
	if (dbg->fault_batch >= 0) {
		vslm->fault_batch_enabled = (dbg->fault_batch != 0);
	}
	if (dbg->prefetch_batch >= 0) {
		vslm->prefetch_batch_enabled = (dbg->prefetch_batch != 0);
	}
	if (dbg->background_cleaner >= 0) {
		vslm->background_cleaner_enabled = (dbg->background_cleaner != 0);
	}
	if (dbg->streaming_mode >= 0) {
		vslm->streaming_mode_enabled = (dbg->streaming_mode != 0);
	}
	if (dbg->force_dma_fallback >= 0) {
		/* Read lock-free on the I/O path; publish atomically. */
		__atomic_store_n(&vslm->force_dma_fallback,
				 (dbg->force_dma_fallback != 0), __ATOMIC_RELAXED);
	}
	if (dbg->disable_cow_bypass >= 0) {
		__atomic_store_n(&vslm->disable_cow_bypass,
				 (dbg->disable_cow_bypass != 0), __ATOMIC_RELAXED);
	}

	SPDK_NOTICELOG("vslm[%s] debug: num_shards=%u async=%d fault_batch=%d "
		       "prefetch_batch=%d cleaner=%d streaming=%d force_dma_fallback=%d "
		       "disable_cow_bypass=%d\n",
		       vslm->vbdev.name, vslm->num_shards, vslm->async_exec_enabled,
		       vslm->fault_batch_enabled, vslm->prefetch_batch_enabled,
		       vslm->background_cleaner_enabled, vslm->streaming_mode_enabled,
		       vslm->force_dma_fallback, vslm->disable_cow_bypass);
out:
	pthread_mutex_unlock(&vslm->policy_lock);
	return rc;
}

void
bdev_vslm_debug_init(struct spdk_bdev_vslm_debug *dbg)
{
	if (dbg == NULL) {
		return;
	}
	dbg->num_shards = 0;
	dbg->async_exec = -1;
	dbg->fault_batch = -1;
	dbg->prefetch_batch = -1;
	dbg->background_cleaner = -1;
	dbg->streaming_mode = -1;
	dbg->force_dma_fallback = -1;
	dbg->disable_cow_bypass = -1;
}

int
bdev_vslm_set_debug(const char *name, const struct spdk_bdev_vslm_debug *dbg)
{
	struct vbdev_vslm *vslm;

	if (name == NULL || dbg == NULL) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_name(name);
	if (vslm == NULL) {
		return -ENOENT;
	}

	return vslm_apply_debug(vslm, dbg);
}

int
bdev_vslm_get_policy(const char *name, struct spdk_bdev_vslm_policy *policy)
{
	struct vbdev_vslm *vslm;

	if (name == NULL || policy == NULL) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_name(name);
	if (vslm == NULL) {
		return -ENOENT;
	}

	pthread_mutex_lock(&vslm->policy_lock);
	*policy = vslm->policy;
	pthread_mutex_unlock(&vslm->policy_lock);
	return 0;
}

int
bdev_vslm_set_fdp_mode(const char *name, bool enabled, uint16_t dspec)
{
	struct vbdev_vslm *vslm;

	if (name == NULL) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_name(name);
	if (vslm == NULL) {
		return -ENOENT;
	}

	if (enabled && !vslm->base_fdp_supported) {
		return -ENOTSUP;
	}

	vslm->fdp_mode_enabled = enabled;
	vslm->fdp_dspec = dspec;
	return 0;
}

int
bdev_vslm_reset_stats(const char *name)
{
	struct vbdev_vslm *vslm;
	uint64_t dirty_pages;
	uint64_t resident_bytes_current;

	if (name == NULL) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_name(name);
	if (vslm == NULL) {
		return -ENOENT;
	}

	/*
	 * Keep dirty_resident_pages baseline without scanning all SRAM pages.
	 * Full walks under mmu_lock can stall I/O threads when SRAM is large.
	 */
	dirty_pages = __atomic_load_n(&vslm->stats.dirty_resident_pages, __ATOMIC_RELAXED);
	resident_bytes_current =
		__atomic_load_n(&vslm->stats.vslm_resident_bytes_current, __ATOMIC_RELAXED);
	memset(&vslm->stats, 0, sizeof(vslm->stats));
	memset(&vslm->perf_stats, 0, sizeof(vslm->perf_stats));
	pthread_mutex_lock(&vslm->policy_lock);
	vslm->admission_window_start_ticks = 0;
	vslm->admission_faults_in_window = 0;
	vslm->last_exec_read_vpn = UINT64_MAX;
	vslm->sequential_exec_read_count = 0;
	vslm->last_exec_read_stride = 0;
	vslm->strided_exec_read_count = 0;
	vslm->prefetch_until_vpn = 0;
	pthread_mutex_unlock(&vslm->policy_lock);
	/* last_batch_* are read lock-free by the prefetch path; publish atomically. */
	__atomic_store_n(&vslm->last_batch_start_vpn, UINT64_MAX, __ATOMIC_RELAXED);
	__atomic_store_n(&vslm->last_batch_pages, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&vslm->stats.dirty_resident_pages, dirty_pages, __ATOMIC_RELAXED);
	__atomic_store_n(&vslm->stats.vslm_resident_bytes_current,
			 resident_bytes_current, __ATOMIC_RELAXED);
	__atomic_store_n(&vslm->stats.vslm_resident_bytes_peak,
			 resident_bytes_current, __ATOMIC_RELAXED);
	return 0;
}

int
bdev_vslm_get_stats(const char *name, struct spdk_bdev_vslm_stats *stats)
{
	struct vbdev_vslm *vslm;

	if (name == NULL || stats == NULL) {
		return -EINVAL;
	}

	vslm = vbdev_vslm_get_by_name(name);
	if (vslm == NULL) {
		return -ENOENT;
	}

	stats->backing_write_ops = __atomic_load_n(&vslm->stats.backing_write_ops, __ATOMIC_RELAXED);
	stats->backing_write_bytes = __atomic_load_n(&vslm->stats.backing_write_bytes, __ATOMIC_RELAXED);
	stats->backing_write_tagged_bytes = __atomic_load_n(&vslm->stats.backing_write_tagged_bytes,
					    __ATOMIC_RELAXED);
	stats->backing_write_untagged_bytes = __atomic_load_n(&vslm->stats.backing_write_untagged_bytes,
					      __ATOMIC_RELAXED);
	stats->page_faults = __atomic_load_n(&vslm->stats.page_faults, __ATOMIC_RELAXED);
	stats->page_fault_bytes = __atomic_load_n(&vslm->stats.page_fault_bytes, __ATOMIC_RELAXED);
	stats->vslm_fault_clean_total =
		__atomic_load_n(&vslm->stats.vslm_fault_clean_total, __ATOMIC_RELAXED);
	stats->vslm_fault_private_total =
		__atomic_load_n(&vslm->stats.vslm_fault_private_total, __ATOMIC_RELAXED);
	stats->page_evictions = __atomic_load_n(&vslm->stats.page_evictions, __ATOMIC_RELAXED);
	stats->page_eviction_bytes = __atomic_load_n(&vslm->stats.page_eviction_bytes, __ATOMIC_RELAXED);
	stats->vslm_eviction_clean_total =
		__atomic_load_n(&vslm->stats.vslm_eviction_clean_total, __ATOMIC_RELAXED);
	stats->vslm_eviction_private_total =
		__atomic_load_n(&vslm->stats.vslm_eviction_private_total, __ATOMIC_RELAXED);
	stats->page_writebacks = __atomic_load_n(&vslm->stats.page_writebacks, __ATOMIC_RELAXED);
	stats->page_writeback_bytes = __atomic_load_n(&vslm->stats.page_writeback_bytes, __ATOMIC_RELAXED);
	stats->vslm_spill_write_bytes =
		__atomic_load_n(&vslm->stats.vslm_spill_write_bytes, __ATOMIC_RELAXED);
	stats->vslm_spill_read_bytes =
		__atomic_load_n(&vslm->stats.vslm_spill_read_bytes, __ATOMIC_RELAXED);
	stats->vslm_fast_tier_bytes = vslm->sram_size_bytes;
	stats->vslm_logical_working_set_bytes = vslm->virtual_size_bytes;
	stats->dirty_resident_pages = __atomic_load_n(&vslm->stats.dirty_resident_pages, __ATOMIC_RELAXED);
	stats->vslm_resident_bytes_current =
		__atomic_load_n(&vslm->stats.vslm_resident_bytes_current,
				__ATOMIC_RELAXED);
	stats->vslm_resident_bytes_peak =
		__atomic_load_n(&vslm->stats.vslm_resident_bytes_peak,
				__ATOMIC_RELAXED);
	stats->dirty_writeback_bytes = __atomic_load_n(&vslm->stats.dirty_writeback_bytes,
				       __ATOMIC_RELAXED);
	stats->lease_conflicts = __atomic_load_n(&vslm->stats.lease_conflicts, __ATOMIC_RELAXED);
	stats->lease_blocked_ns = __atomic_load_n(&vslm->stats.lease_blocked_ns, __ATOMIC_RELAXED);
	stats->vslm_host_write_blocked_ns_total =
		__atomic_load_n(&vslm->stats.vslm_host_write_blocked_ns_total, __ATOMIC_RELAXED);
	stats->admission_rejects = __atomic_load_n(&vslm->stats.admission_rejects, __ATOMIC_RELAXED);
	stats->vslm_lease_create_total =
		__atomic_load_n(&vslm->stats.vslm_lease_create_total, __ATOMIC_RELAXED);
	stats->vslm_lease_release_total =
		__atomic_load_n(&vslm->stats.vslm_lease_release_total, __ATOMIC_RELAXED);
	stats->vslm_host_read_during_execution_total =
		__atomic_load_n(&vslm->stats.vslm_host_read_during_execution_total,
				__ATOMIC_RELAXED);
	stats->vslm_host_write_conflict_total =
		__atomic_load_n(&vslm->stats.vslm_host_write_conflict_total,
				__ATOMIC_RELAXED);
	stats->vslm_host_write_blocked_total =
		__atomic_load_n(&vslm->stats.vslm_host_write_blocked_total,
				__ATOMIC_RELAXED);
	stats->vslm_host_write_nonconflict_total =
		__atomic_load_n(&vslm->stats.vslm_host_write_nonconflict_total,
				__ATOMIC_RELAXED);
	stats->vslm_publish_total =
		__atomic_load_n(&vslm->stats.vslm_publish_total, __ATOMIC_RELAXED);
	stats->vslm_discard_total =
		__atomic_load_n(&vslm->stats.vslm_discard_total, __ATOMIC_RELAXED);
	stats->vslm_visibility_violation_total =
		__atomic_load_n(&vslm->stats.vslm_visibility_violation_total,
				__ATOMIC_RELAXED);
	stats->perf_fault_in_io_ns_total =
		__atomic_load_n(&vslm->perf_stats.fault_in_io_ns_total, __ATOMIC_RELAXED);
	stats->perf_fault_in_io_ns_max =
		__atomic_load_n(&vslm->perf_stats.fault_in_io_ns_max, __ATOMIC_RELAXED);
	stats->perf_mmu_lock_acquire_total =
		__atomic_load_n(&vslm->perf_stats.mmu_lock_acquire_total, __ATOMIC_RELAXED);
	stats->perf_mmu_lock_hold_ns_total =
		__atomic_load_n(&vslm->perf_stats.mmu_lock_hold_ns_total, __ATOMIC_RELAXED);
	stats->perf_mmu_lock_hold_ns_max =
		__atomic_load_n(&vslm->perf_stats.mmu_lock_hold_ns_max, __ATOMIC_RELAXED);
	stats->perf_sync_base_io_total =
		__atomic_load_n(&vslm->perf_stats.sync_base_io_total, __ATOMIC_RELAXED);
	stats->perf_sync_base_io_ns_total =
		__atomic_load_n(&vslm->perf_stats.sync_base_io_ns_total, __ATOMIC_RELAXED);
	stats->perf_sync_base_io_ns_max =
		__atomic_load_n(&vslm->perf_stats.sync_base_io_ns_max, __ATOMIC_RELAXED);
	stats->perf_sync_read_desc_io_total =
		__atomic_load_n(&vslm->perf_stats.sync_read_desc_io_total, __ATOMIC_RELAXED);
	stats->perf_sync_read_desc_io_ns_total =
		__atomic_load_n(&vslm->perf_stats.sync_read_desc_io_ns_total, __ATOMIC_RELAXED);
	stats->perf_sync_read_desc_io_ns_max =
		__atomic_load_n(&vslm->perf_stats.sync_read_desc_io_ns_max, __ATOMIC_RELAXED);
	stats->perf_fault_batch_attempt_total =
		__atomic_load_n(&vslm->perf_stats.fault_batch_attempt_total, __ATOMIC_RELAXED);
	stats->perf_fault_batch_fallback_total =
		__atomic_load_n(&vslm->perf_stats.fault_batch_fallback_total, __ATOMIC_RELAXED);
	stats->perf_prefetch_async_wait_total =
		__atomic_load_n(&vslm->perf_stats.prefetch_async_wait_total, __ATOMIC_RELAXED);
	stats->perf_prefetch_async_wait_ns_total =
		__atomic_load_n(&vslm->perf_stats.prefetch_async_wait_ns_total, __ATOMIC_RELAXED);
	return 0;
}

static const struct spdk_bdev_fn_table vbdev_vslm_fn_table = {
	.destruct = vbdev_vslm_destruct,
	.submit_request = vbdev_vslm_submit_request,
	.io_type_supported = vbdev_vslm_io_type_supported,
	.get_io_channel = vbdev_vslm_get_io_channel,
};

SPDK_BDEV_MODULE_REGISTER(vslm, &vslm_if)

static bool
vbdev_vslm_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	(void)ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
		return true;
	default:
		return false;
	}
}
static int
vbdev_vslm_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_vslm *vslm = io_device;
	struct vslm_channel *ch = ctx_buf;

	ch->base_ch = spdk_bdev_get_io_channel(vslm->base_desc);
	if (!ch->base_ch) {
		return -ENOMEM;
	}

	return 0;
}

static void
vbdev_vslm_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct vslm_channel *ch = ctx_buf;

	(void)io_device;
	spdk_put_io_channel(ch->base_ch);
}

static struct spdk_io_channel *
vbdev_vslm_get_io_channel(void *ctx)
{
	struct vbdev_vslm *vslm = ctx;

	return spdk_get_io_channel(vslm);
}

static void
vbdev_vslm_complete_nvme_status(struct spdk_bdev_io *bdev_io, int sct, int sc)
{
	spdk_bdev_io_complete_nvme_status(bdev_io, 0, sct, sc);
}

static int
vbdev_vslm_nvme_prepare_iovs(struct spdk_bdev_io *bdev_io, uint32_t length,
			     struct iovec *local_iov, struct iovec **iovs_out,
			     int *iovcnt_out)
{
	struct iovec *iovs;
	int iovcnt;
	size_t nbytes;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_NVME_IOV_MD) {
		iovs = bdev_io->u.nvme_passthru.iovs;
		iovcnt = bdev_io->u.nvme_passthru.iovcnt;
		nbytes = bdev_io->u.nvme_passthru.nbytes;
	} else {
		local_iov->iov_base = bdev_io->u.nvme_passthru.buf;
		local_iov->iov_len = bdev_io->u.nvme_passthru.nbytes;
		iovs = local_iov;
		iovcnt = 1;
		nbytes = bdev_io->u.nvme_passthru.nbytes;
	}

	if (iovs == NULL || iovcnt <= 0 || nbytes == 0 || length > nbytes) {
		return -EINVAL;
	}

	*iovs_out = iovs;
	*iovcnt_out = iovcnt;
	return 0;
}

/*
 * Stream the committed-view read into the caller iovs one page-sized chunk at a
 * time. vslm_read_committed_range() already iterates per page internally, so a
 * single bounded on-stack bounce avoids a per-I/O malloc of host-controlled size.
 */
static int
vbdev_vslm_nvme_memory_read(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
			    uint64_t starting_byte, uint32_t length,
			    struct iovec *iovs, int iovcnt)
{
	uint8_t page[VSLM_PAGE_SIZE];
	struct spdk_iov_xfer ix;
	uint32_t processed = 0;
	int rc;

	spdk_iov_xfer_init(&ix, iovs, iovcnt);

	while (processed < length) {
		uint32_t chunk = spdk_min((uint32_t)VSLM_PAGE_SIZE, length - processed);

		rc = vslm_read_committed_range(vslm, base_ch, starting_byte + processed,
					       chunk, page);
		if (rc != 0) {
			return rc;
		}

		spdk_iov_xfer_from_buf(&ix, page, chunk);
		processed += chunk;
	}

	return 0;
}

/*
 * Stream the host write out of the caller iovs one page-sized chunk at a time.
 * vslm_copy_range() iterates per page internally, so a bounded on-stack bounce
 * avoids a per-I/O malloc of host-controlled size.
 */
static int
vbdev_vslm_nvme_memory_write(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
			     uint64_t starting_byte, uint32_t length,
			     struct iovec *iovs, int iovcnt)
{
	uint8_t page[VSLM_PAGE_SIZE];
	struct spdk_iov_xfer ix;
	uint32_t processed = 0;
	int rc;

	spdk_iov_xfer_init(&ix, iovs, iovcnt);

	while (processed < length) {
		uint32_t chunk = spdk_min((uint32_t)VSLM_PAGE_SIZE, length - processed);

		spdk_iov_xfer_to_buf(&ix, page, chunk);
		rc = vslm_copy_range(vslm, base_ch, starting_byte + processed, chunk, page,
				     true);
		if (rc != 0) {
			return rc;
		}

		processed += chunk;
	}

	return 0;
}

/*
 * Decode and bounds-validate the SLM memory range (start offset / length) shared
 * by the MEMORY_FILL/READ/WRITE opcodes. Returns the decoded range via the out
 * params and an NVMe status code: SPDK_NVME_SC_SUCCESS for a valid non-empty
 * range, SPDK_NVME_SC_INVALID_FIELD for an unaligned/out-of-bounds range. A
 * zero-length (no-op) range is reported via *is_empty so the caller can complete
 * it as success without further work.
 */
static int
decode_and_validate_slm_range(const struct spdk_nvme_cmd *cmd, uint64_t buffer_size,
			      uint64_t *starting_byte, uint32_t *length, bool *is_empty)
{
	uint64_t start;
	uint32_t len;
	uint64_t end;

	start = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
	len = cmd->cdw12;

	if ((start & 0x3) != 0 || (len & 0x3) != 0) {
		return SPDK_NVME_SC_INVALID_FIELD;
	}

	end = start + len;
	if (end < start || end > buffer_size) {
		return SPDK_NVME_SC_INVALID_FIELD;
	}

	*starting_byte = start;
	*length = len;
	*is_empty = (len == 0);
	return SPDK_NVME_SC_SUCCESS;
}

static void
vbdev_vslm_submit_nvme_passthru(struct vbdev_vslm *vslm, struct spdk_io_channel *base_ch,
				struct spdk_bdev_io *bdev_io)
{
	const struct spdk_nvme_cmd *cmd = &bdev_io->u.nvme_passthru.cmd;
	uint64_t starting_byte = 0;
	uint32_t length = 0;
	uint64_t buffer_size = vslm->virtual_size_bytes;
	struct iovec *iovs = NULL;
	struct iovec local_iov;
	bool is_empty = false;
	int iovcnt = 0;
	int sct = SPDK_NVME_SCT_GENERIC;
	int sc = SPDK_NVME_SC_SUCCESS;
	int rc;

	switch (cmd->opc) {
	case SPDK_NVME_SLM_OPC_MEMORY_FILL:
		sc = decode_and_validate_slm_range(cmd, buffer_size, &starting_byte,
						   &length, &is_empty);
		if (sc != SPDK_NVME_SC_SUCCESS || is_empty) {
			goto out;
		}

		if (vslm_has_lease_conflict(vslm, starting_byte, length)) {
			rc = vslm_queue_blocked_cmd(vslm, bdev_io, starting_byte, length);
			if (rc == 0) {
				return;
			}

			sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}
		__atomic_add_fetch(&vslm->stats.vslm_host_write_nonconflict_total, 1,
				   __ATOMIC_RELAXED);
		vslm_trace_emitf(vslm, "host_write_nonconflict",
				 ",\"range_start\":%" PRIu64 ",\"range_len\":%u,\"result\":\"allowed\"",
				 starting_byte, length);

		rc = vslm_fill_range(vslm, base_ch, starting_byte, length);
		if (rc != 0) {
			sc = (rc == -EINVAL) ? SPDK_NVME_SC_INVALID_FIELD :
			     SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}
		goto out;
	case SPDK_NVME_SLM_OPC_MEMORY_READ:
		sc = decode_and_validate_slm_range(cmd, buffer_size, &starting_byte,
						   &length, &is_empty);
		if (sc != SPDK_NVME_SC_SUCCESS || is_empty) {
			goto out;
		}

		if (vslm_has_lease_conflict(vslm, starting_byte, length)) {
			__atomic_add_fetch(&vslm->stats.vslm_host_read_during_execution_total, 1,
					   __ATOMIC_RELAXED);
			vslm_trace_emitf(vslm, "host_read_committed",
					 ",\"range_start\":%" PRIu64 ",\"range_len\":%u,\"result\":\"committed_view\"",
					 starting_byte, length);
		}

		rc = vbdev_vslm_nvme_prepare_iovs(bdev_io, length, &local_iov, &iovs, &iovcnt);
		if (rc != 0) {
			sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			goto out;
		}

		rc = vbdev_vslm_nvme_memory_read(vslm, base_ch, starting_byte, length, iovs, iovcnt);
		if (rc != 0) {
			sc = (rc == -EINVAL) ? SPDK_NVME_SC_INVALID_FIELD :
			     SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
		break;
	case SPDK_NVME_SLM_OPC_MEMORY_WRITE:
		sc = decode_and_validate_slm_range(cmd, buffer_size, &starting_byte,
						   &length, &is_empty);
		if (sc != SPDK_NVME_SC_SUCCESS || is_empty) {
			goto out;
		}

		if (vslm_has_lease_conflict(vslm, starting_byte, length)) {
			rc = vslm_queue_blocked_cmd(vslm, bdev_io, starting_byte, length);
			if (rc == 0) {
				return;
			}

			sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			goto out;
		}
		__atomic_add_fetch(&vslm->stats.vslm_host_write_nonconflict_total, 1,
				   __ATOMIC_RELAXED);
		vslm_trace_emitf(vslm, "host_write_nonconflict",
				 ",\"range_start\":%" PRIu64 ",\"range_len\":%u,\"result\":\"allowed\"",
				 starting_byte, length);

		rc = vbdev_vslm_nvme_prepare_iovs(bdev_io, length, &local_iov, &iovs, &iovcnt);
		if (rc != 0) {
			sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			goto out;
		}

		rc = vbdev_vslm_nvme_memory_write(vslm, base_ch, starting_byte, length, iovs, iovcnt);
		if (rc != 0) {
			sc = (rc == -EINVAL) ? SPDK_NVME_SC_INVALID_FIELD :
			     SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
		break;
	default:
		sc = SPDK_NVME_SC_INVALID_OPCODE;
		goto out;
	}

out:
	vbdev_vslm_complete_nvme_status(bdev_io, sct, sc);
}

/*
 * Asynchronous, non-blocking host block I/O.
 *
 * The previous path drove base-bdev I/O with an inline
 * "while (!done) spdk_thread_poll()" wait inside submit_request. That blocks the
 * reactor and deadlocks once eviction is needed (the spilled writeback is
 * nomem-queued and never retried because no sibling I/O can complete while the
 * reactor spins). These paths submit base I/O asynchronously and complete the
 * bdev_io from a callback, so the reactor is never blocked.
 *
 * Writes reuse the async page-fault/eviction engine (vslm_exec_rw_async_submit).
 * Reads use a dedicated committed-view state machine that preserves the
 * host-read-during-execution visibility guarantee: a miss reads the committed
 * source rather than faulting the page into the compute working set.
 */

struct vslm_host_read_ctx {
	struct vbdev_vslm *vslm;
	struct spdk_bdev_io *bdev_io;
	struct spdk_io_channel *base_ch;
	uint8_t *buf;
	uint64_t starting_byte;
	uint64_t length;
	uint64_t offset;
	uint8_t *page_buf;
	/* in-flight committed-source read */
	struct spdk_bdev_desc *source_desc;
	struct spdk_io_channel *source_ch;
	bool source_desc_opened;
	bool source_ch_owned;
	uint64_t pend_page_offset;
	uint64_t pend_chunk;
};

static void vslm_host_read_step(struct vslm_host_read_ctx *ctx);

static void
vslm_host_read_release_source(struct vslm_host_read_ctx *ctx)
{
	if (ctx->source_ch_owned && ctx->source_ch != NULL) {
		spdk_put_io_channel(ctx->source_ch);
	}
	if (ctx->source_desc_opened && ctx->source_desc != NULL) {
		spdk_bdev_close(ctx->source_desc);
	}
	ctx->source_ch = NULL;
	ctx->source_desc = NULL;
	ctx->source_ch_owned = false;
	ctx->source_desc_opened = false;
}

static void
vslm_host_read_finish(struct vslm_host_read_ctx *ctx, int status)
{
	struct spdk_bdev_io *bdev_io = ctx->bdev_io;

	vslm_host_read_release_source(ctx);
	if (ctx->page_buf != NULL) {
		spdk_dma_free(ctx->page_buf);
	}
	free(ctx);
	spdk_bdev_io_complete(bdev_io,
			      status == 0 ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static void
vslm_host_read_source_complete(struct spdk_bdev_io *source_io, bool success, void *cb_arg)
{
	struct vslm_host_read_ctx *ctx = cb_arg;

	spdk_bdev_free_io(source_io);
	if (!success) {
		vslm_host_read_finish(ctx, -EIO);
		return;
	}

	memcpy(ctx->buf + ctx->offset, ctx->page_buf + ctx->pend_page_offset, ctx->pend_chunk);
	ctx->offset += ctx->pend_chunk;
	vslm_host_read_step(ctx);
}

static int
vslm_host_read_submit_source(struct vslm_host_read_ctx *ctx, struct spdk_bdev *source_bdev,
			     uint64_t source_offset_bytes)
{
	struct vbdev_vslm *vslm = ctx->vslm;
	struct spdk_bdev_desc *source_desc;
	struct spdk_io_channel *source_ch;
	uint64_t block_size;
	uint64_t page_blocks;
	uint64_t source_offset_blocks;
	int rc;

	if (source_bdev == NULL) {
		source_bdev = vslm->base_bdev;
	}

	block_size = spdk_bdev_get_block_size(source_bdev);
	if (block_size == 0 || (VSLM_PAGE_SIZE % block_size) != 0 ||
	    (source_offset_bytes % block_size) != 0) {
		return -EINVAL;
	}
	page_blocks = VSLM_PAGE_SIZE / block_size;
	source_offset_blocks = source_offset_bytes / block_size;

	if (ctx->page_buf == NULL) {
		ctx->page_buf = spdk_dma_malloc(VSLM_PAGE_SIZE, VSLM_PAGE_SIZE, NULL);
		if (ctx->page_buf == NULL) {
			return -ENOMEM;
		}
	}

	/* Drop any channel opened for a previous (alias) source. */
	vslm_host_read_release_source(ctx);

	if (source_bdev == vslm->base_bdev) {
		source_desc = vslm->base_desc;
		source_ch = ctx->base_ch;
	} else {
		rc = spdk_bdev_open_ext(source_bdev->name, false, vbdev_vslm_bdev_event_cb,
					vslm, &source_desc);
		if (rc != 0) {
			return rc;
		}
		ctx->source_desc = source_desc;
		ctx->source_desc_opened = true;
		source_ch = spdk_bdev_get_io_channel(source_desc);
		if (source_ch == NULL) {
			return -ENOMEM;
		}
		ctx->source_ch = source_ch;
		ctx->source_ch_owned = true;
	}

	return spdk_bdev_read_blocks(source_desc, source_ch, ctx->page_buf, source_offset_blocks,
				     page_blocks, vslm_host_read_source_complete, ctx);
}

static void
vslm_host_read_waiter_cb(void *cb_arg, int status, struct vslm_page *page)
{
	struct vslm_host_read_ctx *ctx = cb_arg;

	(void)page;
	if (status != 0) {
		vslm_host_read_finish(ctx, status);
		return;
	}
	vslm_host_read_step(ctx);
}

static void
vslm_host_read_step(struct vslm_host_read_ctx *ctx)
{
	struct vbdev_vslm *vslm = ctx->vslm;

	while (ctx->offset < ctx->length) {
		uint64_t absolute = ctx->starting_byte + ctx->offset;
		uint64_t vpn = absolute / VSLM_PAGE_SIZE;
		uint64_t page_offset = absolute % VSLM_PAGE_SIZE;
		uint64_t chunk = spdk_min((uint64_t)(VSLM_PAGE_SIZE - page_offset),
					  ctx->length - ctx->offset);
		struct vslm_mmu_shard *shard = vslm_shard_for_vpn(vslm, vpn);
		struct vslm_page *page;
		struct vslm_lpage *lpage;
		struct spdk_bdev *source_bdev;
		uint64_t source_offset_bytes;
		int rc;

		pthread_mutex_lock(&shard->lock);
		page = vslm_lookup_page(vslm, vpn);
		lpage = vslm_lookup_lpage(vslm, vpn);

		if (page != NULL && (page->load_state == VSLM_PAGE_LOAD_LOADING ||
				     page->load_state == VSLM_PAGE_LOAD_EVICTING)) {
			rc = vslm_page_add_waiter_locked(vslm, page, spdk_get_thread(),
							 vslm_host_read_waiter_cb, ctx);
			pthread_mutex_unlock(&shard->lock);
			if (rc != 0) {
				vslm_host_read_finish(ctx, rc);
			}
			return;
		}
		if (page != NULL && page->load_state != VSLM_PAGE_LOAD_RESIDENT) {
			page = NULL;
		}

		if (page != NULL && lpage == NULL && !page->dirty) {
			memcpy(ctx->buf + ctx->offset,
			       vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset, chunk);
			pthread_mutex_unlock(&shard->lock);
			ctx->offset += chunk;
			continue;
		}
		if (page != NULL && lpage != NULL && !page->dirty &&
		    vslm_lpage_can_collapse_to_default_backing(vslm, lpage)) {
			memcpy(ctx->buf + ctx->offset,
			       vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset, chunk);
			pthread_mutex_unlock(&shard->lock);
			ctx->offset += chunk;
			continue;
		}
		if (page != NULL && lpage != NULL &&
		    lpage->state == SPDK_BDEV_VSLM_LPAGE_PRIVATE_DIRTY_RESIDENT &&
		    !lpage->pending_publish) {
			if (vslm_vpn_has_active_lease(vslm, vpn)) {
				vslm_note_visibility_violation(vslm, vpn,
							       "host_read_private_dirty_with_active_lease");
			}
			memcpy(ctx->buf + ctx->offset,
			       vslm->sram_buffer + (page->ppn * VSLM_PAGE_SIZE) + page_offset, chunk);
			pthread_mutex_unlock(&shard->lock);
			ctx->offset += chunk;
			continue;
		}

		vslm_get_committed_source(vslm, vpn, &source_bdev, &source_offset_bytes);
		pthread_mutex_unlock(&shard->lock);

		ctx->pend_page_offset = page_offset;
		ctx->pend_chunk = chunk;
		rc = vslm_host_read_submit_source(ctx, source_bdev, source_offset_bytes);
		if (rc != 0) {
			vslm_host_read_finish(ctx, rc);
		}
		return;
	}

	vslm_host_read_finish(ctx, 0);
}

struct vslm_host_write_ctx {
	struct spdk_bdev_io *bdev_io;
	void *bounce;
};

static void
vslm_host_write_done(void *cb_arg, int status)
{
	struct vslm_host_write_ctx *ctx = cb_arg;
	struct spdk_bdev_io *bdev_io = ctx->bdev_io;

	free(ctx->bounce);
	free(ctx);
	spdk_bdev_io_complete(bdev_io,
			      status == 0 ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static void
vbdev_vslm_submit_host_write(struct vbdev_vslm *vslm, struct spdk_bdev_io *bdev_io)
{
	uint64_t block_size = bdev_io->bdev->blocklen;
	uint64_t offset = bdev_io->u.bdev.offset_blocks * block_size;
	uint64_t length = bdev_io->u.bdev.num_blocks * block_size;
	struct iovec *iovs = bdev_io->u.bdev.iovs;
	int iovcnt = bdev_io->u.bdev.iovcnt;
	struct vslm_host_write_ctx *ctx;
	void *buf;
	int rc;

	if (length == 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return;
	}
	if (iovs == NULL || iovcnt <= 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (vslm_has_lease_conflict(vslm, offset, length)) {
		__atomic_add_fetch(&vslm->stats.vslm_host_write_conflict_total, 1, __ATOMIC_RELAXED);
		vslm_trace_emitf(vslm, "lease_conflict",
				 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"rejected\"",
				 offset, length);
		vslm_trace_emitf(vslm, "host_write_conflict",
				 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"rejected\"",
				 offset, length);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	__atomic_add_fetch(&vslm->stats.vslm_host_write_nonconflict_total, 1, __ATOMIC_RELAXED);
	vslm_trace_emitf(vslm, "host_write_nonconflict",
			 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"allowed\"",
			 offset, length);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	ctx->bdev_io = bdev_io;

	if (iovcnt == 1) {
		buf = iovs[0].iov_base;
	} else {
		ctx->bounce = malloc((size_t)length);
		if (ctx->bounce == NULL) {
			free(ctx);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
		spdk_copy_iovs_to_buf(ctx->bounce, length, iovs, iovcnt);
		buf = ctx->bounce;
	}

	rc = vslm_exec_rw_async_submit(bdev_io->bdev, offset, length, buf, true,
				       vslm_host_write_done, ctx);
	if (rc != 0) {
		free(ctx->bounce);
		free(ctx);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/*
 * Read bdev_ios may arrive with no data buffer (iov_base == NULL); per the SPDK
 * bdev contract a module that copies data into the caller's iovs must request a
 * buffer via spdk_bdev_io_get_buf() before touching them. get_buf provides a
 * single contiguous buffer; start the asynchronous committed-view read on it.
 */
static void
vbdev_vslm_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
			   bool success)
{
	struct vslm_channel *vch = spdk_io_channel_get_ctx(ch);
	struct vbdev_vslm *vslm = bdev_io->bdev->ctxt;
	struct iovec *iovs = bdev_io->u.bdev.iovs;
	int iovcnt = bdev_io->u.bdev.iovcnt;
	struct vslm_host_read_ctx *ctx;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	if (iovs == NULL || iovcnt != 1 || iovs[0].iov_base == NULL) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	ctx->vslm = vslm;
	ctx->bdev_io = bdev_io;
	ctx->base_ch = vch->base_ch;
	ctx->buf = iovs[0].iov_base;
	ctx->starting_byte = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
	ctx->length = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

	if (vslm_has_lease_conflict(vslm, ctx->starting_byte, ctx->length)) {
		__atomic_add_fetch(&vslm->stats.vslm_host_read_during_execution_total, 1,
				   __ATOMIC_RELAXED);
		vslm_trace_emitf(vslm, "host_read_committed",
				 ",\"range_start\":%" PRIu64 ",\"range_len\":%" PRIu64 ",\"result\":\"committed_view\"",
				 ctx->starting_byte, ctx->length);
	}

	vslm_host_read_step(ctx);
}

static void
vbdev_vslm_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vslm_channel *vch;
	struct vbdev_vslm *vslm;

	vslm = bdev_io->bdev->ctxt;
	vch = spdk_io_channel_get_ctx(ch);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, vbdev_vslm_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		vbdev_vslm_submit_host_write(vslm, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
		vbdev_vslm_submit_nvme_passthru(vslm, vch->base_ch, bdev_io);
		break;
	default:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

SPDK_LOG_REGISTER_COMPONENT(vslm)

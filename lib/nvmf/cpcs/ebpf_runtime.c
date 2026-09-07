/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 CPCS Implementation Team. All rights reserved.
 */

/**
 * eBPF Runtime for CPCS Programs
 *
 * Provides eBPF bytecode execution using uBPF library.
 * Falls back to simulation mode if uBPF is not available.
 */

#include "ebpf_runtime.h"
#include "program.h"
#include "execute.h"
#include "memory_range_set.h"
#include "spdk/bdev_slm.h"
#include "spdk/log.h"
#include "spdk/thread.h"

/* Check if uBPF library is available */
#ifdef HAVE_UBPF
#include <ubpf.h>
#define UBPF_AVAILABLE 1
#else
#define UBPF_AVAILABLE 0
/* Define minimal uBPF types for compilation without library */
struct ubpf_vm;
typedef uint64_t (*ubpf_jit_fn)(void *mem, size_t mem_len);
#endif

/* Maximum VM memory size (1 MB) */
#define EBPF_VM_MEM_SIZE (1024 * 1024)

struct cpcs_ebpf_wait_ctx {
	bool done;
	int status;
};

/*
 * Sanitized guest-ABI view of the execution context.
 *
 * Only scalar parameters the program legitimately needs are exposed in
 * guest-readable VM memory.  Host pointers and lease identifiers from
 * struct cpcs_exec_context are deliberately NOT placed here, to avoid
 * leaking host addresses (ASLR defeat) into the sandbox.
 */
struct cpcs_ebpf_guest_ctx {
	uint64_t cparam1;
	uint64_t cparam2;
	uint32_t data_len;
	uint32_t resolved_range_count;
};

/*
 * Per-execution scope made reachable from the registered helpers.
 *
 * uBPF helpers do not receive the host execution context directly, so the
 * runtime stashes the real context plus the VM memory base/length here for
 * the duration of a single (synchronous) program invocation.  Guest-supplied
 * register values (buf_ptr / msg_ptr) are treated as OFFSETS into
 * [mem_base, mem_base + mem_len) and validated before any host access.
 */
struct cpcs_ebpf_exec_scope {
	struct cpcs_exec_context *exec_ctx;
	uint8_t                  *mem_base;
	size_t                    mem_len;
};

struct cpcs_ebpf_execute_async_ctx {
	struct cpcs_program *prog;
	struct cpcs_exec_context *exec_ctx;
	cpcs_runtime_execute_done_cb done_cb;
	void *cb_arg;
};

#if UBPF_AVAILABLE
static void
ebpf_slm_wait_done(void *cb_arg, int status)
{
	struct cpcs_ebpf_wait_ctx *wait = cb_arg;

	wait->status = status;
	wait->done = true;
}

static int
ebpf_slm_wait(struct cpcs_ebpf_wait_ctx *wait)
{
	struct spdk_thread *thread = spdk_get_thread();

	if (thread == NULL) {
		while (!wait->done) {
			/* No SPDK thread context; callbacks are expected inline here. */
		}
		return wait->status;
	}

	while (!wait->done) {
		spdk_thread_poll(thread, 0, 0);
	}

	return wait->status;
}

/*
 * Active execution scope for the currently running program.
 *
 * The uBPF interpreter/JIT invokes registered helpers synchronously and
 * nested within ubpf_exec()/the JIT function on the same thread, so a
 * thread-local pointer set immediately before execution and cleared
 * immediately after is sufficient and race-free.
 */
static __thread struct cpcs_ebpf_exec_scope *g_ebpf_active_scope;

/*
 * Per-thread VM memory buffer, reused across Executes instead of
 * calloc()/free() on every call.
 *
 * This is thread-local, not per-program (struct cpcs_ebpf_ctx, stashed in
 * prog->runtime, is a single instance shared by every Execute of a given
 * program -- see ebpf_runtime.h). Concurrent Executes of the *same*
 * program can run on different SPDK reactor threads at the same time:
 * struct cpcs_program.exec_count ("Active executions", program.h:52)
 * tracks multiple simultaneous in-flight executions, and in
 * cpcs_execute_run() (execute.c:519-522) ctx->program->lock is only held
 * around the runtime->execute_async() submission call -- ebpf_execute_async()
 * (below) enqueues a message via spdk_thread_send_msg() and returns
 * immediately, well before ebpf_execute_sync() actually runs on whatever
 * thread that message lands on. A buffer owned by struct cpcs_ebpf_ctx
 * would therefore be raced by two connections executing the same pind
 * concurrently. A thread-local buffer avoids that: it is only ever
 * touched by the single reactor thread that allocated it, matching the
 * existing g_ebpf_active_scope thread-local above.
 */
static __thread void   *g_ebpf_vm_mem;
static __thread size_t  g_ebpf_vm_mem_size;

/**
 * Translate a guest-supplied offset into a validated host pointer inside the
 * sandbox VM memory region.
 *
 * buf_off is an OFFSET into [0, mem_len); the access [buf_off, buf_off + len)
 * must lie fully within the VM memory.  Returns NULL on any out-of-bounds or
 * overflowing request so callers reject the operation.
 */
static void *
ebpf_guest_buf(struct cpcs_ebpf_exec_scope *scope, uint64_t buf_off, uint64_t len)
{
	if (scope == NULL || scope->mem_base == NULL) {
		return NULL;
	}
	if (buf_off > scope->mem_len) {
		return NULL;
	}
	/* Overflow-safe bounds check: buf_off <= mem_len already guaranteed. */
	if (len > scope->mem_len - buf_off) {
		return NULL;
	}
	return scope->mem_base + buf_off;
}

/*
 * eBPF Helper Functions
 * These are callable from eBPF programs
 */

static int
ebpf_resolve_exec_range(struct cpcs_exec_context *exec_ctx, uint64_t mr_id, uint64_t offset,
			uint64_t len, struct spdk_bdev **bdev_out, uint64_t *absolute_offset_out)
{
	const struct cpcs_exec_resolved_range *mr;
	uint64_t absolute_offset;

	if (exec_ctx == NULL || bdev_out == NULL || absolute_offset_out == NULL) {
		return -EINVAL;
	}

	if (exec_ctx->resolved_ranges == NULL || exec_ctx->resolved_range_count == 0) {
		return -EINVAL;
	}
	if (mr_id == 0 || mr_id > exec_ctx->resolved_range_count) {
		return -EINVAL;
	}

	mr = &exec_ctx->resolved_ranges[mr_id - 1];
	if (mr->bdev == NULL) {
		return -EINVAL;
	}

	if (offset > UINT64_MAX - len) {
		return -EINVAL;
	}
	if (offset + len > mr->length) {
		return -EINVAL;
	}
	if (mr->starting_byte > UINT64_MAX - offset) {
		return -EINVAL;
	}

	absolute_offset = mr->starting_byte + offset;
	*bdev_out = mr->bdev;
	*absolute_offset_out = absolute_offset;
	return 0;
}

/**
 * Helper: Read from SLM memory
 *
 * uint64_t slm_read(uint64_t mr_id, uint64_t offset, uint64_t len, uint64_t buf_ptr)
 */
static uint64_t
helper_slm_read(uint64_t mr_id, uint64_t offset,
		uint64_t len, uint64_t buf_ptr, uint64_t unused)
{
	struct cpcs_ebpf_exec_scope *scope = g_ebpf_active_scope;
	struct cpcs_exec_context *exec_ctx;
	struct spdk_bdev *bdev;
	uint64_t absolute_offset;
	struct cpcs_ebpf_wait_ctx wait = {};
	void *dst;
	uint32_t data_len;
	int rc;

	(void)unused;

	if (scope == NULL || scope->exec_ctx == NULL) {
		return (uint64_t) -1;
	}
	exec_ctx = scope->exec_ctx;

	/* buf_ptr is an offset into the sandbox VM memory, never a host pointer. */
	dst = ebpf_guest_buf(scope, buf_ptr, len);
	if (dst == NULL) {
		SPDK_ERRLOG("SLM read dst out of sandbox: buf_off=%lu len=%lu\n", buf_ptr, len);
		return (uint64_t) -1;
	}

	if (mr_id == 0) {
		/* Read from input data buffer (overflow-safe bounds check) */
		data_len = exec_ctx->data_len;
		if (exec_ctx->data_buffer == NULL || len > data_len || offset > data_len - len) {
			SPDK_ERRLOG("SLM read out of bounds: offset=%lu len=%lu data_len=%u\n",
				    offset, len, data_len);
			return (uint64_t) -1;
		}
		memcpy(dst, (uint8_t *)exec_ctx->data_buffer + offset, len);
		return 0;
	}

	rc = ebpf_resolve_exec_range(exec_ctx, mr_id, offset, len, &bdev, &absolute_offset);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to resolve memory range: mr_id=%lu offset=%lu len=%lu rc=%d\n",
			    mr_id, offset, len, rc);
		return (uint64_t) -1;
	}

	rc = bdev_slm_exec_read_by_bdev_async(bdev, absolute_offset, len, dst,
					      ebpf_slm_wait_done, &wait);
	if (rc == 0) {
		rc = ebpf_slm_wait(&wait);
	}
	if (rc != 0) {
		SPDK_ERRLOG("Failed to read memory namespace: bdev=%p offset=%lu len=%lu rc=%d\n",
			    bdev, absolute_offset, len, rc);
		return (uint64_t) -1;
	}

	return 0;
}

/**
 * Helper: Write to SLM memory
 *
 * uint64_t slm_write(uint64_t mr_id, uint64_t offset, uint64_t len, uint64_t buf_ptr)
 */
static uint64_t
helper_slm_write(uint64_t mr_id, uint64_t offset,
		 uint64_t len, uint64_t buf_ptr, uint64_t unused)
{
	struct cpcs_ebpf_exec_scope *scope = g_ebpf_active_scope;
	struct cpcs_exec_context *exec_ctx;
	struct spdk_bdev *bdev;
	uint64_t absolute_offset;
	struct cpcs_ebpf_wait_ctx wait = {};
	void *src;
	int rc;

	(void)unused;

	if (scope == NULL || scope->exec_ctx == NULL) {
		return (uint64_t) -1;
	}
	exec_ctx = scope->exec_ctx;

	if (mr_id == 0) {
		/* Cannot write to input data buffer */
		SPDK_ERRLOG("Cannot write to input data buffer (mr_id=0)\n");
		return (uint64_t) -1;
	}

	/* buf_ptr is an offset into the sandbox VM memory, never a host pointer. */
	src = ebpf_guest_buf(scope, buf_ptr, len);
	if (src == NULL) {
		SPDK_ERRLOG("SLM write src out of sandbox: buf_off=%lu len=%lu\n", buf_ptr, len);
		return (uint64_t) -1;
	}

	rc = ebpf_resolve_exec_range(exec_ctx, mr_id, offset, len, &bdev, &absolute_offset);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to resolve memory range: mr_id=%lu offset=%lu len=%lu rc=%d\n",
			    mr_id, offset, len, rc);
		return (uint64_t) -1;
	}

	rc = bdev_slm_exec_write_by_bdev_async(bdev, absolute_offset, len, src,
					       ebpf_slm_wait_done, &wait);
	if (rc == 0) {
		rc = ebpf_slm_wait(&wait);
	}
	if (rc != 0) {
		SPDK_ERRLOG("Failed to write memory namespace: bdev=%p offset=%lu len=%lu rc=%d\n",
			    bdev, absolute_offset, len, rc);
		return (uint64_t) -1;
	}

	return 0;
}

/**
 * Helper: Get command parameter
 *
 * uint64_t get_param(uint64_t param_id)
 */
static uint64_t
helper_get_param(uint64_t param_id, uint64_t unused1,
		 uint64_t unused2, uint64_t unused3, uint64_t unused4)
{
	struct cpcs_ebpf_exec_scope *scope = g_ebpf_active_scope;
	struct cpcs_exec_context *exec_ctx;

	(void)unused1;
	(void)unused2;
	(void)unused3;
	(void)unused4;

	if (scope == NULL || scope->exec_ctx == NULL) {
		return 0;
	}
	exec_ctx = scope->exec_ctx;

	switch (param_id) {
	case 1:
		return exec_ctx->cparam1;
	case 2:
		return exec_ctx->cparam2;
	default:
		SPDK_WARNLOG("Invalid parameter ID: %lu\n", param_id);
		return 0;
	}
}

/**
 * Helper: Log message
 *
 * uint64_t log(uint64_t level, uint64_t msg_ptr, uint64_t msg_len)
 */
static uint64_t
helper_log(uint64_t level, uint64_t msg_ptr,
	   uint64_t msg_len, uint64_t unused1, uint64_t unused2)
{
	struct cpcs_ebpf_exec_scope *scope = g_ebpf_active_scope;
	char msg[256];
	size_t copy_len = msg_len < sizeof(msg) - 1 ? msg_len : sizeof(msg) - 1;
	void *src;

	(void)unused1;
	(void)unused2;

	/* msg_ptr is an offset into the sandbox VM memory, never a host pointer. */
	src = ebpf_guest_buf(scope, msg_ptr, copy_len);
	if (src == NULL) {
		SPDK_ERRLOG("eBPF log msg out of sandbox: msg_off=%lu len=%lu\n", msg_ptr, msg_len);
		return (uint64_t) -1;
	}

	memcpy(msg, src, copy_len);
	msg[copy_len] = '\0';

	switch (level) {
	case 0: /* ERROR */
		SPDK_ERRLOG("eBPF: %s\n", msg);
		break;
	case 1: /* WARN */
		SPDK_WARNLOG("eBPF: %s\n", msg);
		break;
	case 2: /* NOTICE */
		SPDK_NOTICELOG("eBPF: %s\n", msg);
		break;
	case 3: /* DEBUG */
		SPDK_DEBUGLOG(nvmf_cpcs, "eBPF: %s\n", msg);
		break;
	default:
		SPDK_INFOLOG(nvmf_cpcs, "eBPF: %s\n", msg);
		break;
	}

	return 0;
}
#endif /* UBPF_AVAILABLE */

/*
 * Runtime Operations
 */

static int
ebpf_init(struct cpcs_program *prog)
{
	struct cpcs_ebpf_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed to allocate eBPF context\n");
		return -ENOMEM;
	}

	ctx->jit_enabled = false;
	ctx->mem_size = EBPF_VM_MEM_SIZE;

#if UBPF_AVAILABLE
	/* Create uBPF VM */
	ctx->vm = ubpf_create();
	if (!ctx->vm) {
		SPDK_ERRLOG("Failed to create uBPF VM\n");
		free(ctx);
		return -ENOMEM;
	}

	/* Register helper functions */
	ubpf_register(ctx->vm, CPCS_EBPF_HELPER_SLM_READ, "slm_read",
		      (void *)helper_slm_read);
	ubpf_register(ctx->vm, CPCS_EBPF_HELPER_SLM_WRITE, "slm_write",
		      (void *)helper_slm_write);
	ubpf_register(ctx->vm, CPCS_EBPF_HELPER_GET_PARAM, "get_param",
		      (void *)helper_get_param);
	ubpf_register(ctx->vm, CPCS_EBPF_HELPER_LOG, "log",
		      (void *)helper_log);

	SPDK_NOTICELOG("eBPF runtime initialized with uBPF for program %u\n", prog->pind);
#else
	SPDK_NOTICELOG("eBPF runtime initialized in simulation mode for program %u\n", prog->pind);
#endif

	prog->runtime = (struct cpcs_runtime_ctx *)ctx;
	return 0;
}

/*
 * Find the actual bytecode length by stripping trailing zero padding.
 * eBPF instructions are 8 bytes each. The host-side xNVMe library pads
 * program data to 4096-byte alignment (lpg_align), so uBPF sees the
 * zero padding as invalid instructions. eBPF programs always end with
 * an 'exit' instruction (opcode 0x95), so we scan backwards over
 * all-zero 8-byte blocks to find the real program boundary.
 */
static uint32_t
ebpf_strip_zero_padding(const uint8_t *data, uint32_t total_size)
{
	uint32_t real_len = total_size;

	while (real_len >= 8) {
		const uint8_t *insn = data + real_len - 8;
		bool is_zero = true;
		for (int i = 0; i < 8; i++) {
			if (insn[i] != 0) {
				is_zero = false;
				break;
			}
		}
		if (!is_zero) {
			break;
		}
		real_len -= 8;
	}

	if (real_len == 0) {
		real_len = total_size;
	}

	return real_len;
}

static int
ebpf_validate(struct cpcs_program *prog)
{
	struct cpcs_ebpf_ctx *ctx = (struct cpcs_ebpf_ctx *)prog->runtime;

	if (!ctx) {
		SPDK_ERRLOG("No runtime context\n");
		return -EINVAL;
	}

	if (!prog->data || prog->loaded_bytes != prog->total_size) {
		SPDK_ERRLOG("Program data incomplete: loaded=%u total=%u\n",
			    prog->loaded_bytes, prog->total_size);
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}

#if UBPF_AVAILABLE
	char *errmsg = NULL;

	/* Strip trailing zero padding from 4096-byte alignment */
	uint32_t real_len = ebpf_strip_zero_padding(prog->data, prog->total_size);
	if (real_len != prog->total_size) {
		SPDK_NOTICELOG("Stripped zero padding: %u → %u bytes (pind=%u)\n",
			       prog->total_size, real_len, prog->pind);
	}

	/* Load eBPF bytecode into VM */
	int rc = ubpf_load(ctx->vm, prog->data, real_len, &errmsg);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to load eBPF program: %s\n",
			    errmsg ? errmsg : "unknown error");
		free(errmsg);
		return -SPDK_NVME_CPCS_SC_INVALID_PROGRAM_DATA;
	}

	SPDK_DEBUGLOG(nvmf_cpcs, "eBPF program %u validated (%u bytes)\n",
		      prog->pind, real_len);
#else
	/* In simulation mode, just verify data exists */
	SPDK_DEBUGLOG(nvmf_cpcs, "eBPF program %u validated in simulation mode (%u bytes)\n",
		      prog->pind, prog->total_size);
#endif

	return 0;
}

static int
ebpf_activate(struct cpcs_program *prog)
{
	struct cpcs_ebpf_ctx *ctx = (struct cpcs_ebpf_ctx *)prog->runtime;

	if (!ctx) {
		SPDK_ERRLOG("No runtime context\n");
		return -EINVAL;
	}

#if UBPF_AVAILABLE
	char *errmsg = NULL;

	/* Attempt JIT compilation */
	ctx->jit_func = ubpf_compile(ctx->vm, &errmsg);
	if (ctx->jit_func) {
		ctx->jit_enabled = true;
		SPDK_NOTICELOG("eBPF program %u JIT compiled successfully\n", prog->pind);
	} else {
		ctx->jit_enabled = false;
		SPDK_WARNLOG("eBPF JIT compilation failed, using interpreter: %s\n",
			     errmsg ? errmsg : "unknown error");
		free(errmsg);
		/* Continue with interpreter - not a fatal error */
	}
#else
	SPDK_DEBUGLOG(nvmf_cpcs, "eBPF program %u activated in simulation mode\n", prog->pind);
#endif

	return 0;
}

static int
ebpf_execute_sync(struct cpcs_program *prog,
		  struct cpcs_exec_context *exec_ctx,
		  uint64_t *return_value)
{
	struct cpcs_ebpf_ctx *ctx = (struct cpcs_ebpf_ctx *)prog->runtime;
	uint64_t ret = 0;

	if (!ctx) {
		SPDK_ERRLOG("No runtime context\n");
		return -EINVAL;
	}

#if UBPF_AVAILABLE
	struct cpcs_ebpf_guest_ctx guest_ctx;
	struct cpcs_ebpf_exec_scope scope;

	/*
	 * Reuse this thread's VM memory buffer across Executes (see
	 * g_ebpf_vm_mem above for why it is thread-local rather than stored
	 * in ctx / struct cpcs_ebpf_ctx). ctx->mem_size is constant per
	 * program (set once in ebpf_init(), never changed), and in practice
	 * identical -- EBPF_VM_MEM_SIZE -- across every eBPF program, so this
	 * malloc() only actually runs once per reactor thread; the size check
	 * just guards against that assumption changing later.
	 */
	if (g_ebpf_vm_mem == NULL || g_ebpf_vm_mem_size < ctx->mem_size) {
		free(g_ebpf_vm_mem);
		g_ebpf_vm_mem = malloc(ctx->mem_size);
		if (g_ebpf_vm_mem == NULL) {
			g_ebpf_vm_mem_size = 0;
			SPDK_ERRLOG("Failed to allocate VM memory\n");
			return -ENOMEM;
		}
		g_ebpf_vm_mem_size = ctx->mem_size;
	}
	void *mem = g_ebpf_vm_mem;

	/*
	 * Zero the buffer on every Execute -- this is sandbox hygiene, not an
	 * allocation detail: it stops both host heap contents and the
	 * *previous* Execute's leftover VM state (potentially another
	 * guest's data, since the buffer is now reused across calls) from
	 * leaking into this run.
	 */
	memset(mem, 0, ctx->mem_size);

	/*
	 * Expose only a sanitized, scalar view of the execution context to the
	 * sandbox.  Host pointers and lease ids from struct cpcs_exec_context are
	 * never placed in guest-readable memory.
	 */
	guest_ctx.cparam1 = exec_ctx->cparam1;
	guest_ctx.cparam2 = exec_ctx->cparam2;
	guest_ctx.data_len = exec_ctx->data_len;
	guest_ctx.resolved_range_count = exec_ctx->resolved_range_count;

	size_t ctx_size = sizeof(guest_ctx);
	if (ctx_size > ctx->mem_size) {
		ctx_size = ctx->mem_size;
	}
	memcpy(mem, &guest_ctx, ctx_size);

	/*
	 * Publish the execution scope for the helpers (real context kept on the
	 * host side only; guest sees offsets translated against [mem, mem_size)).
	 */
	scope.exec_ctx = exec_ctx;
	scope.mem_base = mem;
	scope.mem_len = ctx->mem_size;
	g_ebpf_active_scope = &scope;

	/* Execute program */
	if (ctx->jit_enabled && ctx->jit_func) {
		/* Use JIT compiled function */
		ubpf_jit_fn fn = (ubpf_jit_fn)ctx->jit_func;
		ret = fn(mem, ctx->mem_size);
		SPDK_DEBUGLOG(nvmf_cpcs, "eBPF program %u executed (JIT) returned 0x%lx\n",
			      prog->pind, ret);
	} else {
		/* Use interpreter */
		int exec_rc = ubpf_exec(ctx->vm, mem, ctx->mem_size, &ret);
		if (exec_rc != 0) {
			SPDK_ERRLOG("eBPF program %u interpreter execution failed\n", prog->pind);
			g_ebpf_active_scope = NULL;
			/* mem is the thread-local g_ebpf_vm_mem buffer; not freed, only reused. */
			return -EIO;
		}
		SPDK_DEBUGLOG(nvmf_cpcs, "eBPF program %u executed (interpreter) returned 0x%lx\n",
			      prog->pind, ret);
	}

	g_ebpf_active_scope = NULL;
	/* mem is the thread-local g_ebpf_vm_mem buffer; not freed, only reused (see above). */
#else
	/* Simulation mode - just return success */
	ret = 0xDEADBEEF; /* Distinctive pattern for testing */
	SPDK_DEBUGLOG(nvmf_cpcs, "eBPF program %u executed in simulation mode\n", prog->pind);
#endif

	*return_value = ret;
	return 0;
}

static void
ebpf_execute_async_msg(void *arg)
{
	struct cpcs_ebpf_execute_async_ctx *ctx = arg;
	uint64_t return_value = 0;
	int rc;

	rc = ebpf_execute_sync(ctx->prog, ctx->exec_ctx, &return_value);
	ctx->done_cb(ctx->cb_arg, rc, return_value);

	free(ctx);
}

static int
ebpf_execute_async(struct cpcs_program *prog,
		   struct cpcs_exec_context *exec_ctx,
		   cpcs_runtime_execute_done_cb done_cb,
		   void *cb_arg)
{
	struct cpcs_ebpf_execute_async_ctx *ctx;
	struct spdk_thread *thread;
	int rc;

	if (done_cb == NULL || prog == NULL || exec_ctx == NULL) {
		return -EINVAL;
	}

	/*
	 * Never invoke done_cb synchronously from this "async" entry point: the
	 * caller (cpcs_execute_run) sets runtime_pending before calling and may
	 * own/free the exec context in the completion callback, so a synchronous
	 * completion would risk use-after-free of that context.  Without an SPDK
	 * thread there is no message loop to defer onto, so fail and let the
	 * caller own completion (mirrors the builtin runtime's no-thread path).
	 */
	thread = spdk_get_thread();
	if (thread == NULL) {
		return -SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	/* Remove after solved problem: temporary async bridge for synchronous eBPF helpers. */
	ctx->prog = prog;
	ctx->exec_ctx = exec_ctx;
	ctx->done_cb = done_cb;
	ctx->cb_arg = cb_arg;
	rc = spdk_thread_send_msg(thread, ebpf_execute_async_msg, ctx);
	if (rc == 0) {
		return 0;
	}

	free(ctx);
	return rc;
}

static void
ebpf_deactivate(struct cpcs_program *prog)
{
	struct cpcs_ebpf_ctx *ctx = (struct cpcs_ebpf_ctx *)prog->runtime;

	if (!ctx) {
		return;
	}

#if UBPF_AVAILABLE
	/* Reset JIT state; ubpf_destroy() frees VM/JIT resources. */
	ctx->jit_func = NULL;
	ctx->jit_enabled = false;

	SPDK_DEBUGLOG(nvmf_cpcs, "eBPF program %u deactivated\n", prog->pind);
#else
	SPDK_DEBUGLOG(nvmf_cpcs, "eBPF program %u deactivated (simulation)\n", prog->pind);
#endif
}

static void
ebpf_fini(struct cpcs_program *prog)
{
	struct cpcs_ebpf_ctx *ctx = (struct cpcs_ebpf_ctx *)prog->runtime;

	if (!ctx) {
		return;
	}

#if UBPF_AVAILABLE
	/* Destroy VM */
	if (ctx->vm) {
		ubpf_destroy(ctx->vm);
	}

	SPDK_DEBUGLOG(nvmf_cpcs, "eBPF runtime cleaned up for program %u\n", prog->pind);
#else
	SPDK_DEBUGLOG(nvmf_cpcs, "eBPF runtime cleaned up for program %u (simulation)\n", prog->pind);
#endif

	free(ctx);
	prog->runtime = NULL;
}

/* eBPF runtime operations */
static const struct cpcs_runtime_ops g_ebpf_runtime = {
	.name = "ebpf",
	.ptype = CPCS_PTYPE_EBPF,
	.init = ebpf_init,
	.validate = ebpf_validate,
	.activate = ebpf_activate,
	.execute_async = ebpf_execute_async,
	.deactivate = ebpf_deactivate,
	.fini = ebpf_fini,
};

int
cpcs_ebpf_runtime_register(void)
{
	int rc;

	rc = cpcs_runtime_register(&g_ebpf_runtime);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register eBPF runtime: %d\n", rc);
		return rc;
	}

#if UBPF_AVAILABLE
	SPDK_NOTICELOG("eBPF runtime registered with uBPF support\n");
#else
	SPDK_NOTICELOG("eBPF runtime registered in simulation mode (uBPF not available)\n");
#endif

	return 0;
}

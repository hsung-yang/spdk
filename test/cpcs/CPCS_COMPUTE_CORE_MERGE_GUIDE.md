# CPCS Compute Core Merge Guide

This guide is for applying the CPCS compute-core change to another SPDK fork.
It explains the behavior, the files that changed, and the checks another agent
should run after merging.

## Goal

`spdk_tgt` already accepts an app core mask with `-m`. This change adds a
second mask for CPCS builtin compute work:

```bash
build/bin/spdk_tgt -m 0xF --cpcs-compute-core-mask 0xC --wait-for-rpc
```

The compute mask must be non-empty and must be a subset of the app core mask.
NVMf poll groups do not run on those cores. Long-running builtin CPCS programs
can dispatch work onto dedicated SPDK threads on the compute cores, then send
completion back to the original submit SPDK thread.

The runtime must not create POSIX worker threads for builtin execution.
`lib/nvmf/cpcs/builtin_runtime.c` should not call `pthread_create()` or
`pthread_detach()`.

## Public Interfaces

- `spdk_tgt --cpcs-compute-core-mask <mask or list>`
- inventory key: `runtime.target_compute_core_mask`
- normal scenario flag: `--compute-core-mask`
- vector scenario flag: `--target-compute-core-mask`

The compute mask follows the same syntax as SPDK CPU masks, for example `0xC`
or `[2-3]`.

## Implementation Summary

### Target startup

Files:

- `app/spdk_tgt/spdk_tgt.c`
- `app/spdk_tgt/Makefile`

Required behavior:

- Parse `--cpcs-compute-core-mask`.
- Store the parsed mask with `cpcs_builtin_runtime_set_compute_core_mask()`.
- After SPDK app initialization, validate the compute mask against
  `spdk_app_get_core_mask()`.
- Start compute SPDK threads from `spdk_tgt_started()`.
- Stop compute SPDK threads from the app shutdown callback.
- Add the include path needed for `nvmf/cpcs/builtin_runtime.h`.

### CPCS builtin runtime

Files:

- `lib/nvmf/cpcs/builtin_runtime.c`
- `lib/nvmf/cpcs/builtin_runtime.h`

Required behavior:

- Expose APIs for compute-core mask setup, start, stop, query, and membership.
- Create one SPDK lightweight thread per compute core using `spdk_thread_create()`.
- Select compute threads round-robin for long-running builtin work.
- Dispatch `FILTER_AGG`, `FILTERED_TOPK_EXACT`, and KV builtin execution through
  `spdk_thread_send_msg()` when compute threads are configured.
- Send the runtime completion callback back to the submit SPDK thread.
- Fall back to the submit SPDK thread when no compute mask is configured.
- Keep the KV request header static assert consistent with the packed structure
  size, currently `44`.

### NVMf poll groups

Files:

- `module/event/subsystems/nvmf/nvmf_tgt.c`
- `module/event/subsystems/nvmf/Makefile`

Required behavior:

- Exclude `cpcs_builtin_runtime_is_compute_core(lcore)` from poll-group core
  selection.
- Fail startup if no poll-group cores remain after excluding compute cores.
- Create each NVMf poll-group SPDK thread with a single non-compute core mask.
- Add the include path needed for `nvmf/cpcs/builtin_runtime.h`.

### Experiment scripts

Files:

- `test/cpcs/vslm_pslm_perf_compare.py`
- `test/cpcs/cpcs_data_movement_compare.py`
- `test/cpcs/cpcs_slm_capacity_gap_compare.py`
- `test/cpcs/cpcs_transport_bottleneck_compare.py`
- `test/cpcs/cpcs_vector_eval_compare.py`
- `test/cpcs/experiment_platform.py`
- `test/cpcs/examples/inventory_*.yaml`

Required behavior:

- `SpdkTarget` accepts an optional `compute_core_mask`.
- Local and remote target launches append
  `--cpcs-compute-core-mask <mask>` when set.
- Scenario parsers expose `--compute-core-mask`.
- Vector evaluation exposes `--target-compute-core-mask`.
- Inventory parsing reads `runtime.target_compute_core_mask` and forwards it to
  the correct scenario flag.

## Apply Plan For Another Agent

1. Create a branch in the fork.

```bash
git checkout -b cpcs-compute-core-mask
```

2. Apply or reimplement the change in the files listed above.

If a direct patch is available, apply it first:

```bash
git apply /path/to/cpcs-compute-core-mask.patch
```

If the fork has diverged, reimplement by subsystem in this order:

1. Add runtime APIs in `lib/nvmf/cpcs/builtin_runtime.h`.
2. Add compute SPDK thread management in `lib/nvmf/cpcs/builtin_runtime.c`.
3. Replace pthread worker execution in `builtin_runtime.c` with
   `spdk_thread_send_msg()` dispatch.
4. Add the `spdk_tgt` CLI option and startup/shutdown lifecycle.
5. Exclude compute cores from NVMf poll-group creation.
6. Add scenario script and inventory forwarding.
7. Update docs and inventory examples.

3. Run the kickoff script from the SPDK root.

```bash
./test/cpcs/cpcs_compute_core_kickoff.sh check
./test/cpcs/cpcs_compute_core_kickoff.sh validate
```

4. Review the output and fix missing markers before running heavier integration
   tests.

## Validation Commands

Lightweight checks:

```bash
rg -n "pthread_create|pthread_detach" lib/nvmf/cpcs/builtin_runtime.c
python3 -m py_compile \
  test/cpcs/vslm_pslm_perf_compare.py \
  test/cpcs/cpcs_data_movement_compare.py \
  test/cpcs/cpcs_slm_capacity_gap_compare.py \
  test/cpcs/cpcs_transport_bottleneck_compare.py \
  test/cpcs/cpcs_vector_eval_compare.py \
  test/cpcs/experiment_platform.py
cc -fsyntax-only -Iinclude -Ilib -Ilib/nvmf -Ilib/nvmf/cpcs \
  -Ibuild/include -include include/spdk/config.h \
  app/spdk_tgt/spdk_tgt.c
cc -fsyntax-only -Iinclude -Ilib -Ilib/nvmf -Ilib/nvmf/cpcs \
  -Ibuild/include -include include/spdk/config.h \
  module/event/subsystems/nvmf/nvmf_tgt.c
cc -fsyntax-only -Iinclude -Ilib -Ilib/nvmf -Ilib/nvmf/cpcs \
  -Ibuild/include -include include/spdk/config.h \
  lib/nvmf/cpcs/builtin_runtime.c
```

Build checks:

```bash
make -C module/event/subsystems/nvmf DEFAULT_CC="${CC:-cc}" V=1
make -C app/spdk_tgt DEFAULT_CC="${CC:-cc}" V=1
```

If the full fork environment is configured:

```bash
./configure --with-cpcs
make
```

## Expected Markers

The following markers should exist after the change is applied:

- `--cpcs-compute-core-mask` in `app/spdk_tgt/spdk_tgt.c`
- `spdk_tgt_validate_cpcs_compute_cores()` in `app/spdk_tgt/spdk_tgt.c`
- `cpcs_builtin_runtime_start_compute_threads()` in
  `lib/nvmf/cpcs/builtin_runtime.c`
- `spdk_thread_send_msg(thread, _cpcs_builtin_extended_msg, ...)` in
  `lib/nvmf/cpcs/builtin_runtime.c`
- `SPDK_STATIC_ASSERT(sizeof(struct cpcs_builtin_kv_req_header) == 44, ...)`
  in `lib/nvmf/cpcs/builtin_runtime.c`
- `cpcs_builtin_runtime_is_compute_core(cpu)` in
  `module/event/subsystems/nvmf/nvmf_tgt.c`
- `target_compute_core_mask` in `test/cpcs/experiment_platform.py`

## Known Porting Notes

- Do not use `spdk_app_parse_core_mask()` for this option if the fork needs to
  reject out-of-range compute cores. That helper intersects the parsed mask with
  the app mask, which can hide mistakes. Parse with `spdk_cpuset_parse()` and
  validate explicitly after SPDK app initialization.
- The compute threads are SPDK lightweight threads, not OS worker threads.
- The NVMf poll-group exclusion only reserves cores that are part of the app
  core mask. A compute mask outside `-m` should fail startup.
- Some environments may fail `make -C lib/nvmf` before reaching CPCS if OpenSSL
  development headers are missing. Use direct syntax probes to isolate CPCS
  compile issues from dependency setup.

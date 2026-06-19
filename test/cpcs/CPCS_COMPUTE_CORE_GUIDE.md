# CPCS Compute Core Mask Guide

## Operator Interface

Start `spdk_tgt` with a normal SPDK app core mask and a CPCS compute subset:

```bash
build/bin/spdk_tgt -m 0xF --cpcs-compute-core-mask 0xC --wait-for-rpc
```

Rules:

- `--cpcs-compute-core-mask` accepts the same mask/list syntax as `-m`.
- The compute mask must be non-empty and must be part of the app core mask.
- NVMf poll groups skip CPCS compute cores, so those cores are reserved for builtin compute work.
- Long-running builtin work is dispatched round-robin to CPCS compute SPDK threads, then completion is sent back to the submit SPDK thread.

## Experiment Inventory

Use `runtime.target_compute_core_mask`:

```yaml
runtime:
  target_core_mask: "0xF"
  target_compute_core_mask: "0xC"
```

`cpcs_experiments.py` forwards this as:

- `--compute-core-mask` for normal scenarios.
- `--target-compute-core-mask` for `vector-eval`.

The scenario launchers then pass `--cpcs-compute-core-mask` to `spdk_tgt`.

## Fork Merge Handoff

For a file-by-file merge checklist and validation commands, see:

```bash
test/cpcs/CPCS_COMPUTE_CORE_MERGE_GUIDE.md
./test/cpcs/cpcs_compute_core_kickoff.sh check
```

## Code Change Checklist

1. Add `--cpcs-compute-core-mask` to `app/spdk_tgt/spdk_tgt.c`.
2. Store the parsed mask through `cpcs_builtin_runtime_set_compute_core_mask()`.
3. Start compute SPDK threads from `spdk_tgt_started()` with `cpcs_builtin_runtime_start_compute_threads()`.
4. Stop compute SPDK threads from the app shutdown callback.
5. Exclude `cpcs_builtin_runtime_is_compute_core(lcore)` from NVMf poll-group creation in `module/event/subsystems/nvmf/nvmf_tgt.c`.
6. In `lib/nvmf/cpcs/builtin_runtime.c`, dispatch FILTER_AGG, FILTERED_TOPK_EXACT, and KV builtin work to compute threads when configured.
7. Preserve completion affinity by sending the runtime completion callback back to the submit SPDK thread.

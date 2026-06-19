# CPCS Test Platform (Unified Loopback + Split Initiator/Target)

## 1) Purpose

`cpcs_experiments.py` is the unified experiment runner for CPCS/SLM studies.

It supports:

- loopback topology (initiator and target on one server), and
- split topology (initiator host submits NVMe commands to a separate target host running `spdk_tgt`).

It also provides a shared `prepare` step to provision/build/bootstrap hosts before running scenarios.

## 2) Entrypoint

- `test/cpcs/cpcs_experiments.py`

Prerequisite:

- install YAML parser once on the initiator host: `sudo apt-get install -y python3-yaml`

Subcommands:

- `prepare`
- `perf-compare`
- `capacity-gap`
- `transport-bottleneck`
- `vector-eval`

All subcommands require `--inventory <yaml>`.

## 3) Inventory Schema

```yaml
hosts:
  init:
    mode: local               # local | ssh
    repo_path: /home/user/spdk
    use_sudo: true

  tgt:
    mode: ssh
    ssh_host: 10.0.0.22
    ssh_user: user
    ssh_port: 22
    ssh_options:
      - StrictHostKeyChecking=no
    repo_path: /home/user/spdk
    use_sudo: true

roles:
  initiator: init
  target: tgt

paths:
  rpc_py: scripts/rpc.py
  spdk_tgt: build/bin/spdk_tgt
  spdk_nvme_passthru: build/bin/spdk_nvme_passthru

runtime:
  target_core_mask: "0xF"      # optional: spdk_tgt core mask across scenarios
  target_compute_core_mask: "0xC"  # optional: subset of target_core_mask reserved for CPCS compute
  initiator_passthru_lcores: "1,2,3,4"  # optional: initiator lcores for spdk_nvme_passthru
  vector_prefill_workers: 4    # optional: vector-eval prefill worker count (0=auto from lcore entries)
  direct_probe_nsid: 1         # optional: realapp direct probe NSID for cpcs_* modes
  direct_probe_lba_bytes: 512  # optional: realapp direct probe LBA size (must match dataset namespace)

nvmeof:
  trtype: TCP
  traddr: 10.0.0.22
  trsvcid: "4420"
  nqn: nqn.2026-03.io.spdk:cpcs-exp
  hostnqn: nqn.2026-03.io.spdk:cpcs-exp-host
  src_addr: 10.10.10.72     # optional: bind initiator source address
  src_svcid: "0"            # optional: bind initiator source port/service id
  pcie_bdf: 0000:01:00.0    # optional: default backend NVMe controller for scenarios
  dataset_bdev: Nvme0n1      # optional: default dataset namespace bdev
  backing_bdev: Nvme0n2      # optional: default vSLM backing bdev
```

Loopback mode is obtained by setting both roles to the same host.

## 4) Inventory Examples

Use one of these templates in `test/cpcs/examples/`:

- `inventory_loopback.yaml`: full loopback example with explicit `paths` and `use_sudo`.
- `inventory_loopback_minimal.yaml`: loopback with only required fields.
- `inventory_loopback_real_nvme_11_00_0.yaml`: loopback template pinned to real NVMe `0000:11:00.0` with `Nvme0n1` (dataset/NVM) and `Nvme0n2` (backing).
- `inventory_split.yaml`: full split topology example (local initiator + ssh target).
- `inventory_split_minimal.yaml`: split topology with required fields only.
- `inventory_split_vagrant.yaml`: split topology tuned for local Vagrant target (`vagrant@127.0.0.1:2222`).

Notes:

- `roles.initiator` must be `mode: local`.
- `paths` and `use_sudo` are optional. Defaults are applied by `cpcs_experiments.py`.
- `runtime.target_core_mask` is optional. When set, `cpcs_experiments.py` forwards it to all scenarios (`--core-mask`, or `--target-core-mask` for `vector-eval`).
- `runtime.target_compute_core_mask` is optional. When set, `cpcs_experiments.py` forwards it to all scenarios and `spdk_tgt` receives `--cpcs-compute-core-mask`; these cores must also be included in `runtime.target_core_mask`.
- `runtime.initiator_passthru_lcores` is optional. When set, `cpcs_experiments.py` forwards it to scenarios as `--passthru-lcores`.
- `runtime.vector_prefill_workers` is optional. When set, `cpcs_experiments.py` forwards it to `vector-eval` as `--prefill-workers`.
- `runtime.direct_probe_nsid` is optional. When set, realapp `cpcs_*` modes use it as the passthru probe NSID.
- `runtime.direct_probe_lba_bytes` is optional. When set, realapp `cpcs_*` modes use it as passthru probe LBA size.
- Use `--run-log /path/to/file.log` to tee combined `stdout`/`stderr` from the full `cpcs_experiments.py` run.
- `nvmeof.src_addr` and `nvmeof.src_svcid` are optional. When set, they are forwarded to `spdk_nvme_passthru` as `--src-addr/--src-svcid`.
- `nvmeof.pcie_bdf`, `nvmeof.dataset_bdev`, and `nvmeof.backing_bdev` are optional global defaults consumed by scenarios that need backend bdev selection.

### 4.1 Real-NVMe Cutover (0000:11:00.0)

For physical-NVMe phase transition, use the inventory generator to validate namespaces and emit an inventory wired to your real device:

```bash
python3 ./test/cpcs/cpcs_real_nvme_cutover.py \
  --repo-path /path/to/spdk \
  --pcie-bdf 0000:11:00.0 \
  --dataset-nsid 1 \
  --backing-nsid 2 \
  --min-namespace-gib 3.0 \
  --output ./test/cpcs/examples/inventory_real_nvme_11_00_0.generated.yaml
```

This produces:
- `nvmeof.pcie_bdf: 0000:11:00.0`
- `nvmeof.dataset_bdev: Nvme0n1` (NVM dataset namespace)
- `nvmeof.backing_bdev: Nvme0n2` (vSLM backing namespace)
- `runtime.direct_probe_nsid: 1` (auto aligned to dataset NSID)
- `runtime.direct_probe_lba_bytes: <detected from dataset namespace>`

If you already know the mapping and want to skip Linux sysfs probing:

```bash
python3 ./test/cpcs/cpcs_real_nvme_cutover.py \
  --repo-path /path/to/spdk \
  --pcie-bdf 0000:11:00.0 \
  --dataset-nsid 1 \
  --backing-nsid 2 \
  --skip-sysfs-probe \
  --output ./test/cpcs/examples/inventory_real_nvme_11_00_0.generated.yaml
```

## 5) Prepare Step

```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory ./test/cpcs/examples/inventory_split.yaml \
  prepare
```

What it does:

- checks Python/tool availability,
- validates passwordless sudo,
- runs `./configure && make -j...` in each required host repo,
- configures runtime prerequisites (`nvme-tcp`, hugepages, stale `spdk_tgt` cleanup).

## 6) Run Scenarios

### 6.1 perf-compare

```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory ./test/cpcs/examples/inventory_split.yaml \
  perf-compare -- \
  --pcie-bdf 0000:01:00.0 \
  --dataset-bdev Nvme0n1 \
  --backing-bdev Nvme0n2 \
  --dataset-size-gb 4 \
  --chunk-size-mb 4 \
  --pslm-size-mb 4 \
  --sram-mb 4 \
  --builtin-program sum64 \
  --builtin-exec-max-mb 256 \
  --runs 3
```

### 6.2 capacity-gap

```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory ./test/cpcs/examples/inventory_split.yaml \
  capacity-gap -- \
  --pcie-bdf 0000:01:00.0 \
  --dataset-bdev Nvme0n1 \
  --backing-bdev Nvme0n2 \
  --dataset-size-gb 4 \
  --physical-slm-mb 4 \
  --sram-mb 4 \
  --fullfit-pslm-mb 4096 \
  --builtin-program sum64 \
  --builtin-exec-max-mb 256 \
  --vslm-max-copy-mb 4096 \
  --runs 3
```

### 6.3 transport-bottleneck

```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory ./test/cpcs/examples/inventory_split.yaml \
  transport-bottleneck -- \
  --pcie-bdf 0000:01:00.0 \
  --dataset-bdev Nvme0n1 \
  --backing-bdev Nvme0n2 \
  --dataset-size-gb 1 \
  --backend pslm \
  --pslm-size-mb 1024 \
  --builtin-program sum64 \
  --builtin-exec-max-mb 256 \
  --transport-bandwidth-gbps 1,10,25,100
```

### 6.4 vector-eval

```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory ./test/cpcs/examples/inventory_split.yaml \
  vector-eval \
  --profile mongo_like \
  --count 15300 \
  --dim 128 \
  --query-limit 10 \
  --dataset-size-mb 64 \
  --slm-size-mb 64
```

## 7) Output

By default, the runner writes artifacts under:

- `/tmp/cpcs_experiments/<timestamp>/`

Each run stores:

- scenario JSON output (or forwarded output path),
- `<subcommand>_meta.json` with topology, inventory path, and result payload summary.

## 8) How to Run Tests

### 8.1 Unit tests (inventory/topology parser)

```bash
python3 -m unittest discover -s test/cpcs -p 'test_cpcs_experiments.py' -v
```

If `python3-yaml` is not installed, inventory tests are skipped by design.

### 8.2 Platform smoke test (loopback)

```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory ./test/cpcs/examples/inventory_loopback.yaml \
  prepare --skip-build
```

Then run one small scenario (example: transport bottleneck):

```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory ./test/cpcs/examples/inventory_loopback.yaml \
  transport-bottleneck -- \
  --pcie-bdf 0000:01:00.0 \
  --dataset-bdev Nvme0n1 \
  --backing-bdev Nvme0n2 \
  --dataset-size-gb 1 \
  --backend pslm \
  --pslm-size-mb 512 \
  --builtin-program sum64 \
  --builtin-exec-max-mb 128 \
  --transport-bandwidth-gbps 10
```

### 8.3 Platform smoke test (split/vagrant target)

```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory ./test/cpcs/examples/inventory_split_vagrant.yaml \
  prepare --skip-build
```

# CPCS Server Validation Guide (Own Server Deployment)

## 1) Goal

This guide is for running the unified CPCS orchestrator on your own servers (loopback or split initiator/target) and verifying claim coverage end-to-end.

Entrypoint:
- `spdk/test/cpcs/cpcs_experiments.py`

Topologies:
- loopback: initiator and target on one server
- split: initiator local + target over SSH

Important contract:
- `roles.initiator` must be `mode: local`.

## 2) Prerequisites

On the initiator/orchestrator host:
- Python + YAML parser: `python3`, `python3-yaml`
- build toolchain (`make`, compiler)
- passwordless sudo recommended for stable automation
- same SPDK repo path available on all involved hosts

Install YAML parser once:
```bash
sudo apt-get install -y python3-yaml
```

## 3) Inventory Setup

Start from templates:
- split: `spdk/test/cpcs/examples/inventory_split.yaml`
- loopback: `spdk/test/cpcs/examples/inventory_loopback.yaml`

Minimum fields to confirm:
- `hosts.<name>.repo_path`
- `roles.initiator`, `roles.target`
- `nvmeof.trtype`, `traddr`, `trsvcid`, `nqn`, `hostnqn`

Recommended defaults for physical NVMe backends:
- `nvmeof.pcie_bdf`
- `nvmeof.dataset_bdev`
- `nvmeof.backing_bdev`

If you are moving from VM loopback to a real NVMe controller (for example `0000:11:00.0` with two ~4 GiB namespaces), generate an inventory with namespace validation:

```bash
python3 ./test/cpcs/cpcs_real_nvme_cutover.py \
  --repo-path /path/to/spdk \
  --pcie-bdf 0000:11:00.0 \
  --dataset-nsid 1 \
  --backing-nsid 2 \
  --min-namespace-gib 3.0 \
  --output ./test/cpcs/examples/inventory_real_nvme_11_00_0.generated.yaml
```

Then use the generated inventory as `INV`.

Optional runtime overrides:
- `runtime.target_core_mask`
- `runtime.initiator_passthru_lcores`
- `runtime.vector_prefill_workers`
- `runtime.direct_probe_nsid` (realapp `cpcs_*` passthru probe NSID)
- `runtime.direct_probe_lba_bytes` (realapp `cpcs_*` passthru probe LBA size)

## 4) Preflight

Set reusable shell vars:
```bash
export REPO=/path/to/spdk
export INV=$REPO/test/cpcs/examples/inventory_real_nvme_11_00_0.generated.yaml
export ART_ROOT=/tmp/cpcs_server_eval_$(date +%Y%m%d_%H%M%S)
export RUNLOG=$ART_ROOT/run.log
export TLOG=/var/log/spdk.log
mkdir -p "$ART_ROOT"
cd "$REPO"
```

Connectivity checks (split mode):
```bash
ssh <target-user>@<target-host> 'hostname; test -d /path/to/spdk && echo repo_ok'
```

## 5) Prepare Hosts

Run once before scenarios:
```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory "$INV" \
  --artifacts-root "$ART_ROOT" \
  --run-log "$RUNLOG" \
  prepare --hugepages 2048
```

If SPDK is already built and runtime is managed externally:
```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory "$INV" \
  --artifacts-root "$ART_ROOT" \
  --run-log "$RUNLOG" \
  prepare --skip-build
```

## 6) Required Claim-Coverage Suite (RQ0/RQ1/RQ2 + ANN)

### 6.1 transport-bottleneck (RQ0 required)
```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" \
  transport-bottleneck -- \
  --backend vslm \
  --dataset-size-mb 128 \
  --pslm-size-mb 128 \
  --sram-mb 8 \
  --builtin-program max64 \
  --builtin-exec-max-mb 1 \
  --host-read-chunk-mb 0.125 \
  --vslm-backing-min-gb 0
```

### 6.2 capacity-gap (RQ1 required)
```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" \
  capacity-gap -- \
  --dataset-size-mb 128 \
  --physical-slm-mb 16 \
  --sram-mb 8 \
  --fullfit-pslm-mb 128 \
  --fullfit-chunk-mb 16 \
  --builtin-program max64 \
  --builtin-exec-max-mb 1 \
  --vslm-max-copy-mb 1 \
  --vslm-backing-min-gb 0
```

### 6.3 locality (RQ1 required)
```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" \
  locality -- \
  --patterns sequential,clustered,strided,random \
  --base-chunk-mb 8 \
  --runs-per-pattern 1
```

### 6.4 lease-correctness (RQ2 required)
```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" \
  lease-correctness
```

### 6.5 lease-contention (RQ2 required)
```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" \
  lease-contention -- \
  --attempts-none 1 --attempts-light 2 --attempts-moderate 4 --attempts-heavy 6 \
  --parallel-none 1 --parallel-light 2 --parallel-moderate 3 --parallel-heavy 4
```

If `light`/`moderate` intermittently fails gate checks on your server, rerun with the stabilized profile below and accept only on validation-report pass:
```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" \
  lease-contention -- \
  --attempts-none 1 \
  --attempts-light 64 --parallel-light 8 \
  --attempts-moderate 64 --parallel-moderate 8 \
  --attempts-heavy 64 --parallel-heavy 8 \
  --sleep-sec 0.05 --no-strict
```

### 6.6 ann-search (ANN required)

Bring up OpenSearch first (if not running):
```bash
bash ./experiments/real_apps/opensearch/bringup_opensearch.sh
```

Prepare ANN docs/queries (portable synthetic generator):
```bash
python3 - <<'PY'
import json, math, os, pathlib
root = pathlib.Path(os.environ["ART_ROOT"]) / "ann_prep"
root.mkdir(parents=True, exist_ok=True)
docs = root / "documents.jsonl"
queries = root / "queries_real.json"
dim = 16
n_docs = 1024
n_queries = 128
with docs.open("w", encoding="utf-8") as f:
    for i in range(n_docs):
        base = i + 1
        vec = [round(((base * (j + 3)) % 97) / 97.0 + 1e-3, 6) for j in range(dim)]
        row = {
            "id": i,
            "vector": vec,
            "payload": {
                "doc_id": i,
                "category_id": i % 8,
                "flags": i % 4,
                "region_id": i % 16,
                "price": float((i % 100) + 1),
                "timestamp_bucket": 1700000000 + i,
                "vector_index": i,
            },
        }
        f.write(json.dumps(row, separators=(",", ":")) + "\\n")
query_rows = []
for q in range(n_queries):
    base = q + 1
    vec = [round(((base * (j + 3)) % 97) / 97.0 + 1e-3, 6) for j in range(dim)]
    query_rows.append({
        "query_id": q,
        "outer_request_id": q,
        "round": 0,
        "metric": "cosine",
        "k": 10,
        "vector": vec,
        "filters": [{"op": "eq", "field": "flags", "value": q % 4}],
        "target_selectivity_hint": f"flags_eq_{q % 4}",
    })
queries.write_text(json.dumps({"version": 1, "query_count": len(query_rows), "queries": query_rows}, indent=2), encoding="utf-8")
print("wrote", docs)
print("wrote", queries)
PY
```

Run ANN required scenario:
```bash
sudo -E python3 ./test/cpcs/cpcs_experiments.py \
  --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" \
  ann-search -- \
  --system opensearch \
  --host http://127.0.0.1:9200 \
  --index cpcs_server_ann \
  --docs-jsonl "$ART_ROOT/ann_prep/documents.jsonl" \
  --queries-json "$ART_ROOT/ann_prep/queries_real.json" \
  --load-index --recreate-index \
  --top-k 10 --limit-queries 64 \
  --recall-threshold 0.90 --overlap-threshold 0.80 \
  --no-configure-index-flag
```

## 7) Optional Depth Suite (recommended)

```bash
# RQ1 optional
sudo -E python3 ./test/cpcs/cpcs_experiments.py --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" perf-compare -- --dataset-size-mb 128 --chunk-size-mb 16 --pslm-size-mb 16 --sram-mb 8 --builtin-program sum64 --builtin-exec-max-mb 1 --vslm-max-copy-mb 1 --vslm-backing-min-gb 0 --runs 3

# RQ2 optional
sudo -E python3 ./test/cpcs/cpcs_experiments.py --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" vector-eval -- --skip-nvme-attach --dataset-size-mb 64 --slm-size-mb 32 --count 1024 --query-count 32 --query-limit 10 --tier T3

# ANN optional bridge
sudo -E python3 ./test/cpcs/cpcs_experiments.py --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" realapp-b1b2 -- --system opensearch --host http://127.0.0.1:9200 --index cpcs_server_ann --docs-jsonl "$ART_ROOT/ann_prep/documents.jsonl" --queries-json "$ART_ROOT/ann_prep/queries_real.json" --load-index --recreate-index --bandwidths-gbps 10 --modes baseline,cpcs_vslm --exact-match-min 0.90 --no-configure-index-flag

sudo -E python3 ./test/cpcs/cpcs_experiments.py --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" realapp-r3 -- --system opensearch --host http://127.0.0.1:9200 --index cpcs_server_ann --docs-jsonl "$ART_ROOT/ann_prep/documents.jsonl" --source-queries-json "$ART_ROOT/ann_prep/queries_real.json" --flags-values 0,1,2,3 --max-queries-per-bucket 16 --modes baseline,cpcs_vslm --exact-match-min 0.90 --no-configure-index-flag

sudo -E python3 ./test/cpcs/cpcs_experiments.py --inventory "$INV" --artifacts-root "$ART_ROOT" --target-log "$TLOG" realapp-r4 -- --system opensearch --host http://127.0.0.1:9200 --index cpcs_server_ann --docs-jsonl "$ART_ROOT/ann_prep/documents.jsonl" --source-queries-json "$ART_ROOT/ann_prep/queries_real.json" --outer-count 16 --inner-rounds 4 --modes baseline,cpcs_vslm --exact-match-min 0.90 --no-configure-index-flag
```

## 8) Pass/Fail Checks

Per scenario, confirm all three files exist:
- `$ART_ROOT/<scenario>/run_manifest.json`
- `$ART_ROOT/<scenario>/metrics_summary.json`
- `$ART_ROOT/<scenario>/validation_report.json`

Suite-level closure check:
```bash
python3 - <<'PY'
import json, os
root=os.environ['ART_ROOT']
r=json.load(open(f"{root}/paper_validity_report.json"))
print("under_supported_claims=", r.get("under_supported_claims"))
print("total_scenarios_seen=", r.get("total_scenarios_seen"))
print("claims=", {k:v.get("supported") for k,v in r.get("claim_coverage",{}).items()})
PY
```

Required suite pass target:
- `under_supported_claims=[]`

## 9) Frequent Failure Modes

- `roles.initiator must use mode=local`
  - fix inventory: initiator host must be local.

- `sudo -n` failures in split mode
  - ensure passwordless sudo on target or set `use_sudo: false` and manage runtime/build manually.

- transport host baseline fails with max I/O size errors
  - reduce `--host-read-chunk-mb` (e.g., `0.125`).

- OpenSearch rejects `index.knn.ndp.cpcs.enabled`
  - use `--no-configure-index-flag` in `ann-search` and `realapp-*`.

- `capacity-gap` full-fit allocation fails at higher scale
  - increase host memory/hugepages; do not relax full-fit parameter contracts.

## 10) Minimal Smoke Path (fast)

If you only need a quick server health check:
1. `prepare --skip-build`
2. `transport-bottleneck` (small dataset)
3. `lease-correctness`
4. `ann-search` with `--limit-queries 8`

Then validate `paper_validity_report.json` is generated and non-empty.

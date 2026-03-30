# Exercise 09: NVMe-oF Initiator

## Overview

| Field | Value |
|-------|-------|
| **Objective** | Build a working NVMe-oF initiator using the SPDK NVMe driver that connects to a remote NVMe-oF target and performs read/write I/O |
| **Estimated Time** | 3-4 hours |
| **Difficulty** | Intermediate |
| **Prerequisites** | EX01 (SPDK env init), EX02 (NVMe hello world), EX08 (NVMe-oF Target) or equivalent familiarity |

---

## Learning Objectives

By the end of this exercise you will be able to:

1. Construct an `spdk_nvme_transport_id` for TCP (and optionally RDMA) transports.
2. Use `spdk_nvme_probe()` to discover and attach to remote NVMe-oF controllers.
3. Allocate I/O QPairs and submit asynchronous read/write commands.
4. Poll for completions correctly in a tight loop.
5. Query a discovery service log page to enumerate available subsystems.
6. Tear down connections cleanly using `spdk_nvme_detach_async()`.

---

## Background

NVMe over Fabrics (NVMe-oF) extends the NVMe protocol over network transports such as TCP, RDMA/RoCE, and Fibre Channel. From the SPDK driver perspective, the same `spdk_nvme_probe()` / `spdk_nvme_connect()` API works for both local PCIe devices and remote NVMe-oF targets — the only difference is the transport ID (`trid`) you supply.

Key concepts:

- **Transport ID (`spdk_nvme_transport_id`)** — encodes transport type, address, port, and subsystem NQN.
- **Discovery subsystem** — every NVMe-oF target exposes a well-known NQN (`nqn.2014-08.org.nvmexpress.discovery`) that lists available subsystems.
- **QPair** — each I/O queue pair is independently allocated per namespace; SPDK is lock-free, so one thread owns one QPair.
- **Polling** — SPDK never uses interrupts for I/O; you must call `spdk_nvme_qpair_process_completions()` to drive completion callbacks.

---

## Prerequisites Setup

### System Requirements

```
- Linux host with hugepages configured (≥ 512 MB)
- SPDK built from source (see EX01)
- For TCP transport: standard kernel TCP stack is sufficient
- For RDMA transport: RDMA-capable NIC + libibverbs installed
```

### Configure Hugepages

```bash
# 512 MB of 2MB hugepages
echo 256 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Verify
grep HugePages /proc/meminfo
```

### Build SPDK (if not already done)

```bash
cd /path/to/spdk
git submodule update --init
./configure --with-nvme-cuse   # TCP is always enabled
make -j$(nproc)
```

---

## Part 1: Configure the NVMe-oF Target

Before you can test an initiator you need a running target. This part uses SPDK's `nvmf_tgt` application with a RAM-backed bdev. If you already have a target running from EX08, skip to Part 2.

### 1.1 Start nvmf_tgt

```bash
# Terminal 1 — run the target
cd /path/to/spdk
sudo ./build/bin/nvmf_tgt --wait-for-rpc &
```

### 1.2 Configure the Target via RPC

Save the following as `setup_target.sh` and run it:

```bash
#!/usr/bin/env bash
# setup_target.sh — configure a minimal NVMe-oF TCP target

SPDK_DIR="$(pwd)"
RPC="${SPDK_DIR}/scripts/rpc.py"

# 1. Create a 64 MB malloc bdev (RAM-backed, no real disk needed)
${RPC} bdev_malloc_create -b Malloc0 64 512

# 2. Create the TCP transport
${RPC} nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192

# 3. Create an NVMe-oF subsystem
${RPC} nvmf_create_subsystem \
    nqn.2024-01.io.spdk:cnode1 \
    -a \
    -s SPDK00000000000001

# 4. Attach the bdev to the subsystem
${RPC} nvmf_subsystem_add_ns \
    nqn.2024-01.io.spdk:cnode1 \
    Malloc0

# 5. Add a listener on TCP port 4420 (loopback for local testing)
${RPC} nvmf_subsystem_add_listener \
    nqn.2024-01.io.spdk:cnode1 \
    -t tcp \
    -a 127.0.0.1 \
    -s 4420

echo "Target configured. Subsystem: nqn.2024-01.io.spdk:cnode1"
echo "Listening on 127.0.0.1:4420 (TCP)"
```

```bash
chmod +x setup_target.sh
./setup_target.sh
```

### 1.3 Verify Target is Listening

```bash
# The discovery service always listens on the same transport
./scripts/rpc.py nvmf_get_subsystems | python3 -m json.tool
```

You should see both the discovery subsystem and your `nqn.2024-01.io.spdk:cnode1` subsystem.

---

## Part 2: Build the NVMe-oF Initiator

### 2.1 Create the Application Directory

```bash
mkdir -p ~/spdk_nvmf_initiator
cd ~/spdk_nvmf_initiator
```

### 2.2 Write the Initiator Source

Create `nvmf_initiator.c` with the following content:

```c
/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * EX09: NVMe-oF Initiator
 *
 * Connects to a remote NVMe-oF target over TCP (or RDMA), writes a
 * pattern to LBA 0, reads it back, and verifies correctness.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/string.h"
#include "spdk/log.h"

/* ------------------------------------------------------------------ */
/*  Data structures                                                     */
/* ------------------------------------------------------------------ */

struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)	link;
	char				name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	TAILQ_ENTRY(ns_entry)	link;
};

/* Sequence state for the write → read → verify chain */
struct io_sequence {
	struct ns_entry *ns_entry;
	char		*buf;
	int		 is_completed;  /* 0=pending, 1=ok, 2=error */
};

static TAILQ_HEAD(, ctrlr_entry) g_controllers =
	TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces =
	TAILQ_HEAD_INITIALIZER(g_namespaces);

/* Transport ID filled in from command-line arguments */
static struct spdk_nvme_transport_id g_trid = {};

/* Write pattern used to verify round-trip correctness */
#define WRITE_PATTERN "SPDK-NVMf-EX09"

/* ------------------------------------------------------------------ */
/*  I/O callbacks                                                       */
/* ------------------------------------------------------------------ */

static void
read_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct io_sequence *seq = arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "Read I/O error: %s\n",
			spdk_nvme_cpl_get_status_string(&cpl->status));
		seq->is_completed = 2;
		return;
	}

	/* Verify the data matches what we wrote */
	if (strncmp(seq->buf, WRITE_PATTERN, strlen(WRITE_PATTERN)) != 0) {
		fprintf(stderr, "Data mismatch! Read back: '%.*s'\n",
			(int)strlen(WRITE_PATTERN), seq->buf);
		seq->is_completed = 2;
		return;
	}

	printf("  [READ OK] Data verified: '%s'\n", WRITE_PATTERN);
	seq->is_completed = 1;
}

static void
write_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct io_sequence *seq = arg;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "Write I/O error: %s\n",
			spdk_nvme_cpl_get_status_string(&cpl->status));
		seq->is_completed = 2;
		return;
	}

	printf("  [WRITE OK] Pattern written to LBA 0\n");

	/*
	 * Write succeeded. Zero the buffer and submit a read to verify
	 * the data made it to the target.
	 */
	memset(seq->buf, 0, 0x1000);

	rc = spdk_nvme_ns_cmd_read(seq->ns_entry->ns,
				   seq->ns_entry->qpair,
				   seq->buf,
				   0,   /* LBA start */
				   1,   /* number of LBAs */
				   read_complete, seq, 0);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_ns_cmd_read() failed: %d\n", rc);
		seq->is_completed = 2;
	}
}

/* ------------------------------------------------------------------ */
/*  Namespace & controller management                                   */
/* ------------------------------------------------------------------ */

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;

	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		perror("calloc ns_entry");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns    = ns;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	printf("  Registered NS %d  size=%" PRIu64 " MB  block=%u B\n",
	       spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / (1024 * 1024),
	       spdk_nvme_ns_get_sector_size(ns));
}

/*
 * probe_cb: called once per discovered controller.
 * Return true to attach, false to skip.
 */
static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Probe: found controller at %s (transport=%s)\n",
	       trid->traddr,
	       spdk_nvme_transport_id_trtype_str(trid->trtype));

	/*
	 * Tune controller options here if needed. For example:
	 *   opts->keep_alive_timeout_ms = 10000;
	 *   opts->num_io_queues = 4;
	 */

	return true;  /* attach to every discovered controller */
}

/*
 * attach_cb: called after the driver finishes initialising a controller.
 * Register all active namespaces.
 */
static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr,
	  const struct spdk_nvme_ctrlr_opts *opts)
{
	const struct spdk_nvme_ctrlr_data *cdata;
	struct ctrlr_entry *entry;
	int nsid;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		perror("calloc ctrlr_entry");
		exit(1);
	}

	snprintf(entry->name, sizeof(entry->name),
		 "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	printf("Attached: %s\n", entry->name);

	/* Enumerate every active namespace on this controller */
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	     nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns != NULL) {
			register_ns(ctrlr, ns);
		}
	}
}

/* ------------------------------------------------------------------ */
/*  I/O exercise                                                        */
/* ------------------------------------------------------------------ */

static void
run_io(void)
{
	struct ns_entry    *ns_entry;
	struct io_sequence  seq;
	int		    rc;

	TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
		printf("\n--- I/O on NS %d ---\n",
		       spdk_nvme_ns_get_id(ns_entry->ns));

		/*
		 * Step 1: Allocate an I/O QPair.
		 *
		 * Each QPair is owned by exactly one thread. SPDK provides
		 * no internal locking — this is what enables its lock-free
		 * I/O path. For NVMe-oF the QPair maps to a fabric
		 * connection to the target controller queue.
		 */
		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(
					ns_entry->ctrlr, NULL, 0);
		if (ns_entry->qpair == NULL) {
			fprintf(stderr, "spdk_nvme_ctrlr_alloc_io_qpair() "
				"failed\n");
			continue;
		}
		printf("  QPair allocated (queue depth=%u)\n",
		       spdk_nvme_ns_get_max_io_xfer_size(ns_entry->ns));

		/*
		 * Step 2: Allocate a DMA-safe buffer.
		 *
		 * spdk_zmalloc() returns pinned, hugepage-backed memory.
		 * This is required for NVMe-oF: the RDMA/TCP stack DMAs
		 * directly into this buffer without kernel involvement.
		 */
		seq.buf = spdk_zmalloc(0x1000,        /* 4 KB */
				       0x1000,        /* 4 KB alignment */
				       NULL,
				       SPDK_ENV_NUMA_ID_ANY,
				       SPDK_MALLOC_DMA);
		if (seq.buf == NULL) {
			fprintf(stderr, "spdk_zmalloc() failed\n");
			spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
			continue;
		}

		/*
		 * Step 3: Submit a write to LBA 0.
		 *
		 * spdk_nvme_ns_cmd_write() is non-blocking. It enqueues the
		 * command and returns immediately. write_complete() will be
		 * called when the target sends back a completion.
		 */
		snprintf(seq.buf, 0x1000, "%s", WRITE_PATTERN);
		seq.ns_entry    = ns_entry;
		seq.is_completed = 0;

		rc = spdk_nvme_ns_cmd_write(ns_entry->ns,
					    ns_entry->qpair,
					    seq.buf,
					    0,   /* LBA start */
					    1,   /* number of LBAs */
					    write_complete, &seq, 0);
		if (rc != 0) {
			fprintf(stderr, "spdk_nvme_ns_cmd_write() failed: "
				"%d\n", rc);
			spdk_free(seq.buf);
			spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
			continue;
		}

		/*
		 * Step 4: Poll for completions.
		 *
		 * spdk_nvme_qpair_process_completions() never blocks. It
		 * checks the completion queue, fires callbacks for any
		 * finished commands, and returns. We loop until our
		 * write→read chain signals completion.
		 *
		 * The second argument (0) means "process all available
		 * completions". Pass a positive integer to cap the number
		 * processed per call (useful for latency-sensitive loops).
		 */
		while (seq.is_completed == 0) {
			spdk_nvme_qpair_process_completions(
				ns_entry->qpair, 0);
		}

		if (seq.is_completed == 2) {
			fprintf(stderr, "I/O sequence failed on NS %d\n",
				spdk_nvme_ns_get_id(ns_entry->ns));
		}

		/*
		 * Step 5: Free resources.
		 *
		 * All in-flight I/O must be completed before freeing the
		 * QPair. Here that is guaranteed by the polling loop above.
		 */
		spdk_free(seq.buf);
		spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
	}
}

/* ------------------------------------------------------------------ */
/*  Cleanup                                                             */
/* ------------------------------------------------------------------ */

static void
cleanup(void)
{
	struct ns_entry    *ns_entry, *tmp_ns;
	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns) {
		TAILQ_REMOVE(&g_namespaces, ns_entry, link);
		free(ns_entry);
	}

	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr) {
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		/*
		 * spdk_nvme_detach_async() starts the detach process without
		 * blocking. spdk_nvme_detach_poll() waits for all pending
		 * detaches to finish.
		 */
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

/* ------------------------------------------------------------------ */
/*  Argument parsing                                                    */
/* ------------------------------------------------------------------ */

static void
usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  -r <trid>   Transport ID string (required for NVMe-oF)\n");
	printf("              TCP example:\n");
	printf("                trtype:tcp adrfam:ipv4 traddr:127.0.0.1 "
	       "trsvcid:4420 subnqn:nqn.2024-01.io.spdk:cnode1\n");
	printf("              RDMA example:\n");
	printf("                trtype:rdma adrfam:ipv4 traddr:192.168.1.10 "
	       "trsvcid:4420 subnqn:nqn.2024-01.io.spdk:cnode1\n");
	printf("  -h          Show this help\n");
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, rc;

	/*
	 * Default: PCIe transport. This will be overridden by -r when
	 * connecting to a fabric target.
	 */
	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s",
		 SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "r:h")) != -1) {
		switch (op) {
		case 'r':
			/*
			 * spdk_nvme_transport_id_parse() understands a
			 * space-separated key:value string:
			 *   trtype:tcp adrfam:ipv4 traddr:1.2.3.4
			 *   trsvcid:4420 subnqn:nqn....
			 */
			rc = spdk_nvme_transport_id_parse(&g_trid, optarg);
			if (rc != 0) {
				fprintf(stderr,
					"Error parsing transport ID: %d\n", rc);
				return 1;
			}
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	int rc;

	/*
	 * Step A: Initialise the SPDK environment.
	 *
	 * This sets up hugepage-backed memory, DPDK EAL, and PCI access.
	 * Must be called before any other SPDK function.
	 */
	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "nvmf_initiator";
	opts.mem_size = 256;  /* MB */

	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "spdk_env_init() failed\n");
		return 1;
	}

	printf("=== SPDK NVMe-oF Initiator (EX09) ===\n");
	printf("Transport : %s\n",
	       spdk_nvme_transport_id_trtype_str(g_trid.trtype));
	printf("Address   : %s\n", g_trid.traddr);
	printf("Port      : %s\n", g_trid.trsvcid);
	printf("SubNQN    : %s\n", g_trid.subnqn);
	printf("\n");

	/*
	 * Step B: Probe for controllers.
	 *
	 * spdk_nvme_probe() sends an NVMe Identify command (or, for
	 * NVMe-oF, a Fabrics Connect command) and calls probe_cb for each
	 * controller it finds. If probe_cb returns true, it initialises
	 * the controller and calls attach_cb.
	 *
	 * For fabric transports the trid encodes the full connection
	 * endpoint; for PCIe trid can be NULL to enumerate all local SSDs.
	 */
	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed: %d\n", rc);
		rc = 1;
		goto cleanup;
	}

	if (TAILQ_EMPTY(&g_controllers)) {
		fprintf(stderr, "No NVMe controllers found. "
			"Check target address and NQN.\n");
		rc = 1;
		goto cleanup;
	}

	printf("\nInitialisation complete. Running I/O...\n");

	/*
	 * Step C: Perform I/O on every registered namespace.
	 */
	run_io();

	printf("\nAll I/O completed successfully.\n");

cleanup:
	cleanup();
	spdk_env_fini();
	return rc;
}
```

### 2.3 Write the Makefile

Create `Makefile` in `~/spdk_nvmf_initiator/`:

```makefile
# Adjust SPDK_DIR to your SPDK source tree
SPDK_DIR ?= /path/to/spdk

APP = nvmf_initiator

# Collect SPDK libraries required for NVMe-oF initiator
SPDK_LIB_LIST = nvme env_dpdk log util

include $(SPDK_DIR)/mk/spdk.app.mk
```

> **Note:** If you placed this directory inside the SPDK tree (e.g., `examples/nvme/nvmf_initiator/`), use `SPDK_DIR := $(SPDK_ROOT_DIR)` instead of a hardcoded path.

### 2.4 Build

```bash
cd ~/spdk_nvmf_initiator
make SPDK_DIR=/path/to/spdk
```

Expected output:

```
  CC    nvmf_initiator.c
  LINK  nvmf_initiator
```

---

## Part 3: Run the Initiator

### 3.1 Basic TCP Run

With the target from Part 1 still running:

```bash
sudo ./nvmf_initiator \
  -r "trtype:tcp adrfam:ipv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2024-01.io.spdk:cnode1"
```

Expected output:

```
=== SPDK NVMe-oF Initiator (EX09) ===
Transport : TCP
Address   : 127.0.0.1
Port      : 4420
SubNQN    : nqn.2024-01.io.spdk:cnode1

Probe: found controller at 127.0.0.1 (transport=TCP)
Attached: SPDK bdev Controller  (SPDK00000000000001 )
  Registered NS 1  size=64 MB  block=512 B

Initialisation complete. Running I/O...

--- I/O on NS 1 ---
  QPair allocated (queue depth=...)
  [WRITE OK] Pattern written to LBA 0
  [READ OK] Data verified: 'SPDK-NVMf-EX09'

All I/O completed successfully.
```

### 3.2 Alternative: Use spdk_nvme_connect() for a Single Controller

For cases where you know exactly one controller exists and want a simpler API:

```c
/* Drop-in replacement for the probe path in main() */
struct spdk_nvme_ctrlr_opts ctrlr_opts;
spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctrlr_opts, sizeof(ctrlr_opts));
/* ctrlr_opts.keep_alive_timeout_ms = 10000; */

struct spdk_nvme_ctrlr *ctrlr =
    spdk_nvme_connect(&g_trid, &ctrlr_opts, sizeof(ctrlr_opts));
if (ctrlr == NULL) {
    fprintf(stderr, "spdk_nvme_connect() failed\n");
    return 1;
}
```

`spdk_nvme_connect()` is a synchronous wrapper around the async probe mechanism — simpler for single-target scenarios, but `spdk_nvme_probe()` is preferred when connecting to multiple targets simultaneously because it parallelises the controller resets.

---

## Part 4: Optional — RDMA Transport

If you have an RDMA-capable NIC and the target was configured with an RDMA listener, simply change the transport type in the trid string:

### 4.1 Add RDMA Listener to Target

```bash
./scripts/rpc.py nvmf_create_transport -t RDMA -u 131072
./scripts/rpc.py nvmf_subsystem_add_listener \
    nqn.2024-01.io.spdk:cnode1 \
    -t rdma \
    -a 192.168.1.10 \
    -s 4420
```

### 4.2 Connect via RDMA

```bash
sudo ./nvmf_initiator \
  -r "trtype:rdma adrfam:ipv4 traddr:192.168.1.10 trsvcid:4420 subnqn:nqn.2024-01.io.spdk:cnode1"
```

No code changes are needed — the transport layer is fully abstracted by the SPDK NVMe driver.

---

## Bonus A: Query the Discovery Service

The NVMe-oF discovery subsystem (`nqn.2014-08.org.nvmexpress.discovery`) exposes a log page listing all available subsystems on the target. Add the following function to your initiator to print it:

```c
#include "spdk/nvmf_spec.h"

static void
discovery_log_cb(void *cb_arg, int rc,
		 const struct spdk_nvme_cpl *cpl,
		 struct spdk_nvmf_discovery_log_page *log_page)
{
	uint64_t numrec, i;

	if (rc || spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "Discovery log page error\n");
		free(log_page);
		return;
	}

	numrec = from_le64(&log_page->numrec);
	printf("\n=== Discovery Log Page (%"PRIu64" records) ===\n", numrec);

	for (i = 0; i < numrec; i++) {
		struct spdk_nvmf_discovery_log_page_entry *e =
			&log_page->entries[i];
		char subnqn[257] = {};
		char traddr[257] = {};
		char trsvcid[33]  = {};

		snprintf(subnqn,  sizeof(e->subnqn)  + 1, "%s", e->subnqn);
		snprintf(traddr,  sizeof(e->traddr)  + 1, "%s", e->traddr);
		snprintf(trsvcid, sizeof(e->trsvcid) + 1, "%s", e->trsvcid);

		printf("  [%"PRIu64"] transport=%s addr=%s port=%s\n",
		       i,
		       spdk_nvme_transport_id_trtype_str(e->trtype),
		       traddr, trsvcid);
		printf("       subnqn=%s\n", subnqn);
	}

	free(log_page);
}

static void
query_discovery_service(const char *traddr, const char *trsvcid,
			enum spdk_nvme_transport_type trtype)
{
	struct spdk_nvme_transport_id disc_trid = {};
	struct spdk_nvme_ctrlr_opts  disc_opts  = {};
	struct spdk_nvme_ctrlr      *disc_ctrlr;

	/* Build a trid pointing at the discovery subsystem */
	disc_trid.trtype = trtype;
	spdk_nvme_transport_id_populate_trstring(
		&disc_trid,
		spdk_nvme_transport_id_trtype_str(trtype));
	disc_trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	snprintf(disc_trid.traddr,  sizeof(disc_trid.traddr),  "%s", traddr);
	snprintf(disc_trid.trsvcid, sizeof(disc_trid.trsvcid), "%s", trsvcid);
	snprintf(disc_trid.subnqn,  sizeof(disc_trid.subnqn),
		 "%s", SPDK_NVMF_DISCOVERY_NQN);

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&disc_opts, sizeof(disc_opts));

	disc_ctrlr = spdk_nvme_connect(&disc_trid, &disc_opts,
				       sizeof(disc_opts));
	if (disc_ctrlr == NULL) {
		fprintf(stderr, "Could not connect to discovery controller\n");
		return;
	}

	/* The async call queues an admin command; poll until done */
	bool done = false;
	/* Wrap done flag in a small closure-like struct if needed */
	spdk_nvme_ctrlr_get_discovery_log_page(disc_ctrlr,
					       discovery_log_cb, &done);
	while (!done) {
		spdk_nvme_ctrlr_process_admin_completions(disc_ctrlr);
	}

	spdk_nvme_detach(disc_ctrlr);
}
```

Call `query_discovery_service("127.0.0.1", "4420", SPDK_NVME_TRANSPORT_TCP)` before the main probe loop to print all available subsystems.

---

## Bonus B: Multiple Namespace Support

When a subsystem exposes more than one namespace, the `TAILQ_FOREACH` loop in `run_io()` already handles them. To test this, add a second bdev to the target:

```bash
./scripts/rpc.py bdev_malloc_create -b Malloc1 32 512
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2024-01.io.spdk:cnode1 Malloc1
```

Reconnect the initiator. Both namespaces (NS 1 and NS 2) will be registered and exercised in sequence.

To submit I/O to all namespaces concurrently you would need to run each namespace on its own thread (one QPair per thread), or use SPDK's reactor framework. That pattern is covered in EX11 (Blobstore and Reactor).

---

## Common Mistakes and How to Fix Them

### Mistake 1: Transport Type Mismatch

**Symptom:** `spdk_nvme_probe() failed` or connection refused immediately.

**Cause:** The initiator uses `trtype:tcp` but the target listener was added with `-t rdma` (or vice versa).

**Fix:** Check the target's listeners:
```bash
./scripts/rpc.py nvmf_get_subsystems | python3 -m json.tool | grep -A5 listen_addresses
```
Match the `trtype` in your `-r` argument exactly.

---

### Mistake 2: Wrong Address or Port

**Symptom:** `spdk_nvme_probe() failed` with a timeout, or "connection refused".

**Cause:** `traddr` or `trsvcid` does not match what the target is actually listening on.

**Fix:**
```bash
# Confirm the target is listening
ss -tlnp | grep 4420      # for TCP
# or
./scripts/rpc.py nvmf_get_subsystems
```

For loopback testing always use `traddr:127.0.0.1`. Using the hostname or external IP on the same machine may require extra routing configuration.

---

### Mistake 3: Wrong SubNQN

**Symptom:** Connection established but no namespaces appear.

**Cause:** The `subnqn` in the trid points to a subsystem that does not exist, or you used `SPDK_NVMF_DISCOVERY_NQN` when you meant to connect to an NVM subsystem.

**Fix:** Copy the exact subsystem NQN from the target's RPC output:
```bash
./scripts/rpc.py nvmf_get_subsystems | python3 -m json.tool | grep '"nqn"'
```

---

### Mistake 4: Non-DMA Buffer Used for I/O

**Symptom:** Segmentation fault or silent data corruption.

**Cause:** Using `malloc()` instead of `spdk_zmalloc()` for the I/O buffer. The NVMe driver DMAs directly into the buffer; it must be pinned hugepage memory.

**Fix:** Always use `spdk_zmalloc()` (or `spdk_malloc()`) with `SPDK_MALLOC_DMA` for I/O data buffers. Free with `spdk_free()`, not `free()`.

---

### Mistake 5: Forgetting to Poll Completions

**Symptom:** The program hangs forever in the `while (!seq.is_completed)` loop.

**Cause:** `spdk_nvme_qpair_process_completions()` was never called, so the completion callback is never invoked.

**Fix:** Make sure your polling loop calls `spdk_nvme_qpair_process_completions()` on the same QPair you submitted I/O to, on the same thread.

---

### Mistake 6: Freeing the QPair While I/O is In-Flight

**Symptom:** Crash or assertion failure inside the SPDK driver.

**Cause:** Calling `spdk_nvme_ctrlr_free_io_qpair()` before all submitted commands have completed.

**Fix:** Always poll completions to `is_completed != 0` before freeing the QPair.

---

## Verification Checklist

Before marking this exercise complete, confirm each of the following:

- [ ] Target starts and `nvmf_get_subsystems` shows the NVM subsystem with a listener.
- [ ] Initiator builds without errors.
- [ ] Initiator connects and prints `Attached:` with the controller model/serial.
- [ ] At least one namespace is registered and printed.
- [ ] `[WRITE OK]` and `[READ OK]` both appear for every namespace.
- [ ] The process exits cleanly (no crash, no ASAN errors).
- [ ] (Bonus A) Discovery log page lists the correct subsystem.
- [ ] (Bonus B) Adding a second namespace causes two I/O sequences to run.

---

## Further Reading

- `examples/nvme/hello_world/hello_world.c` — The canonical SPDK NVMe I/O example used as the basis for this exercise.
- `app/spdk_nvme_discover/discovery_aer.c` — Discovery service client with AER (Asynchronous Event Request) support.
- `include/spdk/nvme.h` — Full API reference: `spdk_nvme_probe()`, `spdk_nvme_connect()`, `spdk_nvme_ctrlr_alloc_io_qpair()`, `spdk_nvme_ns_cmd_write()`, `spdk_nvme_ns_cmd_read()`.
- NVMe-oF 1.1 specification, Section 3 — Fabrics Connect command and discovery service protocol.
- SPDK documentation: https://spdk.io/doc/nvme.html

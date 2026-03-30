# Module 28: Testing Strategies

**Stage 3 - Mastery | Estimated Time: 4-5 hours**

---

## Overview

Testing storage software is fundamentally different from testing typical application code. SPDK's design — lockless, polling-based, running entirely in user space with direct hardware access — creates unique testing challenges. You cannot simply spin up a test and call assert; you must carefully control threading context, mock hardware interactions, and simulate asynchronous I/O completions.

This module covers SPDK's complete testing stack: from the CUnit-based unit test framework and the `ut_mock` library, through integration and fuzz testing, to the automated CI pipeline built around `autotest_common.sh`. By the end, you will be able to write reliable tests for new SPDK modules, run the full test suite, and integrate new tests into the CI framework.

---

## 1. SPDK Testing Philosophy

### 1.1 Design Goals

SPDK testing follows several explicit principles rooted in the constraints of the software itself.

**Isolation without OS threads.** SPDK's threading model means that most code executes on a single SPDK thread per reactor core. Unit tests must simulate this environment without actually creating reactor threads. The `ut_multithread` helpers in `test/common/lib/` exist precisely for this reason.

**Source-level inclusion for isolation.** SPDK unit tests include the `.c` source file under test directly rather than linking against a library. This pattern — seen in every file under `test/unit/lib/` — means the test binary gets the exact translation unit it exercises. It also means internal static functions are visible to tests, which is intentional: SPDK unit tests test implementation, not just public API.

**Mock at link time, not at runtime.** SPDK uses the GNU linker's `--wrap` mechanism and a set of macro-driven stub patterns to replace functions at link time. This avoids dynamic dispatch overhead and keeps the approach compatible with SPDK's no-dynamic-allocation-in-fast-path philosophy.

**Valgrind clean by default.** The `unittest.sh` driver runs every unit test binary under Valgrind by default. Any memory error, leak, or invalid access fails the test. This is not optional; new tests must pass Valgrind checks before they are accepted.

**Nightly vs. per-commit scope.** Fast unit tests run on every commit. Longer integration tests, fuzz campaigns, and performance benchmarks run nightly. The `RUN_NIGHTLY` environment variable gates this split throughout `autotest_common.sh`.

### 1.2 Test Categories

| Category | Location | What it covers | Run frequency |
|---|---|---|---|
| Unit tests | `test/unit/` | Individual translation units, mocked dependencies | Every commit |
| Integration tests | `test/bdev/`, `test/nvmf/`, `test/iscsi_tgt/`, etc. | Full subsystem interaction with real or emulated hardware | Nightly or per-PR |
| Fuzz tests | `test/fuzz/` | Protocol parsers, input validation, message handling | Nightly campaigns |
| Performance tests | `examples/bdev/bdevperf/`, `test/nvme/` | Throughput, latency, CPU efficiency | Nightly/manual |
| MT unit tests | `test/unit/lib/bdev/mt/` | Thread-safety of bdev layer | Every commit |

---

## 2. Unit Testing Infrastructure

### 2.1 CUnit Integration

SPDK wraps the [CUnit](http://cunit.sourceforge.net/) framework with its own thin layer defined in `include/spdk_internal/cunit.h` and implemented in `lib/ut/ut.c`.

The wrapper provides:
- Command-line argument parsing (`-t <test>`, `-s <suite>`, `-l` for listing)
- A single entry point `spdk_ut_run_tests()` that all test binaries call from `main()`
- Support for custom options and per-suite setup/teardown hooks
- Verbose output and abort-on-failure mode by default

Every unit test binary follows the same skeleton:

```c
#include "spdk_internal/cunit.h"

/* Include the source under test directly */
#include "bdev/bdev.c"

/* --- Test state and helpers --- */

static int
suite_setup(void)
{
    /* Initialize resources shared across tests in this suite */
    return 0;
}

static int
suite_teardown(void)
{
    /* Release shared resources */
    return 0;
}

/* --- Individual test functions --- */

static void
test_bdev_open_close(void)
{
    struct spdk_bdev bdev = {};
    int rc;

    /* Setup */
    bdev.blocklen = 512;
    bdev.blockcnt = 1024;

    /* Exercise */
    rc = some_internal_function(&bdev);

    /* Assert */
    CU_ASSERT(rc == 0);
    CU_ASSERT(bdev.internal.claim_type == SPDK_BDEV_CLAIM_NONE);
}

static void
test_bdev_error_path(void)
{
    int rc;

    /* Force a dependency to return an error */
    MOCK_SET(spdk_zmalloc, NULL);

    rc = function_that_allocates();

    CU_ASSERT(rc == -ENOMEM);

    MOCK_CLEAR(spdk_zmalloc);
}

/* --- Registration and main --- */

int
main(int argc, char **argv)
{
    CU_pSuite suite;

    CU_initialize_registry();
    suite = CU_add_suite("bdev", suite_setup, suite_teardown);

    CU_ADD_TEST(suite, test_bdev_open_close);
    CU_ADD_TEST(suite, test_bdev_error_path);

    return spdk_ut_run_tests(argc, argv, NULL);
}
```

### 2.2 Assertion Macros

CUnit provides the standard assertion set. SPDK adds `SPDK_CU_ASSERT_FATAL` for assertions where failure means the test cannot continue safely:

```c
/* Standard CUnit assertions */
CU_ASSERT(expr)                  /* expr is true */
CU_ASSERT_EQUAL(actual, expected)
CU_ASSERT_NOT_EQUAL(actual, expected)
CU_ASSERT_PTR_NULL(ptr)
CU_ASSERT_PTR_NOT_NULL(ptr)
CU_ASSERT_STRING_EQUAL(actual, expected)
CU_ASSERT_NSTRING_EQUAL(actual, expected, count)

/* SPDK addition: abort the test if expr is false */
SPDK_CU_ASSERT_FATAL(expr)       /* terminates test immediately on failure */
```

Use `SPDK_CU_ASSERT_FATAL` when subsequent code would dereference a pointer that was just checked:

```c
struct spdk_bdev *bdev = spdk_bdev_get_by_name("test_disk");
SPDK_CU_ASSERT_FATAL(bdev != NULL);  /* not CU_ASSERT_PTR_NOT_NULL — that would continue */
CU_ASSERT_EQUAL(bdev->blocklen, 512);
```

### 2.3 Running Unit Tests

The top-level entry point is `test/unit/unittest.sh`. It iterates through all registered test binaries and runs each under Valgrind:

```bash
# Run all unit tests
cd /path/to/spdk
./test/unit/unittest.sh

# Run with custom Valgrind options
valgrind="valgrind --leak-check=full --error-exitcode=1" ./test/unit/unittest.sh

# Run a single test binary directly
./test/unit/lib/bdev/bdev.c/bdev_ut

# Run a single test case within a binary
./test/unit/lib/bdev/bdev.c/bdev_ut -t test_bdev_open_close

# Run all tests in a specific suite
./test/unit/lib/bdev/bdev.c/bdev_ut -s bdev

# List all registered tests
./test/unit/lib/bdev/bdev.c/bdev_ut -l
```

---

## 3. Mock Objects and the `ut_mock` Library

### 3.1 Architecture

SPDK's mock system lives in two places:
- `include/spdk_internal/mock.h` — macro definitions for all mock patterns
- `lib/ut_mock/mock.c` — implementations for syscall wrappers (`calloc`, `pthread_mutex_init`, `recvmsg`, `sendmsg`, `writev`, `unlink`)

The system uses three distinct patterns depending on what is being mocked.

### 3.2 Pattern 1: DEFINE_STUB — Simple Return Value Substitution

`DEFINE_STUB` replaces an entire SPDK function with a stub that returns a controllable value. Use this when the test does not need to inspect call arguments and only needs to control what a dependency returns.

```c
/* In test file: replace spdk_env_get_core_count() with a stub returning 4 */
DEFINE_STUB(spdk_env_get_core_count, uint32_t, (void), 4);

/* For void functions, use DEFINE_STUB_V */
DEFINE_STUB_V(spdk_bdev_io_complete,
              (struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status));
```

At test time, change the return value using `MOCK_SET`:

```c
static void
test_allocation_failure(void)
{
    /* Override the stub to return 0 cores, simulating a misconfigured environment */
    MOCK_SET(spdk_env_get_core_count, 0);

    int rc = module_that_checks_cores();
    CU_ASSERT(rc == -EINVAL);

    /* Restore */
    MOCK_CLEAR(spdk_env_get_core_count);
}
```

### 3.3 Pattern 2: DEFINE_RETURN_MOCK — Stateful Return Values

`DEFINE_RETURN_MOCK` creates a mock that can return different values on successive calls, using an internal FIFO queue. Use this when the code under test calls the same dependency multiple times with expected distinct results.

```c
/* Declaration at file scope */
DEFINE_RETURN_MOCK(spdk_nvme_ctrlr_alloc_io_qpair, struct spdk_nvme_qpair *);

static void
test_qpair_allocation_partial_failure(void)
{
    /* First call succeeds, second fails */
    MOCK_ENQUEUE(spdk_nvme_ctrlr_alloc_io_qpair, &g_qpair1);
    MOCK_ENQUEUE(spdk_nvme_ctrlr_alloc_io_qpair, NULL);

    int rc = create_two_qpairs(&g_ctrlr);

    CU_ASSERT(rc == -ENOMEM);
    CU_ASSERT(g_qpair1_released == true);  /* verify cleanup happened */

    MOCK_CLEAR(spdk_nvme_ctrlr_alloc_io_qpair);
}
```

When the queue is exhausted, the mock falls back to the value set by `MOCK_SET`. This lets tests set a default and override it for specific call sequences.

### 3.4 Pattern 3: DEFINE_WRAPPER — Syscall Interception

`DEFINE_WRAPPER` replaces a libc function using the GNU linker `--wrap` flag. The production code calls `calloc()`; the linker redirects it to `__wrap_calloc()` which, when mocked, returns the configured value instead of calling the real `__real_calloc()`.

```c
/* Already declared in mock.h; just use it: */
static void
test_calloc_failure(void)
{
    MOCK_SET(calloc, NULL);

    struct spdk_bdev *bdev = bdev_create_with_internal_alloc();
    CU_ASSERT_PTR_NULL(bdev);

    MOCK_CLEAR(calloc);
}
```

The `unlink` wrapper is handled differently — it uses a global path variable rather than the generic mock queue, because path matching logic is needed:

```c
/* Set g_unlink_path so __wrap_unlink accepts and records the call */
g_unlink_path = "/tmp/test_file";
g_unlink_callback = my_callback;
/* Now code that calls unlink("/tmp/test_file") will invoke my_callback */
```

### 3.5 Custom Function Stubs in Test Files

For complex dependencies, the test file simply defines the function directly. Because the source under test is `#include`d (not linked), there is no symbol conflict with the real implementation — the test file's definition wins:

```c
/* bdev_ut.c: override spdk_scsi_nvme_translate entirely */
void
spdk_scsi_nvme_translate(const struct spdk_bdev_io *bdev_io,
                         int *sc, int *sk, int *asc, int *ascq)
{
    /* Intentionally empty: tests do not need SCSI translation */
}
```

### 3.6 Memory Domain Mocking Example

The `bdev_ut.c` file demonstrates the complete pattern for mocking async operations that take completion callbacks:

```c
DEFINE_RETURN_MOCK(spdk_memory_domain_pull_data, int);
int
spdk_memory_domain_pull_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
                             struct iovec *src_iov, uint32_t src_iov_cnt,
                             struct iovec *dst_iov, uint32_t dst_iov_cnt,
                             spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
    g_memory_domain_pull_data_called = true;
    /* Check if test wants to override the return value */
    HANDLE_RETURN_MOCK(spdk_memory_domain_pull_data);
    /* Default: call completion inline (synchronous simulation) */
    cpl_cb(cpl_cb_arg, 0);
    return 0;
}
```

The key insight: real memory domain operations are asynchronous. The mock calls the completion callback immediately, making asynchronous code testable without an event loop. When a test needs to simulate a delayed or failed completion, it uses `MOCK_SET` to change the return value and defers calling the callback manually.

---

## 4. Writing Unit Tests for SPDK Modules

### 4.1 Thread Context Management

Most SPDK code asserts that it is called from an SPDK thread. Tests must establish this context. The `ut_multithread` helpers in `test/common/lib/ut_multithread.c` provide this:

```c
#include "common/lib/ut_multithread.c"

static void
test_bdev_io_submit(void)
{
    struct spdk_thread *thread;

    /* Create a fake SPDK thread for this test */
    thread = spdk_thread_create("test_thread", NULL);
    SPDK_CU_ASSERT_FATAL(thread != NULL);
    spdk_set_thread(thread);

    /* Now all SPDK thread assertions will pass */
    struct spdk_io_channel *ch = spdk_get_io_channel(&g_bdev);
    CU_ASSERT_PTR_NOT_NULL(ch);

    /* Run pending messages/pollers */
    poll_thread(0);

    spdk_put_io_channel(ch);
    poll_thread(0);

    spdk_thread_exit(thread);
    poll_thread(0);
    spdk_thread_destroy(thread);
}
```

The `poll_thread(n)` function drains the message queue of thread `n` (threads are indexed in creation order). This is how you advance asynchronous state machines in tests without a real reactor.

### 4.2 I/O Channel Setup

Subsystems that use I/O channels require device registration in the test setup:

```c
static int g_accel_io_device;

static int
ut_accel_ch_create_cb(void *io_device, void *ctx)
{
    return 0;
}

static void
ut_accel_ch_destroy_cb(void *io_device, void *ctx)
{
}

static int
suite_setup(void)
{
    spdk_io_device_register(&g_accel_io_device,
                            ut_accel_ch_create_cb,
                            ut_accel_ch_destroy_cb,
                            0, NULL);
    return 0;
}

static int
suite_teardown(void)
{
    spdk_io_device_unregister(&g_accel_io_device, NULL);
    return 0;
}
```

### 4.3 Testing Asynchronous Completions

SPDK I/O completions are asynchronous. Tests intercept them using a completion callback and a flag:

```c
static bool g_io_done;
static enum spdk_bdev_io_status g_io_status;

static void
io_done_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    g_io_done = true;
    g_io_status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
    spdk_bdev_free_io(bdev_io);
}

static void
test_bdev_read(void)
{
    struct iovec iov = {.iov_base = g_buf, .iov_len = 4096};

    g_io_done = false;

    spdk_bdev_readv_blocks(g_desc, g_ch, &iov, 1, 0, 8, io_done_cb, NULL);

    /* Poll until the completion fires */
    poll_threads();

    CU_ASSERT(g_io_done == true);
    CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
}
```

### 4.4 Expected I/O Pattern

The bdev unit tests use an "expected I/O" queue pattern: before submitting an I/O, the test records what it expects the driver layer to receive. The stub I/O device verifies each submitted I/O against the expected queue:

```c
struct ut_expected_io {
    uint8_t         type;      /* SPDK_BDEV_IO_TYPE_READ, etc. */
    uint64_t        offset;    /* expected LBA offset */
    uint64_t        length;    /* expected block count */
    int             iovcnt;
    struct iovec    iov[SPDK_BDEV_IO_NUM_CHILD_IOV];
    TAILQ_ENTRY(ut_expected_io) link;
};

static void
test_split_io(void)
{
    /* Bdev has max_segment_size = 4096, so a 2-block I/O at offset 0 splits */
    ut_expected_io *first  = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 0, 1, 1);
    ut_expected_io *second = ut_alloc_expected_io(SPDK_BDEV_IO_TYPE_READ, 1, 1, 1);

    TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, first,  link);
    TAILQ_INSERT_TAIL(&g_bdev_ut_channel->expected_io, second, link);

    g_io_done = false;
    spdk_bdev_read_blocks(g_desc, g_ch, g_buf, 0, 2, io_done_cb, NULL);
    poll_threads();

    CU_ASSERT(g_io_done == true);
    CU_ASSERT(TAILQ_EMPTY(&g_bdev_ut_channel->expected_io));
}
```

### 4.5 Test File Structure and Naming

Follow these conventions to match the existing codebase:

```
test/unit/lib/<subsystem>/<source_file>.c/<source_file>_ut.c
```

For example, a test for `lib/bdev/bdev.c` lives at `test/unit/lib/bdev/bdev.c/bdev_ut.c`. This nesting makes the build system's file generation straightforward: the Makefile in each directory produces an executable named `<source_file>_ut`.

Each test directory contains a `Makefile` that follows the pattern:

```makefile
# test/unit/lib/bdev/bdev.c/Makefile
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

TEST_FILE = bdev_ut.c
TEST_NAME = bdev_ut

SPDK_LIB_LIST = $(BDEV_MODULES_LIST) bdev_ut_mock ...

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
```

The `spdk.unittest.mk` fragment handles `--wrap` flags for all declared wrappers automatically.

---

## 5. Multithreaded Unit Tests

Some bdev behavior requires concurrent threads. The `test/unit/lib/bdev/mt/` directory contains tests that exercise the bdev layer from multiple SPDK threads simultaneously.

### 5.1 The MT Test Harness

```c
#include "common/lib/ut_multithread.c"

/* Number of threads to simulate */
#define NUM_THREADS 3

static void
test_bdev_concurrent_open(void)
{
    uint32_t i;

    /* Create NUM_THREADS fake threads */
    allocate_threads(NUM_THREADS);
    set_thread(0);

    /* Register the bdev on thread 0 */
    register_bdev(&g_bdev, "test");

    /* Open from threads 1 and 2 concurrently */
    set_thread(1);
    spdk_bdev_open_ext("test", true, event_cb, NULL, &g_desc[1]);

    set_thread(2);
    spdk_bdev_open_ext("test", true, event_cb, NULL, &g_desc[2]);

    /* Poll all threads to let async work complete */
    poll_threads();

    CU_ASSERT(g_desc[1] != NULL);
    CU_ASSERT(g_desc[2] != NULL);

    /* Clean up */
    set_thread(1);
    spdk_bdev_close(g_desc[1]);
    set_thread(2);
    spdk_bdev_close(g_desc[2]);

    poll_threads();
    free_threads();
}
```

The `set_thread(n)` function makes subsequent SPDK API calls execute as if they were invoked on thread `n`. This lets a single OS thread simulate concurrent SPDK thread interactions without real parallelism, making races deterministic and reproducible.

---

## 6. Integration Testing

### 6.1 Structure

Integration tests run full SPDK applications against real or emulated hardware. They live in subsystem-specific directories:

```
test/bdev/           - bdev layer integration (bdevperf, JSON config tests)
test/nvmf/           - NVMe-oF target/initiator tests
test/iscsi_tgt/      - iSCSI target tests
test/nvme/           - NVMe driver direct tests
test/vhost/          - vhost-scsi/blk tests
test/lvol/           - Logical volume tests
test/ftl/            - Flash Translation Layer tests
```

Each subsystem directory contains shell scripts that:
1. Configure and launch an SPDK application (or target+initiator pair)
2. Drive workloads via RPC (`rpc.py`) or an initiator application
3. Verify outcomes and clean up

### 6.2 RPC-Driven Tests

Most integration tests use `rpc.py` to configure a running SPDK process. A typical bdev integration test looks like:

```bash
#!/usr/bin/env bash
# test/bdev/bdevperf.sh

function bdevperf_rpc_test() {
    # Start bdevperf application
    $SPDK_BIN_DIR/bdevperf --json $testdir/bdevperf.json &
    bdevperf_pid=$!

    waitforlisten $bdevperf_pid

    # Configure via RPC
    $rpc_py bdev_null_create Null0 100 512
    $rpc_py bdev_null_create Null1 100 4096

    # Start I/O
    $rpc_py bdevperf_run_tests

    # Wait for completion
    $rpc_py bdevperf_stop_tests

    # Verify stats
    local iops=$($rpc_py bdev_get_iostat -b Null0 | jq '.bdevs[0].num_read_ops')
    [ "$iops" -gt 0 ] || { echo "No read ops recorded"; exit 1; }

    killprocess $bdevperf_pid
}
```

### 6.3 The `autotest_common.sh` Framework

`test/common/autotest_common.sh` is the shared infrastructure for all integration tests. It provides:

- Standard environment variable gates (`SPDK_TEST_NVME`, `SPDK_TEST_ISCSI`, etc.)
- `waitforlisten` — waits for an SPDK application's RPC socket to appear
- `killprocess` — sends SIGTERM and waits for clean exit
- `run_test` — wraps a test function with timing, logging, and error capture
- Output directory management
- Build configuration detection

Environment variables controlling test scope:

```bash
SPDK_TEST_UNITTEST=1       # Enable unit tests
SPDK_TEST_NVME=1           # Enable NVMe integration tests (requires hardware)
SPDK_TEST_NVMF=1           # Enable NVMe-oF tests
SPDK_TEST_ISCSI=1          # Enable iSCSI tests
SPDK_TEST_VHOST=1          # Enable vhost tests
SPDK_TEST_BLOBFS=1         # Enable BlobFS tests
SPDK_TEST_FTL=1            # Enable FTL tests
RUN_NIGHTLY=1              # Enable nightly-only tests
SPDK_RUN_VALGRIND=1        # Run integration tests under Valgrind
```

### 6.4 Writing an Integration Test

Integration tests follow a consistent pattern:

```bash
#!/usr/bin/env bash
# test/bdev/my_new_test.sh

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source "$rootdir/test/common/autotest_common.sh"

function test_my_feature() {
    local app_pid

    # Launch SPDK application
    "$SPDK_BIN_DIR/spdk_tgt" -m 0x1 &
    app_pid=$!

    trap 'killprocess $app_pid; exit 1' ERR

    # Wait for RPC socket
    waitforlisten $app_pid

    # Configure
    $rpc_py bdev_malloc_create -b Malloc0 128 512

    # Exercise the feature
    $rpc_py bdev_get_bdevs | jq -e '.[] | select(.name == "Malloc0")'

    # Verify disk stats
    local stats
    stats=$($rpc_py bdev_get_iostat -b Malloc0)
    echo "$stats"

    # Cleanup
    $rpc_py bdev_malloc_delete Malloc0
    killprocess $app_pid
    trap - ERR
}

run_test "my_feature" test_my_feature
```

---

## 7. Fuzz Testing

### 7.1 Overview

SPDK uses LLVM libFuzzer for fuzz testing of protocol parsers and message handlers. The fuzz infrastructure lives in `test/fuzz/` and targets the most externally-exposed attack surfaces:

- NVMe-oF protocol message handling
- iSCSI protocol parsing
- vhost message processing

### 7.2 Build Configuration

Fuzz targets require a special build:

```bash
./configure --enable-asan --enable-ubsan --enable-debug \
            CC=clang CXX=clang++

make -j$(nproc)
```

AddressSanitizer (`--enable-asan`) and UndefinedBehaviorSanitizer (`--enable-ubsan`) are required for fuzz testing. They instrument the binary to catch memory errors and undefined behavior that libFuzzer exercises would otherwise miss.

### 7.3 Running the Autofuzz Campaign

`test/fuzz/autofuzz.sh` orchestrates multi-module fuzz campaigns:

```bash
# Fuzz NVMe-oF TCP transport for 1200 seconds
TEST_TRANSPORT=tcp ./test/fuzz/autofuzz.sh --module=nvmf --timeout=1200

# Fuzz iSCSI parser
TEST_TRANSPORT=tcp ./test/fuzz/autofuzz.sh --module=iscsi --timeout=600

# Fuzz vhost
./test/fuzz/autofuzz.sh --module=vhost --timeout=600
```

The script builds SPDK with ASan+UBSan, generates an initial corpus from known-good messages, then runs libFuzzer with coverage-guided mutation.

### 7.4 Writing a Fuzz Target

A libFuzzer target for an SPDK parser has this structure:

```c
/* test/fuzz/llvm/fuzz_nvmf_tcp.c */
#include "spdk/stdinc.h"
#include "spdk/nvmf.h"

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
    /* One-time setup: initialize SPDK subsystems needed by the parser */
    return 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < sizeof(struct spdk_nvme_tcp_common_pdu_hdr)) {
        return 0;
    }

    /* Feed the raw bytes to the parser under test */
    parse_nvmf_tcp_pdu(data, size);

    return 0;
}
```

libFuzzer calls `LLVMFuzzerTestOneInput` repeatedly with mutated inputs. Any crash (detected by ASan) or undefined behavior (detected by UBSan) is a finding.

### 7.5 Corpus Management

Maintain a seed corpus of valid protocol messages:

```
test/fuzz/llvm/corpus/nvmf_tcp/
    connect_pdu.bin
    read_cmd_pdu.bin
    write_cmd_pdu.bin
    capsule_cmd_pdu.bin
```

libFuzzer uses these as starting points, mutating them to explore edge cases. Starting from valid inputs is significantly more effective than starting from random bytes for structured protocol parsers.

---

## 8. Performance and Benchmark Testing

### 8.1 bdevperf

`examples/bdev/bdevperf/bdevperf.c` is the primary bdev performance benchmark. It exercises any bdev through the standard bdev API, making it suitable for comparing performance across different bdev types (NVMe, malloc, null, raid, etc.).

**Key options:**

```bash
# Run a 60-second sequential read benchmark against an NVMe bdev
bdevperf -q 128 -o 4096 -w read -t 60 -b NVMe0n1

# Mixed read/write workload
bdevperf -q 64 -o 4096 -w randreadwrite -M 70 -t 60 -b NVMe0n1

# Verify data integrity while benchmarking
bdevperf -q 32 -o 4096 -w verify -t 60 -b NVMe0n1

# Multiple bdevs, JSON config
bdevperf --json bdevperf_config.json -q 128 -o 4096 -w randread -t 60
```

**Options reference:**

| Option | Meaning |
|---|---|
| `-q <depth>` | I/O queue depth per bdev per thread |
| `-o <size>` | I/O size in bytes |
| `-w <workload>` | `read`, `write`, `randread`, `randwrite`, `randreadwrite`, `verify`, `reset`, `unmap` |
| `-M <pct>` | Read percentage for mixed workloads |
| `-t <sec>` | Test duration |
| `-b <name>` | Specific bdev to test (omit for all) |
| `-m <mask>` | CPU core mask |

**Reading results:**

```
Device : NVMe0n1
Core Mask : 0x1
Current thread count : 1

Device : NVMe0n1, Core : 0
 Total I/Os      : 18547621
 Total Bytes     : 76006318080
 Total Errors    : 0
 I/Os per second : 308889.50
 MiB per second  : 1206.60
 Average Latency : 414.32 usec
 Min Latency     : 84.00 usec
 Max Latency     : 2124.00 usec
```

### 8.2 nvme_perf (nvmeperf)

The NVMe performance tool (`spdk_nvme_perf` or `nvmeperf`) bypasses the bdev layer and drives NVMe devices directly, allowing measurement of raw NVMe driver performance:

```bash
# Basic sequential read benchmark
spdk_nvme_perf -q 128 -s 4096 -w read -t 60 -r 'trtype:PCIe traddr:0000:01:00.0'

# NVMe-oF TCP target benchmark
spdk_nvme_perf -q 64 -s 4096 -w randread -t 60 \
    -r 'trtype:TCP adrfam:IPv4 traddr:192.168.1.1 trsvcid:4420'

# Multiple namespaces
spdk_nvme_perf -q 128 -s 4096 -w randread -t 60 \
    -r 'trtype:PCIe traddr:0000:01:00.0' \
    -r 'trtype:PCIe traddr:0000:02:00.0'
```

### 8.3 Scripted Performance Regression Tests

The performance test framework in `scripts/perf/` provides tools for running structured benchmarks and comparing results across builds:

```bash
# Run the full NVMe perf suite
scripts/perf/run_perf.sh

# Run bdev perf with multiple configurations
scripts/perf/bdev/run_bdev_perf.sh
```

Performance regression testing strategy:
1. Establish a baseline with a known-good commit
2. Record results in a structured JSON format
3. After changes, run the same workloads
4. Compare: flag regressions beyond a configurable threshold (typically 2-3%)

---

## 9. CI/CD Integration and the Autotest Framework

### 9.1 `autotest_common.sh` Environment Variables

The test framework is entirely environment-variable driven, enabling flexible CI configurations:

```bash
# Minimum set for a unit-test-only CI run
export SPDK_TEST_UNITTEST=1
export output_dir=/tmp/spdk_test_output

./test/unit/unittest.sh

# Full integration test run (requires appropriate hardware)
export SPDK_TEST_NVME=1
export SPDK_TEST_NVMF=1
export SPDK_TEST_ISCSI=1
export SPDK_RUN_VALGRIND=0       # too slow for CI, use in nightly
export RUN_NIGHTLY=0

source test/common/autotest_common.sh
run_test "nvme" test/nvme/nvme.sh
run_test "nvmf" test/nvmf/nvmf.sh
```

### 9.2 `run_test` Wrapper

The `run_test` function in `autotest_common.sh` is the standard way to invoke each test:

```bash
# Signature
run_test <test_name> <test_function_or_script> [args...]

# It provides:
# - Timing information
# - Output capture to $output_dir/<test_name>.log
# - Automatic failure detection and reporting
# - Clean error messages on failure

run_test "bdev_raid" test/bdev/bdev.sh raid
run_test "nvmf_tcp"  test/nvmf/nvmf.sh "tcp"
```

### 9.3 CI Pipeline Structure

A typical CI pipeline for an SPDK change:

```
Stage 1: Build verification (fast, no hardware)
    ├── make CHECK_FORMAT=1      # Code style check
    ├── make -j$(nproc)          # Full build
    └── SPDK_TEST_UNITTEST=1 ./test/unit/unittest.sh

Stage 2: Functional tests (virtual hardware where possible)
    ├── test/bdev/bdev.sh null   # Null bdev (no hardware)
    ├── test/bdev/bdev.sh malloc # Malloc bdev
    └── test/nvmf/nvmf.sh tcp   # NVMe-oF TCP (can use loopback)

Stage 3: Hardware tests (nightly, real hardware)
    ├── SPDK_TEST_NVME=1 test/nvme/nvme.sh
    ├── SPDK_TEST_NVMF=1 test/nvmf/nvmf.sh rdma
    └── test/vhost/vhost.sh

Stage 4: Long-running validation (weekly)
    ├── Fuzz campaigns
    ├── Performance regression
    └── SPDK_RUN_VALGRIND=1 full integration tests
```

### 9.4 Adding a New Test to the CI Pipeline

1. Write the test script in the appropriate subsystem directory
2. Add it to `unittest.sh` (for unit tests) or the relevant integration test orchestration script
3. Gate it on the appropriate `SPDK_TEST_*` environment variable
4. Add the variable to `autotest_common.sh` with a default of `0`
5. Update CI configuration to set the variable in the appropriate stage

---

## 10. Test Coverage and Quality Metrics

### 10.1 Code Coverage with gcov

Enable coverage instrumentation:

```bash
./configure --enable-coverage --enable-debug
make -j$(nproc)

# Run tests
./test/unit/unittest.sh

# Generate coverage report
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/dpdk/*' '*/test/*' \
     --output-file coverage_filtered.info
genhtml coverage_filtered.info --output-directory coverage_html

# View report
open coverage_html/index.html
```

### 10.2 Coverage Targets

SPDK does not enforce a single coverage percentage project-wide because heavily hardware-dependent paths cannot be covered in simulation. Apply coverage analysis at the module level:

| Module type | Reasonable coverage target |
|---|---|
| Pure algorithmic (JSON parser, util) | 85-95% |
| Bdev layer logic | 70-85% |
| Driver hardware paths | 50-70% (limited by simulation fidelity) |
| Protocol state machines | 75-90% |

### 10.3 Sanitizers

SPDK's configure script supports multiple sanitizers that catch bugs at runtime:

```bash
# AddressSanitizer: catches use-after-free, buffer overflows, use-after-return
./configure --enable-asan

# UndefinedBehaviorSanitizer: catches signed overflow, null pointer dereference, etc.
./configure --enable-ubsan

# ThreadSanitizer: catches data races (note: high false-positive rate with DPDK)
./configure --enable-tsan

# Combine ASan + UBSan for fuzz testing
./configure --enable-asan --enable-ubsan
```

Running unit tests with sanitizers enabled catches a different class of bugs than Valgrind. Both are used in SPDK CI:
- **Valgrind** catches memory leaks and uninitialized reads with full accuracy
- **ASan** catches the same classes faster (2-3x overhead vs Valgrind's 10-50x), suitable for integration tests
- **UBSan** catches C undefined behavior that Valgrind cannot see

---

## 11. Best Practices

### 11.1 Test Independence

Every test function must leave global state clean. Use `MOCK_CLEAR()` after every `MOCK_SET()`, and restore any modified globals in teardown:

```c
static int
suite_teardown(void)
{
    /* Always clear all mocks, even if a test failed */
    MOCK_CLEAR(spdk_zmalloc);
    MOCK_CLEAR(spdk_env_get_core_count);
    g_unlink_path = NULL;
    g_unlink_callback = NULL;
    return 0;
}
```

### 11.2 Test One Thing Per Function

Each test function should exercise one behavior. Prefer many small focused tests over fewer large ones. The `-t <test_name>` option only works if individual tests are granular.

```c
/* Good: each function tests one scenario */
static void test_bdev_open_success(void) { ... }
static void test_bdev_open_already_claimed(void) { ... }
static void test_bdev_open_invalid_name(void) { ... }
static void test_bdev_open_alloc_failure(void) { ... }

/* Avoid: one large function testing everything */
static void test_bdev_open(void) { /* 400 lines covering 8 scenarios */ }
```

### 11.3 Avoid Real Sleep

Never use `sleep()` or `usleep()` in unit tests. All asynchronous behavior must be driven by `poll_thread()` or `poll_threads()`. Tests that sleep are fragile on slow CI machines and mask real timing bugs.

### 11.4 Test Error Paths Explicitly

SPDK error paths — allocation failures, hardware errors, invalid inputs — are critical but often undertested. For each allocation in the code path under test, add a test that mocks the allocator to return NULL and verifies clean error handling:

```c
static void
test_nvmf_subsystem_create_oom(void)
{
    struct spdk_nvmf_subsystem *subsys;

    MOCK_SET(calloc, NULL);
    subsys = spdk_nvmf_subsystem_create(g_tgt, "nqn.test", SPDK_NVMF_SUBTYPE_NVME, 0);
    CU_ASSERT_PTR_NULL(subsys);
    MOCK_CLEAR(calloc);
}
```

### 11.5 Use SPDK_CU_ASSERT_FATAL at Pointer Checks

Always use `SPDK_CU_ASSERT_FATAL` rather than `CU_ASSERT_PTR_NOT_NULL` when the code following the check will dereference the pointer. A failed `CU_ASSERT_PTR_NOT_NULL` does not stop the test function; the subsequent dereference will segfault and produce a misleading error.

### 11.6 Document Mock State in Tests

Complex mock interactions should be commented:

```c
static void
test_qpair_connect_timeout(void)
{
    /*
     * Simulate: first qpair allocation succeeds, then the transport
     * connect attempt returns -ETIMEDOUT, triggering the cleanup path
     * which calls spdk_nvme_ctrlr_free_io_qpair.
     */
    MOCK_ENQUEUE(spdk_nvme_ctrlr_alloc_io_qpair, &g_qpair);
    MOCK_SET(nvme_transport_qpair_connect, -ETIMEDOUT);

    int rc = nvme_ctrlr_connect_qpair(&g_ctrlr, &g_qpair);

    CU_ASSERT(rc == -ETIMEDOUT);
    CU_ASSERT(g_qpair_freed == true);

    MOCK_CLEAR(spdk_nvme_ctrlr_alloc_io_qpair);
    MOCK_CLEAR(nvme_transport_qpair_connect);
}
```

### 11.7 Register New Tests in unittest.sh

Every new unit test binary must be added to `test/unit/unittest.sh`. The convention is to add it to the appropriate `unittest_<subsystem>()` function:

```bash
function unittest_nvmf() {
    $valgrind $testdir/lib/nvmf/nvmf.c/nvmf_ut
    $valgrind $testdir/lib/nvmf/fc.c/fc_ut
    $valgrind $testdir/lib/nvmf/tcp.c/tcp_ut
    # Your new test:
    $valgrind $testdir/lib/nvmf/my_new_feature.c/my_new_feature_ut
}
```

---

## 12. Complete Example: Writing a Unit Test from Scratch

This section walks through writing a unit test for a hypothetical new bdev module `bdev_fancy.c`.

### Step 1: Create the test directory structure

```
test/unit/lib/bdev/bdev_fancy.c/
    Makefile
    bdev_fancy_ut.c
```

### Step 2: Write the Makefile

```makefile
# test/unit/lib/bdev/bdev_fancy.c/Makefile
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

TEST_FILE = bdev_fancy_ut.c
TEST_NAME = bdev_fancy_ut

SPDK_LIB_LIST = thread bdev ut_mock

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
```

### Step 3: Write the test file

```c
/* test/unit/lib/bdev/bdev_fancy.c/bdev_fancy_ut.c */
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Example Corp. All rights reserved.
 */

#include "spdk_internal/cunit.h"
#include "common/lib/ut_multithread.c"

/* Include the source under test directly */
#include "bdev/bdev_fancy.c"

/* ================================================================
 * Stubs for functions bdev_fancy.c calls that we don't want to
 * exercise in this test.
 * ================================================================ */

DEFINE_STUB_V(spdk_bdev_module_finish_done, (void));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *bdev), 0);
DEFINE_STUB_V(spdk_bdev_unregister, (struct spdk_bdev *bdev,
                                      spdk_bdev_unregister_cb cb_fn, void *cb_arg));

/* ================================================================
 * Test state
 * ================================================================ */

static struct spdk_bdev_io *g_completed_io;
static enum spdk_bdev_io_status g_io_status;

static void
io_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    g_completed_io = bdev_io;
    g_io_status = success ? SPDK_BDEV_IO_STATUS_SUCCESS
                          : SPDK_BDEV_IO_STATUS_FAILED;
}

/* ================================================================
 * Setup / teardown
 * ================================================================ */

static int
suite_setup(void)
{
    allocate_threads(1);
    set_thread(0);
    return 0;
}

static int
suite_teardown(void)
{
    free_threads();
    return 0;
}

/* ================================================================
 * Test functions
 * ================================================================ */

static void
test_create_destroy(void)
{
    struct bdev_fancy *fancy;
    int rc;

    rc = bdev_fancy_create("fancy0", 1024, 512);
    CU_ASSERT(rc == 0);

    fancy = bdev_fancy_find("fancy0");
    SPDK_CU_ASSERT_FATAL(fancy != NULL);
    CU_ASSERT_STRING_EQUAL(fancy->bdev.name, "fancy0");
    CU_ASSERT_EQUAL(fancy->bdev.blockcnt, 1024);
    CU_ASSERT_EQUAL(fancy->bdev.blocklen, 512);

    bdev_fancy_destroy(fancy);
}

static void
test_create_invalid_blocklen(void)
{
    int rc;

    /* blocklen must be a power of 2 */
    rc = bdev_fancy_create("fancy_bad", 1024, 300);
    CU_ASSERT(rc == -EINVAL);
}

static void
test_create_alloc_failure(void)
{
    int rc;

    MOCK_SET(calloc, NULL);
    rc = bdev_fancy_create("fancy_oom", 1024, 512);
    CU_ASSERT(rc == -ENOMEM);
    MOCK_CLEAR(calloc);
}

static void
test_read_io(void)
{
    struct bdev_fancy *fancy;
    struct spdk_bdev_io bdev_io = {};
    int rc;

    rc = bdev_fancy_create("fancy0", 1024, 512);
    SPDK_CU_ASSERT_FATAL(rc == 0);

    fancy = bdev_fancy_find("fancy0");
    SPDK_CU_ASSERT_FATAL(fancy != NULL);

    /* Submit a read */
    g_completed_io = NULL;
    bdev_io.type = SPDK_BDEV_IO_TYPE_READ;
    bdev_io.u.bdev.offset_blocks = 0;
    bdev_io.u.bdev.num_blocks = 8;

    bdev_fancy_submit_request(NULL, &bdev_io);
    poll_thread(0);

    SPDK_CU_ASSERT_FATAL(g_completed_io != NULL);
    CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

    bdev_fancy_destroy(fancy);
}

/* ================================================================
 * Registration and main
 * ================================================================ */

int
main(int argc, char **argv)
{
    CU_pSuite suite;

    CU_initialize_registry();
    suite = CU_add_suite("bdev_fancy", suite_setup, suite_teardown);

    CU_ADD_TEST(suite, test_create_destroy);
    CU_ADD_TEST(suite, test_create_invalid_blocklen);
    CU_ADD_TEST(suite, test_create_alloc_failure);
    CU_ADD_TEST(suite, test_read_io);

    return spdk_ut_run_tests(argc, argv, NULL);
}
```

### Step 4: Register in unittest.sh

```bash
# In test/unit/unittest.sh, inside unittest_bdev():
function unittest_bdev() {
    # ... existing tests ...
    $valgrind $testdir/lib/bdev/bdev_fancy.c/bdev_fancy_ut
}
```

### Step 5: Build and run

```bash
make -j$(nproc)
./test/unit/lib/bdev/bdev_fancy.c/bdev_fancy_ut

# Verify all tests pass under Valgrind
valgrind --error-exitcode=1 --leak-check=full \
    ./test/unit/lib/bdev/bdev_fancy.c/bdev_fancy_ut
```

---

## Key Takeaways

- SPDK unit tests include source files directly (`#include "bdev/bdev.c"`) to enable testing of internal static functions and to isolate the translation unit under test.

- The mock system provides three patterns: `DEFINE_STUB` for simple return value substitution, `DEFINE_RETURN_MOCK` + `MOCK_ENQUEUE` for sequential call scenarios, and `DEFINE_WRAPPER` for intercepting libc syscalls at link time.

- `poll_thread(n)` and `poll_threads()` drive asynchronous state machines without a real reactor — never use `sleep()` in unit tests.

- `SPDK_CU_ASSERT_FATAL` stops the current test immediately; use it at every pointer check where subsequent code dereferences the pointer.

- Integration tests are shell scripts driven by `rpc.py` against live SPDK applications; they are gated by `SPDK_TEST_*` environment variables and orchestrated by `autotest_common.sh`.

- Fuzz tests target externally-exposed protocol parsers using LLVM libFuzzer with ASan + UBSan enabled; they require a clang build.

- `bdevperf` and `spdk_nvme_perf` are the primary performance benchmarking tools; both support JSON configuration, multiple workload types, and structured output for regression comparison.

- New unit tests must pass Valgrind clean before acceptance; sanitizer (`--enable-asan --enable-ubsan`) builds catch a complementary class of bugs.

---

## Exercises

### Exercise 1: Write a Unit Test Suite

Pick any file in `lib/` that does not yet have 100% unit test coverage (check with `lcov`). Write at least three new test functions covering:
- A success path
- An allocation failure path
- An invalid-argument path

Run your tests under Valgrind and confirm zero errors.

### Exercise 2: Debug a Mock Interaction

Introduce a deliberate bug in `bdev_ut.c`: comment out a `MOCK_CLEAR()` call and observe what happens when subsequent tests run. Understand why mock state leaking between tests causes false failures and non-deterministic test behavior.

### Exercise 3: Run the Autofuzz Campaign

Configure a machine with ASan+UBSan (clang required), build SPDK, and run:

```bash
TEST_TRANSPORT=tcp ./test/fuzz/autofuzz.sh --module=nvmf --timeout=120
```

Examine the corpus that gets generated and the coverage report. Identify at least one code path in `lib/nvmf/tcp.c` that the fuzzer exercises but that the unit tests do not.

### Exercise 4: Add a Performance Regression Test

Write a shell script using `bdevperf` that:
1. Creates a malloc bdev
2. Runs a 10-second randread workload
3. Parses the IOPS output
4. Fails if IOPS drops below a configurable threshold

This simulates a simple performance regression gate suitable for CI integration.

### Exercise 5: Multithreaded Test

Write a test in the style of `test/unit/lib/bdev/mt/bdev.c/bdev_ut.c` that exercises concurrent descriptor opens from three threads. Use `allocate_threads(3)`, `set_thread()`, and `poll_threads()` to simulate concurrent access and verify that the bdev layer serializes the operations correctly.

---

## Additional Resources

- `include/spdk_internal/cunit.h` — SPDK's CUnit integration header
- `include/spdk_internal/mock.h` — Complete mock macro documentation
- `lib/ut/ut.c` — Unit test runner implementation
- `lib/ut_mock/mock.c` — Syscall wrapper implementations
- `test/common/lib/ut_multithread.c` — Multithreaded test helpers
- `test/unit/unittest.sh` — Master unit test orchestration script
- `test/common/autotest_common.sh` — Integration test framework
- `test/fuzz/autofuzz.sh` — Fuzz campaign orchestration
- `examples/bdev/bdevperf/` — bdevperf source and documentation
- SPDK documentation: [Testing](https://spdk.io/doc/testing.html)
- CUnit documentation: [http://cunit.sourceforge.net/doc/](http://cunit.sourceforge.net/doc/)

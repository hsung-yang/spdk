# Exercise 01: Hello Bdev

**Module**: 13 - Bdev Layer
**Stage**: 2 (Implementation)
**Difficulty**: Beginner-Intermediate
**Estimated Time**: 1-2 hours
**Prerequisites**: Modules 01-12, SPDK development environment set up

---

## Objective

Build a standalone SPDK application that initializes the bdev subsystem, iterates over
every registered block device, and prints its key properties: name, total size in bytes,
block size, product name, and UUID. By the end you will understand how SPDK's bdev
abstraction layer is structured, how to enumerate devices at runtime, and how to run
application logic inside the SPDK event framework.

---

## Background

### The Bdev Abstraction

SPDK's block device (bdev) layer sits between your application and the storage backend.
Whether the backing store is an NVMe SSD, a kernel AIO file, an in-memory malloc device,
or a RAID array, your application code is identical — you always call the same bdev API.

```
Application
    |
    v
[ Bdev API ]          <-- uniform interface
    |
    +-- NVMe bdev  --> NVMe SSD
    +-- AIO bdev   --> Kernel file / block device
    +-- Malloc bdev --> RAM buffer
    +-- RAID bdev  --> Multiple backing bdevs
```

### Key API Functions Used in This Exercise

| Function | Purpose |
|---|---|
| `spdk_bdev_first()` | Return the first registered bdev (or NULL if none) |
| `spdk_bdev_next(prev)` | Return the next bdev after `prev` |
| `spdk_bdev_get_name(bdev)` | Return the bdev name string |
| `spdk_bdev_get_product_name(bdev)` | Return the product/module name string |
| `spdk_bdev_get_block_size(bdev)` | Return the logical block size in bytes |
| `spdk_bdev_get_num_blocks(bdev)` | Return the total number of logical blocks |
| `spdk_bdev_get_uuid(bdev)` | Return pointer to the bdev UUID |
| `spdk_uuid_fmt_lower(buf, sz, uuid)` | Format a UUID to a lowercase string |

### The SPDK Application Framework

SPDK applications run inside a reactor/poller model. The standard entry point is
`spdk_app_start()`, which takes a startup callback. All bdev-related calls must happen
on an SPDK thread (i.e., inside that callback or in a function it calls).

---

## Task Description

Write a C program `hello_bdev.c` that:

1. Parses SPDK application arguments (JSON config file, reactor mask, etc.).
2. Calls `spdk_app_start()` to initialize SPDK and enter the event loop.
3. Inside the start callback, iterates over all registered bdevs using
   `spdk_bdev_first()` / `spdk_bdev_next()`.
4. For each bdev, prints:
   - Name
   - Total capacity in bytes (`num_blocks * block_size`)
   - Block size in bytes
   - Product name
   - UUID (formatted as a lowercase hyphenated string)
5. Calls `spdk_app_stop(0)` after printing to exit cleanly.

---

## Step-by-Step Instructions

### Step 1 - Create the project directory

```bash
mkdir -p ~/spdk-exercises/ex01-hello-bdev
cd ~/spdk-exercises/ex01-hello-bdev
```

### Step 2 - Write the skeleton

Create `hello_bdev.c` with the structure below. Read the comments — each `TODO` marks
where you will fill in code.

```c
/* hello_bdev.c - Exercise 01: Enumerate all bdevs and print their properties */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/uuid.h"

/* Called by spdk_app_start() once SPDK has initialized. */
static void
hello_bdev_start(void *arg)
{
    struct spdk_bdev *bdev;
    uint32_t count = 0;

    SPDK_NOTICELOG("=== Bdev Enumeration ===\n");

    /* TODO 1: Obtain the first bdev using spdk_bdev_first().
     * Assign the result to `bdev`. */

    /* TODO 2: Loop while `bdev` is not NULL.
     * Inside the loop:
     *   a) Declare a char array of 37 bytes for the UUID string.
     *   b) Format the UUID using spdk_uuid_fmt_lower().
     *   c) Compute total_bytes = num_blocks * block_size.
     *   d) Print all five properties with SPDK_NOTICELOG or printf.
     *   e) Advance to the next bdev using spdk_bdev_next().
     *   f) Increment the count.
     */

    SPDK_NOTICELOG("=== Total bdevs found: %u ===\n", count);

    /* TODO 3: Call spdk_app_stop(0) to exit cleanly. */
}

int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    /* Initialize opts with defaults. */
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "hello_bdev";

    /* Parse SPDK standard arguments (-c <config>, -m <mask>, etc.). */
    rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    /* Start the SPDK application framework. hello_bdev_start is called
     * after all subsystems (including bdev) have been initialized. */
    rc = spdk_app_start(&opts, hello_bdev_start, NULL);

    spdk_app_fini();
    return rc;
}
```

### Step 3 - Implement the TODOs

Fill in the three TODO sections. The solution is shown in the next section if you get
stuck, but try to write it yourself first.

**Hints:**

- `spdk_bdev_first()` returns `struct spdk_bdev *` (NULL when no bdevs exist).
- `spdk_bdev_next(bdev)` takes the current bdev and returns the next one.
- UUID string buffer must be at least 37 bytes (`xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\0`).
- `spdk_bdev_get_uuid()` returns `const struct spdk_uuid *`, not a string.
- `spdk_uuid_fmt_lower()` signature: `int spdk_uuid_fmt_lower(char *uuid_str, size_t uuid_str_size, const struct spdk_uuid *uuid)`.

### Step 4 - Write the Makefile

Create `Makefile` in the same directory:

```makefile
# Makefile for Exercise 01: Hello Bdev

SPDK_PATH ?= $(HOME)/spdk

# pkg-config provides the correct compiler and linker flags for SPDK.
SPDK_CFLAGS  := $(shell PKG_CONFIG_PATH=$(SPDK_PATH)/build/lib/pkgconfig \
                    pkg-config --cflags spdk_event spdk_event_bdev)
SPDK_LDFLAGS := $(shell PKG_CONFIG_PATH=$(SPDK_PATH)/build/lib/pkgconfig \
                    pkg-config --libs spdk_event spdk_event_bdev)

CC      = gcc
CFLAGS  = -g -Wall -Wextra $(SPDK_CFLAGS)
LDFLAGS = $(SPDK_LDFLAGS) -ldpdk -lnuma -luuid -pthread

TARGET  = hello_bdev

$(TARGET): hello_bdev.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
```

### Step 5 - Prepare a minimal JSON configuration

SPDK needs a config file to know which bdevs to create. For this exercise, create one
malloc bdev (no hardware required):

```json
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_malloc_create",
          "params": {
            "name": "Malloc0",
            "num_blocks": 4096,
            "block_size": 512
          }
        },
        {
          "method": "bdev_malloc_create",
          "params": {
            "name": "Malloc1",
            "num_blocks": 8192,
            "block_size": 4096
          }
        }
      ]
    }
  ]
}
```

Save as `bdev.json`.

### Step 6 - Build and run

```bash
# Build (set SPDK_PATH if your SPDK tree is not at ~/spdk)
make SPDK_PATH=/path/to/your/spdk

# Run (requires hugepages; use sudo or configure permissions)
sudo ./hello_bdev -c bdev.json
```

If your environment uses vfio or UIO for NVMe, add your normal SPDK DPDK mask flags:

```bash
sudo ./hello_bdev -c bdev.json -m 0x1
```

---

## Complete Solution

```c
/* hello_bdev.c - Exercise 01: SOLUTION */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/uuid.h"

#define UUID_STR_LEN 37   /* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\0" */

static void
hello_bdev_start(void *arg)
{
    struct spdk_bdev *bdev;
    uint32_t count = 0;

    SPDK_NOTICELOG("=== Bdev Enumeration ===\n");

    /* Iterate over every registered bdev. */
    for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
        char uuid_str[UUID_STR_LEN];
        uint64_t total_bytes;

        /* Format UUID to lowercase hyphenated string. */
        spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), spdk_bdev_get_uuid(bdev));

        /* Total capacity in bytes. */
        total_bytes = spdk_bdev_get_num_blocks(bdev) *
                      (uint64_t)spdk_bdev_get_block_size(bdev);

        SPDK_NOTICELOG(
            "  [%u] name=%-20s  size=%10" PRIu64 " B  "
            "block_size=%5u B  product=%-20s  uuid=%s\n",
            count,
            spdk_bdev_get_name(bdev),
            total_bytes,
            spdk_bdev_get_block_size(bdev),
            spdk_bdev_get_product_name(bdev),
            uuid_str);

        count++;
    }

    SPDK_NOTICELOG("=== Total bdevs found: %u ===\n", count);

    spdk_app_stop(0);
}

int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "hello_bdev";

    rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    rc = spdk_app_start(&opts, hello_bdev_start, NULL);

    spdk_app_fini();
    return rc;
}
```

---

## Expected Output

Running against the two malloc bdevs defined in `bdev.json` produces output similar to:

```
[2026-03-30 10:12:01.543] NOTICE: ==> Bdev Enumeration <==
[2026-03-30 10:12:01.543] NOTICE:   [0] name=Malloc0               size=   2097152 B  block_size=  512 B  product=Malloc Disk            uuid=a4e1bc3f-22d0-4f71-9c7a-1b3e8d905f20
[2026-03-30 10:12:01.543] NOTICE:   [1] name=Malloc1               size= 33554432 B  block_size= 4096 B  product=Malloc Disk            uuid=f7d3a012-8b4c-4e61-adf3-09c7e5820b11
[2026-03-30 10:12:01.543] NOTICE: ==> Total bdevs found: 2 <==
```

Note: UUIDs are generated randomly at device creation time and will differ on each run
unless the config file specifies them explicitly via the `uuid` parameter.

---

## Bonus Challenges

### Challenge A: Filter by product type

Modify the loop to accept an optional `-P <product_prefix>` argument and print only
bdevs whose product name starts with that prefix (e.g., `-P Malloc` or `-P NVMe`).

Hint: add a custom option to `spdk_app_parse_args()` using the `getopt_str` and
`parse_arg` callback parameters, and compare with `strncmp()`.

### Challenge B: Display capacity in human-readable units

Instead of printing raw bytes, format the capacity as KB, MB, or GB depending on size:

```c
static void
fmt_bytes(uint64_t bytes, char *buf, size_t len)
{
    if (bytes >= (1ULL << 30)) {
        snprintf(buf, len, "%.2f GiB", (double)bytes / (1ULL << 30));
    } else if (bytes >= (1ULL << 20)) {
        snprintf(buf, len, "%.2f MiB", (double)bytes / (1ULL << 20));
    } else if (bytes >= (1ULL << 10)) {
        snprintf(buf, len, "%.2f KiB", (double)bytes / (1ULL << 10));
    } else {
        snprintf(buf, len, "%" PRIu64 " B", bytes);
    }
}
```

### Challenge C: Count bdevs by product type

After enumeration, print a summary table showing how many bdevs of each product type
were found, using a simple array of `{product_name, count}` structs.

### Challenge D: Use spdk_bdev_first_leaf() / spdk_bdev_next_leaf()

Replace `spdk_bdev_first()` / `spdk_bdev_next()` with `spdk_bdev_first_leaf()` /
`spdk_bdev_next_leaf()`. Run with a config that has a RAID or Crypto bdev layered on
top of Malloc bdevs and observe which bdevs appear in each enumeration mode.

`spdk_bdev_first_leaf()` returns only leaf (non-virtual) devices — useful when you want
to operate directly on physical backing stores rather than logical stacks.

---

## Common Mistakes

### Mistake 1: Forgetting `spdk_app_stop()`

```c
/* WRONG - application hangs forever waiting for more events */
static void hello_bdev_start(void *arg) {
    /* ... enumerate bdevs ... */
    /* missing: spdk_app_stop(0); */
}
```

`spdk_app_start()` blocks the calling thread until `spdk_app_stop()` is invoked. If
you forget to call it, the process will idle indefinitely after your callback returns.

### Mistake 2: Using `spdk_bdev_get_uuid()` as a string directly

```c
/* WRONG - spdk_bdev_get_uuid() returns const struct spdk_uuid *, not char * */
printf("uuid=%s\n", spdk_bdev_get_uuid(bdev));  /* undefined behavior */

/* CORRECT */
char uuid_str[37];
spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), spdk_bdev_get_uuid(bdev));
printf("uuid=%s\n", uuid_str);
```

### Mistake 3: Integer overflow when computing total bytes

```c
/* WRONG on large devices - num_blocks is uint64_t but block_size is uint32_t;
 * the multiplication may overflow if block_size is cast to uint32_t first */
uint32_t total = spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev);

/* CORRECT - cast to uint64_t before multiplying */
uint64_t total_bytes = spdk_bdev_get_num_blocks(bdev) *
                       (uint64_t)spdk_bdev_get_block_size(bdev);
```

### Mistake 4: Calling bdev APIs before SPDK is initialized

```c
/* WRONG - called from main() directly, before spdk_app_start() */
int main(int argc, char **argv) {
    spdk_app_opts_init(&opts, sizeof(opts));
    struct spdk_bdev *bdev = spdk_bdev_first(); /* NULL, subsystem not ready */
    ...
}
```

All bdev API calls must be made from within an SPDK thread — i.e., inside the callback
passed to `spdk_app_start()`, or in functions that callback dispatches.

### Mistake 5: Using a too-small UUID buffer

```c
/* WRONG - UUID string is 36 chars + NUL = 37 bytes minimum */
char uuid_str[36];  /* off by one; no room for the NUL terminator */
spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), spdk_bdev_get_uuid(bdev));
/* result: truncated UUID, or stack corruption */

/* CORRECT */
char uuid_str[37];
```

### Mistake 6: Modifying bdev state during enumeration

Do not open, close, or create bdevs inside the `spdk_bdev_first()` / `spdk_bdev_next()`
loop. The list may be mutated, causing the iterator to skip devices or access freed
memory. Collect device names first, then operate on them after the loop.

---

## Key Takeaways

- The bdev layer provides a uniform block device interface regardless of backend.
- `spdk_bdev_first()` and `spdk_bdev_next()` provide a simple forward iterator over
  all registered devices.
- Device properties (name, size, product name, UUID) are read-only and available
  without opening a descriptor.
- All bdev API calls must run on an SPDK thread — inside the `spdk_app_start()`
  callback or any function it calls.
- Always call `spdk_app_stop()` to terminate the reactor loop cleanly.

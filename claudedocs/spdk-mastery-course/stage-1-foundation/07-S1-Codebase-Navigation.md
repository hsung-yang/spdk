# Module 07: Codebase Navigation

**Stage**: 1 (Foundation)
**Difficulty**: Beginner
**Estimated Time**: 2 hours
**Prerequisites**: Module 01-06

**Version History**:
- v1.0 (2026-01-30): Initial version

---

## Learning Objectives

By the end of this module, you will be able to:
- Navigate SPDK source code efficiently
- Find function implementations quickly
- Understand header file organization
- Locate API documentation
- Use code patterns to understand implementations
- Contribute to SPDK effectively

---

## Overview

SPDK contains ~200,000 lines of code across hundreds of files. Knowing how to navigate this codebase efficiently is essential for development and debugging.

---

## Core Concepts

### Concept 1: Directory Quick Reference

```mermaid
graph TD
    A[spdk/] --> B[lib/<br/>Core libraries - read-heavy]
    A --> C[module/<br/>Pluggable modules - read-heavy]
    A --> D[include/spdk/<br/>Public APIs - reference-heavy]
    A --> E[app/<br/>Applications - example code]
    A --> F[examples/<br/>Simple examples - learning]
    A --> G[test/<br/>Tests - understanding behavior]
    A --> H[scripts/<br/>Utilities]
    A --> I[doc/<br/>Documentation]

    style A fill:#e1f5ff
    style B fill:#fff4e1
    style C fill:#fff4e1
    style D fill:#e1ffe1
    style E fill:#f0f0f0
    style F fill:#f0f0f0
    style G fill:#f0f0f0
    style H fill:#f0f0f0
    style I fill:#f0f0f0
```

**Navigation Strategy**:
1. Need API? → `include/spdk/`
2. Need implementation? → `lib/[component]/`
3. Need example? → `examples/` or `app/`
4. Need test? → `test/`

---

### Concept 2: Finding Code

**By Feature**:
```
Question: Where is NVMe command submission?
Answer: lib/nvme/nvme_qpair.c

Question: Where is bdev I/O routing?
Answer: lib/bdev/bdev.c

Question: Where is JSON-RPC handling?
Answer: lib/jsonrpc/jsonrpc_server.c
```

**By Function Name**:
```bash
# Find function definition
grep -r "function_name" lib/ module/

# Find function usage
grep -r "function_name(" lib/ app/

# Better: use ctags or LSP
```

**By Module**:
```
Need: NVMe driver
Look: lib/nvme/

Need: RAID implementation
Look: module/bdev/raid/

Need: NVMe-oF target
Look: lib/nvmf/
```

---

### Concept 3: Header Organization

**Public Headers** (`include/spdk/`):
```mermaid
graph TD
    A[include/spdk/] --> B[nvme.h - NVMe driver API]
    A --> C[bdev.h - Bdev layer API]
    A --> D[nvmf.h - NVMe-oF target API]
    A --> E[thread.h - Threading API]
    A --> F[event.h - Event framework API]
    A --> G[...]

    N["Use: #include \"spdk/nvme.h\""]

    style A fill:#e1f5ff
    style B fill:#fff4e1
    style C fill:#fff4e1
    style D fill:#fff4e1
    style E fill:#fff4e1
    style F fill:#fff4e1
```

**Internal Headers** (`lib/[component]/`):
```mermaid
graph TD
    A[lib/nvme/] --> B[nvme_internal.h - Internal definitions]
    A --> C[nvme_pcie.h - PCIe transport]
    A --> D[...]

    N[Use: Internal to library only]

    style A fill:#ffe1f5
    style B fill:#f0f0f0
    style C fill:#f0f0f0
```

**Common Headers** (`include/spdk_internal/`):
```mermaid
graph TD
    A[include/spdk_internal/] --> B[assert.h]
    A --> C[log.h]
    A --> D[...]

    N[Use: Shared across libraries,<br/>not public API]

    style A fill:#ffe1f5
    style B fill:#f0f0f0
    style C fill:#f0f0f0
```

---

### Concept 4: Code Patterns

**Module Registration**:
```c
// Look for this pattern
SPDK_BDEV_MODULE_REGISTER(name, &module_if)
SPDK_LOG_REGISTER_COMPONENT(name)
SPDK_RPC_REGISTER(method, handler, flags)
```

**Init/Fini Pattern**:
```c
static int
module_init(void)
{
    // Initialization
    return 0;
}

static void
module_fini(void)
{
    // Cleanup
}
```

**Callback Pattern**:
```c
typedef void (*callback_fn)(void *ctx, int status);

void async_operation(callback_fn cb, void *ctx)
{
    // Start operation
    // ...
    // On completion:
    cb(ctx, 0);
}
```

---

## Navigation Techniques

### Technique 1: Follow the Headers

```c
// In your code
#include "spdk/bdev.h"

// Find bdev.h
$ find include/ -name "bdev.h"
include/spdk/bdev.h

// Read bdev.h for API documentation
$ less include/spdk/bdev.h

// Find implementation
$ ls lib/bdev/
bdev.c  bdev.h  ...
```

### Technique 2: Grep Effectively

```bash
# Find function definition
grep -rn "^spdk_bdev_read" lib/

# Find struct definition
grep -rn "^struct spdk_bdev {" lib/

# Find macro definition
grep -rn "^#define SPDK_" include/

# Find RPC method
grep -rn "SPDK_RPC_REGISTER.*bdev_get_bdevs" lib/

# Case-insensitive, show filename
grep -rin "nvme_qpair" lib/nvme/
```

### Technique 3: Using ctags

```bash
# Generate tags
cd spdk
ctags -R .

# In vim:
# Ctrl-] on function name to jump to definition
# Ctrl-T to jump back
```

### Technique 4: Using Git

```bash
# Find when function was added
git log -p --all -S "spdk_bdev_read"

# Find file history
git log --follow -- lib/bdev/bdev.c

# Find who changed this
git blame lib/bdev/bdev.c

# Search commit messages
git log --grep="bdev" --oneline
```

---

## Common Code Locations

### Core Infrastructure

| Component | Location | Key Files |
|-----------|----------|-----------|
| Threading | `lib/thread/` | `thread.c`, `thread.h` |
| Event framework | `lib/event/` | `app.c`, `reactor.c` |
| JSON-RPC | `lib/jsonrpc/` | `jsonrpc_server.c` |
| Logging | `lib/log/` | `log.c` |
| Tracing | `lib/trace/` | `trace.c` |
| Utilities | `lib/util/` | `string.c`, `cpuset.c` |

### Storage Stack

| Component | Location | Key Files |
|-----------|----------|-----------|
| Bdev layer | `lib/bdev/` | `bdev.c`, `bdev.h` |
| NVMe driver | `lib/nvme/` | `nvme.c`, `nvme_qpair.c` |
| NVMe bdev | `module/bdev/nvme/` | `bdev_nvme.c` |
| RAID | `module/bdev/raid/` | `raid.c` |
| Crypto | `module/bdev/crypto/` | `vbdev_crypto.c` |

### Protocols

| Component | Location | Key Files |
|-----------|----------|-----------|
| NVMe-oF target | `lib/nvmf/` | `nvmf.c`, `transport.c` |
| RDMA transport | `lib/nvmf/` | `rdma.c` |
| TCP transport | `lib/nvmf/` | `tcp.c` |
| iSCSI target | `lib/iscsi/` | `iscsi.c` |
| Vhost target | `lib/vhost/` | `vhost.c` |

---

## Reading Code Effectively

### Start with Examples

```mermaid
graph TD
    subgraph Step1["1. Read examples/nvme/hello_world/"]
        A[Simple, complete program]
        B[Shows initialization]
        C[Shows basic I/O]
        D[Good starting point]
    end

    subgraph Step2["2. Read app/ applications"]
        E[More complex]
        F[Real-world patterns]
        G[Production-ready code]
    end

    style Step1 fill:#e1f5ff
    style Step2 fill:#fff4e1
```

### Follow I/O Path

```mermaid
graph TD
    A[Application] --> B[examples/bdev/hello_bdev/hello_bdev.c]
    B --> C[Bdev API]
    C --> D[lib/bdev/bdev.c:spdk_bdev_read]
    D --> E[Bdev Module]
    E --> F[module/bdev/nvme/bdev_nvme.c]
    F --> G[NVMe Driver]
    G --> H[lib/nvme/nvme_qpair.c:spdk_nvme_ns_cmd_read]
    H --> I[Hardware]

    style A fill:#e1f5ff
    style C fill:#fff4e1
    style E fill:#ffe1f5
    style G fill:#e1ffe1
    style I fill:#f0f0f0
```

### Understand Initialization

```mermaid
graph TD
    A[main] --> B[app/spdk_tgt/spdk_tgt.c]
    B --> C[spdk_app_start]
    C --> D[lib/event/app.c]
    D --> E[Subsystem init]
    E --> F[lib/event/subsystem.c]
    F --> G[Module init]
    G --> H[module/bdev/nvme/bdev_nvme.c:bdev_nvme_init]

    style A fill:#e1f5ff
    style C fill:#fff4e1
    style E fill:#ffe1f5
    style G fill:#e1ffe1
```

---

## Documentation

### Doxygen Documentation

```bash
# Build documentation
cd doc
make

# Open in browser
firefox html/index.html

# Online: https://spdk.io/doc/
```

### Markdown Documentation

```mermaid
graph TD
    A[doc/] --> B[README.md - Start here]
    A --> C[getting_started.md]
    A --> D[nvme.md - NVMe driver]
    A --> E[bdev.md - Bdev layer]
    A --> F[nvmf.md - NVMe-oF]
    A --> G[iscsi.md - iSCSI]
    A --> H[vhost.md - Vhost]
    A --> I[concurrency.md - Threading]
    A --> J[memory.md - Memory]

    style A fill:#e1f5ff
    style B fill:#fff4e1
    style C fill:#f0f0f0
    style D fill:#f0f0f0
    style E fill:#f0f0f0
    style F fill:#f0f0f0
    style G fill:#f0f0f0
    style H fill:#f0f0f0
    style I fill:#f0f0f0
    style J fill:#f0f0f0
```

### Code Comments

```c
/**
 * Read data from a block device.
 *
 * \param desc Block device descriptor
 * \param ch I/O channel
 * \param buf Data buffer (must be DMA-capable)
 * \param offset_blocks Offset in blocks
 * \param num_blocks Number of blocks
 * \param cb Completion callback
 * \param cb_arg Callback argument
 *
 * \return 0 on success, negative errno on failure
 */
int spdk_bdev_read(struct spdk_bdev_desc *desc, ...);
```

---

## Practical Examples

### Example 1: Finding NVMe Read Implementation

```bash
# 1. Find public API
$ grep -rn "spdk_nvme_ns_cmd_read" include/spdk/
include/spdk/nvme.h:1234:int spdk_nvme_ns_cmd_read(...);

# 2. Find implementation
$ grep -rn "^spdk_nvme_ns_cmd_read" lib/nvme/
lib/nvme/nvme_ns_cmd.c:123:int spdk_nvme_ns_cmd_read(...)

# 3. Read implementation
$ vim lib/nvme/nvme_ns_cmd.c +123
```

### Example 2: Understanding Bdev Modules

```bash
# 1. List all bdev modules
$ ls module/bdev/
aio/  crypto/  delay/  ...

# 2. Pick one to study
$ cd module/bdev/nvme/

# 3. Find module registration
$ grep "SPDK_BDEV_MODULE_REGISTER" *.c
bdev_nvme.c:SPDK_BDEV_MODULE_REGISTER(nvme, &nvme_if)

# 4. Study module interface
$ vim bdev_nvme.c
# Look for: nvme_if structure
```

---

## Best Practices

1. **Start with public APIs**
   - Read `include/spdk/*.h` first
   - Understand contracts before implementations

2. **Follow examples**
   - Learn patterns from working code
   - Don't reinvent the wheel

3. **Use git history**
   - Understand why code exists
   - See evolution of features

4. **Read tests**
   - `test/unit/` for unit tests
   - Shows expected behavior
   - Good for edge cases

5. **Contribute early**
   - Fix documentation typos
   - Learn review process
   - Build confidence

---

## Knowledge Check

1. **Where do you find public API documentation?**

2. **How do you find where a function is implemented?**

3. **Where are bdev module implementations located?**

4. **What's the best place to start learning SPDK?**

5. **How do you search for RPC method implementations?**

---

## Additional Resources

- **SPDK Source**: Browse entire repository
- **GitHub**: https://github.com/spdk/spdk
- **Documentation**: https://spdk.io/doc/
- **Related Modules**:
  - This completes Stage 1!
  - Next: [Stage 2: Implementation](../stage-2-implementation/)

---

## Summary

**Navigation Strategy**:
```mermaid
graph LR
    A[APIs] --> B[include/spdk/]
    C[Implementation] --> D[lib/component/]
    E[Examples] --> F[examples/, app/]
    G[Tests] --> H[test/]
    I[Docs] --> J[doc/]

    style A fill:#e1f5ff
    style C fill:#fff4e1
    style E fill:#ffe1f5
    style G fill:#e1ffe1
    style I fill:#f0f0f0
```

**Tools**:
- grep for searching
- ctags for jumping
- git for history
- Doxygen for API docs

**Learning Path**:
1. Read examples first
2. Study public headers
3. Trace I/O paths
4. Read implementations
5. Study tests

**You now have the foundation!**

Stage 1 Complete! You understand:
- Why SPDK exists
- Core principles
- Architecture
- Threading model
- Memory management
- Build system
- Codebase navigation

**Next**: Move to Stage 2 for hands-on implementation!

---

*End of Module 07 - Stage 1 Complete!*

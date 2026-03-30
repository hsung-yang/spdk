# Module 30: Contributing to SPDK

**Stage 3 - Mastery** | Prerequisites: Modules 1-29, solid C programming experience, familiarity with git

---

## Overview

Contributing to SPDK is one of the most effective ways to deepen your expertise. The project is governed
through the Linux Foundation, reviewed by a small team of core maintainers, and follows a well-defined
patch submission process. This module walks through everything you need to know: the governance structure,
setting up your environment, coding standards, the end-to-end patch workflow, CI expectations, documentation
requirements, code review etiquette, and the path to becoming a maintainer.

---

## 1. Project Governance

### 1.1 Core Maintainers

SPDK has a small team of core maintainers who carry out day-to-day technical oversight. As of 2024-2025
the team includes:

| Maintainer | Affiliation |
|---|---|
| Jim Harris | NVIDIA |
| Jacek Kalwas | Intel/Solidigm |
| Mateusz Kozlowski | Intel/Solidigm |
| Changpeng Liu | Intel |
| Alexey Marchuk | NVIDIA |
| Shuhei Matsumoto | Fujitsu |
| Konrad Sztyber | Intel |
| Ben Walker | Intel/Solidigm |
| Tomek Zawadzki | Nutanix |

Core maintainer responsibilities include:

- Reviewing and approving patches
- Setting code review and development guidelines
- Making decisions on community processes
- Role modeling good development practices
- Fostering a positive, productive community
- Participating in project roadmap definition
- Identifying and organizing development tasks

### 1.2 Technical Steering Committee (TSC)

SPDK has a Technical Steering Committee (TSC) chartered through the Linux Foundation for overall technical
oversight. The TSC voting members consist of:

- All core maintainers listed above
- One representative appointed by each participating organization

**Participating organizations**: ARM, Dell, HPE, Nutanix, NVIDIA, Samsung, Solidigm, Starwind, Tencent

TSC contacts: Tomek Zawadzki (tomasz.zawadzki@nutanix.com) and Jim Harris (jim.harris@nvidia.com).

### 1.3 Decision Making

The TSC handles high-level project direction and policy. Day-to-day technical decisions — such as whether
a patch is accepted, how an API should be designed, or which features land in a release — are handled by
the core maintainer team through the patch review process.

---

## 2. Code of Conduct

SPDK uses the Contributor Covenant Code of Conduct (version 2.1). Participation in the community requires
adherence to these standards.

### Positive Behaviors Expected

- Demonstrating empathy and kindness toward other people
- Being respectful of differing opinions, viewpoints, and experiences
- Giving and gracefully accepting constructive feedback
- Accepting responsibility and apologizing to those affected by mistakes, and learning from the experience
- Focusing on what is best not just for individuals, but for the overall community

### Unacceptable Behaviors

- Sexualized language or imagery, and sexual attention or advances of any kind
- Trolling, insulting or derogatory comments, and personal or political attacks
- Public or private harassment
- Publishing others' private information (physical or email address) without explicit permission
- Other conduct which could reasonably be considered inappropriate in a professional setting

### Enforcement

Violations may be reported privately to any SPDK core maintainer. The maintainers will review and
investigate all complaints promptly and fairly. Consequences range from a private written warning,
to a temporary ban, to a permanent ban depending on severity and pattern of violation.

**Key point**: The code of conduct applies within all community spaces (mailing lists, GitHub, IRC/Matrix,
conferences) and when individuals officially represent the community in public spaces.

---

## 3. Setting Up Your Development Environment for Contribution

### 3.1 Prerequisites

```bash
# Core build tools
sudo apt-get install -y build-essential git python3 python3-pip meson ninja-build

# Format checking tools (versions matter)
sudo apt-get install -y astyle    # need version 3.0.1 - 3.1
pip3 install ruff mypy            # Python style and type checking

# Shell format checker
# shfmt v3.8.0 is the supported version - install via pkgdep.sh
./scripts/pkgdep.sh -d

# Optional but useful for static analysis
sudo apt-get install -y clang-tools  # provides scan-build
```

### 3.2 Initial Repository Setup

```bash
# Clone the repository
git clone https://github.com/spdk/spdk.git
cd spdk

# Initialize all submodules (required - SPDK uses many)
git submodule update --init

# Verify the build works before making changes
./configure
make -j$(nproc)

# Run the format checker to establish baseline
./scripts/check_format.sh
```

### 3.3 Configuring Git for Contribution

SPDK requires a `Signed-off-by` trailer on every commit (Developer Certificate of Origin). Set up your
identity so `git commit -s` automatically generates the correct trailer:

```bash
git config --global user.name  "Your Full Name"
git config --global user.email "your.email@example.com"

# Useful aliases for SPDK workflow
git config --global alias.logone "log --oneline"
git config --global alias.fixup  "commit --fixup"
```

### 3.4 Staying in Sync with Upstream

```bash
# Add the upstream remote if you cloned from a fork
git remote add upstream https://github.com/spdk/spdk.git

# Fetch latest changes
git fetch upstream

# Keep your master branch current
git checkout master
git merge upstream/master
```

---

## 4. Coding Standards and Style Guide

SPDK enforces coding style automatically through `scripts/check_format.sh`. Understanding what the
script checks lets you write conformant code from the start rather than iterating after the fact.

### 4.1 C/C++ Style (astyle)

SPDK uses **astyle** with a specific configuration stored in `.astylerc` at the repository root:

| Rule | Value |
|---|---|
| Bracket style | K&R |
| Indentation | Tabs (force-tab=8) |
| Line length limit | 100 characters |
| Pointer alignment | `*` adjacent to variable name (`int *ptr`) |
| Operator padding | spaces around operators |
| Keyword padding | space between `if`/`while`/`for` and `(` |
| Parenthesis padding | no extra spaces inside parens |
| Braces | one-line conditional statements get braces added |
| Line endings | LF only (Linux) |

```c
/* CORRECT - K&R brackets, tabs, space around operators */
static int
my_function(struct spdk_bdev *bdev, uint64_t offset)
{
	if (bdev == NULL) {
		return -EINVAL;
	}

	bdev->offset = offset + 1;
	return 0;
}

/* WRONG - allman brackets, spaces for indentation, no space around operators */
static int my_function(struct spdk_bdev* bdev, uint64_t offset){
    if(bdev==NULL){
        return -EINVAL;
    }
    bdev->offset=offset+1;
    return 0;
}
```

**Running the C style check**:

```bash
# Check all C/C++ files tracked by git
./scripts/check_format.sh

# astyle version must be between 3.0.1 and 3.1
astyle --version

# To auto-fix in place (WARNING: modifies files)
astyle --options=.astylerc --break-return-type --attach-return-type-decl <file.c>
```

### 4.2 Python Style (ruff + mypy)

Python code is checked with `ruff` and `mypy` using the configuration in `python/pyproject.toml`:

```bash
# Run Python style check
ruff --config python/pyproject.toml check python/

# Run type checking
mypy --config-file python/pyproject.toml python/
```

Key rules:
- Follow PEP 8
- Type annotations required for new Python code
- No wildcard imports (`from module import *`)

### 4.3 Shell Script Style (shfmt)

Shell scripts are formatted with `shfmt` version **v3.8.0** exactly (other versions are not accepted):

```bash
# Install supported shfmt version
./scripts/pkgdep.sh -d

# The check runs automatically inside check_format.sh
# Settings used:
#   -i 0    indent with tabs
#   -bn     binary operators start the next line
#   -ci     switch/case indented
#   -ln bash  bash variant
#   -sr     space after redirect operators
```

### 4.4 General C Conventions in SPDK

Beyond what astyle enforces, SPDK has strong conventions derived from the Linux kernel style and the
project's own practices:

**Error handling**:
```c
/* Use negative errno values for errors */
if (rc != 0) {
    SPDK_ERRLOG("Operation failed: %s\n", spdk_strerror(-rc));
    return rc;
}

/* Check pointers immediately */
if (ctx == NULL) {
    return -ENOMEM;
}
```

**Naming conventions**:
```c
/* Public API functions: spdk_<module>_<verb>_<noun> */
int spdk_bdev_read(struct spdk_bdev_desc *desc, ...);

/* Internal functions: <module>_<verb>_<noun> (no spdk_ prefix) */
static int bdev_read_internal(struct spdk_bdev *bdev, ...);

/* Structs: spdk_<module>_<noun> for public, <module>_<noun> for internal */
struct spdk_bdev_io;      /* public */
struct bdev_io_channel;   /* internal */

/* Macros and constants: SPDK_<MODULE>_<NAME> */
#define SPDK_BDEV_LARGE_BUF_MAX_SIZE (64 * 1024)
```

**Memory management**:
```c
/* Use spdk_ allocators for SPDK-managed memory */
buf = spdk_malloc(size, alignment, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
if (buf == NULL) {
    return -ENOMEM;
}

/* Corresponding free */
spdk_free(buf);
```

**Documentation comments**:
```c
/**
 * Brief one-line description.
 *
 * Longer description if needed. Explain the "why" not just the "what".
 *
 * \param desc Bdev descriptor opened with spdk_bdev_open_ext().
 * \param buf  DMA-safe buffer allocated with spdk_malloc().
 * \param nbytes Number of bytes to read. Must be a multiple of block size.
 * \param cb Completion callback. Called on the same thread that issued the I/O.
 * \param cb_arg Opaque argument passed to \p cb.
 *
 * \return 0 on success, negative errno on failure. -ENOMEM if no resources.
 */
int spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                   void *buf, uint64_t offset, uint64_t nbytes,
                   spdk_bdev_io_completion_cb cb, void *cb_arg);
```

All public API functions (those declared in `include/spdk/`) must have Doxygen comments.

### 4.5 Header File Conventions

```c
/* Every header needs an include guard using the filename */
#ifndef SPDK_BDEV_H
#define SPDK_BDEV_H

/* Include SPDK common types first */
#include "spdk/stdinc.h"

/* ... declarations ... */

#ifdef __cplusplus
extern "C" {
#endif

/* ... C-linkage declarations ... */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_H */
```

---

## 5. SPDK Development Workflow

SPDK uses **GitHub pull requests** as its primary code review mechanism, with a CI system that runs
automatically on each PR.

### 5.1 Step-by-Step Patch Submission

**Step 1: Create a focused branch**

```bash
# Always branch from the latest master
git fetch upstream
git checkout -b <module>/<short-description> upstream/master

# Examples of good branch names:
#   bdev/add-uring-passthrough
#   nvmf/fix-tcp-reconnect-timeout
#   doc/update-bdev-api-guide
```

**Step 2: Make your changes**

- Keep each commit focused on one logical change
- Avoid mixing refactoring with bug fixes or new features
- Write the implementation and tests together

**Step 3: Write the unit test**

```bash
# Unit tests live under test/unit/
# Find the test file for your module:
ls test/unit/lib/<module>/

# Run unit tests to verify
./test/unit/unittest.sh

# Run only the relevant test suite
./test/unit/unittest.sh <module>
```

**Step 4: Run the format checker**

```bash
# Must pass with zero errors before submitting
./scripts/check_format.sh

# If astyle finds formatting issues, it will report them.
# Fix manually or let astyle reformat (then review the diff carefully).
git diff  # review any formatting changes
```

**Step 5: Run static analysis (recommended)**

```bash
# Clang static analyzer
scan-build make

# Address sanitizer build
./configure --enable-asan --enable-ubsan
make -j$(nproc)
```

**Step 6: Write good commit messages** (see Section 6)

```bash
git add <files>
git commit -s   # -s adds Signed-off-by automatically
```

**Step 7: Rebase onto latest master before submitting**

```bash
git fetch upstream
git rebase upstream/master

# Resolve any conflicts, then:
git rebase --continue
```

**Step 8: Push and open a pull request**

```bash
git push origin <your-branch>
# Then open a PR on https://github.com/spdk/spdk
```

**Step 9: Respond to review feedback**

```bash
# Make changes requested by reviewers
git add <files>
git commit --fixup HEAD   # or interactive rebase to squash

# Clean up the history before final merge
git rebase -i upstream/master  # squash fixup commits

# Force push to update your PR branch
git push --force-with-lease origin <your-branch>
```

### 5.2 What Happens After You Submit

1. CI runs automatically (build, unit tests, style check, static analysis)
2. Core maintainers or community members review the code
3. Review comments are posted on GitHub - you respond and update the PR
4. Once the patch is approved by at least one core maintainer and CI passes, a maintainer merges it
5. Patches should not be merged by their own author

---

## 6. Writing Good Commit Messages for SPDK

SPDK commit messages follow a strict format. Getting this right is important — the project history
is a primary form of documentation.

### 6.1 Format

```
<module>: <short imperative summary under 72 characters>

<Blank line>

<Body: explain WHAT changed and WHY. Wrap at 72 characters.
Do not explain HOW — that is what the code is for.
If fixing a bug, describe the bug and the conditions under which
it occurs. If adding a feature, explain the motivation.>

<Blank line>

Signed-off-by: Your Name <your.email@example.com>
```

### 6.2 Module Prefix Rules

The prefix identifies which subsystem the change belongs to. Use lowercase and be specific:

| Prefix | Scope |
|---|---|
| `bdev` | Block device layer |
| `nvme` | NVMe driver |
| `nvmf` | NVMe-oF target |
| `bdev/nvme` | NVMe bdev module specifically |
| `lib/env_dpdk` | DPDK environment library |
| `test/bdev` | Bdev tests |
| `doc` | Documentation only |
| `scripts` | Build or utility scripts |
| `ci` | CI configuration |
| `json` | JSON parsing library |

### 6.3 Good vs. Bad Commit Messages

```
# GOOD: specific module, imperative verb, explains why in body
bdev/nvme: handle timeout during controller reset

When an NVMe controller reset is in progress, I/O commands submitted
by other threads can time out before the reset completes. Previously
these timeouts triggered an additional reset attempt, leading to a
reset storm. Fix this by checking the controller state in the timeout
handler and deferring the response until the reset completes.

Signed-off-by: Your Name <your@email.com>

# BAD: vague, no module, no explanation
fix: fixed a bug in nvme code

This fixes the timeout issue.

Signed-off-by: Your Name <your@email.com>
```

### 6.4 The Developer Certificate of Origin (DCO)

Every commit must include a `Signed-off-by` line with the name and email matching your git identity.
This is the Developer Certificate of Origin (DCO) — it certifies that you wrote the code or have the
right to submit it under the project's open source license (BSD-3-Clause).

```bash
# Add automatically with every commit
git commit -s

# If you forgot, add to the last commit
git commit --amend -s

# For multiple commits
git rebase --signoff HEAD~<N>
```

---

## 7. Continuous Integration System

SPDK's CI runs on every pull request. Patches must not break CI. Understanding what CI checks helps
you catch failures locally before pushing.

### 7.1 What CI Checks

| Check | Local Equivalent |
|---|---|
| Format check | `./scripts/check_format.sh` |
| Build (GCC) | `./configure && make -j$(nproc)` |
| Build (Clang) | `CC=clang ./configure && make -j$(nproc)` |
| Unit tests | `./test/unit/unittest.sh` |
| Static analysis | `scan-build make` |
| Python style | `ruff check && mypy` |
| Shell style | `shfmt -d scripts/` |
| Autotest (functional) | `./test/autotest.sh` (requires hardware) |

### 7.2 Running CI Checks Locally

```bash
# The single most important pre-submission check
./scripts/check_format.sh

# Unit test suite (runs fast, no hardware needed)
./test/unit/unittest.sh

# Full build verification
./configure && make -j$(nproc) 2>&1 | tee build.log

# Python checks (if you modified Python code)
ruff --config python/pyproject.toml check python/
mypy --config-file python/pyproject.toml python/

# Check for common issues (undefined symbols, missing headers)
./configure --enable-werror
make -j$(nproc)
```

### 7.3 Understanding CI Failures

When CI fails, the first step is to reproduce the failure locally. Common issues:

**Format failure**: Run `./scripts/check_format.sh` locally and fix the reported lines.

**Build failure with `-Werror`**: SPDK builds with warnings as errors in CI. Fix all warnings, not
just the ones that happen to be off locally.

**Unit test failure**: Run `./test/unit/unittest.sh` and check the output carefully. Unit test
failures are always regressions — fix the production code, do not modify the test to pass.

**Python type error**: mypy type errors indicate a real issue. Add proper type annotations or fix
the logic.

---

## 8. Documentation Requirements

SPDK takes documentation seriously. Code without adequate documentation will not be accepted.

### 8.1 What Requires Documentation

- All public API functions in `include/spdk/` (Doxygen comments, see Section 4.4)
- New modules must have a `doc/<module>.md` file
- New RPCs must be documented in `doc/jsonrpc.md`
- Significant behavior changes need updates to existing documentation
- New features should include an entry in `CHANGELOG.md`

### 8.2 CHANGELOG.md Format

```markdown
## 25.01

### New Features

- **bdev**: Added `spdk_bdev_get_qd` to query current queue depth. See
  `include/spdk/bdev.h` for the API.

### Bug Fixes

- **nvmf/tcp**: Fixed connection teardown when the initiator closes the TCP
  connection before all in-flight I/Os complete.
```

### 8.3 Building the Documentation

```bash
# Install Doxygen
sudo apt-get install -y doxygen graphviz

# Build HTML documentation
make doc

# Open in browser
xdg-open doc/output/html/index.html
```

### 8.4 JSON-RPC Documentation

If your patch adds or modifies an RPC method, update `doc/jsonrpc.md` with:
- Method name and description
- Parameters (name, type, required/optional, description)
- Response fields
- Example request and response

```markdown
### bdev_example_create {#rpc_bdev_example_create}

Create an Example bdev.

#### Parameters

Name               | Optional | Type   | Description
------------------ | -------- | ------ | -----------
name               | Required | string | Bdev name
size_in_mb         | Required | number | Size in MiB

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_example_create",
  "id": 1,
  "params": {
    "name": "Example0",
    "size_in_mb": 64
  }
}
~~~
```

---

## 9. Code Review Expectations and Etiquette

### 9.1 As a Patch Author

**Before submitting**:
- Test your patch thoroughly, including edge cases
- Read through your own diff as if you were a reviewer seeing it for the first time
- Verify CI passes locally (`check_format.sh`, unit tests, build)
- Keep the patch series small and focused — one logical change per commit

**During review**:
- Respond to every review comment, even if only to acknowledge it
- Do not argue against feedback without explanation — explain your reasoning respectfully
- If you disagree, say so constructively: "I considered that approach, but it has issue X because Y"
- When you update the patch, summarize changes in the PR description or a comment
- Do not squash commits until the reviewer is satisfied with the changes (it makes re-review harder)

**What to expect**:
- Reviews can take days to weeks depending on maintainer bandwidth
- Multiple rounds of feedback are normal, especially for large or complex changes
- A "needs work" result is not a rejection — it means the maintainers see value but require changes

### 9.2 As a Reviewer

**Technical feedback**:
- Test the patch if you have hardware available
- Check for correctness, not just style
- Look for: off-by-one errors, lock ordering issues, memory leaks, missing error paths, thread safety
- If something is confusing, say so — confusing code may indicate a design issue

**Communication**:
- Be specific. "This is wrong" is not useful. "This will fail when X because Y — consider Z" is.
- Distinguish blocking issues from suggestions: use "nit:" prefix for non-blocking style suggestions
- Be respectful. The author put effort into the patch. Critique the code, not the person.
- If the patch is good, say so. Positive feedback is valuable and often overlooked.

**Example review comment patterns**:
```
# Blocking issue (must be fixed)
This can deadlock if spdk_thread_send_msg() fails and the callback
is never called. Need to handle the -ENOMEM return value.

# Non-blocking suggestion
nit: Consider renaming `tmp` to `io_channel` for clarity.

# Positive reinforcement
The batching approach here is clever — good use of the ring buffer
to amortize submission overhead.
```

### 9.3 Community Channels

- **GitHub**: Primary venue for patch review (https://github.com/spdk/spdk)
- **Mailing list**: spdk@lists.01.org — use for design discussions and announcements
- **IRC/Matrix**: #spdk on Libera.Chat — good for quick questions
- **SPDK development page**: https://spdk.io/development/ — authoritative reference for process

---

## 10. How to Become a Maintainer

Maintainers are not appointed on a fixed schedule. The path is earned through sustained, high-quality
contribution over time.

### 10.1 What the Maintainer Team Looks For

**Technical competence**: Deep understanding of at least one area of the codebase. This is demonstrated
through patches that are correct on the first or second submission, and through code review comments that
catch real issues.

**Judgment**: The ability to evaluate trade-offs — performance vs. complexity, correctness vs. simplicity.
Maintainers need to make calls on whether a patch belongs in the project.

**Communication**: Clear, professional, constructive feedback on others' patches. Maintainers set the
tone for the community.

**Reliability**: Consistent participation over a period of months or years. One burst of activity does
not make a maintainer.

**Community orientation**: Prioritizing the project's long-term health over short-term goals or
employer interests.

### 10.2 The Practical Path

1. **Start small**: Fix a bug, improve documentation, add a unit test. Every accepted patch builds
   your reputation and familiarity with the codebase.

2. **Review other patches**: Comment on open PRs even before you have merge rights. High-quality
   review comments are noticed. This also accelerates your own learning.

3. **Own a subsystem**: Become the go-to person for a specific module (bdev layer, NVMe driver,
   NVMe-oF target, etc.). Deep expertise in one area is more valuable than shallow knowledge across many.

4. **Participate in design discussions**: Engage on the mailing list and GitHub issues when architectural
   decisions are being made. Thoughtful contributions to design discussions demonstrate judgment.

5. **Consistency over time**: Aim for steady contribution across 6-12+ months rather than a burst of
   activity. The maintainer team needs to trust that you will still be engaged when your patches need
   maintenance.

6. **Be nominated**: Typically a core maintainer nominates a contributor based on observed track record.
   There is no formal application process.

---

## 11. Common Pitfalls for New Contributors

**Pitfall 1: Mixing concerns in one patch**

A patch that fixes a bug AND refactors surrounding code AND adds a new feature is hard to review and hard
to bisect later. Split into separate commits or separate PRs.

**Pitfall 2: Breaking the thread model**

SPDK's lockless model requires that all operations on a given object occur on the same `spdk_thread`.
Forgetting this causes race conditions that are hard to reproduce but serious in production.

**Pitfall 3: Allocating memory in the I/O path**

Allocating memory during I/O processing (`spdk_malloc`, `calloc`) can cause latency spikes and failures
when memory is fragmented. Pre-allocate resources during initialization.

**Pitfall 4: Missing error path cleanup**

```c
/* Wrong: leaks buf on the second allocation failure */
buf1 = spdk_malloc(size1, ...);
if (!buf1) return -ENOMEM;

buf2 = spdk_malloc(size2, ...);
if (!buf2) return -ENOMEM;  /* buf1 leaked! */

/* Correct: clean up on each failure */
buf1 = spdk_malloc(size1, ...);
if (!buf1) return -ENOMEM;

buf2 = spdk_malloc(size2, ...);
if (!buf2) {
    spdk_free(buf1);
    return -ENOMEM;
}
```

**Pitfall 5: Not updating submodules**

```bash
# When upstream changes submodule pointers, update yours
git submodule update --init --recursive
```

**Pitfall 6: Submitting without running check_format.sh**

The single most common reason for immediate CI failure. Make it a habit to run this before every push.

---

## 12. Practical Exercises

### Exercise 1: Environment Setup and First Format Run

1. Clone SPDK and initialize all submodules.
2. Run `./scripts/check_format.sh` on the clean tree. Verify it passes with no errors.
3. Deliberately introduce a formatting violation (wrong indentation) and run the script again.
   Observe the output. Revert the change.

### Exercise 2: Write and Submit a Documentation Fix

1. Find a public API function in `include/spdk/` that has incomplete or missing Doxygen comments.
2. Write a proper Doxygen comment following the format in Section 4.4.
3. Create a branch, commit with a proper message (using `doc:` or the module prefix), and open a
   draft PR on GitHub. Verify CI passes.

### Exercise 3: Add a Unit Test

1. Choose a module with incomplete unit test coverage.
   Look under `test/unit/lib/` for the corresponding test file.
2. Write one new test case covering an edge case or error path.
3. Verify the test runs and passes: `./test/unit/unittest.sh`.
4. Submit as a patch following the full workflow in Section 5.1.

---

## References

- https://spdk.io/development/ — authoritative SPDK development process guide
- https://github.com/spdk/spdk — primary repository and PR tracker
- `CONTRIBUTING.md` — in the SPDK repository root
- `GOVERNANCE.md` — maintainer list and TSC structure
- `CODE_OF_CONDUCT.md` — Contributor Covenant v2.1
- `.astylerc` — C/C++ formatting configuration
- `scripts/check_format.sh` — full format checking script
- `python/pyproject.toml` — Python style configuration
- Linux kernel coding style: https://www.kernel.org/doc/html/latest/process/coding-style.html
- Developer Certificate of Origin: https://developercertificate.org/

---

## Key Takeaways

- SPDK is governed through the Linux Foundation with a small core maintainer team and a TSC.
  Understanding who maintains the project and how decisions are made helps you contribute effectively.

- `./scripts/check_format.sh` is non-negotiable. Run it before every push. CI will fail if you do not.

- The C style uses K&R brackets, 8-space tabs (forced), 100-character line limit, and `*` adjacent to
  the variable name — enforced by astyle with the `.astylerc` configuration file.

- Every commit must carry a `Signed-off-by` trailer (DCO). Use `git commit -s` always.

- Commit messages follow `<module>: <imperative summary>` format with a body explaining what changed
  and why. The module prefix is required and must be accurate.

- Keep patches focused. One logical change per commit. Mix refactoring and feature work at your peril.

- Public API functions in `include/spdk/` require Doxygen comments. New RPCs require documentation
  in `doc/jsonrpc.md`. New features require CHANGELOG entries.

- Code review is collaborative, not adversarial. Respond to all comments, explain disagreements
  respectfully, and do not squash until the reviewer is satisfied.

- The path to maintainership is earned through sustained, high-quality patches and code reviews over
  months or years. Depth in one subsystem beats breadth across many.

- SPDK's lockless, polled-mode architecture means threading mistakes are especially costly. Understand
  the thread model before modifying the I/O path.

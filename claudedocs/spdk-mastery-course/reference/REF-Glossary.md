# SPDK Mastery Course — Glossary of Terms

> Quick-reference glossary organized alphabetically. Each entry includes a
> definition and, where applicable, related terms to aid navigation.
>
> **Conventions used below**
> - *Italics* denote an SPDK-specific concept or data structure name.
> - **Bold** cross-references point to other glossary entries.
> - Code font (`monospace`) is used for API symbols, file names, and
>   command-line strings.

---

## A

### Acceleration Framework (`accel`)
SPDK's hardware-acceleration abstraction layer that offloads compute-heavy
operations (CRC, copy, encrypt/decrypt, compress) to dedicated engines such as
Intel DSA, IDXD, or software fallbacks. Applications submit operations through
a single API regardless of the underlying engine. Related: **DIF**, **IDXD**,
**offload**.

### AIO (Asynchronous I/O)
A Linux kernel interface (`io_submit` / `io_getevents`) that allows
non-blocking disk I/O without dedicated kernel threads. SPDK's `bdev_aio`
module wraps Linux AIO to expose arbitrary kernel block devices as SPDK
**bdevs**. Related: **bdev**, **libaio**.

### Arbitration (NVMe)
A hardware scheduling mechanism within an NVMe controller that determines the
order in which commands from different submission queues are executed. NVMe
supports round-robin, weighted round-robin, and vendor-specific arbitration.
Related: **NVMe**, **submission queue**, **completion queue**.

### Asynchronous Callback
The primary I/O completion model in SPDK. Instead of blocking until an
operation finishes, the caller provides a function pointer (`cb_fn`) and a
context pointer (`cb_arg`). SPDK invokes the callback on the same
**reactor** thread when the operation completes. Related: **poller**,
**reactor**, **run-to-completion**.

---

## B

### bdev (Block Device)
The central SPDK abstraction for a storage device. `spdk_bdev` is a C
structure representing a logical block storage endpoint. Any backend — NVMe
SSD, RAM disk, Ceph RBD, iSCSI LUN, logical volume — is exposed through the
same `bdev.h` API, enabling a fully pluggable **block device layer**.
Related: **bdev module**, **bdev channel**, **lvol**, **NVMe bdev**.

### bdev Channel (`spdk_bdev_desc` / `spdk_io_channel`)
A per-thread handle to a **bdev** used to submit I/O without locks. Each
**reactor** thread opens its own channel, which maps to a dedicated hardware
queue or software queue, eliminating cross-thread contention in the I/O path.
Related: **IO channel**, **reactor**, **queue pair**.

### bdev Module
A plugin that implements the `spdk_bdev_fn_table` interface to register one
or more **bdevs** with the bdev layer. Examples include `bdev_nvme`,
`bdev_malloc`, `bdev_aio`, `bdev_rbd`, `bdev_virtio`. A special subclass
called a **vbdev** (virtual bdev) sits on top of existing bdevs (e.g., `lvol`,
`gpt`, `crypto`). Related: **bdev**, **vbdev**.

### Blob
An ordered list of **clusters** managed by **Blobstore**. Blobs are the
fundamental named storage object in Blobstore — analogous to a file in a
filesystem but deliberately not POSIX-compliant. Each blob can store
user-defined key/value metadata (xattrs) and persists across power failures.
Related: **Blobstore**, **cluster**, **page (Blobstore)**, **lvol**.

### Blobstore
A persistent, power-fail-safe block allocator built on top of a **bdev**.
It organizes the device into **logical blocks** → **pages** → **clusters** →
**blobs** and maintains its own metadata region. Blobstore is the storage
engine underlying SPDK **logical volumes** (`lvol`). It is intentionally not
a general-purpose filesystem. Related: **blob**, **lvol**, **cluster**,
**page (Blobstore)**.

### Block Device Layer
The SPDK library (`libspdk_bdev`) that provides a unified, pluggable interface
to all storage backends. Equivalent in function to the kernel block layer but
implemented entirely in userspace. Provides queueing, timeout handling, reset
handling, and stacking of devices. Related: **bdev**, **bdev module**.

### Block Size (Logical / Physical)
The minimum addressable unit of a storage device. NVMe SSDs commonly report a
512-byte or 4096-byte (4K) **Logical Block Address (LBA)** size. The physical
(optimal) block size may differ, affecting alignment and performance. Related:
**LBA**, **sector**, **DIF**.

---

## C

### Callback (see Asynchronous Callback)

### Channel (see IO Channel)

### Cluster (Blobstore)
A fixed-size, contiguous group of **pages** within a **Blobstore**. Clusters
are the allocation unit for **blobs**; a blob grows by adding whole clusters.
The default cluster size is 1 MiB (256 × 4 KiB pages). Related: **blob**,
**page (Blobstore)**, **Blobstore**.

### Completion Queue (CQ)
An NVMe data structure in host memory into which the NVMe controller places
command completion entries (CCEs). The host polls the CQ (or receives an
interrupt) to learn which commands have finished. SPDK always polls CQs without
interrupts. Related: **submission queue**, **polled-mode driver**, **NVMe**.

### Controller (NVMe)
The NVMe entity that manages one or more **namespaces** and exposes them to
the host via **queue pairs**. In SPDK, `spdk_nvme_ctrlr` represents a
discovered NVMe controller. Multiple controllers can be attached concurrently
from a single process. Related: **namespace**, **queue pair**, **NVMe**.

### Copy-on-Write (CoW)
A data-management strategy where writes to a shared region are redirected to
a new location, leaving the original data intact. SPDK **lvol snapshots** use
CoW semantics — the snapshot retains the original cluster, and new writes go to
a new cluster in the clone. Related: **lvol**, **snapshot**, **blob**.

### CPU Core Affinity
The binding of a **reactor** thread to a specific physical CPU core. SPDK
assigns one reactor per core using DPDK's **EAL** CPU mask (`-c` or
`--lcores` options), ensuring cache locality and avoiding OS scheduler
interference. Related: **reactor**, **EAL**, **DPDK**.

---

## D

### Device Passthrough (VFIO / UIO)
A mechanism that allows SPDK's userspace drivers to access PCIe devices
directly, bypassing kernel drivers. **VFIO** (Virtual Function I/O) is the
modern, IOMMU-aware method; **UIO** (Userspace I/O) is the legacy method.
Both enable DMA-safe, direct hardware access from userspace. Related: **VFIO**,
**UIO**, **DPDK**, **userspace driver**.

### DIF (Data Integrity Field)
A 8-byte protection field appended to each 512-byte or 4 KiB block, defined by
the T10 standard. Contains a Guard (CRC-16), Application Tag, and Reference
Tag. SPDK's `dif.h` provides software DIF generation and verification; some
NVMe drives support hardware DIF end-to-end. Related: **DIX**, **accel**,
**block size**.

### DIX (Data Integrity Extension)
An extension of **DIF** where the protection information is stored in a
separate buffer rather than interleaved with data. Used in some storage
protocols for efficient integrity checking. Related: **DIF**.

### DMA (Direct Memory Access)
A hardware capability allowing a device (e.g., NVMe SSD, RDMA NIC) to read
from or write to host memory without CPU involvement. SPDK requires all I/O
buffers to be DMA-safe, meaning they must be allocated from **hugepage**-backed
memory that is physically contiguous and pinned. Related: **hugepage**,
**IOVA**, **zero-copy**.

### DPDK (Data Plane Development Kit)
An open-source framework of libraries and poll-mode drivers for fast packet and
storage processing. SPDK uses DPDK primarily for its **EAL** (environment
abstraction layer), memory management (`rte_malloc`, **hugepages**), lockless
rings, and PCIe device enumeration. Related: **EAL**, **hugepage**, **PMD**,
**reactor**.

### DSA (Data Streaming Accelerator)
Intel's on-die hardware accelerator for memory copy, CRC, and other data
movement operations. SPDK's **acceleration framework** can offload work to DSA
via the IDXD driver. Related: **accel**, **IDXD**, **offload**.

---

## E

### EAL (Environment Abstraction Layer)
The DPDK component that initializes the system at startup: allocates
**hugepages**, sets **CPU core affinity**, registers PCIe devices with
**VFIO/UIO**, and establishes the lockless ring infrastructure. SPDK
applications initialize EAL via `spdk_env_dpdk_post_init()` or
`spdk_app_start()`. Related: **DPDK**, **hugepage**, **reactor**, **VFIO**.

### Event
In SPDK's event framework (`event.h`), an event is a bundled function pointer
plus arguments destined for a specific CPU core. Events are the primary
mechanism for cross-thread communication. A sender allocates an event with
`spdk_event_allocate()` and dispatches it with `spdk_event_call()`. Related:
**reactor**, **message passing**, **poller**.

### Event Framework (`spdk_app`)
The high-level SPDK application framework that spawns one **reactor** thread
per configured CPU core, connects them with lockless queues, and runs the
polled-mode event loop. Activated by calling `spdk_app_start()`. Applications
that do not use the event framework can integrate SPDK's lower-level
`spdk_thread` primitives directly. Related: **reactor**, **poller**, **event**,
**spdk_thread**.

---

## F

### Fabric (NVMe-oF)
The physical or logical transport network over which NVMe-oF commands travel.
Supported fabrics include **RDMA** (RoCE, iWARP), **TCP**, **FC** (Fibre
Channel), and **in-process** (for testing). Related: **NVMe-oF**, **RDMA**,
**TCP transport**, **transport**.

### Fabrics Command
A special NVMe-oF command set used to establish and manage the connection
between initiator and target (e.g., Connect, Property Get/Set,
Authentication Send/Receive). Defined in the NVMe-oF specification. Related:
**NVMe-oF**, **subsystem**, **NQN**.

### Flash Translation Layer (FTL)
SPDK's software FTL (`ftl.h`) implements the mapping between logical block
addresses (host-visible) and physical locations on a next-generation storage
device (e.g., ZNS SSD). It handles garbage collection and wear leveling in
software, providing a conventional block interface. Related: **ZNS**, **LBA**,
**bdev**.

### FC (Fibre Channel)
A high-speed network technology traditionally used for SAN storage. NVMe-oF
over Fibre Channel (FC-NVMe) is supported by SPDK's `nvmf` target and uses the
Fibre Channel fabric as the transport. Related: **NVMe-oF**, **transport**,
**SAN**.

### Firmware Slot (NVMe)
A storage location within an NVMe device that holds a firmware image. NVMe
defines up to 7 firmware slots; SPDK exposes firmware download and commit
commands through `spdk_nvme_ctrlr_download_fw()`. Related: **NVMe**,
**controller**.

---

## G

### GPT (GUID Partition Table)
A partition table format used on storage devices. SPDK provides a `bdev_gpt`
vbdev module that reads a GPT partition table from a bdev and exposes each
partition as a child bdev. Related: **vbdev**, **bdev**.

### Guard (DIF)
The CRC-16 checksum field within a **DIF** protection tuple. It provides
end-to-end data integrity verification from the host buffer through to the
storage medium. Related: **DIF**, **CRC**.

---

## H

### Hugepage
A large memory page (typically 2 MiB or 1 GiB on x86) allocated at DPDK
startup and kept resident (non-swappable) for the lifetime of the application.
Hugepages are required for **DMA** buffers because they provide physically
contiguous, pinned memory visible to hardware. SPDK's memory allocator
(`spdk_malloc`, `spdk_zmalloc`) draws from hugepage pools. Related: **DMA**,
**EAL**, **DPDK**, **IOVA**.

### Host NQN
The **NQN** that uniquely identifies an NVMe-oF host (initiator). Host NQNs
may be used by an NVMe-oF target to implement access control (allowed hosts
list) on a **subsystem**. Related: **NQN**, **NVMe-oF**, **subsystem**.

---

## I

### IDXD (Intel Data Accelerator Driver)
The SPDK driver for Intel's on-die DSA and IAA (In-Memory Analytics
Accelerator) engines. Applications use the SPDK **accel** API, which routes
work to IDXD hardware when available, falling back to software. Related:
**accel**, **DSA**, **offload**.

### IOMMU (Input-Output Memory Management Unit)
Hardware that translates device virtual addresses (IOVAs) to physical memory
addresses. The IOMMU enables SPDK's **VFIO** passthrough model — devices can
DMA to arbitrary guest memory safely, and the OS kernel maintains isolation.
Related: **VFIO**, **DMA**, **IOVA**.

### IOPS (Input/Output Operations Per Second)
The standard metric for storage throughput in terms of operation count rather
than bandwidth. SPDK's polled-mode, lockless design is specifically optimized
to maximize IOPS at low latency. Related: **latency**, **queue depth**,
**polled-mode driver**.

### IOVA (I/O Virtual Address)
The address a DMA-capable device uses to access host memory. With **VFIO** and
an **IOMMU**, IOVAs are translated by hardware to physical addresses, providing
isolation. SPDK's memory layer tracks IOVA mappings for registered buffers.
Related: **IOMMU**, **DMA**, **hugepage**, **VFIO**.

### iSCSI (Internet Small Computer Systems Interface)
A storage protocol that encapsulates SCSI commands inside TCP/IP packets. SPDK
provides a high-performance iSCSI target (`iscsi_tgt`) that exposes **bdevs**
over standard TCP networks. Linux initiators (`open-iscsi`) interoperate with
the SPDK target. Related: **target**, **initiator**, **TCP**, **SCSI**.

### Initiator
A client in a storage network that sends I/O requests to a **target**. In
NVMe-oF terminology, the initiator is also called the host. The Linux kernel
includes NVMe-oF initiator drivers (`nvme-rdma`, `nvme-tcp`) that work with
SPDK targets. Related: **target**, **NVMe-oF**, **host NQN**.

### Interrupt Mode vs. Polled Mode
Two models for receiving notifications from hardware. In interrupt mode, the
CPU is interrupted when I/O completes; latency is non-deterministic. In SPDK's
**polled mode**, a thread continuously reads completion queues with no
interrupts, achieving lower and more consistent latency at the cost of
dedicated CPU time. Related: **polled-mode driver**, **poller**, **reactor**.

### IO Channel (`spdk_io_channel`)
A per-thread, per-device context object that provides lockless access to a
device's resources (e.g., an NVMe queue pair). Created by calling
`spdk_get_io_channel()` on an `spdk_io_device`. Each **reactor** thread
maintains its own channel, eliminating the need for locks in the I/O path.
Related: **IO device**, **bdev channel**, **reactor**, **queue pair**.

### IO Device (`spdk_io_device`)
Any object registered with SPDK's thread library that needs to maintain
per-thread state. The pattern emerges repeatedly: a device has global state and
per-thread **IO channels** to avoid locking. NVMe controllers, bdevs, and
NVMe-oF transports are all IO devices. Related: **IO channel**, **reactor**.

---

## J

### JSON-RPC
The remote procedure call protocol SPDK uses for runtime configuration and
management. All SPDK subsystems expose management operations as JSON-RPC 2.0
methods. The reference client is `scripts/rpc.py`. Related: **RPC**, **rpc.py**.

---

## L

### Latency
The time elapsed from I/O submission to completion notification. SPDK's
polled-mode, lockless, zero-copy design minimizes both average latency and
tail latency (99th, 99.9th percentile). Related: **IOPS**, **polled-mode
driver**, **queue depth**, **tail latency**.

### LBA (Logical Block Address)
The zero-based index identifying a specific logical block on a storage device.
NVMe commands specify a starting LBA and a transfer length. Related: **block
size**, **namespace**, **NVMe**.

### Lockless Ring
A circular, multi-producer / multi-consumer (MPMC) data structure from DPDK
(`rte_ring`) used for inter-thread communication without spinlocks. SPDK uses
lockless rings as the transport layer for **events** and **messages** between
**reactors**. Related: **message passing**, **reactor**, **DPDK**, **event**.

### Logical Volume (`lvol`)
An SPDK block device built on top of a **Blobstore**. Logical volumes support
thin provisioning, snapshots, and clones. Each lvol is backed by one or more
**blobs** within a **Blobstore**. The `lvol` module exposes lvols as
**bdevs**. Related: **Blobstore**, **blob**, **snapshot**, **bdev**.

### Logical Volume Store (`lvs`)
A **Blobstore** instance that has been prepared for use by the `lvol` module.
An lvs is created on a base **bdev** and then used to create **lvols**. Related:
**lvol**, **Blobstore**, **bdev**.

### LUN (Logical Unit Number)
A SCSI addressing concept identifying an addressable storage device (logical
unit) on a SCSI target. In SPDK's iSCSI target, each LUN maps to one
**bdev**. Related: **iSCSI**, **SCSI**, **bdev**.

---

## M

### malloc bdev
A **bdev** backed by DRAM (hugepage memory). Used for testing and benchmarking
because it provides extremely high throughput with no physical device required.
Created with `bdev_malloc_create`. Related: **bdev**, **hugepage**.

### Memory Registration
The act of notifying an RDMA NIC or IOMMU of a memory region's physical
mapping so that hardware can DMA to/from it. SPDK's memory layer
(`spdk_mem_map`) tracks all registered regions; buffers allocated from the
hugepage pool are automatically registered. Related: **DMA**, **RDMA**,
**hugepage**, **IOVA**.

### Message Passing
SPDK's primary concurrency model. Instead of protecting shared data with
mutexes, each piece of data is owned by a single **reactor** thread. Other
threads communicate by posting function-pointer messages (via
`spdk_thread_send_msg()`) to the owning thread's lockless queue. Related:
**reactor**, **lockless ring**, **event**, **shared-nothing**.

### Metadata (NVMe)
Additional bytes that can be attached to each logical block by an NVMe device.
Used for **DIF/DIX** protection information or application-specific data. SPDK
exposes metadata operations in `spdk_nvme_ns_cmd_read_with_md()` and similar
functions. Related: **DIF**, **LBA**, **namespace**.

---

## N

### Namespace (NVMe)
A quantity of non-volatile memory that can be formatted and attached to one or
more NVMe controllers. Each namespace is addressed independently by its
Namespace ID (NSID). SPDK represents namespaces with `spdk_nvme_ns`. A single
NVMe controller can expose many namespaces. Related: **controller**, **NSID**,
**NVMe**.

### NBD (Network Block Device)
A Linux kernel mechanism that exposes a block device over a socket. SPDK's
`nbd` module bridges an SPDK **bdev** to a Linux `/dev/nbdX` device, enabling
use of SPDK storage from kernel-space tools (e.g., `mkfs`, `mount`). Related:
**bdev**, **kernel bypass**.

### NQN (NVMe Qualified Name)
A unique string identifier for an NVMe-oF **subsystem** or host. NQNs follow
either the format `nqn.yyyy-mm.reverse.domain:suffix` or the UUID format
`nqn.2014-08.org.nvmexpress:uuid:...`. NQNs are used during the Connect
Fabrics command to route the connection to the correct subsystem. Related:
**NVMe-oF**, **subsystem**, **host NQN**.

### NSID (Namespace ID)
A 32-bit integer (starting at 1) that uniquely identifies a **namespace**
within an NVMe controller. NSID 0xFFFFFFFF is the broadcast NSID used in some
admin commands. Related: **namespace**, **NVMe**.

### NVMe (Non-Volatile Memory Express)
The PCIe-based host controller interface specification for accessing solid-state
storage. NVMe replaces AHCI with a deeply queued, parallel command set (up to
65,535 queue pairs, each with up to 65,535 commands). SPDK's NVMe driver
(`libspdk_nvme`) is the foundational userspace component. Related: **queue
pair**, **controller**, **namespace**, **NVMe-oF**.

### NVMe-oF (NVMe over Fabrics)
An extension of the NVMe protocol that transports NVMe commands over a network
fabric (RDMA, TCP, FC) rather than a local PCIe bus. SPDK provides both an
NVMe-oF target (`nvmf_tgt`) and an NVMe-oF initiator (**bdev_nvme** with
remote transport). Related: **NVMe**, **fabric**, **transport**, **subsystem**,
**NQN**.

### NVMe-oF Target
An SPDK application or subsystem that accepts NVMe-oF connections from
remote hosts and serves **bdevs** as namespaces. Configured via JSON-RPC: create
a transport, create a subsystem, add listeners, add namespaces. Related:
**subsystem**, **transport**, **namespace**, **NVMe-oF**.

---

## O

### Offload
Delegating a compute or data-movement operation to dedicated hardware
(accelerator, smart NIC, or co-processor) rather than the host CPU. SPDK's
**accel** framework provides a uniform API for offloading CRC, copy,
compression, and encryption. Related: **accel**, **DSA**, **IDXD**.

### OPC (Opcode)
The 8-bit command opcode field in an NVMe command. Admin commands and I/O
commands each have their own opcode space (e.g., 0x00 = Flush, 0x01 = Write,
0x02 = Read for the NVM command set). Related: **NVMe**, **SQE**.

---

## P

### Page (Blobstore)
A fixed-size group of **logical blocks**, the unit of atomicity in
**Blobstore**. A page is typically 4 KiB (1 or 8 logical blocks). Reads and
writes within Blobstore are aligned to pages. Related: **blob**, **cluster**,
**Blobstore**.

### PCIe (Peripheral Component Interconnect Express)
The high-bandwidth serial bus that physically connects NVMe SSDs to the host.
SPDK's userspace driver accesses PCIe BARs (Base Address Registers) via
**VFIO** or **UIO** memory mappings, bypassing the kernel entirely. Related:
**NVMe**, **VFIO**, **UIO**, **EAL**.

### PMD (Poll-Mode Driver)
A DPDK driver that uses CPU polling rather than interrupts to process
I/O completions. SPDK's NVMe driver is conceptually a PMD for storage. The term
is more commonly used for DPDK network PMDs (e.g., `mlx5`, `i40e`). Related:
**polled-mode driver**, **DPDK**, **interrupt mode**.

### Poller (`spdk_poller`)
An SPDK function registered on a specific **reactor** thread that is called
repeatedly every iteration of the event loop (or on a configured timer
interval). Pollers replace interrupts: a poller checks hardware completion
queues, processes network buffers, or performs periodic maintenance tasks.
Registered with `spdk_poller_register()`. Related: **reactor**, **event
framework**, **polled-mode driver**.

### Polled-Mode Driver
A device driver that continuously queries (polls) hardware status registers or
completion queues rather than sleeping and waiting for an interrupt. SPDK's
entire I/O stack is polled-mode, which eliminates interrupt latency and
provides predictable, low tail latency. Related: **poller**, **interrupt mode**,
**latency**, **NVMe**.

### PRD (Physical Region Descriptor)
A data structure (used in older AHCI) that describes a scatter-gather list
entry. NVMe uses **PRP** and **SGL** instead. Related: **PRP**, **SGL**.

### PRP (Physical Region Page)
An NVMe mechanism for specifying I/O buffer locations in host memory using up
to two physical page addresses (or a PRP list for larger transfers). PRP
entries must be 4 KiB-page-aligned. For transfers larger than two pages, a PRP
list stored in memory is used. Related: **SGL**, **DMA**, **NVMe**.

### PRCHK (Protection Information Checking)
Flags in NVMe commands that tell the controller which **DIF** fields to check
or generate (Guard, Application Tag, Reference Tag). Related: **DIF**,
**metadata**, **NVMe**.

---

## Q

### Queue Depth
The number of I/O operations outstanding (in flight) at any given moment. Higher
queue depth typically increases throughput on SSDs but may increase average
latency. SPDK applications can saturate NVMe queues (up to 65,535 entries per
queue) due to their asynchronous design. Related: **submission queue**,
**queue pair**, **IOPS**, **latency**.

### Queue Pair (QP)
An NVMe I/O pair consisting of one **submission queue** (SQ) and one
**completion queue** (CQ). SPDK allocates one queue pair per **reactor** thread
per NVMe controller to provide lock-free I/O. NVMe-oF connections also use
queue pairs. Related: **submission queue**, **completion queue**, **IO channel**,
**reactor**.

---

## R

### RDMA (Remote Direct Memory Access)
A network technology that allows one computer to directly read or write
another's memory without involving the remote CPU. Used as an NVMe-oF fabric
(via RoCE or iWARP). SPDK's NVMe-oF RDMA transport delivers the lowest
latency of all supported fabrics. Related: **RoCE**, **iWARP**, **NVMe-oF**,
**transport**.

### RBD (RADOS Block Device)
Ceph's distributed block storage primitive. SPDK's `bdev_rbd` module exposes
Ceph RBD images as SPDK **bdevs** via `librbd` and `librados`. Related:
**bdev**, **Ceph**.

### Reactor (`spdk_reactor`)
A system thread pinned to a CPU core that runs SPDK's event loop. The reactor
continuously polls its **lockless ring** for incoming **events**, calls
registered **pollers**, and drives I/O completion callbacks. One reactor per
core, no OS scheduling interference. Related: **event framework**, **poller**,
**event**, **CPU core affinity**, **spdk_thread**.

### RoCE (RDMA over Converged Ethernet)
An Ethernet-based **RDMA** protocol. RoCE v2 routes RDMA traffic over standard
IP/UDP, making it suitable for datacenter networks. SPDK's NVMe-oF RDMA
transport supports RoCE via the `rdma-core` / `libibverbs` stack. Related:
**RDMA**, **NVMe-oF**, **iWARP**, **transport**.

### rpc.py
A Python command-line tool (`scripts/rpc.py`) that sends JSON-RPC commands to
a running SPDK application over a Unix domain socket. The primary interface for
dynamic configuration of bdevs, NVMe-oF targets, iSCSI targets, and more.
Related: **JSON-RPC**.

### Run-to-Completion
A threading model in which a single thread drives an I/O operation from
submission to completion without yielding the CPU. SPDK uses run-to-completion
within each **reactor**: the reactor submits I/O, polls for completions, and
invokes callbacks — all on the same core without context switches. Related:
**reactor**, **polled-mode driver**, **shared-nothing**.

---

## S

### SAN (Storage Area Network)
A dedicated network for block storage access. SPDK can serve as a high-
performance SAN target via **NVMe-oF**, **iSCSI**, or **vhost**. Related:
**NVMe-oF**, **iSCSI**, **target**.

### SCSI (Small Computer System Interface)
A legacy command set for storage devices. SPDK implements a SCSI layer for
its **iSCSI** target and **vhost-SCSI** target, translating SCSI commands to
native **bdev** operations. Related: **iSCSI**, **vhost**, **LUN**.

### Sector
An alternate term for a **logical block**, often used in the context of HDDs
and older SCSI-based storage. In modern NVMe usage, "block" or "LBA" is
preferred. Related: **LBA**, **block size**.

### SGL (Scatter-Gather List)
An NVMe data transfer descriptor that points to non-contiguous memory regions.
SGLs provide more flexibility than **PRP** lists and are required for
**NVMe-oF** data transfers. Each SGL element specifies an address and length.
Related: **PRP**, **DMA**, **NVMe**, **NVMe-oF**.

### Shared-Nothing Architecture
A design principle where each processing unit (reactor) owns its data and never
shares mutable state with other units. Communication happens only via explicit
**message passing**. SPDK's entire architecture is shared-nothing, which
eliminates lock contention and allows linear scaling with core count. Related:
**reactor**, **message passing**, **run-to-completion**.

### Snapshot (lvol)
A point-in-time, read-only copy of an **lvol**. Implemented via
**copy-on-write** at the **cluster** level in **Blobstore**. New writes to
the original volume go to new clusters; the snapshot retains the original
data clusters. Related: **lvol**, **clone**, **Blobstore**, **copy-on-write**.

### spdk_app
The SPDK application framework entry point. Calling `spdk_app_start()` parses
command-line arguments, initializes EAL and DPDK, spawns **reactors**, and
invokes the user-supplied `start_fn`. Calling `spdk_app_stop()` initiates
graceful shutdown. Related: **event framework**, **reactor**, **EAL**.

### spdk_thread
SPDK's lightweight, stackless thread abstraction. An `spdk_thread` is executed
by calling `spdk_thread_poll()` from a system thread. It provides the
messaging, **poller**, and **IO channel** infrastructure. Multiple `spdk_thread`
instances can be multiplexed onto a single OS thread. Related: **reactor**,
**poller**, **IO channel**, **event**.

### SQ (Submission Queue) — see Submission Queue

### Submission Queue (SQ)
A ring buffer in host memory into which the driver places NVMe commands
(SQEs). The driver rings a doorbell register to notify the controller that new
entries are available. SPDK's NVMe driver manages one SQ per queue pair.
Related: **completion queue**, **queue pair**, **NVMe**, **doorbell**.

### Subsystem (NVMe-oF)
A named grouping on an NVMe-oF **target** that aggregates one or more
**namespaces** and is identified by a unique **NQN**. Hosts connect to a
subsystem by NQN. Two types exist: NVM subsystems (expose block namespaces) and
Discovery subsystems (used for service discovery). Related: **NQN**, **NVMe-oF
target**, **namespace**, **transport**.

---

## T

### Tail Latency
The latency at high percentiles (99th, 99.9th percentile), often called P99 or
P99.9. SPDK's polled-mode design eliminates interrupt jitter and OS scheduling
delays, reducing tail latency compared to kernel storage stacks. Related:
**latency**, **polled-mode driver**, **jitter**.

### Target
A server in a storage network that receives and services I/O requests from an
**initiator**. SPDK provides high-performance targets for **NVMe-oF**,
**iSCSI**, and **vhost**. Related: **initiator**, **NVMe-oF target**,
**iSCSI**, **vhost**.

### TCP Transport (NVMe-oF)
An NVMe-oF fabric that runs over standard TCP/IP networks. Easier to deploy
than **RDMA** (no special hardware required) at the cost of slightly higher CPU
utilization. SPDK's NVMe-oF TCP transport (`nvmf_tcp`) supports both target
and initiator roles. Related: **NVMe-oF**, **transport**, **RDMA**, **RoCE**.

### Thin Provisioning
Allocating storage space on demand rather than pre-allocating all space upfront.
SPDK **lvols** support thin provisioning — disk space is not consumed until
data is actually written to a cluster. Related: **lvol**, **Blobstore**,
**cluster**.

### Transport (NVMe-oF)
The network layer over which NVMe-oF commands and data flow. SPDK implements
pluggable transports: `RDMA`, `TCP`, `FC`, and `PCIE` (in-process). A transport
is created with `nvmf_create_transport` and bound to a specific network address
via a listener. Related: **NVMe-oF**, **RDMA**, **TCP transport**, **fabric**.

### Transport ID (trid)
A structure (`spdk_nvme_transport_id`) that fully identifies a remote NVMe
device: transport type (RDMA, TCP, PCIe), address family, address, port, and
NQN. Used when connecting an NVMe-oF initiator or attaching a bdev. Related:
**NVMe-oF**, **NQN**, **transport**.

---

## U

### UIO (Userspace I/O)
A Linux kernel subsystem that maps PCIe device registers and interrupt
information to userspace via `/dev/uioX`. A simpler but less secure alternative
to **VFIO** for PCIe passthrough. SPDK supports UIO (`uio_pci_generic`,
`igb_uio`) but **VFIO** is preferred for production. Related: **VFIO**,
**DPDK**, **PCIe**, **userspace driver**.

### Unmap / Deallocate
An NVMe command (`Dataset Management` with Deallocate bit) instructing the
device to release physical storage for specified LBA ranges. Equivalent to SCSI
UNMAP or ATA TRIM. SPDK bdevs and the NVMe-oF target propagate unmap commands
to underlying devices for space reclamation. Related: **NVMe**, **LBA**,
**thin provisioning**.

### Userspace Driver
A device driver that runs in user-mode rather than the OS kernel. SPDK's NVMe
driver is a userspace driver: it maps PCIe BARs into the process address space,
programs the hardware directly, and never invokes kernel syscalls in the I/O
path. This eliminates kernel–user context switches and enables zero-copy I/O.
Related: **kernel bypass**, **VFIO**, **UIO**, **zero-copy**, **polled-mode
driver**.

---

## V

### vbdev (Virtual Block Device)
A **bdev module** that sits on top of one or more existing **bdevs** and
transforms I/O (e.g., encryption, compression, RAID, partitioning, logical
volumes). Examples: `bdev_crypto`, `bdev_compress`, `bdev_lvol`, `bdev_gpt`.
Related: **bdev**, **bdev module**, **lvol**.

### VFIO (Virtual Function I/O)
A Linux kernel framework that safely exposes PCIe devices to userspace
processes via the **IOMMU**. SPDK (via DPDK's EAL) unbinds NVMe devices from
kernel drivers and binds them to the `vfio-pci` driver, enabling safe, direct
hardware access. VFIO is the recommended method for SPDK deployments. Related:
**UIO**, **IOMMU**, **PCIe**, **EAL**, **DPDK**.

### virtio
A paravirtualized device standard used in virtual machines. SPDK provides a
`bdev_virtio` initiator that connects to virtio-SCSI or virtio-blk devices
inside a VM (e.g., QEMU), as well as **vhost** targets that serve SPDK bdevs
to VMs via the virtio protocol. Related: **vhost**, **QEMU**, **bdev**.

### vhost
A protocol enabling a userspace process to service virtio devices on behalf of
a hypervisor. SPDK's vhost target (`vhost`) receives virtio-blk and
virtio-SCSI requests from QEMU VMs via a Unix socket, processes them against
SPDK **bdevs**, and returns completions — all without kernel involvement.
Related: **virtio**, **QEMU**, **bdev**, **target**.

### vhost-blk
A vhost target variant that presents a single block device to a VM using the
`virtio-blk` protocol. Simpler than **vhost-SCSI** but does not support SCSI
commands. Related: **vhost**, **virtio**, **bdev**.

### vhost-SCSI
A vhost target variant that presents SCSI devices to a VM using the
`virtio-SCSI` protocol. Supports multiple LUNs per target and SCSI-level
commands (e.g., INQUIRY, UNMAP). Related: **vhost**, **SCSI**, **LUN**.

---

## W

### Write Atomicity (NVMe)
A guarantee that a write of one or more logical blocks either completes fully
or not at all, with no partial-write visible after a power failure. NVMe devices
report their atomic write unit (AWUN) in the Identify Namespace data structure.
**Blobstore** relies on the device's page-level atomicity guarantee. Related:
**NVMe**, **Blobstore**, **page (Blobstore)**.

### Write-Through / Write-Back Cache
NVMe devices may have volatile write caches. "Write-through" flushes each write
to stable media before acknowledging; "write-back" acknowledges from cache. SPDK
can issue `Flush` commands to force cache writeback, and Blobstore uses this
for data integrity. Related: **NVMe**, **Blobstore**, **write atomicity**.

---

## X

### xattr (Extended Attribute)
A user-defined key/value pair stored in **Blobstore** metadata alongside a
**blob**. Applications can use xattrs to store blob-level metadata (e.g.,
volume name, creation time, logical size) that persists across reboots. Related:
**blob**, **Blobstore**.

---

## Z

### Zero-Copy I/O
An I/O model where data travels directly between application buffers and device
DMA buffers without any intermediate CPU-driven copies. SPDK achieves zero-copy
by requiring applications to use DMA-safe (**hugepage**) buffers and passing
their physical addresses directly to NVMe commands via **PRP** or **SGL**
descriptors. Related: **DMA**, **hugepage**, **PRP**, **SGL**, **userspace
driver**.

### ZNS (Zoned Namespace)
An NVMe command set extension (NVMe-ZNS) that divides a namespace into fixed-
size zones with sequential-write-only semantics. SPDK supports ZNS via
`spdk_nvme_zns_*` APIs and the **FTL** module. ZNS aligns the host's write
pattern with the device's internal physical structure, reducing write
amplification. Related: **FTL**, **namespace**, **NVMe**, **bdev_zone**.

---

## Quick-Reference Index

| Term | Category | Key Header / Source |
|------|----------|---------------------|
| bdev | Storage abstraction | `include/spdk/bdev.h` |
| blob | Storage abstraction | `include/spdk/blob.h` |
| Blobstore | Storage engine | `include/spdk/blob.h` |
| DIF | Data integrity | `include/spdk/dif.h` |
| DMA | Memory/hardware | `include/spdk/env.h` |
| DPDK / EAL | Platform | `include/spdk/env_dpdk.h` |
| Event framework | Concurrency | `include/spdk/event.h` |
| hugepage | Memory | `include/spdk/env.h` |
| IO channel | Concurrency | `include/spdk/thread.h` |
| iSCSI target | Protocol | `lib/iscsi/` |
| JSON-RPC | Management | `include/spdk/jsonrpc.h` |
| lvol | Storage abstraction | `include/spdk/lvol.h` |
| NQN | Protocol | `include/spdk/nvmf_spec.h` |
| NVMe controller | Hardware | `include/spdk/nvme.h` |
| NVMe-oF target | Protocol | `include/spdk/nvmf.h` |
| namespace | Hardware | `include/spdk/nvme.h` |
| poller | Concurrency | `include/spdk/thread.h` |
| queue pair | Hardware | `include/spdk/nvme.h` |
| RDMA transport | Protocol | `lib/nvmf/rdma.c` |
| reactor | Concurrency | `include/spdk/event.h` |
| SGL | Data transfer | `include/spdk/nvme_spec.h` |
| spdk_thread | Concurrency | `include/spdk/thread.h` |
| subsystem | Protocol | `include/spdk/nvmf.h` |
| TCP transport | Protocol | `lib/nvmf/tcp.c` |
| transport | Protocol | `include/spdk/nvmf.h` |
| VFIO | Platform | kernel `vfio-pci` / EAL |
| vhost | Protocol | `lib/vhost/` |
| virtio | Protocol | `lib/virtio/` |
| ZNS | Hardware | `include/spdk/nvme_zns.h` |
| zero-copy | Architecture | `include/spdk/env.h` |

---

*Last updated: SPDK Mastery Course reference materials.*

# SPDK 설정 레퍼런스

**과정**: SPDK 마스터리
**문서 유형**: 레퍼런스
**범위**: 애플리케이션 시작 옵션, JSON 설정, RPC 메서드, 환경 변수, 디바이스 바인딩

---

## 목차

1. [애플리케이션 명령줄 옵션](#1-application-command-line-options)
2. [JSON 설정 파일 형식](#2-json-configuration-file-format)
3. [RPC 메서드: 블록 디바이스 (bdev)](#3-rpc-methods-block-devices-bdev)
4. [RPC 메서드: NVMe-oF 타겟](#4-rpc-methods-nvme-of-target)
5. [RPC 메서드: iSCSI 타겟](#5-rpc-methods-iscsi-target)
6. [RPC 메서드: Vhost](#6-rpc-methods-vhost)
7. [환경 변수](#7-environment-variables)
8. [휴즈페이지 설정](#8-hugepage-configuration)
9. [디바이스 바인딩: setup.sh](#9-device-binding-setupsh)
10. [RPC 소켓 및 CLI 레퍼런스](#10-rpc-socket-and-cli-reference)

---

## 1. 애플리케이션 명령줄 옵션

SPDK 애플리케이션은 `struct spdk_app_opts`(`include/spdk/event.h`에 정의)에서 파생된 공통 옵션 세트를 공유합니다. 이 옵션들은 DPDK 환경, CPU 할당, 메모리, PCI 접근, 트레이싱을 제어합니다.

### 1.1 핵심 프레임워크 옵션

| 약칭 | 전체 | 타입 | 기본값 | 설명 |
|-------|------|------|---------|-------------|
| `-c` | `--config` | string | none | Path to JSON configuration file. Mutually exclusive with `--json`. |
| `-d` | `--limit-coredump` | flag | false | Do not change core dump size limit. |
| `-e` | `--tpoint-group-mask` | hex string | none | Tracepoint group mask (hex). Enables specific subsystem tracepoints. |
| `-g` | `--single-file-segments` | flag | false | Allocate hugepages in single-file segments. |
| `-h` | `--help` | flag | — | Print usage information and exit. |
| `-i` | `--shm-id` | integer | -1 | Shared memory ID. Multiple instances with the same ID share memory. |
| `-m` | `--cpumask` | hex mask | `0x1` | CPU core mask for SPDK reactors. Example: `0xf` uses cores 0-3. |
| `-n` | `--mem-channels` | integer | auto | Number of DPDK memory channels. |
| `-p` | `--main-core` | integer | first set | Main core number (must be in cpumask). |
| `-r` | `--rpc-socket` | string | `/var/tmp/spdk.sock` | UNIX domain socket path (or IP:port) for RPC. |
| `-s` | `--mem-size` | integer (MB) | DPDK default | Total hugepage memory to allocate in MB. |
| `-u` | `--no-pci` | flag | false | Disable PCI bus scanning entirely. |
| `-w` | `--wait-for-rpc` | flag | false | Pause subsystem initialization; wait for `framework_start_init` RPC. |
| `-A` | `--iova-mode` | string | auto | IOVA mode: `pa` (physical), `va` (virtual). |
| `-B` | `--pci-blocked` | BDF | none | Block a specific PCI device. Repeatable. |
| `-R` | `--enable-unreliable-msg` | flag | false | Allow unreliable messaging (experimental). |
| `-S` | `--interrupt-mode` | flag | false | Run in interrupt mode instead of busy-polling. |
| `-T` | `--lcore-map` | string | none | Explicit lcore-to-CPU mapping passed to DPDK. |
| `-W` | `--pci-allowed` | BDF | none | Explicitly allow a PCI device. Repeatable. |
| `-x` | `--disable-cpumask-locks` | flag | false | Disable per-core CPU lock files. |
| `—` | `--huge-dir` | string | system default | Path to a mounted hugetlbfs filesystem. |
| `—` | `--no-huge` | flag | false | Run without hugepages (testing only, not for production). |
| `—` | `--num-trace-entries` | integer | 32768 | Number of trace entries per core. |
| `—` | `--enforce-numa` | flag | false | Abort if memory cannot be allocated on required NUMA node. |

### 1.2 CPU 마스크 예시

```bash
# Single core (core 0)
-m 0x1

# Four cores (cores 0-3)
-m 0xf

# Eight cores (cores 0-7)
-m 0xff

# Cores 0, 2, 4, 6 (alternating)
-m 0x55

# All 16 cores
-m 0xffff
```

### 1.3 PCI 디바이스 필터링

```bash
# Allow only two specific NVMe devices
--pci-allowed 0000:01:00.0 --pci-allowed 0000:02:00.0

# Block one device, allow all others
--pci-blocked 0000:03:00.0

# No PCI access at all (software-only workloads)
--no-pci
```

### 1.4 메모리 설정

```bash
# Allocate 4 GB of hugepage memory
-s 4096

# Use a custom hugepage mount point
--huge-dir /mnt/huge_2mb

# Use single-file hugepage segments (reduces /proc/mounts noise)
--single-file-segments
```

### 1.5 지연 초기화

`--wait-for-rpc` 플래그(또는 `-w`)는 명시적 RPC 호출이 전송될 때까지 서브시스템이 시작되는 것을 방지합니다. 이를 통해 서브시스템이 리소스를 할당하기 전에 설정을 로드할 수 있습니다.

```bash
# Start application in deferred mode
spdk_tgt --wait-for-rpc &

# Configure via RPC while subsystems wait
rpc.py bdev_malloc_create 64 512 -b Malloc0
rpc.py nvmf_create_transport -t TCP

# Release the hold and start all subsystems
rpc.py framework_start_init
```

---

## 2. JSON 설정 파일 형식

SPDK는 시작 시 JSON 파일에서 초기 설정을 로드하는 것을 지원합니다(`-c` / `--config`). 파일은 초기화 중에 실행될 RPC 메서드 호출 시퀀스를 인코딩합니다.

### 2.1 파일 구조

JSON 설정 파일은 RPC 메서드 객체의 배열입니다. 각 객체에는 RPC를 명명하는 `method` 필드와 인수를 포함하는 `params` 객체가 있습니다.

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
            "num_blocks": 131072,
            "block_size": 512
          }
        },
        {
          "method": "bdev_nvme_attach_controller",
          "params": {
            "name": "Nvme0",
            "trtype": "PCIe",
            "traddr": "0000:01:00.0"
          }
        }
      ]
    },
    {
      "subsystem": "nvmf",
      "config": [
        {
          "method": "nvmf_create_transport",
          "params": {
            "trtype": "TCP"
          }
        },
        {
          "method": "nvmf_create_subsystem",
          "params": {
            "nqn": "nqn.2023-01.io.spdk:cnode1",
            "allow_any_host": true,
            "serial_number": "SPDK00000000000001",
            "model_number": "SPDK Controller"
          }
        },
        {
          "method": "nvmf_subsystem_add_ns",
          "params": {
            "nqn": "nqn.2023-01.io.spdk:cnode1",
            "namespace": {
              "bdev_name": "Malloc0",
              "nsid": 1
            }
          }
        },
        {
          "method": "nvmf_subsystem_add_listener",
          "params": {
            "nqn": "nqn.2023-01.io.spdk:cnode1",
            "listen_address": {
              "trtype": "TCP",
              "adrfam": "IPv4",
              "traddr": "0.0.0.0",
              "trsvcid": "4420"
            }
          }
        }
      ]
    }
  ]
}
```

### 2.2 서브시스템 순서

`subsystems` 배열은 순서대로 처리됩니다. 의존성이 있는 서브시스템은 의존하는 서브시스템 뒤에 나타나야 합니다. 일반적인 순서:

| Order | Subsystem | Notes |
|-------|-----------|-------|
| 1 | `accel` | Acceleration framework; no dependencies |
| 2 | `bdev` | Block devices; depends on accel for some operations |
| 3 | `nvmf` | NVMe-oF target; depends on bdev |
| 4 | `iscsi` | iSCSI target; depends on bdev |
| 5 | `vhost` | Vhost target; depends on bdev |

### 2.3 실행 중인 인스턴스에서 JSON 설정 생성

```bash
# Dump the current running configuration to a JSON file
rpc.py save_config > /tmp/spdk_config.json

# Load it on next startup
spdk_tgt -c /tmp/spdk_config.json
```

### 2.4 JSON 스키마

A JSON Schema for the configuration file is located at:

```
$SPDK_DIR/schema.json
```

Use it to validate configuration files before deployment:

```bash
# With Python jsonschema
python3 -m jsonschema -i my_config.json schema.json
```

---

## 3. RPC 메서드: 블록 디바이스 (bdev)

모든 bdev RPC 호출은 `rpc.py` 또는 모든 JSON-RPC 2.0 클라이언트를 통해 실행 중인 SPDK 애플리케이션에 보낼 수 있습니다. 기본 소켓 경로는 `/var/tmp/spdk.sock`입니다.

### 3.1 일반 bdev 옵션

```bash
# Set global bdev subsystem options (call before creating bdevs)
rpc.py bdev_set_options \
  --bdev-io-pool-size 65536 \
  --bdev-io-cache-size 256 \
  --small-buf-pool-size 8192 \
  --large-buf-pool-size 1024
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `bdev-io-pool-size` | 65536 | Number of bdev I/O structures in the pool |
| `bdev-io-cache-size` | 256 | Per-thread cache size for bdev I/O structures |
| `small-buf-pool-size` | 8192 | Pool size for small (≤8 KB) I/O buffers |
| `large-buf-pool-size` | 1024 | Pool size for large (>8 KB) I/O buffers |

### 3.2 Malloc Bdev

휴즈페이지 메모리로 지원되는 인메모리 블록 디바이스입니다. 테스트 및 개발에 사용됩니다.

```bash
# Create a 64 MB malloc bdev with 512-byte blocks
rpc.py bdev_malloc_create 64 512 -b Malloc0

# Create with 4 KB blocks and metadata
rpc.py bdev_malloc_create 128 4096 -b Malloc1 -m 64 -i

# Delete
rpc.py bdev_malloc_delete Malloc0
```

| Parameter | Required | Description |
|-----------|----------|-------------|
| `total_size` | yes | Size in MB (float > 0) |
| `block_size` | yes | Data block size in bytes |
| `-b NAME` | no | Bdev name (auto-generated if omitted) |
| `-u UUID` | no | Assign a specific UUID |
| `-p PHYS_BS` | no | Physical block size |
| `-m MD_SIZE` | no | Metadata size per block (0, 8, 16, 32, 64, 128) |
| `-i` | no | Interleave metadata with data |
| `-n NUMA_ID` | no | NUMA node for memory allocation (-1 = any) |

### 3.3 Null Bdev

모든 쓰기를 폐기하고 읽기에 대해 0을 반환하는 무작업 블록 디바이스입니다. 오버헤드 없는 성능 벤치마킹에 유용합니다.

```bash
# Create a 1 GB null bdev with 4 KB blocks
rpc.py bdev_null_create Null0 1024 4096

# Resize an existing null bdev
rpc.py bdev_null_resize Null0 2048

# Delete
rpc.py bdev_null_delete Null0
```

### 3.4 NVMe Bdev (PCIe)

Attach a local NVMe device by PCIe BDF address.

```bash
# Attach NVMe device at BDF 0000:01:00.0
rpc.py bdev_nvme_attach_controller \
  -b Nvme0 \
  -t PCIe \
  -a 0000:01:00.0

# Results in bdevs named Nvme0n1, Nvme0n2, etc. (one per namespace)
```

### 3.5 NVMe Bdev (Fabrics / 원격)

Attach a remote NVMe-oF target as a local bdev.

```bash
# Attach via TCP
rpc.py bdev_nvme_attach_controller \
  -b RemoteNvme0 \
  -t TCP \
  -f IPv4 \
  -a 192.168.1.100 \
  -s 4420 \
  -n nqn.2023-01.io.spdk:target1

# Attach via RDMA
rpc.py bdev_nvme_attach_controller \
  -b RemoteNvme1 \
  -t RDMA \
  -f IB \
  -a 192.168.2.10 \
  -s 4420 \
  -n nqn.2023-01.io.spdk:target1

# Detach
rpc.py bdev_nvme_detach_controller RemoteNvme0
```

| Parameter | Required | Description |
|-----------|----------|-------------|
| `-b NAME` | yes | Controller name prefix (bdevs: NAMEn1, NAMEn2, …) |
| `-t TRTYPE` | yes | Transport: `PCIe`, `TCP`, `RDMA`, `FC` |
| `-a TRADDR` | yes | Address: BDF for PCIe, IP for TCP/RDMA |
| `-f ADRFAM` | no | Address family: `IPv4`, `IPv6`, `IB`, `FC` |
| `-s TRSVCID` | no | Service ID (port number for TCP/RDMA) |
| `-n SUBNQN` | no | Target subsystem NQN |
| `-x MULTIPATH` | no | Multipath mode: `disable`, `failover`, `multipath` |
| `-l TIMEOUT` | no | Controller loss timeout in seconds (-1 = infinite retry) |
| `-o DELAY` | no | Reconnect delay in seconds |
| `-e` | no | Enable TCP header digest |
| `-d` | no | Enable TCP data digest |

### 3.6 AIO Bdev

Expose a file or block device via Linux AIO (kernel I/O path).

```bash
# Wrap a file
rpc.py bdev_aio_create /tmp/spdk_disk.img AioFile0 512

# Wrap a raw block device
rpc.py bdev_aio_create /dev/sdb AioDisk0 4096

# Delete
rpc.py bdev_aio_delete AioFile0
```

### 3.7 io_uring Bdev

Similar to AIO but using Linux io_uring for potentially lower latency.

```bash
rpc.py bdev_uring_create /dev/nvme0n1 Uring0 4096
rpc.py bdev_uring_delete Uring0
```

### 3.8 Crypto Bdev

Wrap any bdev with transparent encryption/decryption.

```bash
# Create an encryption key
rpc.py accel_crypto_key_create -c AES_CBC -k "0123456789abcdef0123456789abcdef" -n MyKey

# Create a crypto bdev layered on top of Malloc0
rpc.py bdev_crypto_create Malloc0 CryptoBdev0 -n MyKey

# Delete
rpc.py bdev_crypto_delete CryptoBdev0
```

### 3.9 오류 주입 Bdev

Inject I/O errors on top of an existing bdev for fault testing.

```bash
# Create the error bdev wrapper
rpc.py bdev_error_create Malloc0

# Inject a read error (1 error per 10 I/Os)
rpc.py bdev_error_inject_error EE_Malloc0 read -t 1 -e 10

# Delete
rpc.py bdev_error_delete EE_Malloc0
```

### 3.10 QoS 속도 제한

```bash
# Limit Malloc0 to 100K IOPS and 500 MB/s read bandwidth
rpc.py bdev_set_qos_limit Malloc0 \
  --rw-ios-per-sec 100000 \
  --r-mbytes-per-sec 500
```

| Limit Parameter | Unit | Description |
|----------------|------|-------------|
| `--rw-ios-per-sec` | IOPS | Total read+write IOPS limit |
| `--r-mbytes-per-sec` | MB/s | Read throughput limit |
| `--w-mbytes-per-sec` | MB/s | Write throughput limit |
| `--rw-mbytes-per-sec` | MB/s | Combined read+write throughput limit |

### 3.11 Bdev 검색 및 상태

```bash
# List all registered bdevs
rpc.py bdev_get_bdevs

# Show I/O statistics for all bdevs
rpc.py bdev_get_iostat

# Show stats for a specific bdev
rpc.py bdev_get_iostat -b Malloc0

# Reset I/O statistics
rpc.py bdev_reset_iostat

# Trigger bdev examination (useful after hotplug)
rpc.py bdev_examine Nvme0n1

# Wait for all bdev examinations to complete
rpc.py bdev_wait_for_examine
```

---

## 4. RPC 메서드: NVMe-oF 타겟

NVMe-oF 타겟은 TCP, RDMA 또는 FC 전송을 통해 로컬 bdev를 원격 이니시에이터에 NVMe 네임스페이스로 노출합니다.

### 4.1 전송 생성

Create a transport listener. One transport per type is typical.

```bash
# TCP transport (most common)
rpc.py nvmf_create_transport -t TCP

# TCP transport with tuning
rpc.py nvmf_create_transport \
  -t TCP \
  -q 128 \
  -m 8 \
  -c 8192 \
  -i 131072 \
  -u 131072 \
  -n 2048 \
  -b 32

# RDMA transport
rpc.py nvmf_create_transport \
  -t RDMA \
  -q 128 \
  -m 16 \
  -s 512
```

| Parameter | Short | Default | Description |
|-----------|-------|---------|-------------|
| `-t TRTYPE` | — | required | Transport type: `TCP`, `RDMA`, `FC`, `VFIOUSER` |
| `-q MAX_QUEUE_DEPTH` | — | 128 | Max outstanding I/O per queue pair |
| `-m MAX_IO_QPAIRS_PER_CTRLR` | — | 128 | Max I/O queue pairs per controller |
| `-c IN_CAPSULE_DATA_SIZE` | — | 4096 | Max in-capsule data size (bytes) |
| `-i MAX_IO_SIZE` | — | 131072 | Max I/O size (bytes) |
| `-u IO_UNIT_SIZE` | — | 131072 | I/O unit size (bytes) |
| `-a MAX_AQ_DEPTH` | — | 32 | Max admin queue depth |
| `-n NUM_SHARED_BUFFERS` | — | 4096 | Transport buffer pool size |
| `-b BUF_CACHE_SIZE` | — | 32 | Per-poll-group buffer cache size |
| `-z` | — | false | Enable zero-copy I/O (if bdev supports it) |
| `-s MAX_SRQ_DEPTH` | — | 0 | RDMA only: Shared Receive Queue depth |
| `-r` | — | false | RDMA only: Disable SRQ |
| `-o` | — | false | TCP only: Disable C2H success optimization |
| `-f` | — | false | TCP only: Enable DIF insert/strip |
| `--kas KAS` | — | 0 | Keep-alive support interval |

### 4.2 서브시스템 생성

서브시스템은 고유한 NQN을 가진 하나의 NVMe-oF 타겟에 대응합니다.

```bash
# Minimal subsystem (any host allowed)
rpc.py nvmf_create_subsystem \
  nqn.2023-01.io.spdk:cnode1 \
  -a \
  -s SPDK00000000000001 \
  -d "SPDK NVMe Controller"

# Restricted subsystem (host NQN whitelist required)
rpc.py nvmf_create_subsystem \
  nqn.2023-01.io.spdk:cnode2 \
  -s SPDK00000000000002

# Discovery subsystem (usually created automatically)
rpc.py nvmf_create_subsystem nqn.2014-08.org.nvmexpress.discovery -a
```

| Parameter | Required | Description |
|-----------|----------|-------------|
| `nqn` | yes | Subsystem NQN string |
| `-s SERIAL_NUMBER` | no | Serial number string |
| `-d MODEL_NUMBER` | no | Model number string |
| `-a` | no | Allow any host to connect |
| `-m MAX_NAMESPACES` | no | Maximum namespace count |
| `-r` | no | Enable ANA (Asymmetric Namespace Access) reporting |
| `-p` | no | Enable NVMe passthrough for all I/O |
| `-n` | no | Enable NVMe Subsystem Reset (NSSR) support |

### 4.3 네임스페이스 추가

```bash
# Add Malloc0 as namespace 1 in the subsystem
rpc.py nvmf_subsystem_add_ns \
  nqn.2023-01.io.spdk:cnode1 \
  Malloc0 \
  -n 1

# Add with explicit UUID
rpc.py nvmf_subsystem_add_ns \
  nqn.2023-01.io.spdk:cnode1 \
  Nvme0n1 \
  -n 2 \
  -u 12345678-1234-1234-1234-123456789abc

# Remove namespace 1
rpc.py nvmf_subsystem_remove_ns nqn.2023-01.io.spdk:cnode1 1
```

| Parameter | Required | Description |
|-----------|----------|-------------|
| `nqn` | yes | Subsystem NQN |
| `bdev_name` | yes | Name of the bdev to back this namespace |
| `-n NSID` | no | Namespace ID (auto-assigned if omitted) |
| `-u UUID` | no | Namespace UUID |
| `-g NGUID` | no | Namespace globally unique identifier |
| `-e EUI64` | no | Namespace EUI-64 identifier |
| `-a ANAGRPID` | no | ANA group ID |
| `-i` | no | Do not auto-make namespace visible to controllers |

### 4.4 리스너 추가

```bash
# TCP listener on all interfaces, port 4420
rpc.py nvmf_subsystem_add_listener \
  nqn.2023-01.io.spdk:cnode1 \
  -t TCP \
  -f IPv4 \
  -a 0.0.0.0 \
  -s 4420

# TCP listener on a specific interface
rpc.py nvmf_subsystem_add_listener \
  nqn.2023-01.io.spdk:cnode1 \
  -t TCP \
  -f IPv4 \
  -a 192.168.1.10 \
  -s 4420

# RDMA listener
rpc.py nvmf_subsystem_add_listener \
  nqn.2023-01.io.spdk:cnode1 \
  -t RDMA \
  -f IB \
  -a 192.168.2.10 \
  -s 4420

# Remove listener
rpc.py nvmf_subsystem_remove_listener \
  nqn.2023-01.io.spdk:cnode1 \
  -t TCP -f IPv4 -a 0.0.0.0 -s 4420
```

| Parameter | Required | Description |
|-----------|----------|-------------|
| `nqn` | yes | Subsystem NQN (`discovery` is a shortcut for the discovery NQN) |
| `-t TRTYPE` | yes | Transport type |
| `-a TRADDR` | yes | Transport address |
| `-f ADRFAM` | no | Address family: `IPv4`, `IPv6`, `IB`, `FC`, `intra_host` |
| `-s TRSVCID` | no | Service ID / port (required for TCP and RDMA) |
| `-k` | no | Establish secure channel (TLS) immediately |
| `-n ANA_STATE` | no | ANA state: `optimized`, `non_optimized`, `inaccessible` |

### 4.5 호스트 접근 제어

```bash
# Allow a specific host NQN
rpc.py nvmf_subsystem_add_host \
  nqn.2023-01.io.spdk:cnode1 \
  nqn.2021-06.io.spdk:host1

# Remove host
rpc.py nvmf_subsystem_remove_host \
  nqn.2023-01.io.spdk:cnode1 \
  nqn.2021-06.io.spdk:host1

# Switch to allow-any mode
rpc.py nvmf_subsystem_allow_any_host \
  nqn.2023-01.io.spdk:cnode1 \
  -e true
```

### 4.6 NVMe-oF 상태 및 통계

```bash
# List all transports
rpc.py nvmf_get_transports

# List all subsystems
rpc.py nvmf_get_subsystems

# Get NVMe-oF target statistics
rpc.py nvmf_get_stats

# List controllers connected to a subsystem
rpc.py nvmf_subsystem_get_controllers nqn.2023-01.io.spdk:cnode1

# List queue pairs
rpc.py nvmf_subsystem_get_qpairs nqn.2023-01.io.spdk:cnode1

# List listeners
rpc.py nvmf_subsystem_get_listeners nqn.2023-01.io.spdk:cnode1
```

### 4.7 완전한 NVMe-oF 설정 예시

```bash
# 1. Create bdev backend
rpc.py bdev_malloc_create 512 4096 -b Malloc0

# 2. Create TCP transport
rpc.py nvmf_create_transport -t TCP -q 128 -n 4096

# 3. Create subsystem
rpc.py nvmf_create_subsystem nqn.2023-01.io.spdk:cnode1 \
  -a -s SPDK00000000000001 -d "SPDK Controller"

# 4. Add namespace
rpc.py nvmf_subsystem_add_ns nqn.2023-01.io.spdk:cnode1 Malloc0 -n 1

# 5. Add listener
rpc.py nvmf_subsystem_add_listener nqn.2023-01.io.spdk:cnode1 \
  -t TCP -f IPv4 -a 0.0.0.0 -s 4420
```

---

## 5. RPC 메서드: iSCSI 타겟

iSCSI 타겟은 표준 iSCSI 이니시에이터를 통해 접근 가능한 SCSI LUN으로 bdev를 노출합니다.

### 5.1 전역 iSCSI 옵션

Set before creating any iSCSI resources:

```bash
rpc.py iscsi_set_options \
  --auth-file /etc/spdk/auth.conf \
  --node-base "iqn.2023-01.io.spdk" \
  --nop-timeout 60 \
  --nop-in-interval 30 \
  --max-connections-per-session 0 \
  --max-queue-depth 128 \
  --immediate-data yes \
  --error-recovery-level 0
```

| Option | Default | Description |
|--------|---------|-------------|
| `--node-base` | `iqn.2016-06.io.spdk` | Base IQN for target nodes |
| `--auth-file` | none | Path to CHAP auth file |
| `--nop-timeout` | 60 | NOP-Out timeout in seconds |
| `--nop-in-interval` | 30 | NOP-In send interval in seconds |
| `--max-connections-per-session` | 0 | 0 = unlimited |
| `--max-queue-depth` | 64 | Max outstanding commands per session |
| `--immediate-data` | yes | Enable immediate data |
| `--error-recovery-level` | 0 | iSCSI error recovery level (0-2) |

### 5.2 포털 그룹 생성

포털 그룹은 타겟이 수신하는 IP:포트 쌍을 정의합니다.

```bash
# Create portal group 1 listening on all interfaces, port 3260
rpc.py iscsi_create_portal_group 1 "0.0.0.0:3260"

# Multiple portals in the same group
rpc.py iscsi_create_portal_group 1 "192.168.1.10:3260 192.168.1.11:3260"

# Start accepting connections
rpc.py iscsi_start_portal_group 1
```

### 5.3 이니시에이터 그룹 생성

이니시에이터 그룹은 연결이 허용되는 이니시에이터 호스트를 정의합니다.

```bash
# Allow all initiators (wildcard)
rpc.py iscsi_create_initiator_group 1 "ANY" "ANY"

# Allow specific initiator IQN and subnet
rpc.py iscsi_create_initiator_group 2 \
  "iqn.2021-01.com.example:initiator1" \
  "192.168.1.0/24"

# Add more initiators to existing group
rpc.py iscsi_initiator_group_add_initiators 1 \
  --initiators "iqn.2021-01.com.example:initiator2" \
  --netmasks "192.168.2.0/24"
```

### 5.4 타겟 노드 생성

```bash
# Create a target node with one LUN
rpc.py iscsi_create_target_node \
  "target1" \
  "SPDK iSCSI Target" \
  "Malloc0:0" \
  "1:1" \
  64 \
  -d

# Parameters: alias, target_name, lun_mapping, pg:ig_mapping, queue_depth, [-d disable_chap]
```

Full parameter reference:

| Parameter | Description |
|-----------|-------------|
| `name` | Target name (appended to node-base IQN) |
| `alias_name` | Human-readable alias |
| `luns` | Bdev:LUN pairs, e.g., `Malloc0:0 Malloc1:1` |
| `pg_ig_maps` | Portal group : initiator group mappings, e.g., `1:1` |
| `queue_depth` | Max outstanding tasks per connection |
| `-d` | Disable CHAP authentication |
| `-m` | Require mutual CHAP |
| `-H` | Require header digest |
| `-D` | Require data digest |

```bash
# Add an additional LUN to an existing target node
rpc.py iscsi_target_node_add_lun \
  "iqn.2016-06.io.spdk:target1" \
  Malloc1 \
  -l 1

# Delete target node
rpc.py iscsi_delete_target_node "iqn.2016-06.io.spdk:target1"
```

### 5.5 iSCSI 상태 명령

```bash
# List all target nodes
rpc.py iscsi_get_target_nodes

# List portal groups
rpc.py iscsi_get_portal_groups

# List initiator groups
rpc.py iscsi_get_initiator_groups

# List active connections
rpc.py iscsi_get_connections

# Show global options
rpc.py iscsi_get_options

# Show connection statistics
rpc.py iscsi_get_stats
```

### 5.6 완전한 iSCSI 설정 예시

```bash
# 1. Create bdev
rpc.py bdev_malloc_create 256 512 -b iSCSIBdev0

# 2. Set global options
rpc.py iscsi_set_options --node-base "iqn.2023-01.io.spdk"

# 3. Create portal group
rpc.py iscsi_create_portal_group 1 "0.0.0.0:3260"
rpc.py iscsi_start_portal_group 1

# 4. Create initiator group (allow all)
rpc.py iscsi_create_initiator_group 1 "ANY" "ANY"

# 5. Create target node
rpc.py iscsi_create_target_node \
  "disk1" "SPDK Disk 1" \
  "iSCSIBdev0:0" "1:1" \
  64 -d
```

---

## 6. RPC 메서드: Vhost

Vhost 타겟은 Unix 도메인 소켓을 통해 QEMU 가상 머신에 bdev를 virtio-blk 또는 virtio-scsi 디바이스로 노출합니다.

### 6.1 Vhost 블록 컨트롤러

Exposes a single bdev as a virtio-blk device.

```bash
# Create vhost-blk controller backed by Malloc0
rpc.py vhost_create_blk_controller \
  /tmp/vhost.0 \
  Malloc0

# Create read-only vhost-blk controller
rpc.py vhost_create_blk_controller \
  /tmp/vhost_ro.0 \
  Nvme0n1 \
  -r

# Create with CPU mask restriction
rpc.py vhost_create_blk_controller \
  /tmp/vhost.0 \
  Malloc0 \
  --cpumask 0x2

# Create with packed ring support (virtio 1.1)
rpc.py vhost_create_blk_controller \
  /tmp/vhost.0 \
  Malloc0 \
  -p
```

| Parameter | Required | Description |
|-----------|----------|-------------|
| `ctrlr` | yes | Socket path for the vhost controller |
| `dev_name` | yes | Bdev name to back this controller |
| `--cpumask` | no | CPU affinity mask for this controller |
| `-r` | no | Make controller read-only |
| `-p` | no | Enable packed ring (virtio 1.1) |
| `--transport` | no | Transport name (default: `vhost_user_blk`) |

### 6.2 Vhost SCSI 컨트롤러

Exposes multiple bdevs as SCSI targets within a single virtio-scsi device.

```bash
# Create vhost-scsi controller
rpc.py vhost_create_scsi_controller /tmp/vhost_scsi.0

# Add a bdev as SCSI target 0
rpc.py vhost_scsi_controller_add_target \
  /tmp/vhost_scsi.0 \
  0 \
  Malloc0

# Add another bdev as SCSI target 1
rpc.py vhost_scsi_controller_add_target \
  /tmp/vhost_scsi.0 \
  1 \
  Malloc1

# Start the controller (make it available to VMs)
rpc.py vhost_start_scsi_controller /tmp/vhost_scsi.0

# Remove a SCSI target
rpc.py vhost_scsi_controller_remove_target \
  /tmp/vhost_scsi.0 \
  0
```

### 6.3 인터럽트 병합

```bash
# Set coalescing on a vhost controller
rpc.py vhost_controller_set_coalescing \
  /tmp/vhost.0 \
  --delay-base-us 100 \
  --iops-threshold 100000
```

| Parameter | Description |
|-----------|-------------|
| `--delay-base-us` | Base coalescing delay in microseconds |
| `--iops-threshold` | IOPS threshold above which coalescing activates |

### 6.4 Vhost 상태

```bash
# List all vhost controllers
rpc.py vhost_get_controllers

# Show a specific controller
rpc.py vhost_get_controllers -n /tmp/vhost.0
```

### 6.5 Vhost 컨트롤러를 사용하기 위한 QEMU 명령

```bash
# Use vhost-blk controller in QEMU
qemu-system-x86_64 \
  -object memory-backend-file,id=mem,size=2G,mem-path=/dev/hugepages,share=on \
  -numa node,memdev=mem \
  -chardev socket,id=chr0,path=/tmp/vhost.0 \
  -device vhost-user-blk-pci,chardev=chr0,num-queues=1
```

### 6.6 컨트롤러 삭제

```bash
rpc.py vhost_delete_controller /tmp/vhost.0
rpc.py vhost_delete_controller /tmp/vhost_scsi.0
```

---

## 7. 환경 변수

### 7.1 setup.sh 변수

These variables control the behavior of `scripts/setup.sh`, which binds devices to VFIO/UIO and allocates hugepages.

| 변수 | 기본값 | 설명 |
|----------|---------|-------------|
| `HUGEMEM` | `2048` | Total hugepage memory to allocate in MB. |
| `NRHUGE` | derived from `HUGEMEM` | Number of hugepages to allocate. Overrides `HUGEMEM` when set. |
| `HUGENODE` | none | Comma-separated list of NUMA nodes for hugepage allocation. Example: `0,1` for nodes 0 and 1. Node-specific counts: `0:2048,1:512` allocates 2048 pages on node 0 and 512 on node 1. |
| `PCI_ALLOWED` | empty (all) | Whitespace-separated list of BDF addresses to allow. Setting this restricts binding to only the listed devices. Use `none` to deny all PCI devices. |
| `PCI_BLOCKED` | empty | Whitespace-separated list of BDF addresses to skip. Takes precedence over `PCI_ALLOWED`. |
| `DRIVER_OVERRIDE` | auto | Force a specific driver instead of automatic vfio-pci/uio_pci_generic selection. Example: `uio_pci_generic`. Use `none` to unbind without rebinding. |
| `TARGET_USER` | `$SUDO_USER` | User that will own the hugepage mountpoint and VFIO groups. Enables running SPDK as non-root. |
| `SHRINK_HUGE` | no | Set to `yes` to allow reducing an existing hugepage allocation. By default, the script skips allocation if sufficient pages already exist. |
| `ALLOW_NVME_BEHIND_VMD` | unset | When set, allows NVMe devices behind a VMD controller to be bound individually. |
| `FREEBSD_BUFSZ` | `256` | FreeBSD only. Buffer size in MB for contigmem driver calculation. |

### 7.2 사용 예시

```bash
# Allocate 8 GB of hugepages, allow only two NVMe devices
sudo HUGEMEM=8192 \
     PCI_ALLOWED="0000:01:00.0 0000:02:00.0" \
     ./scripts/setup.sh

# Allocate hugepages on specific NUMA nodes
sudo HUGENODE=0:4096,1:4096 ./scripts/setup.sh

# Use uio_pci_generic instead of vfio-pci
sudo DRIVER_OVERRIDE=uio_pci_generic ./scripts/setup.sh

# Allow non-root user to run SPDK
sudo TARGET_USER=myuser ./scripts/setup.sh

# Unbind all devices without rebinding (useful before rebooting)
sudo DRIVER_OVERRIDE=none ./scripts/setup.sh reset

# Block a specific device, allow all others
sudo PCI_BLOCKED="0000:03:00.0" ./scripts/setup.sh
```

### 7.3 DPDK 환경 변수

These variables are recognized by the DPDK EAL and influence how SPDK/DPDK initializes:

| Variable | Description |
|----------|-------------|
| `DPDK_TEST` | Internal DPDK test mode flag. Not used in production. |
| `RTE_EAL_PARAM` | Additional EAL parameters appended to command-line. |

---

## 8. 휴즈페이지 설정

SPDK는 모든 DMA 가능 메모리 할당에 Linux 휴즈페이지를 의존합니다. 적절한 휴즈페이지 설정은 성능과 안정성에 중요합니다.

### 8.1 휴즈페이지 크기

Linux supports two common hugepage sizes:

| Size | sysfs Path | Typical Use |
|------|-----------|-------------|
| 2 MB | `/sys/kernel/mm/hugepages/hugepages-2048kB/` | Default; most workloads |
| 1 GB | `/sys/kernel/mm/hugepages/hugepages-1048576kB/` | Reduces TLB pressure for large memory pools |

SPDK의 `setup.sh`는 시스템 기본 휴즈페이지 크기를 사용합니다. 1 GB 페이지를 사용하려면 부팅 시 커널 파라미터로 설정하십시오.

### 8.2 수동 휴즈페이지 할당

```bash
# Allocate 4096 × 2 MB = 8 GB
echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# NUMA-aware allocation (node 0 and node 1)
echo 2048 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
echo 2048 > /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages

# Mount hugetlbfs
mkdir -p /mnt/huge
mount -t hugetlbfs nodev /mnt/huge

# Persist across reboots (add to /etc/fstab)
echo "nodev /mnt/huge hugetlbfs defaults 0 0" >> /etc/fstab
```

### 8.3 1 GB 휴즈페이지 (부트 설정)

Add to the kernel command line in `/etc/default/grub`:

```
GRUB_CMDLINE_LINUX="default_hugepagesz=1G hugepagesz=1G hugepages=8"
```

Then update grub and reboot:

```bash
sudo update-grub  # or grub2-mkconfig on RHEL/CentOS
sudo reboot
```

### 8.4 휴즈페이지 할당 검증

```bash
# Check current allocation
cat /proc/meminfo | grep -i huge

# Expected output:
# AnonHugePages:         0 kB
# ShmemHugePages:        0 kB
# FileHugePages:         0 kB
# HugePages_Total:    4096    <- allocated
# HugePages_Free:     4096    <- available
# HugePages_Rsvd:        0
# HugePages_Surp:        0
# Hugepagesize:       2048 kB
# Hugetlb:         8388608 kB

# Check per-NUMA-node allocation
cat /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
```

### 8.5 메모리 잠금 제한

Non-root users running SPDK need sufficient `memlock` limits:

```bash
# Check current limits for a user
su myuser -c "ulimit -l"

# Set permanent limit in /etc/security/limits.conf
echo "myuser hard memlock unlimited" >> /etc/security/limits.conf
echo "myuser soft memlock unlimited" >> /etc/security/limits.conf
```

### 8.6 휴즈페이지 메모리 레이아웃

SPDK는 필요한 휴즈페이지를 다음과 같이 계산합니다:

```
Required pages = ceil(HUGEMEM (MB) / hugepage_size_MB)

Example (2 MB pages, HUGEMEM=4096):
  4096 MB / 2 MB = 2048 hugepages
```

`--mem-size` / `-s` 애플리케이션 플래그는 이미 할당된 휴즈페이지 풀에서 메모리를 요청합니다. 총 할당된 휴즈페이지 메모리보다 작거나 같아야 합니다.

---

## 9. 디바이스 바인딩: setup.sh

`scripts/setup.sh` is the primary tool for preparing the system to run SPDK. It binds NVMe, IOAT, VMD, and Virtio devices to VFIO or UIO kernel drivers so that SPDK can access them directly.

### 9.1 작동 모드

```bash
# Default mode: allocate hugepages and bind all compatible PCI devices
sudo ./scripts/setup.sh

# Same as above (explicit)
sudo ./scripts/setup.sh config

# Rebind devices back to their original kernel drivers
sudo ./scripts/setup.sh reset

# Show status of all SPDK-compatible devices
sudo ./scripts/setup.sh status

# Remove leftover SPDK files (hugepage mappings, lock files)
sudo ./scripts/setup.sh cleanup

# Interactive mode (prompts for each device)
sudo ./scripts/setup.sh interactive
```

### 9.2 드라이버 선택

The script automatically selects between `vfio-pci` and `uio_pci_generic`:

| Condition | Selected Driver |
|-----------|----------------|
| IOMMU enabled and `vfio-pci` module loaded | `vfio-pci` (recommended) |
| IOMMU not enabled or VFIO unavailable | `uio_pci_generic` |
| `DRIVER_OVERRIDE` set | Uses specified driver |

```bash
# Check if IOMMU is enabled
dmesg | grep -E "IOMMU|iommu"

# Load VFIO modules
modprobe vfio-pci
modprobe vfio_iommu_type1

# Enable IOMMU in grub (for Intel)
# Add: intel_iommu=on iommu=pt
# For AMD: amd_iommu=on iommu=pt
```

### 9.3 Checking Device Status Before Binding

```bash
# See all NVMe devices and their current drivers
lspci -k | grep -A 3 "Non-Volatile"

# Show device BDFs for SPDK
sudo ./scripts/setup.sh status
```

### 9.4 Binding Specific Devices

```bash
# Bind only two specific NVMe SSDs
sudo PCI_ALLOWED="0000:01:00.0 0000:02:00.0" ./scripts/setup.sh

# Verify binding
ls /sys/bus/pci/drivers/vfio-pci/
```

### 9.5 Unbinding for OS Access

```bash
# Reset all devices back to kernel drivers
sudo ./scripts/setup.sh reset

# After reset, NVMe devices reappear as /dev/nvme* block devices
```

### 9.6 Non-Root Operation

```bash
# Configure hugepages and VFIO groups for non-root user
sudo TARGET_USER=spdk_user HUGEMEM=4096 ./scripts/setup.sh

# Run SPDK as the non-root user (VFIO groups owned by spdk_user)
spdk_tgt -m 0xf -s 4096
```

---

## 10. RPC 소켓 및 CLI 레퍼런스

### 10.1 rpc.py Tool

`scripts/rpc.py` is the primary CLI for sending JSON-RPC 2.0 requests to a running SPDK instance.

```bash
# Basic usage
rpc.py [global options] <method> [method options]

# Global options
rpc.py --help           # Show all available methods
rpc.py -s /var/tmp/spdk.sock <method>   # Custom socket path
rpc.py -h 127.0.0.1 -p 5260 <method>   # TCP socket (IP + port)
rpc.py -t 10 <method>                   # Set timeout (seconds)
rpc.py -v <method>                      # Verbose output (show request/response)
```

### 10.2 Default RPC Socket

| Context | Default Path |
|---------|-------------|
| Single instance | `/var/tmp/spdk.sock` |
| Multi-instance | `/var/tmp/spdk.sock.N` where N = `--shm-id` value |
| Custom | Set with `-r` / `--rpc-socket` at startup |

### 10.3 Framework Control RPCs

```bash
# Start subsystem initialization (used with --wait-for-rpc)
rpc.py framework_start_init

# Block until all subsystems are initialized
rpc.py framework_wait_init

# List all running reactors and their threads
rpc.py framework_get_reactors

# List all available RPC methods
rpc.py rpc_get_methods

# Send a kill signal to the application
rpc.py spdk_kill_instance -s SIGTERM

# Enable/disable CPU core lock files
rpc.py framework_enable_cpumask_locks
rpc.py framework_disable_cpumask_locks
```

### 10.4 Thread Management

```bash
# Get per-thread statistics (polling cycles, idle time)
rpc.py thread_get_stats

# Get per-thread pollers list
rpc.py thread_get_pollers

# Get per-thread I/O channels
rpc.py thread_get_io_channels

# Move a thread to a different CPU core
rpc.py thread_set_cpumask --id <thread_id> --cpumask 0x4
```

### 10.5 Scheduler Configuration

```bash
# Get current scheduler
rpc.py framework_get_scheduler

# Set scheduler type and period
rpc.py framework_set_scheduler \
  --name dynamic \
  --period 1000000

# Set scheduler options
rpc.py scheduler_set_options \
  --load-limit 60 \
  --core-limit 80 \
  --core-busy 80
```

### 10.6 Logging Configuration

```bash
# Get current log flags
rpc.py log_get_flags

# Enable a specific log flag
rpc.py log_set_flag nvme

# Disable a log flag
rpc.py log_clear_flag nvme

# Set log print level
rpc.py log_set_print_level DEBUG

# Available levels: DEBUG, INFO, NOTICE, WARNING, ERROR
```

### 10.7 JSON-RPC via curl

For integration testing or environments without Python, use curl:

```bash
# Send a raw JSON-RPC request via Unix socket
curl --unix-socket /var/tmp/spdk.sock \
  -X POST \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"bdev_get_bdevs"}' \
  http://localhost/

# TCP socket
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"nvmf_get_subsystems"}' \
  http://127.0.0.1:5260/
```

### 10.8 Quick Reference: Common Configuration Sequences

#### NVMe-oF TCP Target

```bash
rpc.py bdev_malloc_create 1024 4096 -b Malloc0
rpc.py nvmf_create_transport -t TCP
rpc.py nvmf_create_subsystem nqn.2023-01.io.spdk:target -a
rpc.py nvmf_subsystem_add_ns nqn.2023-01.io.spdk:target Malloc0
rpc.py nvmf_subsystem_add_listener nqn.2023-01.io.spdk:target -t TCP -f IPv4 -a 0.0.0.0 -s 4420
```

#### iSCSI Target

```bash
rpc.py bdev_malloc_create 1024 512 -b iSCSIBdev
rpc.py iscsi_create_portal_group 1 "0.0.0.0:3260"
rpc.py iscsi_start_portal_group 1
rpc.py iscsi_create_initiator_group 1 "ANY" "ANY"
rpc.py iscsi_create_target_node disk1 "Disk 1" "iSCSIBdev:0" "1:1" 64 -d
```

#### Vhost Block for QEMU

```bash
rpc.py bdev_malloc_create 512 4096 -b VhostBdev
rpc.py vhost_create_blk_controller /tmp/vhost.0 VhostBdev
```

#### PCIe NVMe Device Pass-Through

```bash
sudo ./scripts/setup.sh                         # Bind NVMe to VFIO
rpc.py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a 0000:01:00.0
rpc.py nvmf_create_transport -t TCP
rpc.py nvmf_create_subsystem nqn.2023-01.io.spdk:nvme -a
rpc.py nvmf_subsystem_add_ns nqn.2023-01.io.spdk:nvme Nvme0n1
rpc.py nvmf_subsystem_add_listener nqn.2023-01.io.spdk:nvme -t TCP -f IPv4 -a 0.0.0.0 -s 4420
```

---

*Document generated for SPDK Mastery Course — Configuration Reference.*
*Source: SPDK source tree, include/spdk/event.h, scripts/setup.sh, scripts/rpc.py.*

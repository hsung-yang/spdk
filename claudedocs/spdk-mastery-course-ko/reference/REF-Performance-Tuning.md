# REF: SPDK 성능 튜닝 가이드

**과정**: SPDK 마스터리
**모듈**: 참조 자료
**대상**: 프로덕션 또는 벤치마킹 환경에서 SPDK를 배포하는 엔지니어

---

## 목차

1. [사전 튜닝 체크리스트](#1-사전-튜닝-체크리스트)
2. [CPU 친화도 튜닝](#2-cpu-친화도-튜닝)
3. [메모리 튜닝](#3-메모리-튜닝)
4. [I/O 튜닝](#4-io-튜닝)
5. [NVMe-oF 전용 튜닝](#5-nvme-of-전용-튜닝)
6. [벤치마킹 방법론](#6-벤치마킹-방법론)
7. [수집해야 할 성능 메트릭](#7-수집해야-할-성능-메트릭)
8. [흔한 성능 함정과 해결 방법](#8-흔한-성능-함정과-해결-방법)
9. [빠른 참조 파라미터 표](#9-빠른-참조-파라미터-표)

---

## 1. 사전 튜닝 체크리스트

SPDK를 시작하거나 벤치마크를 실행하기 전에 다음 시스템 수준 설정을 확인하세요. 이 단계를 건너뛰는 것이 예상보다 낮은 성능 결과의 가장 흔한 원인입니다.

### 1.1 BIOS 설정

| 설정 | 권장 값 | 중요한 이유 |
|---|---|---|
| 하이퍼스레딩(Hyper-Threading, HT) | 비활성화 (지연시간), 활성화 (처리량) | HT는 논리적 형제 코어 간에 L1/L2 캐시를 공유하여 SPDK 폴러가 캐시 라인을 놓고 경쟁함 |
| CPU C-State | 비활성화 (C1E/C3/C6/C7 모두 끄기) | C-state 깨어남 지연이 폴링 루프에 10-100 us 지터를 추가 |
| 터보 부스트 / Intel Speed Step | 재현성을 위해 비활성화, 처리량에는 선택 사항 | 주파수 변동으로 벤치마크 실행 간 비교 불가 |
| NUMA 인터리빙 | 비활성화 | SPDK가 명시적인 NUMA-로컬 할당을 수행할 수 있도록 허용 |
| PCIe ASPM | 비활성화 | 활성 상태 전력 관리가 PCIe 트랜잭션에 지연시간 추가 |
| SR-IOV | VF 패스스루 사용 시 활성화 | NVMe-oF 또는 vhost SR-IOV 워크로드에 필요 |

```
# 부팅 후 Linux에서 C-state 확인
cat /sys/devices/system/cpu/cpu*/cpuidle/state*/disable
# C2 이상은 모두 "1" (비활성화)이어야 함
```

### 1.2 커널 파라미터

다음을 `/etc/default/grub`의 `GRUB_CMDLINE_LINUX`에 추가하고 `grub2-mkconfig`를 실행하세요:

```
# 코어 격리 및 NUMA
isolcpus=2-15,18-31          # SPDK용 코어 예약 (토폴로지에 맞게 조정)
nohz_full=2-15,18-31         # 격리된 코어에서 타이머 틱 비활성화
rcu_nocbs=2-15,18-31         # 격리된 코어에서 RCU 콜백 이동
nosoftlockup                  # 소프트 록업 워치독 비활성화 (폴링과 간섭)
intel_idle.max_cstate=0       # Intel 전용 C-state 비활성화
processor.max_cstate=1        # OS 수준에서 C-state 하드 제한
default_hugepagesz=1G         # 1G를 기본 휴지페이지 크기로 설정
hugepagesz=1G
hugepages=32                  # 32 x 1G 페이지 사전 할당 (메모리 예산에 맞게 조정)
iommu=pt                      # 최저 DMA 오버헤드를 위한 패스스루 IOMMU
intel_iommu=on                # IOMMU 활성화 (VFIO에 필요)
```

적용 후 재부팅:
```bash
sudo grub2-mkconfig -o /boot/grub2/grub.cfg
sudo reboot
```

### 1.3 휴지페이지(Hugepage) 설정

SPDK는 모든 DMA 버퍼에 DPDK 스타일 휴지페이지를 사용합니다. 사전 할당된 휴지페이지가 없으면 SPDK 시작이 실패합니다.

```bash
# 현재 휴지페이지 할당 확인
cat /proc/meminfo | grep Huge

# 런타임에 1G 휴지페이지 할당 (재부팅 시 사라짐)
echo 32 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages

# NUMA 인식 할당 (다중 소켓 시스템에 권장)
echo 16 > /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages
echo 16 > /sys/devices/system/node/node1/hugepages/hugepages-1048576kB/nr_hugepages

# hugetlbfs가 마운트되지 않은 경우 마운트
mount -t hugetlbfs nodev /mnt/huge

# /etc/sysctl.conf를 통해 할당 영구화
echo "vm.nr_hugepages = 32" >> /etc/sysctl.conf
```

**경험적 규칙**: SPDK가 I/O 버퍼에 사용할 메모리의 최소 2배를 할당하세요. 큐 뎁스 128과 128KB I/O 크기로 4개의 NVMe 드라이브를 서비스하는 타겟의 경우 대략:
```
4 드라이브 x 128 QD x 128 KB = 최소 64 MB; 안전을 위해 4-8 GB 할당
```

### 1.4 CPU 격리 확인

```bash
# isolcpus가 적용되었는지 확인
cat /sys/devices/system/cpu/isolated

# 격리된 코어에 커널 스레드가 스케줄되지 않는지 확인
ps -eLo psr,pid,comm | awk '$1 >= 2 && $1 <= 15'   # 거의 비어 있어야 함

# nohz_full 확인
cat /sys/devices/system/cpu/nohz_full
```

### 1.5 NVMe 드라이브 사전 점검

```bash
# SPDK 시작 전에 NVMe 디바이스를 vfio-pci에 바인딩
sudo scripts/setup.sh          # SPDK 헬퍼: 커널 드라이버에서 언바인드하고 vfio-pci에 바인딩

# 바인딩 확인
lspci -k -d :0108              # NVMe 클래스; 드라이버가 vfio-pci로 표시되어야 함

# PCIe 링크 속도 확인 (Gen4 x4 = ~7 GB/s; 링크 제한이 아닌지 확인)
sudo lspci -vv -s <BDF> | grep -i "lnksta\|width"
```

---

## 2. CPU 친화도 튜닝

### 2.1 리액터 마스크(Reactor Mask)

`reactor_mask` (CLI에서 `-m` / `--cpumask`으로도 표시)는 SPDK가 어떤 CPU 코어에 리액터를 생성할지 지정합니다. 모든 리액터는 빠른 폴링 루프를 실행합니다. 리액터를 잘못된 코어에 할당하는 것이 가장 큰 영향을 미치는 설정 실수입니다.

**`spdk_app_opts`에서** (프로그래밍 방식):
```c
struct spdk_app_opts opts = {};
spdk_app_opts_init(&opts, sizeof(opts));
opts.reactor_mask = "0x3C";   // 16진수 비트마스크: 코어 2, 3, 4, 5
```

**커맨드 라인에서** (bdevperf, nvmf_tgt 등):
```bash
./build/bin/nvmf_tgt -m 0x3C             # 16진수 마스크
./build/bin/nvmf_tgt -m [2-5]            # 범위 표기법
./build/bin/nvmf_tgt -m "[2,3,4,5]"      # 목록 표기법
```

**형식 옵션** (`doc/applications.md §cpu_mask`에 정의):
- 16진수 문자열: `0x1F07`
- 쉼표 구분: `[0,1,2,8-12]`
- 모든 형식은 대소문자를 구분하지 않음

### 2.2 NUMA 인식

SPDK는 기본적으로 NUMA-로컬 할당을 수행하지만, 리액터가 올바르게 배치된 경우에만 작동합니다.

```bash
# NVMe 디바이스의 NUMA 노드 확인
cat /sys/bus/pci/devices/0000:01:00.0/numa_node

# NIC NUMA 노드 확인 (NVMe-oF용)
cat /sys/bus/pci/devices/0000:82:00.0/numa_node

# 규칙: reactor_mask는 NVMe 디바이스와 NIC 모두에 로컬인 코어를
# 동일 NUMA 노드에서 포함해야 합니다.

# 예시: NVMe가 NUMA 0 (코어 0-15), NIC가 NUMA 0
./build/bin/nvmf_tgt -m [2-7]            # NUMA 0의 코어 2-7: 최적

# 반대 예시: NVMe가 NUMA 0, 리액터가 NUMA 1
./build/bin/nvmf_tgt -m [18-23]          # 크로스-NUMA DMA: I/O당 ~100 ns 추가 지연
```

**NUMA 토폴로지 확인**:
```bash
numactl --hardware
lstopo --of ascii    # hwloc이 설치된 경우
```

### 2.3 isolcpus와 SPDK 코어

`isolcpus`에 나열된 코어는 Linux 스케줄러에 보이지 않습니다. SPDK는 커널 스레드가 폴링 루프를 방해하지 않도록 이러한 코어를 리액터에 사용해야 합니다.

```
# 커널 cmdline
isolcpus=2-15,18-31

# SPDK reactor_mask가 격리된 코어와 일치
-m [2-15,18-31]
```

OS, 관리 스레드, NIC IRQ 처리를 위해 소켓당 최소 1-2개의 코어를 남겨두세요.

### 2.4 NIC의 IRQ 친화도 (NVMe-oF)

NVMe-oF의 경우, NIC의 인터럽트 벡터를 SPDK 리액터와 인접하되 동일하지 않은 코어에 고정해야 합니다:

```bash
# NIC IRQ 확인
cat /proc/interrupts | grep <iface>

# IRQ를 코어 1에 고정 (SPDK 코어 2-7에 인접)
echo 2 > /proc/irq/<IRQ_NUMBER>/smp_affinity_list

# SPDK irq 스크립트가 있는 경우 사용
scripts/irq_set_affinity.sh <iface> <cpumask>
```

---

## 3. 메모리 튜닝

### 3.1 휴지페이지 크기: 2M vs 1G

| 휴지페이지 크기 | 장점 | 단점 | 적합한 용도 |
|---|---|---|---|
| 2M (기본값) | 할당이 쉽고 유연한 크기 조정 | 대규모 할당 시 더 많은 TLB 엔트리 필요 | 개발, 소규모 배포 |
| 1G | GB당 단일 TLB 엔트리, 최저 DMA 오버헤드 | 부팅 시 사전 할당 필요, 유연하지 않음 | 프로덕션 NVMe-oF 타겟, 고 IOPS |

커널 파라미터 `default_hugepagesz=1G hugepagesz=1G hugepages=N`은 메모리 단편화가 발생하기 전인 부팅 시에 1G 페이지를 사전 할당합니다.

### 3.2 SPDK 메모리 크기 (`-s` / `mem_size`)

`-s` 옵션(`spdk_app_opts.mem_size`에 매핑)은 DPDK가 등록할 휴지페이지 메모리 양을 제어합니다. 기본값은 0으로, DPDK가 가용한 모든 휴지페이지를 사용합니다.

```bash
# SPDK를 8 GB 휴지페이지 메모리로 제한
./build/bin/nvmf_tgt -m [2-7] -s 8192    # MB 단위

# opts 구조체에서
opts.mem_size = 8192;   // MB
```

**크기 산정 가이드**:
- I/O 버퍼 풀: `num_reactors x max_queue_depth x max_io_size`
- NVMe-oF 트랜스포트 버퍼: `num_connections x in_capsule_data_size x 16`
- 메타데이터, 제어 구조체, DPDK 내부 사용을 위해 20% 여유 공간 추가

### 3.3 NUMA-로컬 메모리 할당

```bash
# DPDK가 소켓별 할당을 수행하도록 강제 (socket_mem은 소켓별 MB, 쉼표 구분)
# SPDK 설정 또는 --eal-args를 통해 EAL 옵션으로 전달
./build/bin/nvmf_tgt -m [2-7] --eal-args="--socket-mem=4096,4096"

# 단일 세그먼트 휴지페이지 선호 (TLB 압력 추가 감소)
# opts.hugepage_single_segments = true
```

### 3.4 I/O 버퍼 풀 크기 조정

SPDK는 시작 시 DMA 가능 버퍼 풀을 사전 할당합니다. 풀 크기가 달성 가능한 큐 뎁스를 직접 제한합니다:

```bash
# nvmf 타겟에서: 시작 후 RPC를 통해 설정
rpc.py nvmf_set_config --io_unit_size 131072    # 128 KB I/O 단위
rpc.py bdev_nvme_set_options --io_queue_requests 512
```

버퍼 풀이 소진되면 I/O가 내부적으로 큐잉되어 지연시간이 급증합니다. `bdev_get_bdevs` 통계에서 `buf_cache_count`가 0으로 떨어지는지 확인하세요.

---

## 4. I/O 튜닝

### 4.1 큐 뎁스(Queue Depth)

큐 뎁스(QD)는 IOPS 처리량의 주요 레버입니다. 각 NVMe 네임스페이스는 큐당 최대 `io_queue_size`개의 미완료 명령을 지원합니다.

**`spdk_nvme_ctrlr_opts`에서** (프로그래밍 방식):
```c
struct spdk_nvme_ctrlr_opts opts;
spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts));
opts.io_queue_size = 1024;       // 큐당 명령 수
opts.io_queue_requests = 2048;   // 처리 중 요청 객체 (>= io_queue_size)
opts.num_io_queues = 4;          // 컨트롤러당 I/O 큐 쌍 수
```

**최적 QD를 찾기 위한 벤치마크 스윕**:
```bash
for qd in 1 2 4 8 16 32 64 128 256; do
    ./build/bin/spdk_nvme_perf -q $qd -o 4096 -w randread -t 30 -c 0x4 \
      -r "trtype:PCIe traddr:0000:01:00.0" 2>&1 | grep "IOPS\|Latency"
done
```

일반적인 NVMe SSD는 4K 랜덤 읽기 시 QD=32-64에서 포화됩니다. 더 높이면 IOPS 향상 없이 지연시간만 증가합니다.

### 4.2 I/O 크기

```bash
# 4K 랜덤 읽기 (최대 IOPS, 컨트롤러 큐 테스트)
./build/bin/spdk_nvme_perf -q 64 -o 4096 -w randread -t 60 -c 0x4 \
  -r "trtype:PCIe traddr:0000:01:00.0"

# 128K 순차 읽기 (최대 대역폭, PCIe/NAND 대역폭 테스트)
./build/bin/spdk_nvme_perf -q 8 -o 131072 -w read -t 60 -c 0x4 \
  -r "trtype:PCIe traddr:0000:01:00.0"
```

**I/O 크기 가이드라인**:
- 4K: 최대 IOPS, 지연시간에 민감한 워크로드
- 16K-64K: 혼합 OLTP 스타일 워크로드
- 128K-512K: 스트리밍, 백업, 순차 스캔
- NVMe 최적 I/O 경계에 I/O 크기를 정렬 (`nvme id-ns`에서 `Optimal Write Size` 확인)

### 4.3 배치 카운트 (I/O 완료)

SPDK는 완료를 배치로 처리합니다. `spdk_nvme_perf`의 `--io-count` (`-N`) 옵션은 실행당 총 I/O를 제한합니다; 연속 워크로드에서는 설정하지 마세요.

bdev 계층에서 완료 배치 크기는 폴러 반복당 내부적으로 제어됩니다. 더 큰 배치 크기는 꼬리 지연시간(tail latency)을 대가로 처리량을 향상시킵니다:

```bash
# bdevperf: 배치 동작을 암묵적으로 제어하기 위해 큐 뎁스 조정
./build/examples/bdevperf -q 256 -o 4096 -t 60 -w randread -m 0x4 \
  -b Nvme0n1
```

### 4.4 쓰기 백(Write Back) vs 쓰기 스루(Write Through)

캐싱 계층(OCF)이 있는 NVMe bdev의 경우:
```bash
# 쓰기 백: 높은 처리량, 크래시 시 데이터 손실 위험
rpc.py bdev_ocf_create Cache0 wb NvmeCache0n1 NvmeCore0n1

# 쓰기 스루: 안전, 낮은 처리량
rpc.py bdev_ocf_create Cache0 wt NvmeCache0n1 NvmeCore0n1
```

---

## 5. NVMe-oF 전용 튜닝

### 5.1 폴 그룹(Poll Group) 아키텍처

각 NVMe-oF 폴 그룹(`spdk_nvmf_poll_group`)은 하나의 리액터 코어에서 실행되며 일련의 연결을 처리합니다. 폴 그룹 수는 NVMe-oF에 할당된 리액터 코어 수와 같습니다.

```
reactor_mask = [2,3,4,5]  →  4개의 폴 그룹 생성
                               연결이 라운드 로빈으로 분산
```

```bash
# 런타임에 폴 그룹 분산 확인
rpc.py nvmf_get_stats
# "poll_groups" 배열 확인: 각 항목에 스레드 이름과 연결 수가 표시됨
```

**크기 산정 규칙**: NVMe 네임스페이스 4-8개당 리액터 코어 1개, 또는 타겟 대역폭 4 Gbps(TCP)당 코어 1개, 또는 25 Gbps(RDMA)당 코어 1개.

### 5.2 연결 분산

연결은 수락 시 폴 그룹에 라운드 로빈으로 할당됩니다. 분산에 영향을 주려면:

```bash
# 명시적 CPU 친화도를 가진 전용 서브시스템 생성
rpc.py nvmf_create_subsystem nqn.2024-01.io.spdk:cnode1 \
  --allow-any-host \
  --mn "SPDK Target"

# 특정 트랜스포트에 리스너 추가
rpc.py nvmf_subsystem_add_listener nqn.2024-01.io.spdk:cnode1 \
  -t TCP -a 192.168.1.100 -s 4420
```

RDMA의 경우, RDMA 완료 큐 스레드를 HCA와 동일한 NUMA 노드에 고정하세요:
```bash
# RDMA 트랜스포트 옵션 설정
rpc.py nvmf_create_transport -t RDMA \
  --max-queue-depth 128 \
  --max-io-size 131072 \
  --in-capsule-data-size 4096 \
  --num-shared-buffers 4096
```

### 5.3 트랜스포트별 옵션

**TCP 트랜스포트**:
```bash
rpc.py nvmf_create_transport -t TCP \
  --max-queue-depth 128 \
  --max-io-size 131072 \
  --in-capsule-data-size 8192 \   # 인라인 데이터 임계값; 소규모 I/O의 예상 I/O 크기에 맞춤
  --num-shared-buffers 8192 \     # 공유 버퍼 풀 크기
  --sock-priority 6               # 소켓 우선순위 (CAP_NET_ADMIN 필요)
```

**RDMA 트랜스포트**:
```bash
rpc.py nvmf_create_transport -t RDMA \
  --max-queue-depth 128 \
  --max-io-size 131072 \
  --in-capsule-data-size 4096 \
  --num-shared-buffers 4096 \
  --max-srq-depth 4096            # 공유 수신 큐 뎁스
```

### 5.4 Acceptor 폴링 속도

acceptor 스레드 속도는 새 연결의 수락 속도를 제어합니다. 기본값은 10,000 us (10 ms)입니다:

```bash
rpc.py nvmf_set_config --acceptor-poll-rate 1000    # 1 ms: 더 빠른 연결 설정
# 참고: 낮은 값은 acceptor 스레드에서 더 많은 CPU를 소비합니다
```

### 5.5 In-Capsule 데이터 임계값

`in_capsule_data_size`는 I/O 데이터가 NVMe 명령 캡슐에 인라인으로 전송될지(제로 카피 경로) 별도의 데이터 전송을 통해 전송될지를 결정합니다. 이를 예상 소규모 I/O 크기와 동일하게 설정하면 하나의 왕복을 제거합니다:

```
워크로드: 4K 랜덤 읽기
읽기 응답용 in_capsule_data_size = 4096 설정 (TCP)
결과: 데이터가 단일 캡슐로 반환, 추가 RDMA WRITE 없음
```

---

## 6. 벤치마킹 방법론

### 6.1 도구 선택

| 도구 | 적합한 용도 | 트랜스포트 지원 |
|---|---|---|
| `spdk_nvme_perf` | NVMe 로컬 및 NVMe-oF 종단간 지연시간/IOPS | PCIe, TCP, RDMA |
| `bdevperf` | bdev 계층 오버헤드, bdev 모듈 비교 | 모든 bdev 유형 |
| `fio` + SPDK 플러그인 | 현실적인 다중 작업 워크로드, FIO 작업 파일 | ioengine을 통한 모든 bdev 유형 |

### 6.2 spdk_nvme_perf

```bash
# 4K 랜덤 읽기, QD=64, 60초 실행, 지연시간 히스토그램 활성화
./build/bin/spdk_nvme_perf \
  -q 64 \
  -o 4096 \
  -w randread \
  -t 60 \
  -c 0x4 \
  -L \                             # 지연시간 추적 활성화
  -r "trtype:PCIe traddr:0000:01:00.0"

# NVMe-oF TCP 타겟
./build/bin/spdk_nvme_perf \
  -q 128 \
  -o 4096 \
  -w randread \
  -t 300 \
  -c 0xFF \
  -L \
  -r "trtype:TCP adrfam:IPv4 traddr:192.168.1.100 trsvcid:4420"

# 워밍업 기간 포함 (측정 전 10초)
./build/bin/spdk_nvme_perf -q 64 -o 4096 -w randread -t 60 -a 10 \
  -r "trtype:PCIe traddr:0000:01:00.0"

# 주요 옵션 요약:
#   -q  큐 뎁스
#   -o  I/O 크기 (바이트, 또는 접미사: 4k, 128k)
#   -w  패턴: read, write, randread, randwrite, randrw
#   -M  randrw의 읽기 비율 (예: -M 70 = 70% 읽기)
#   -t  지속 시간 (초)
#   -c  코어 마스크
#   -L  지연시간 히스토그램 활성화
#   -a  워밍업 시간 (초)
#   -r  트랜스포트 지정
```

### 6.3 bdevperf

```bash
# 로컬 NVMe bdev에 대해 bdevperf 시작
sudo ./build/examples/bdevperf \
  -c /path/to/bdev.json \
  -m 0x4 \
  -z &                            # -z: RPC로 테스트 시작 대기

# RPC를 통해 테스트 트리거
sudo python3 examples/bdev/bdevperf/bdevperf.py perform_tests \
  -q 64 \
  -o 4096 \
  -t 60 \
  -w randread

# 설정 파일 사용 (FIO 스타일)
cat > /tmp/bdevperf.conf << 'EOF'
[global]
filename=Nvme0n1
bs=4096
rw=randread
iodepth=64

[job0]
cpumask=0x4
EOF

sudo ./build/examples/bdevperf -c bdev.json -j /tmp/bdevperf.conf -m 0x4
```

### 6.4 FIO와 SPDK 플러그인

```bash
# fio 플러그인 지원과 함께 SPDK 빌드
./configure --with-fio=/path/to/fio/source
make

# FIO 작업 파일
cat > /tmp/spdk_fio.job << 'EOF'
[global]
ioengine=/path/to/spdk/build/fio/spdk_bdev
spdk_json_conf=/path/to/bdev.json
thread=1
group_reporting=1
direct=1
bs=4k
rw=randread
iodepth=64
time_based=1
runtime=60

[job0]
filename=Nvme0n1
cpus_allowed=2
EOF

sudo fio /tmp/spdk_fio.job
```

### 6.5 벤치마크 규율

1. **워밍업**: 측정 전 항상 10-30초의 워밍업을 실행하세요. NVMe TLC 드라이브는 초기 수치에 영향을 미치는 쓰기 캐시를 가지고 있습니다.
2. **정상 상태**: 최소 60초 실행; 스토리지 클래스 워크로드의 경우 300초.
3. **반복**: 각 설정을 3회 이상 실행하고 중앙값을 보고하세요.
4. **격리**: 시스템의 다른 모든 워크로드를 중지하세요. `mpstat -P ALL 1`로 CPU 사용률이 깨끗한지 확인하세요.
5. **기록**: BIOS 버전, 커널 버전, SPDK 버전, 드라이브 펌웨어 및 모든 튜닝 파라미터를 결과와 함께 기록하세요.
6. **기준선 먼저**: 참조 델타를 설정하기 위해 항상 커널 NVMe 드라이버(`fio --ioengine=libaio`)로 먼저 측정하세요.

---

## 7. 수집해야 할 성능 메트릭

### 7.1 기본 메트릭

| 메트릭 | 수집 방법 | 참고 |
|---|---|---|
| IOPS | spdk_nvme_perf 출력, bdevperf RPC | 정상 상태 구간의 평균으로 보고 |
| 처리량 (MB/s) | spdk_nvme_perf 출력 | IOPS x 블록 크기 |
| 평균 지연시간 (us) | spdk_nvme_perf `-L` 플래그 | 백분위수보다 덜 유용 |
| P50 지연시간 | spdk_nvme_perf 히스토그램 | 일반적인 지연시간 |
| P99 지연시간 | spdk_nvme_perf 히스토그램 | 서비스 수준 협약(SLA) 메트릭 |
| P99.9 지연시간 | spdk_nvme_perf 히스토그램 | 이상치 탐지 |
| P99.99 지연시간 | spdk_nvme_perf 히스토그램 | 지연시간에 민감한 애플리케이션의 꼬리 지연시간 |

`spdk_nvme_perf`에서 `-L`을 사용하면 다음 컷오프에서 지연시간을 자동으로 보고합니다:
`1%, 10%, 25%, 50%, 75%, 90%, 95%, 98%, 99%, 99.5%, 99.9%, 99.99%, 99.999%, 99.9999%, 99.99999%`

### 7.2 CPU 사용률

```bash
# 테스트 중 코어별 사용률 (별도 터미널에서 실행)
mpstat -P ALL 1 60

# SPDK 리액터 busy vs idle 비율
# 테스트 중 RPC를 통해 조회 가능
rpc.py framework_get_reactors
# 리액터별 "busy" 및 "idle" TSC 카운트 확인

# busy_tsc / (busy_tsc + idle_tsc) = CPU 사용률
# 목표: 리액터 코어에서 95-100% busy (폴링이 설계 의도)
# 경고: IOPS 여유가 있는데 80% 미만이면 병목이 다른 곳에 있음
```

### 7.3 SPDK 전용 메트릭

```bash
# Bdev I/O 통계
rpc.py bdev_get_iostat -b Nvme0n1
# 반환: bytes_read, bytes_written, num_read_ops, num_write_ops,
#          read_latency_ticks, write_latency_ticks

# NVMe-oF 타겟 통계
rpc.py nvmf_get_stats
# 폴 그룹별 반환: io_count, pending_data_buffer_count,
#                         트랜스포트별 카운터

# 스레드 통계
rpc.py thread_get_stats
# 스레드별 반환: idle_tsc, busy_tsc, 폴러 수

# TSC 틱을 마이크로초로 변환
# latency_us = latency_ticks / (tsc_rate / 1e6)
# tsc_rate 조회: rpc.py framework_get_reactors | grep tsc_rate
```

### 7.4 시스템 수준 메트릭

```bash
# PCIe 대역폭 (pcm 또는 turbostat 필요)
sudo pcm-pcie 1

# 메모리 대역폭 (NUMA 크로스 트래픽은 위험 신호)
sudo numastat -m
sudo pcm-memory 1

# NIC 사용률 (NVMe-oF용)
sar -n DEV 1 60

# 인터럽트 수 (SPDK 코어의 높은 수 = IRQ 친화도 문제)
watch -n 1 cat /proc/interrupts
```

---

## 8. 흔한 성능 함정과 해결 방법

### 함정 1: SPDK 리액터가 OS 스레드와 코어 공유

**증상**: 실행 간 IOPS 편차, 지연시간 급증, `mpstat`에서 SPDK 코어의 `%sys` 시간이 보임.

**진단**:
```bash
ps -eLo psr,pid,comm | awk '$1 == 2'   # 코어 2에서 비 SPDK 스레드 확인
```

**해결**: `isolcpus`, `nohz_full`, `rcu_nocbs` 커널 파라미터에 코어를 추가하세요. `reactor_mask`가 격리된 코어만 포함하는지 확인하세요.

---

### 함정 2: NUMA 불일치 할당

**증상**: 처리량이 예상 최대치의 ~50-60%에서 정체, `pcm-memory`에서 높은 메모리 지연시간.

**진단**:
```bash
numastat -p $(pgrep nvmf_tgt)    # 크로스 노드 메모리 접근 확인
```

**해결**: `reactor_mask`가 NVMe 디바이스와 NIC를 소유한 NUMA 노드의 코어만 포함하도록 하세요. 디바이스가 여러 NUMA 노드에 걸쳐 있으면 노드별로 전용 리액터를 할당하세요.

---

### 함정 3: 휴지페이지 부족

**증상**: `Cannot get hugepage information` 또는 `Unable to allocate DMA memory`로 SPDK 시작 실패.

**해결**:
```bash
echo 32 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
# 그런 다음 가용 메모리에 맞는 -s 옵션으로 SPDK 재시작
```

---

### 함정 4: 큐 뎁스 너무 낮음

**증상**: IOPS가 드라이브 사양보다 훨씬 낮음; `spdk_nvme_perf`에서 유휴 시간은 거의 없지만 IOPS가 낮음.

**진단**: IOPS vs QD 스윕 실행 (4.1절 참조). 드라이브의 정격 최대값까지 IOPS가 QD에 선형으로 증가하면 QD가 제약 요소입니다.

**해결**: 벤치마크에서 `-q`를 증가시키세요. 프로덕션에서는 `spdk_nvme_ctrlr_opts`의 `io_queue_size`와 `io_queue_requests`를 증가시키세요.

---

### 함정 5: I/O 크기와 in_capsule_data_size 불일치 (NVMe-oF)

**증상**: 소규모 I/O에서 NVMe-oF TCP 지연시간이 예상보다 2배 높음.

**진단**: `tcpdump`를 실행하여 데이터가 명령 캡슐과 같은 TCP 세그먼트에 도착하는지 확인하세요.

**해결**: `in_capsule_data_size`를 워크로드의 I/O 크기에 맞게 설정하세요:
```bash
rpc.py nvmf_create_transport -t TCP --in-capsule-data-size 4096
```

---

### 함정 6: PCIe 대역폭 포화

**증상**: 큐 뎁스를 높여도 처리량이 Gen3 x4 링크에서 ~3.5 GB/s로 제한됨.

**진단**:
```bash
lspci -vv -s <BDF> | grep LnkSta   # Gen4의 경우 "Speed 16GT/s, Width x4"가 표시되어야 함
```

**해결**: NVMe 드라이브가 Gen4 지원 슬롯에 장착되었는지 확인하세요. 드라이브를 다시 장착하세요. 슬롯 분할(bifurcation) 설정에 대해 메인보드 문서를 확인하세요.

---

### 함정 7: 테스트 중 CPU 주파수 스케일링

**증상**: IOPS 수치가 테스트 실행 간에 크게 변동, 특히 실행 초반에.

**진단**:
```bash
watch -n 0.5 "cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq | sort -u"
```

**해결**: CPU 거버너를 `performance`로 설정하세요:
```bash
cpupower frequency-set -g performance
# 또는
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

---

### 함정 8: 버퍼 풀 소진

**증상**: 지속적인 부하에서 지연시간이 급격히 증가; `nvmf_get_stats`에서 `pending_data_buffer_count`가 증가.

**해결**: 트랜스포트 생성 시 `num_shared_buffers`를 증가시키세요:
```bash
rpc.py nvmf_create_transport -t TCP --num-shared-buffers 16384
```

또는 `-s` (mem_size) 인수를 늘려 SPDK에 더 많은 휴지페이지 메모리를 할당하세요.

---

### 함정 9: 하이퍼스레딩 간섭

**증상**: 수 밀리초마다 지연시간 급증; 캐시 미스율 증가.

**진단**: SPDK 리액터 코어에 격리되지 않은 HT 형제가 있는지 확인하세요:
```bash
cat /sys/devices/system/cpu/cpu2/topology/thread_siblings_list
# 예: "2,18"이 반환되면 코어 2와 논리 코어 18이 물리 코어를 공유
```

**해결**: BIOS에서 HT를 비활성화하거나, `isolcpus`와 `reactor_mask`에 두 논리적 형제를 모두 포함시키세요.

---

## 9. 빠른 참조 파라미터 표

| 파라미터 | 설정 위치 | 권장 값 | 영향 |
|---|---|---|---|
| `reactor_mask` | CLI `-m`, `spdk_app_opts` | 디바이스와 같은 NUMA의 격리된 코어 | 모든 SPDK 리액터의 코어 할당 |
| `mem_size` | CLI `-s`, `spdk_app_opts` | 예상 I/O 버퍼 풋프린트의 2-4배 (MB) | SPDK가 등록하는 총 휴지페이지 메모리 |
| `hugepage_single_segments` | `spdk_app_opts` | 프로덕션에서 `true` | TLB 압력 감소 |
| `io_queue_size` | `spdk_nvme_ctrlr_opts` | 1024 (드라이브 최대값 확인) | NVMe 큐당 명령 수 |
| `io_queue_requests` | `spdk_nvme_ctrlr_opts` | 2048 (>= io_queue_size) | 처리 중 요청 객체 |
| `num_io_queues` | `spdk_nvme_ctrlr_opts` | 해당 디바이스를 폴링하는 리액터당 1개 | NVMe 컨트롤러에 대한 병렬성 |
| 큐 뎁스 (`-q`) | `spdk_nvme_perf`, `bdevperf` | 4K 랜덤: 64-128; 128K 순차: 8-16 | 기본 IOPS/지연시간 트레이드오프 |
| I/O 크기 (`-o`) | `spdk_nvme_perf`, `bdevperf` | 4096 (IOPS 테스트); 131072 (BW 테스트) | 벤치마크용 블록 크기 |
| `in_capsule_data_size` | `nvmf_create_transport` | 워크로드 I/O 크기에 맞춤 | 소규모 I/O의 추가 왕복 제거 |
| `num_shared_buffers` | `nvmf_create_transport` | 8192-16384 (TCP), 4096 (RDMA) | NVMe-oF DMA 버퍼 풀 크기 |
| `max_queue_depth` | `nvmf_create_transport` | 128 | 연결당 NVMe-oF 최대 SQ 뎁스 |
| `max_io_size` | `nvmf_create_transport` | 131072 (128 KB) | 타겟이 수용할 최대 I/O 크기 |
| `acceptor_poll_rate` | `nvmf_set_config` | 1000-10000 us | 연결 설정 지연시간 |
| `isolcpus` (커널) | `/etc/default/grub` | 모든 SPDK 리액터 코어 | SPDK 코어에서 커널 스케줄러 제거 |
| `nohz_full` (커널) | `/etc/default/grub` | `isolcpus`와 동일 | SPDK 코어에서 타이머 틱 비활성화 |
| `rcu_nocbs` (커널) | `/etc/default/grub` | `isolcpus`와 동일 | SPDK 코어에서 RCU 콜백 이동 |
| `intel_idle.max_cstate` | 커널 cmdline | `0` | 깨어남 시 C-state 지연시간 방지 |
| `scaling_governor` | `/sys/…/cpufreq/` | `performance` | 테스트 중 주파수 스케일링 방지 |
| `nr_hugepages` (1G) | `/sys/kernel/mm/hugepages/` | 워크로드에 따라 16-64 | 연속적인 1G 페이지 사전 할당 |
| NIC IRQ 친화도 | `/proc/irq/*/smp_affinity_list` | 같은 NUMA의 인접 비 SPDK 코어 | NIC IRQ가 리액터를 방해하는 것 방지 |

---

## 부록: 진단 명령어 참조

```bash
# 시스템 개요
numactl --hardware                              # NUMA 토폴로지
lstopo --of ascii                               # CPU/캐시/메모리 토폴로지
lspci -k | grep -A2 "NVM"                      # NVMe PCIe 바인딩

# 휴지페이지
cat /proc/meminfo | grep -i huge
numastat -m | grep -i huge

# CPU 상태
cat /sys/devices/system/cpu/isolated
cat /sys/devices/system/cpu/nohz_full
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort -u

# SPDK 런타임
rpc.py framework_get_reactors                  # 리액터 busy/idle 비율
rpc.py thread_get_stats                        # 스레드별 통계
rpc.py bdev_get_iostat                         # bdev I/O 카운터
rpc.py nvmf_get_stats                          # NVMe-oF 폴 그룹 통계

# 드라이브 상태 / 큐 뎁스
nvme id-ctrl /dev/nvme0 | grep -i "mdts\|oacs"   # 최대 데이터 전송 크기
nvme id-ns /dev/nvme0n1 | grep -i "npdg\|nows"   # 최적 I/O 단위

# 테스트 중 실시간 모니터링
watch -n 1 "rpc.py bdev_get_iostat -b Nvme0n1 2>/dev/null | python3 -c \
  \"import sys,json; s=json.load(sys.stdin)['bdevs'][0]; \
  print('IOPS:', s['num_read_ops'], 'BW(MB/s):', s['bytes_read']//1048576)\""
```

---

*이 가이드는 작성 시점의 저장소에 포함된 SPDK를 기반으로 합니다. 트랜스포트 RPC 파라미터와 옵션 이름은 릴리스마다 변경될 수 있습니다. 실행 중인 버전의 `rpc.py --help`와 생성된 `doc/jsonrpc.md`를 항상 교차 참조하세요.*

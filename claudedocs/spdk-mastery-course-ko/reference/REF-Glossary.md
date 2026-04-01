# SPDK 마스터리 과정 — 용어 사전

> 알파벳순으로 정리된 빠른 참조 용어 사전입니다. 각 항목에는
> 정의와 해당되는 경우 탐색을 돕는 관련 용어가 포함되어 있습니다.
>
> **아래에서 사용된 규칙**
> - *이탤릭체*는 SPDK 고유 개념이나 데이터 구조 이름을 나타냅니다.
> - **굵은 글씨** 교차 참조는 다른 용어 사전 항목을 가리킵니다.
> - 코드 글꼴(`고정폭`)은 API 심볼, 파일명, 명령줄 문자열에
>   사용됩니다.

---

## A

### Acceleration Framework (`accel`)
가속 프레임워크. SPDK의 하드웨어 가속 추상화 레이어로, 계산 집약적 작업(CRC, 복사, 암호화/복호화, 압축)을 Intel DSA, IDXD 또는 소프트웨어 폴백 등 전용 엔진에 오프로드합니다. 기반 엔진에 관계없이 단일 API를 통해 작업을 제출합니다. 관련 항목: **DIF**, **IDXD**, **offload**.

### AIO (Asynchronous I/O)
비동기 I/O. 전용 커널 스레드 없이 논블로킹 디스크 I/O를 허용하는 Linux 커널 인터페이스(`io_submit` / `io_getevents`)입니다. SPDK의 `bdev_aio` 모듈은 Linux AIO를 래핑하여 임의의 커널 블록 장치를 SPDK **bdev**로 노출합니다. 관련 항목: **bdev**, **libaio**.

### Arbitration (NVMe)
중재. 서로 다른 제출 큐(Submission Queue)의 명령이 실행되는 순서를 결정하는 NVMe 컨트롤러 내의 하드웨어 스케줄링 메커니즘입니다. NVMe는 라운드 로빈, 가중치 라운드 로빈, 벤더별 중재를 지원합니다. 관련 항목: **NVMe**, **submission queue**, **completion queue**.

### Asynchronous Callback
비동기 콜백. SPDK의 기본 I/O 완료 모델입니다. 작업이 완료될 때까지 블로킹하는 대신, 호출자가 함수 포인터(`cb_fn`)와 컨텍스트 포인터(`cb_arg`)를 제공합니다. 작업이 완료되면 SPDK가 동일한 **리액터(reactor)** 스레드에서 콜백을 호출합니다. 관련 항목: **poller**, **reactor**, **run-to-completion**.

---

## B

### bdev (Block Device)
블록 장치. SPDK의 스토리지 장치에 대한 핵심 추상화입니다. `spdk_bdev`는 논리적 블록 스토리지 엔드포인트를 나타내는 C 구조체입니다. NVMe SSD, RAM 디스크, 암호화 래퍼, 원격 iSCSI LUN 등 어떤 백엔드든 bdev로 등록될 수 있으며, 상위 계층(NVMe-oF 타겟, vhost 타겟 또는 사용자 코드)에 동일한 블록 I/O 인터페이스를 제공합니다. 관련 항목: **bdev module**, **I/O channel**, **spdk_bdev_io**.

### bdev module
bdev 모듈. bdev 레이어에 등록되는 플러그인으로, 실제 I/O를 구현합니다. 각 모듈은 `spdk_bdev_module` 구조체를 제공하며 `spdk_bdev_register()`로 하나 이상의 bdev를 노출합니다. 예: `bdev_nvme`, `bdev_null`, `bdev_malloc`, `bdev_aio`. 관련 항목: **bdev**, **I/O channel**.

### Blob
대형 이진 객체(Binary Large Object). SPDK Blobstore의 할당 단위입니다. blob은 가변 수의 **클러스터**에 걸쳐 있으며, 내부 메타데이터 페이지에 매핑을 유지합니다. BlobFS 파일 또는 논리 볼륨의 백엔드 역할을 합니다. 관련 항목: **Blobstore**, **cluster**, **BlobFS**.

### Blobstore
SPDK 내부 데이터 구조로, blob과 그 메타데이터를 관리합니다. bdev 위에 직접 동작하며, 파일시스템이나 논리 볼륨 계층이 필요 없습니다. 관련 항목: **blob**, **BlobFS**, **logical volume**.

### BlobFS
Blobstore 파일시스템. Blobstore 위에 구축된 경량 파일시스템으로, SPDK 기반 애플리케이션이 블록 대신 파일 시맨틱을 사용하도록 합니다. RocksDB와 같은 데이터베이스의 백엔드로 설계되었습니다. 관련 항목: **Blobstore**, **blob**.

---

## C

### Completion Queue (CQ)
완료 큐. NVMe 장치가 완료된 명령에 대한 상태를 게시하는 원형 버퍼입니다. 각 완료 항목에는 상태 코드(SCT, SC), 명령 ID, SQ 헤드 포인터가 포함됩니다. SPDK에서 CQ는 `spdk_nvme_qpair_process_completions()`를 통해 폴링됩니다. 관련 항목: **submission queue**, **QPair**, **polling**.

### Controller (`spdk_nvme_ctrlr`)
컨트롤러. NVMe 장치의 드라이버 수준 표현으로, 관리 큐, 식별 데이터, 파워 상태를 캡슐화합니다. SPDK에서 `spdk_nvme_probe()` 또는 `spdk_nvme_connect()`에 의해 생성됩니다. 관련 항목: **namespace**, **QPair**, **probe**.

### Cluster
클러스터. Blobstore의 할당 세분화 단위입니다. 기본값은 1 MiB이며, `spdk_bs_opts`의 `cluster_sz`로 설정할 수 있습니다. 각 blob은 하나 이상의 클러스터를 소비합니다. 관련 항목: **blob**, **Blobstore**.

### CPU Mask
CPU 마스크. SPDK 리액터에 할당할 논리 코어를 선택하는 16진수 비트마스크입니다 (명령줄에서 `-m`). 각 설정된 비트는 하나의 리액터 스레드를 생성합니다. 예: `0xf`는 코어 0-3을 선택합니다. 관련 항목: **reactor**, **DPDK**.

---

## D

### DIF (Data Integrity Field)
데이터 무결성 필드. NVMe 엔드투엔드(end-to-end) 데이터 보호의 일부로, 각 논리 블록에 추가 보호 메타데이터(가드, 애플리케이션 태그, 참조 태그)를 붙입니다. SPDK의 bdev 레이어는 DIF 삽입/제거/검증을 지원합니다. 관련 항목: **acceleration framework**, **metadata**.

### DMA (Direct Memory Access)
직접 메모리 접근. CPU 개입 없이 주변 장치와 메모리 간에 데이터를 전송하는 메커니즘입니다. SPDK에서 모든 I/O 버퍼는 DMA 안전(고정된 휴지페이지 지원)이어야 합니다. `spdk_zmalloc()` 또는 `spdk_malloc()`에 `SPDK_MALLOC_DMA` 플래그를 사용하여 할당합니다. 관련 항목: **hugepage**, **pinned memory**.

### DPDK (Data Plane Development Kit)
데이터 플레인 개발 키트. SPDK가 환경 초기화, 휴지페이지 관리, PCI 장치 접근, CPU 코어 관리에 사용하는 라이브러리 집합입니다. SPDK의 `spdk_env_init()`은 DPDK의 EAL(환경 추상화 레이어)을 초기화합니다. 관련 항목: **EAL**, **hugepage**, **reactor**.

---

## E

### EAL (Environment Abstraction Layer)
환경 추상화 레이어. DPDK의 코어 초기화 라이브러리로, 휴지페이지, PCI, CPU 코어 감지를 설정합니다. `spdk_env_init()`에 의해 호출됩니다. 관련 항목: **DPDK**, **hugepage**.

---

## F

### Fabrics
패브릭. NVMe-oF(NVMe over Fabrics)에서 NVMe 명령을 전달하는 네트워크 전송을 의미합니다. SPDK는 TCP, RDMA, VFIO-user 전송을 지원합니다. 관련 항목: **NVMe-oF**, **transport**, **trid**.

### FIO Plugin
SPDK bdev를 fio 벤치마크 도구의 I/O 엔진으로 사용하는 공유 라이브러리입니다. `--ioengine=spdk_bdev`로 호출합니다. 관련 항목: **bdev**, **performance**.

---

## H

### Hugepage
휴지페이지. 커널의 기본 페이지 크기(일반적으로 4 KiB)보다 훨씬 큰 메모리 페이지(2 MiB 또는 1 GiB)입니다. TLB 미스를 줄여 대량 메모리 매핑의 성능을 향상시킵니다. SPDK는 모든 DMA 버퍼와 내부 데이터 구조에 휴지페이지를 사용합니다. 관련 항목: **DMA**, **DPDK**, **pinned memory**.

---

## I

### I/O Channel
I/O 채널. SPDK에서 스레드별, 장치별 리소스 핸들입니다. `spdk_bdev_get_io_channel()`로 획득하며, 해당 스레드의 모든 I/O가 이 채널을 통해 제출됩니다. 각 채널에는 전용 큐, 버퍼, 폴러가 포함되어 잠금 없는(lock-free) I/O를 가능하게 합니다. 관련 항목: **bdev**, **reactor**, **lock-free**.

### IDXD (Intel Data Streaming Accelerator)
Intel 데이터 스트리밍 가속기. 데이터 복사, CRC, 비교 등의 작업을 오프로드하는 Intel 하드웨어 가속기입니다. SPDK의 가속 프레임워크(`accel`)를 통해 접근합니다. 관련 항목: **acceleration framework**, **DSA**.

### iSCSI
인터넷 소형 컴퓨터 시스템 인터페이스. TCP/IP를 통해 SCSI 명령을 전송하는 프로토콜입니다. SPDK는 고성능 iSCSI 타겟 구현을 제공합니다. 관련 항목: **target**, **LUN**.

---

## J

### JSON-RPC
SPDK 애플리케이션의 런타임 설정 및 관리 인터페이스입니다. Unix 도메인 소켓을 통해 JSON 형식 요청/응답을 교환합니다. `scripts/rpc.py`가 표준 클라이언트입니다. 관련 항목: **RPC method**, **configuration**.

---

## K

### Keep-Alive
NVMe-oF에서 호스트와 타겟 간의 연결 상태를 모니터링하는 메커니즘입니다. 타겟이 설정된 타임아웃 내에 keep-alive 명령을 수신하지 못하면 컨트롤러 연결을 끊습니다. 관련 항목: **NVMe-oF**, **SPDK_NVME_SC_KEEP_ALIVE_EXPIRED**.

---

## L

### Lock-free
잠금 없음. SPDK의 핵심 설계 원칙입니다. 뮤텍스나 스핀락 대신 스레드당 데이터 구조와 메시지 전달을 사용하여 동시성 오버헤드를 제거합니다. 각 리액터 스레드는 자체 I/O 채널과 폴러를 소유합니다. 관련 항목: **reactor**, **I/O channel**, **run-to-completion**.

### Logical Volume (lvol)
논리 볼륨. Blobstore 위에 구축된 씬 프로비저닝(thin-provisioned) 블록 장치입니다. 스냅샷(snapshot)과 클론(clone)을 지원합니다. 관련 항목: **Blobstore**, **lvol store**, **snapshot**.

### LUN (Logical Unit Number)
논리 유닛 번호. SCSI/iSCSI에서 개별 스토리지 장치를 식별하는 번호입니다. SPDK iSCSI 타겟에서 각 LUN은 bdev에 매핑됩니다. 관련 항목: **iSCSI**, **bdev**.

---

## M

### Malloc Bdev
메모리(RAM) 지원 bdev입니다. 테스트와 프로토타이핑에 유용하며, `bdev_malloc_create` RPC로 생성합니다. 데이터는 프로세스 종료 시 사라집니다. 관련 항목: **bdev**, **null bdev**.

### Metadata
메타데이터. 논리 블록에 첨부되는 추가 데이터(예: DIF 보호 정보)입니다. NVMe에서 메타데이터는 데이터와 별도 버퍼에 위치하거나(separate) 데이터 끝에 인라인으로 포함될(extended LBA) 수 있습니다. 관련 항목: **DIF**, **extended LBA**.

---

## N

### Namespace (NVMe)
네임스페이스. NVMe 컨트롤러가 노출하는 논리적 블록 주소 공간입니다. 하나의 컨트롤러에 여러 네임스페이스가 있을 수 있습니다. SPDK에서 `spdk_nvme_ns`로 표현됩니다. 관련 항목: **controller**, **NSID**, **bdev**.

### NQN (NVMe Qualified Name)
NVMe 정규화 이름. NVMe-oF에서 서브시스템을 전역적으로 고유하게 식별하는 문자열입니다. 예: `nqn.2024-01.io.spdk:cnode1`. 관련 항목: **NVMe-oF**, **subsystem**.

### Null Bdev
null bdev. 모든 쓰기를 폐기하고 읽기에 대해 0을 반환하는 bdev입니다(Linux의 `/dev/null`과 유사). 테스트와 벤치마킹에 사용됩니다. 관련 항목: **bdev**, **malloc bdev**.

### NVMe (Non-Volatile Memory Express)
비휘발성 메모리 익스프레스. SSD 접근을 위해 설계된 고성능 스토리지 프로토콜입니다. PCIe 위에서 동작하며, 64K 깊이의 큐와 병렬 I/O 경로를 제공합니다. 관련 항목: **NVMe-oF**, **controller**, **namespace**.

### NVMe-oF (NVMe over Fabrics)
NVMe over Fabrics. NVMe 프로토콜을 네트워크 전송(TCP, RDMA, FC)을 통해 확장하여 원격 스토리지에 로컬 NVMe와 동일한 시맨틱으로 접근할 수 있게 합니다. 관련 항목: **fabrics**, **transport**, **subsystem**.

---

## O

### Offload
오프로드. CPU에서 전용 하드웨어(예: DSA, GPU, NIC)로 작업을 이전하는 것입니다. SPDK의 가속 프레임워크가 오프로드 API를 제공합니다. 관련 항목: **acceleration framework**, **IDXD**.

---

## P

### Pinned Memory
고정 메모리. 운영체제가 스왑 아웃하지 않도록 물리 RAM에 잠긴 메모리입니다. DMA 전송에 필수적입니다. SPDK의 `spdk_zmalloc()`은 고정된 휴지페이지 메모리를 반환합니다. 관련 항목: **DMA**, **hugepage**.

### Poller
폴러. 리액터 스레드에서 반복적으로 호출되는 콜백 함수입니다. 바쁜 폴러(busy poller)는 매 리액터 반복마다 호출되고, 시간 기반 폴러(timed poller)는 지정된 간격으로 호출됩니다. `SPDK_POLLER_REGISTER()`로 등록합니다. 관련 항목: **reactor**, **run-to-completion**, **I/O channel**.

### Polling
폴링. 인터럽트 대신 완료 큐를 능동적으로 확인하는 I/O 완료 감지 방식입니다. SPDK의 핵심 설계 원칙으로, 인터럽트 오버헤드를 제거하고 일관된 저지연을 제공합니다. 관련 항목: **poller**, **completion queue**, **reactor**.

### Probe
탐색. NVMe 장치를 검색하고 연결하는 과정입니다. `spdk_nvme_probe()`는 사용 가능한 컨트롤러를 열거하고, 각각에 대해 콜백을 호출합니다. 관련 항목: **controller**, **attach callback**.

---

## Q

### QPair (Queue Pair)
큐 페어. 제출 큐(SQ)와 완료 큐(CQ)의 쌍입니다. NVMe에서 모든 I/O는 QPair를 통해 제출되고 완료됩니다. SPDK에서 각 QPair는 정확히 하나의 스레드가 소유합니다(잠금 없음). `spdk_nvme_ctrlr_alloc_io_qpair()`로 할당합니다. 관련 항목: **submission queue**, **completion queue**, **lock-free**.

---

## R

### Reactor
리액터. SPDK의 이벤트 루프 실행 모델입니다. 각 리액터는 하나의 CPU 코어에 고정되며, 등록된 폴러를 반복적으로 호출합니다. 리액터는 블로킹하지 않으며 실행 완료(run-to-completion) 모델을 따릅니다. 관련 항목: **poller**, **CPU mask**, **run-to-completion**.

### RDMA (Remote Direct Memory Access)
원격 직접 메모리 접근. CPU 개입 없이 한 머신의 메모리에서 다른 머신의 메모리로 데이터를 직접 전송하는 네트워크 기술입니다. SPDK NVMe-oF의 전송 옵션 중 하나입니다. 관련 항목: **NVMe-oF**, **transport**, **InfiniBand**.

### RPC Method
RPC 메서드. JSON-RPC 인터페이스에 등록된 명명된 함수입니다. 예: `bdev_null_create`, `nvmf_create_subsystem`. `SPDK_RPC_REGISTER()` 매크로로 등록합니다. 관련 항목: **JSON-RPC**, **configuration**.

### Run-to-Completion
실행 완료. 각 작업 단위(콜백)가 선점 없이 완료까지 실행되는 실행 모델입니다. 컨텍스트 스위칭 오버헤드를 제거합니다. SPDK의 리액터/폴러 모델의 기반입니다. 관련 항목: **reactor**, **poller**, **lock-free**.

---

## S

### SGL (Scatter-Gather List)
스캐터-게더 리스트. 물리적으로 불연속적인 메모리 영역을 설명하는 디스크립터 목록으로, 단일 I/O 작업이 여러 버퍼에 걸쳐 데이터를 전송할 수 있게 합니다. 관련 항목: **DMA**, **PRP**.

### Shared Memory (shm)
공유 메모리. SPDK 프로세스가 데이터를 공유하기 위해 사용하는 메커니즘입니다. `--shm-id` 옵션으로 여러 SPDK 인스턴스가 동일한 휴지페이지 영역을 공유할 수 있습니다. 관련 항목: **hugepage**, **multi-process**.

### SPDK (Storage Performance Development Kit)
스토리지 성능 개발 키트. Intel이 주도하는 오픈소스 프로젝트로, 사용자 공간에서 실행되는 고성능 스토리지 애플리케이션 개발을 위한 라이브러리와 도구를 제공합니다. 관련 항목: **user-space driver**, **DPDK**.

### Submission Queue (SQ)
제출 큐. 호스트가 NVMe 장치에 명령을 제출하는 원형 버퍼입니다. 각 항목은 64바이트 NVMe 명령입니다. 관련 항목: **completion queue**, **QPair**, **doorbell**.

### Subsystem (NVMe-oF)
서브시스템. NVMe-oF에서 하나 이상의 네임스페이스, 리스너, 허용된 호스트를 그룹화하는 논리적 단위입니다. NQN으로 식별됩니다. `spdk_nvmf_subsystem_create()`로 생성합니다. 관련 항목: **NVMe-oF**, **NQN**, **namespace**, **listener**.

---

## T

### Target
타겟. 스토리지를 네트워크를 통해 노출하는 서비스입니다. SPDK는 NVMe-oF 타겟, iSCSI 타겟, vhost 타겟을 제공합니다. 관련 항목: **NVMe-oF**, **iSCSI**, **vhost**, **initiator**.

### Thread (`spdk_thread`)
SPDK 스레드. SPDK의 경량 스레드 추상화입니다. 하나의 리액터에서 실행되며, 폴러와 I/O 채널의 소유자입니다. 스레드 간 통신은 `spdk_thread_send_msg()`를 사용합니다. 관련 항목: **reactor**, **poller**, **message passing**.

### Transport
전송. NVMe-oF에서 NVMe 명령을 전달하는 네트워크 프로토콜 구현입니다. SPDK는 TCP, RDMA, VFIO-user 전송을 지원합니다. `spdk_nvmf_transport_create_async()`로 생성합니다. 관련 항목: **NVMe-oF**, **fabrics**, **TCP**, **RDMA**.

### Transport ID (trid)
전송 ID. NVMe-oF 엔드포인트를 식별하는 구조체(`spdk_nvme_transport_id`)로, 전송 유형, 주소, 포트, 서브시스템 NQN을 인코딩합니다. 관련 항목: **NVMe-oF**, **NQN**, **transport**.

---

## U

### User-Space Driver
사용자 공간 드라이버. 커널 드라이버를 우회하고 사용자 공간에서 직접 하드웨어에 접근하는 드라이버입니다. SPDK의 핵심 설계 원칙으로, 시스템 콜 오버헤드를 제거하고 폴링 기반의 잠금 없는 I/O를 가능하게 합니다. 관련 항목: **SPDK**, **VFIO**, **UIO**.

---

## V

### VFIO (Virtual Function I/O)
가상 함수 I/O. Linux 커널이 제공하는 안전한 사용자 공간 장치 접근 프레임워크입니다. IOMMU를 사용하여 DMA를 격리합니다. SPDK가 NVMe 장치에 접근하는 데 선호하는 방법입니다. 관련 항목: **user-space driver**, **UIO**, **IOMMU**.

### vhost
가상 호스트. SPDK가 가상 머신(VM)에 고성능 블록 스토리지를 제공하는 데 사용하는 프로토콜입니다. QEMU/KVM의 virtio 백엔드로 동작합니다. 관련 항목: **target**, **virtio**, **VM**.

### virtio
가상화된 I/O를 위한 표준화된 인터페이스입니다. SPDK는 virtio-blk과 virtio-scsi 장치를 bdev로 제공하거나 소비할 수 있습니다. 관련 항목: **vhost**, **VM**.

---

## Z

### Zoned Namespace (ZNS)
존 네임스페이스. NVMe 사양의 확장으로, 네임스페이스를 순차 쓰기 영역(존)으로 분할합니다. SSD의 내부 가비지 컬렉션을 줄여 성능과 내구성을 향상시킵니다. SPDK의 bdev 레이어와 NVMe 드라이버가 ZNS를 지원합니다. 관련 항목: **namespace**, **write pointer**, **zone**.

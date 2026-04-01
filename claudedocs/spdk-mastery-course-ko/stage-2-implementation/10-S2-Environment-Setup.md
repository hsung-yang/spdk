# 모듈 10: 환경 설정(Environment Setup)

**단계**: 2 (구현)
**난이도**: 중급
**예상 소요 시간**: 2시간
**선수 과목**: 1단계 완료

**버전 이력**:
- v1.0 (2026-03-30): 최초 버전
- v1.1 (2026-03-30): 전체 의존성, 빌드, 디바이스, 문제 해결 상세 내용 추가

---

## 학습 목표

이 모듈을 마치면 다음을 할 수 있습니다:
- SPDK 개발을 위한 하드웨어 및 소프트웨어 요구 사항 평가
- `pkgdep.sh`를 사용하여 필수 및 선택적 의존성 설치
- 사용 목적에 맞는 올바른 옵션으로 SPDK 설정 및 빌드
- NVMe 및 기타 PCI 디바이스를 사용자 공간 드라이버(VFIO/UIO)에 바인딩
- 단일 노드 및 NUMA 시스템에서 휴지페이지(Hugepage) 올바르게 구성
- SPDK 테스트 스위트를 실행하여 동작 환경 검증
- 효과적인 IDE 및 디버깅 워크플로우 설정
- 가장 일반적인 빌드 및 런타임 문제 진단 및 해결
- 가상 머신 또는 컨테이너 내에서 SPDK 개발

---

## 개요

SPDK 개발 환경 설정은 일반적인 사용자 공간 라이브러리보다 더 많은 단계를 필요로 합니다. 이는 SPDK가 커널 스토리지 스택을 완전히 우회하기 때문입니다. 올바른 휴지페이지 할당, PCI 디바이스 바인딩, IOMMU 구성은 모두 단일 SPDK 애플리케이션을 실행하기 전의 전제 조건입니다. 이 모듈에서는 각 단계를 완료해야 하는 순서대로 안내하며, 각 단계가 왜 필요한지 설명하고, 각 단계가 성공했는지 확인하는 방법을 문서화합니다.

---

## 1. 시스템 요구 사항

### 1.1 하드웨어 요구 사항

| 구성 요소 | 최소 | 권장 |
|-----------|---------|-------------|
| CPU 아키텍처 | x86_64 또는 ARM64 | AVX-512 지원 x86_64 |
| RAM | 8 GB | 16 GB 이상 |
| 스토리지 | 임의의 블록 디바이스 | NVMe SSD |
| IOMMU | 필수 아님 | VFIO 사용 시 필수 |
| CPU 코어 | 2개 | 4개 이상 |

SPDK의 폴링 모드 드라이버(Polled-mode Driver)는 CPU 코어를 I/O에 완전히 전용합니다. 전용 코어란 프로덕션 워크로드에서 다른 프로세스가 해당 코어에 스케줄링되지 않아야 함을 의미합니다. 개발 및 테스트 목적으로는 코어를 공유해도 괜찮습니다.

VFIO 기반 디바이스 접근(권장 방식)의 경우, CPU와 메인보드 모두 IOMMU를 지원해야 합니다: Intel VT-d 또는 AMD-Vi. 이는 런타임에 `dmesg | grep -i iommu`로 확인됩니다.

NVMe 하드웨어는 학습에 필수는 아닙니다. SPDK에는 모든 블록 디바이스 또는 일반 파일에서 작동하는 null 블록 디바이스와 AIO bdev가 포함되어 있습니다.

### 1.2 운영 체제 요구 사항

SPDK는 Linux와 FreeBSD를 지원합니다. 이 모듈에서는 Linux에 초점을 맞춥니다.

| 구성 요소 | 최소 | 권장 |
|-----------|---------|-------------|
| 커널 버전 | 4.0 | 5.15 LTS 이상 |
| 배포판 | glibc 2.17+ 지원 배포판 | Ubuntu 22.04, Fedora 38+ |
| GCC | 7.0 | 11.0+ |
| Clang | 6.0 | 14.0+ |
| Python | 3.6 | 3.10+ |
| Git | 2.0 | 최신 버전 |
| nasm | 2.13 | 최신 버전 |

더 새로운 커널이 선호되는 이유:
- `vfio-pci` 개선으로 설정 작업이 줄어듦
- uring bdev를 위한 `io_uring` 지원 사용 가능
- IOMMU 패스스루 모드(`iommu=pt`)의 성능 향상

커널 버전 확인:
```bash
uname -r
# 출력 예: 6.8.0-45-generic
```

### 1.3 BIOS/UEFI 설정

VFIO를 사용할 계획이라면 시작 전에 BIOS/UEFI에서 다음 설정을 확인하세요:

- **Intel VT-d** 또는 **AMD-Vi**: 반드시 활성화
- **SR-IOV** (선택사항): SR-IOV NVMe 디바이스 사용 시 활성화
- **Above 4G decoding**: PCIe 디바이스가 많은 시스템에서 활성화
- **Secure Boot**: 서명되지 않은 커널 모듈을 위해 비활성화가 필요할 수 있음

---

## 2. 저장소 복제

```bash
# SPDK 복제 (서브모듈 참조는 포함하지만 내용은 아직 없음)
git clone https://github.com/spdk/spdk
cd spdk

# 모든 서브모듈 초기화 및 가져오기 (DPDK, ISA-L, OCF 등)
# 최초 실행 시 몇 분 소요될 수 있음
git submodule update --init

# git pull 후 서브모듈 업데이트:
git submodule update
```

`--init` 플래그는 서브모듈 URL이 `.gitmodules`에 기록되어 있지만 디렉터리가 이 명령이 실행될 때까지 채워지지 않으므로 최초 복제 시 필수입니다.

주요 서브모듈과 그 역할:

| 서브모듈 | 용도 |
|-----------|---------|
| `dpdk/` | DPDK: EAL, 메모리, PCI, 링 버퍼 |
| `isa-l/` | Intel ISA-L: 이레이저 코딩, CRC |
| `isa-l-crypto/` | Intel ISA-L crypto: AES-GCM 가속 |
| `ocf/` | Open CAS Framework: 캐싱 계층 |
| `libvfio-user/` | vfio-user: 미디에이티드 디바이스 프로토콜 |

---

## 3. 의존성 설치

### 3.1 pkgdep.sh 사용

SPDK는 패키지 설치를 자동화하는 `scripts/pkgdep.sh`를 제공합니다. 이 스크립트는 Linux 배포판(Ubuntu/Debian, Fedora/RHEL, openSUSE, Alpine, Arch)을 감지하고 올바른 패키지를 설치합니다.

```bash
# root 또는 sudo로 실행해야 함
# 최소한의 빌드 의존성 설치
sudo scripts/pkgdep.sh
```

선택적 의존성 그룹을 설치하기 위한 플래그를 지원합니다:

```bash
# 사용 가능한 모든 옵션 보기
sudo scripts/pkgdep.sh --help

# 개발자 도구 설치 (clang-format, lcov, gdb, valgrind 등)
sudo scripts/pkgdep.sh --developer-tools

# RDMA/RoCE 의존성 설치 (libibverbs, librdmacm)
sudo scripts/pkgdep.sh --rdma

# io_uring 지원 설치 (liburing-dev)
sudo scripts/pkgdep.sh --uring

# RBD (Ceph) bdev 의존성 설치
sudo scripts/pkgdep.sh --rbd

# 문서 빌드 의존성 설치 (doxygen, graphviz 등)
sudo scripts/pkgdep.sh --docs

# IDXD (Intel Data Streaming Accelerator) 의존성 설치
sudo scripts/pkgdep.sh --idxd

# 모든 선택적 의존성을 한 번에 설치
sudo scripts/pkgdep.sh --all
```

### 3.2 코어 의존성 (기본 설치)

`pkgdep.sh`를 플래그 없이 실행하면 다음이 설치됩니다:

- **빌드 도구**: gcc, g++, make, pkg-config, nasm
- **Python 패키지**: pyelftools (DPDK 빌드에 필요)
- **라이브러리**: libaio-dev, libssl-dev, libjson-c-dev, libcunit1-dev, uuid-dev
- **커널 헤더**: UIO 모듈 빌드를 위한 linux-headers

Ubuntu/Debian의 경우:
```bash
# pkgdep.sh가 설치하는 것 (대표적인 목록, 전체 목록은 아님)
sudo apt-get install -y \
    gcc g++ make pkg-config git nasm \
    python3 python3-pip python3-pyelftools \
    libaio-dev libssl-dev libjson-c-dev \
    libcunit1-dev uuid-dev libncurses5-dev \
    libcmocka-dev
```

### 3.3 의존성 확인

`pkgdep.sh` 실행 후 주요 도구가 존재하는지 확인합니다:

```bash
gcc --version         # GCC 7+ 출력되어야 함
nasm --version        # DPDK crypto에 필요
python3 --version     # Python 3.6+ 출력되어야 함
pkg-config --version  # configure 중 사용됨
```

---

## 4. 빌드 설정

### 4.1 기본 설정

`./configure` 스크립트가 빌드 구성을 생성합니다. bash 스크립트(autoconf 생성이 아님)이므로 옵션은 SPDK 고유입니다.

```bash
# 기본 구성: DPDK, vhost, virtio 활성화 상태로 빌드
./configure

# 사용 가능한 모든 옵션 보기
grep -E '^\s+echo " --' configure | head -60
```

### 4.2 주요 Configure 옵션

| 옵션 | 효과 |
|--------|--------|
| `--enable-debug` | `-g -O0` 추가, 어서션 활성화, 최적화 비활성화 |
| `--enable-asan` | AddressSanitizer 활성화 (메모리 오류 감지) |
| `--enable-ubsan` | UndefinedBehaviorSanitizer 활성화 |
| `--enable-coverage` | gcov 코드 커버리지 계측 활성화 |
| `--enable-lto` | 링크 타임 최적화 활성화 (빌드 느려짐, 바이너리 빨라짐) |
| `--enable-werror` | 모든 컴파일러 경고를 오류로 처리 |
| `--disable-tests` | 기능 테스트 빌드 건너뜀 |
| `--disable-unit-tests` | 단위 테스트 빌드 건너뜀 |
| `--disable-examples` | 예제 프로그램 빌드 건너뜀 |
| `--disable-apps` | 애플리케이션 바이너리 빌드 건너뜀 |
| `--with-shared` | 정적(.a) 대신 공유(.so) 라이브러리 빌드 |
| `--with-rdma` | NVMe-oF용 RDMA 트랜스포트 활성화 |
| `--with-uring` | io_uring bdev 활성화 |
| `--with-fio[=DIR]` | 벤치마킹용 fio 플러그인 빌드 |
| `--with-crypto` | ISA-L crypto vbdev 모듈 빌드 |
| `--with-rbd` | Ceph RBD bdev 모듈 빌드 |
| `--with-dpdk=DIR` | 번들 DPDK 대신 커스텀 DPDK 설치 사용 |
| `--with-vfio-user=DIR` | vfio-user 트랜스포트 활성화 |
| `--with-usdt` | 사용자 공간 DTrace/SystemTap 프로브 활성화 |
| `--with-ocf` | Open CAS Framework 캐싱 모듈 빌드 |

### 4.3 설정 예시

```bash
# 개발용 디버그 빌드 (가장 일반적인 개발 빌드)
./configure --enable-debug

# 메모리 오류 감지 포함 디버그 빌드
./configure --enable-debug --enable-asan --enable-ubsan

# 최적화 포함 프로덕션 유사 빌드
./configure --enable-lto

# RDMA 트랜스포트 포함 NVMe-oF 개발
./configure --with-rdma

# 빠른 반복을 위한 최소 빌드 (테스트, 예제 없음)
./configure --enable-debug --disable-tests --disable-examples

# io_uring bdev 지원 포함 빌드
./configure --with-uring

# 벤치마킹 지원 포함 빌드 (fio 소스 필요)
./configure --with-fio=/path/to/fio/source

# 공유 라이브러리 빌드 (동적 링킹에 필요)
./configure --with-shared
```

### 4.4 configure 실행 후

`configure`는 선택된 모든 옵션을 기록하는 `mk/config.mk`를 생성합니다. 확인할 수 있습니다:

```bash
# 선택된 설정 보기
cat mk/config.mk | grep -v '^#' | grep -v '^$'
```

configure 옵션을 변경하면 이전 설정의 오래된 오브젝트 파일을 피하기 위해 항상 먼저 `make clean`을 실행하세요:

```bash
./configure --enable-debug
make clean
make -j$(nproc)
```

---

## 5. SPDK 빌드

### 5.1 표준 빌드

```bash
# 사용 가능한 모든 CPU 코어를 사용하여 빌드
# 하드웨어와 옵션에 따라 5-15분 소요
make -j$(nproc)
```

`$(nproc)`는 논리적 CPU 코어 수로 확장됩니다. 8코어 머신에서는 `make -j8`이 됩니다.

### 5.2 빌드 출력물

빌드 성공 후:

```
build/
├── lib/
│   ├── libspdk_nvme.a        # NVMe 드라이버 라이브러리
│   ├── libspdk_bdev.a        # 블록 디바이스 추상화 레이어
│   ├── libspdk_env_dpdk.a    # DPDK 환경
│   └── ...                   # ~50개 이상의 정적 라이브러리
├── examples/
│   ├── nvme/hello_world      # 기본 NVMe 프로브 예제
│   └── ...
└── app/
    ├── spdk_tgt              # 멀티 프로토콜 스토리지 타겟
    ├── nvmf_tgt              # NVMe-oF 타겟
    └── ...
```

빌드가 라이브러리를 생성했는지 확인:

```bash
# 빌드된 라이브러리 나열 (40-60개 파일이어야 함)
ls build/lib/libspdk_*.a | wc -l

# 특정 라이브러리가 빌드되었는지 확인
ls -lh build/lib/libspdk_nvme.a
```

### 5.3 증분 빌드(Incremental Build)

소스 파일 변경 후 `make clean`이 필요하지 않습니다. 일반 `make`만으로 변경된 파일만 다시 빌드합니다:

```bash
# 소스 파일 편집 후:
make -j$(nproc)
```

`make clean`이 필요한 경우:
- `--enable-debug`와 릴리스 설정 간 전환 시
- 컴파일 플래그에 영향을 주는 configure 플래그 변경 시
- `git pull` 후 Makefile이나 헤더에 변경이 도입된 경우

### 5.4 특정 타겟 빌드

```bash
# NVMe 예제만 빌드
make -j$(nproc) examples/nvme/hello_world/hello_world

# 라이브러리만 빌드 (예제나 테스트 없음)
make -j$(nproc) libs

# 단위 테스트 빌드 및 실행
make -j$(nproc) unittest
```

---

## 6. 휴지페이지(Hugepage) 구성

### 6.1 SPDK에 휴지페이지가 필요한 이유

DPDK(SPDK의 메모리 서브시스템)는 휴지페이지를 다음 목적으로 사용합니다:
1. TLB 압력 감소: 동일한 메모리 범위에 대해 더 적은 페이지 테이블 항목
2. DMA 가능: 휴지페이지의 물리적 주소가 안정적이고 연속적
3. 페이지 폴트 방지: 휴지페이지는 물리적 메모리에 고정됨

휴지페이지 없이는 SPDK 애플리케이션이 시작 시 다음과 같은 오류로 실패합니다:
```
EAL: Not enough memory available on socket 0!
```

### 6.2 휴지페이지 크기

Linux는 x86_64에서 두 가지 휴지페이지 크기를 지원합니다:
- **2 MB** (기본값): 표준 선택, 항상 사용 가능
- **1 GB** (거대 페이지): 더 나은 성능, 커널 부트 매개변수 필요

대부분의 개발 작업에는 2 MB 휴지페이지가 충분합니다.

### 6.3 2 MB 휴지페이지 구성

```bash
# 현재 휴지페이지 상태 확인
grep -i huge /proc/meminfo

# 2048개의 휴지페이지 할당 (총 4 GB)
sudo sh -c 'echo 2048 > /proc/sys/vm/nr_hugepages'

# 할당 성공 확인
grep HugePages_Total /proc/meminfo
# 예상: HugePages_Total:     2048

# 재부팅 시에도 유지되도록 설정
echo 'vm.nr_hugepages = 2048' | sudo tee /etc/sysctl.d/99-spdk-hugepages.conf
sudo sysctl -p /etc/sysctl.d/99-spdk-hugepages.conf
```

필요한 휴지페이지 수는 워크로드에 따라 다릅니다. SPDK 기본값은 2048 MB(2 MB 페이지 1024개)입니다. 개발용으로는 1024개 페이지(2 GB)가 보통 충분합니다.

### 6.4 1 GB 휴지페이지 구성

1 GB 휴지페이지는 부팅 시 예약해야 합니다. 커널이 시작된 후에는 할당할 수 없습니다:

```bash
# /etc/default/grub의 GRUB_CMDLINE_LINUX에 추가:
# hugepagesz=1G hugepages=4
sudo vim /etc/default/grub

# GRUB 업데이트 및 재부팅
sudo update-grub   # Ubuntu/Debian
# 또는
sudo grub2-mkconfig -o /boot/grub2/grub.cfg  # Fedora/RHEL

sudo reboot

# 재부팅 후 확인:
grep -i huge /proc/meminfo
# HugePages_Total:       4
# Hugepagesize:    1048576 kB
```

### 6.5 NUMA 인식 휴지페이지 할당

NUMA 시스템(멀티 소켓 서버)에서는 `setup.sh` 환경 변수를 사용하여 특정 노드에 휴지페이지를 할당합니다:

```bash
# NUMA 노드 0에만 1024개의 휴지페이지 할당
sudo HUGENODE=0 NRHUGE=1024 scripts/setup.sh

# 노드별로 다른 수량 할당
sudo HUGENODE='nodes_hp[0]=2048,nodes_hp[1]=512,2' scripts/setup.sh
# node0: 2048 페이지, node1: 512 페이지, node2: 기본값(NRHUGE)
```

NUMA 토폴로지 확인:
```bash
# NUMA 노드 및 메모리 표시
numactl --hardware

# 어떤 휴지페이지가 어떤 노드에 있는지 확인
cat /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
```

### 6.6 Hugetlbfs 마운트

DPDK는 hugetlbfs가 마운트되어 있어야 합니다. `setup.sh`가 자동으로 처리하지만 수동으로도 마운트할 수 있습니다:

```bash
# 이미 마운트되어 있는지 확인
mount | grep hugetlbfs

# 수동 마운트 (마운트되어 있지 않은 경우)
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge

# /etc/fstab에 영구 설정
echo 'nodev /mnt/huge hugetlbfs defaults 0 0' | sudo tee -a /etc/fstab
```

---

## 7. IOMMU 구성

### 7.1 IOMMU가 중요한 이유

VFIO(Virtual Function I/O)는 PCI 디바이스를 사용자 공간에 바인딩하는 데 선호되는 드라이버입니다. 다음을 제공하기 때문입니다:
- IOMMU를 통한 DMA 보호: 사용자 공간 드라이버가 접근해서는 안 되는 메모리에 접근하는 것을 방지
- `vfio-pci` 외에 커널 모듈 불필요
- 컨테이너 및 VM과 호환

IOMMU 없이는 SPDK가 `uio_pci_generic`으로 폴백하며, 이는 DMA 격리를 제공하지 않습니다. 개발용으로는 괜찮지만 프로덕션에서는 VFIO를 사용하세요.

### 7.2 IOMMU 활성화

GRUB 설정을 편집합니다:

```bash
sudo vim /etc/default/grub
```

Intel CPU의 경우 `GRUB_CMDLINE_LINUX`을 찾아 `intel_iommu=on iommu=pt`를 추가:
```
GRUB_CMDLINE_LINUX="quiet splash intel_iommu=on iommu=pt"
```

AMD CPU의 경우:
```
GRUB_CMDLINE_LINUX="quiet splash amd_iommu=on iommu=pt"
```

`iommu=pt` 플래그는 패스스루 모드를 활성화하여 VM에 할당되지 않은 디바이스의 성능을 향상시킵니다.

```bash
# GRUB 변경 사항 적용
sudo update-grub   # Ubuntu/Debian
# 또는
sudo grub2-mkconfig -o /boot/grub2/grub.cfg  # Fedora/RHEL

sudo reboot
```

### 7.3 IOMMU 확인

```bash
# 커널이 IOMMU를 인식했는지 확인
dmesg | grep -i iommu | head -10

# Intel 예상 출력:
# DMAR: IOMMU enabled
# DMAR-IR: Enabled IRQ remapping in xapic mode

# IOMMU 그룹 존재 확인
ls /sys/kernel/iommu_groups/ | wc -l
# 0이 아닌 숫자가 출력되어야 함
```

---

## 8. setup.sh로 디바이스 바인딩

### 8.1 setup.sh가 하는 일

`scripts/setup.sh`는 SPDK 애플리케이션을 실행하기 위한 시스템 준비의 주요 도구입니다. 다음을 수행합니다:
1. 휴지페이지 할당 (HUGEMEM 또는 NRHUGE 환경 변수를 통해)
2. 필요한 커널 모듈 로드 (`vfio-pci` 또는 `uio_pci_generic`)
3. NVMe, I/OAT, VMD, Virtio PCI 디바이스 감지
4. 커널 드라이버에서 언바인드
5. `vfio-pci`에 바인드 (또는 `uio_pci_generic`을 폴백으로)

### 8.2 setup.sh 실행

```bash
# 기본값: 모든 호환 디바이스 바인딩, 2048 MB 휴지페이지 할당
sudo scripts/setup.sh

# 모든 SPDK 호환 디바이스 상태 확인
sudo scripts/setup.sh status

# 리셋: 모든 디바이스를 커널 드라이버로 되돌림
sudo scripts/setup.sh reset

# 크래시 후 고아 SPDK 파일 정리
sudo scripts/setup.sh cleanup
```

### 8.3 setup.sh 환경 변수

```bash
# 휴지페이지 메모리 크기 제어 (MB 단위, 기본값: 2048)
sudo HUGEMEM=4096 scripts/setup.sh

# 휴지페이지 수 직접 제어 (HUGEMEM을 오버라이드)
sudo NRHUGE=1024 scripts/setup.sh

# 특정 NUMA 노드에 휴지페이지 할당
sudo HUGENODE=0 scripts/setup.sh

# 휴지페이지 크기 제어 (kB 단위)
sudo HUGEPGSZ=2048 scripts/setup.sh   # 2 MB 페이지
sudo HUGEPGSZ=1048576 scripts/setup.sh # 1 GB 페이지

# 특정 PCI 디바이스만 바인딩
sudo PCI_ALLOWED="0000:01:00.0 0000:02:00.0" scripts/setup.sh

# 특정 디바이스 차단 (커널 소유 유지)
sudo PCI_BLOCKED="0000:01:00.0" scripts/setup.sh

# vfio-pci/uio_pci_generic 자동 선택 대신 특정 드라이버 강제
sudo DRIVER_OVERRIDE=uio_pci_generic scripts/setup.sh

# 휴지페이지 마운트포인트 및 vfio 그룹의 소유권 설정
sudo TARGET_USER=$USER scripts/setup.sh

# NVMe 디바이스만 바인딩 (I/OAT, VMD, Virtio 건너뜀)
sudo DEV_TYPE=NVME scripts/setup.sh
```

### 8.4 디바이스 바인딩 확인

```bash
# 바인딩 전 NVMe 디바이스 나열
lspci | grep -i nvme

# setup.sh 후 디바이스 상태 확인
sudo scripts/setup.sh status

# 출력 예:
# BDF         Vendor  Device  NUMA  Driver     Device name
# 0000:01:00.0 8086    0953    0     vfio-pci   -

# 디바이스의 드라이버 수동 확인
cat /sys/bus/pci/devices/0000:01:00.0/driver/module/drivers
```

### 8.5 수동 디바이스 바인딩 (고급)

개별 디바이스에 대한 세밀한 제어가 필요할 때:

```bash
# NVMe 디바이스의 PCI 주소 찾기
lspci | grep NVMe
# 0000:01:00.0 Non-Volatile memory controller: ...

# vfio-pci 모듈 로드
sudo modprobe vfio-pci

# 디바이스 vendor:device ID 가져오기
cat /sys/bus/pci/devices/0000:01:00.0/vendor
cat /sys/bus/pci/devices/0000:01:00.0/device

# 현재 드라이버에서 언바인드
echo "0000:01:00.0" | sudo tee /sys/bus/pci/devices/0000:01:00.0/driver/unbind

# vfio-pci에 바인드
echo "0000:01:00.0" | sudo tee /sys/bus/pci/drivers/vfio-pci/bind

# 바인딩 확인
ls -la /sys/bus/pci/devices/0000:01:00.0/driver
```

---

## 9. 환경 검증

### 9.1 hello_world로 빠른 검증

`hello_world` 예제는 NVMe 디바이스를 프로브하고 발견한 내용을 보고합니다:

```bash
# hello_world 실행 (바인딩된 NVMe 디바이스 필요)
sudo ./build/examples/nvme/hello_world

# 예상 출력 (NVMe 디바이스 1개의 경우):
# Initializing NVMe Controllers
# Attached to 0000:01:00.0
# Using controller INTEL SSDPED... (BTPY...)
# Namespace ID: 1 size: 375GB
# Initialization complete.
# Hello World!
```

NVMe 디바이스가 바인딩되어 있지 않으면 hello_world는 오류 없이 즉시 종료됩니다. 디바이스가 바인딩되었는지 확인하려면 `setup.sh status`를 사용하세요.

### 9.2 Null 블록 디바이스 사용 (NVMe 불필요)

SPDK에는 실제 하드웨어 없이 테스트할 수 있는 null bdev가 포함되어 있습니다:

```bash
# null bdev로 bdevperf 실행 (하드웨어 불필요)
sudo ./build/examples/bdev/bdevperf/bdevperf \
    -z \
    -q 128 \
    -o 4096 \
    -w randrw \
    -M 50 \
    -t 5 \
    -b Null0 \
    --json <(echo '{"subsystems":[{"subsystem":"bdev","config":[{"method":"bdev_null_create","params":{"name":"Null0","num_blocks":204800,"block_size":512}}]}]}')
```

### 9.3 전체 테스트 스위트 실행

```bash
# 단위 테스트 실행 (하드웨어 불필요, ~5-10분)
sudo ./test/unit/unittest.sh

# 기능 테스트 실행 (NVMe 하드웨어 필요, ~30-60분)
sudo ./test/nvme/nvme.sh

# 전체 테스트 스위트 실행 (느림, 하드웨어 필요)
sudo ./test/run_tests.sh
```

단위 테스트 스위트는 일부 설정에서 root 없이 실행할 수 있지만, 하드웨어 의존 테스트는 PCI 디바이스에 직접 접근하므로 항상 root가 필요합니다.

---

## 10. 개발 환경 설정

### 10.1 VSCode 구성

IntelliSense가 SPDK 및 DPDK 헤더를 찾을 수 있도록 VSCode C/C++ 구성을 만듭니다:

```json
// .vscode/c_cpp_properties.json
{
    "configurations": [
        {
            "name": "SPDK Linux",
            "includePath": [
                "${workspaceFolder}/include",
                "${workspaceFolder}/include/spdk",
                "${workspaceFolder}/include/spdk_internal",
                "${workspaceFolder}/dpdk/lib/eal/include",
                "${workspaceFolder}/dpdk/lib/eal/linux/include",
                "${workspaceFolder}/dpdk/lib/eal/x86/include",
                "${workspaceFolder}/dpdk/config",
                "${workspaceFolder}/lib"
            ],
            "defines": [
                "SPDK_CONFIG_DEBUG",
                "__linux__"
            ],
            "compilerPath": "/usr/bin/gcc",
            "cStandard": "c11",
            "intelliSenseMode": "gcc-x64",
            "compileCommands": "${workspaceFolder}/compile_commands.json"
        }
    ],
    "version": 4
}
```

정확한 IntelliSense를 위해 `compile_commands.json`을 생성합니다:

```bash
# bear (Build EAR) 설치
sudo apt-get install bear   # Ubuntu
# 또는
sudo dnf install bear       # Fedora

# compile_commands.json 생성
bear -- make -j$(nproc)
```

`compile_commands.json`이 있으면 VSCode는 각 파일에 사용된 정확한 컴파일러 플래그를 이해하여 Go-to-Definition과 Find-References가 정확해집니다.

SPDK 개발을 위한 권장 VSCode 확장:
- **C/C++** (Microsoft): IntelliSense, 디버깅
- **clangd**: Microsoft C/C++의 대안, 더 빠른 인덱싱
- **GitLens**: Blame 주석, 이력
- **Hex Editor**: 원시 I/O 버퍼 검사 시 유용

### 10.2 Vim/Neovim 구성

Vim/Neovim 사용자의 경우 `compile_commands.json`과 함께 clangd가 동일한 기능을 제공합니다:

```bash
# clangd 설치
sudo apt-get install clangd   # Ubuntu

# ~/.config/nvim/init.vim 또는 유사 파일
# nvim-lspconfig를 사용하여 clangd 구성:
# lua require('lspconfig').clangd.setup{}
```

SPDK 루트에 `.clangd` 파일을 만들어 프로젝트별 clangd 구성을 제공합니다:

```yaml
# .clangd
CompileFlags:
  CompilationDatabase: .
  Add: [-Wno-error]
```

### 10.3 코드 포매팅

SPDK는 CI에서 확인하는 엄격한 코딩 스타일을 적용합니다. 커밋 전에 포매터를 실행하세요:

```bash
# clang-format 설치 (pkgdep.sh --developer-tools 또는 수동)
sudo apt-get install clang-format

# 파일의 포매팅 확인
./scripts/check_format.sh include/spdk/nvme.h

# 전체 작업 트리 확인
./scripts/check_format.sh
```

SPDK는 저장소 루트에 커스텀 `.clang-format` 파일을 사용합니다. Linux 커널 스타일을 기반으로 SPDK 고유의 조정이 적용되어 있습니다.

### 10.4 GDB로 디버깅

```bash
# 디버그 심볼로 빌드 (유용한 GDB 출력에 필수)
./configure --enable-debug
make clean && make -j$(nproc)

# GDB에서 SPDK 애플리케이션 실행
sudo gdb --args ./build/examples/nvme/hello_world

# SPDK 개발에 유용한 GDB 명령:
(gdb) set follow-fork-mode child   # 포크된 프로세스 추적
(gdb) break spdk_nvme_probe        # SPDK 함수에 브레이크포인트 설정
(gdb) break nvme_pcie_ctrlr_construct  # 드라이버 내부에 브레이크포인트
(gdb) run
(gdb) backtrace                    # 크래시 시 콜 스택 출력
(gdb) info threads                 # 모든 스레드(리액터) 나열
(gdb) thread 2                     # 리액터 스레드로 전환
(gdb) frame 3                      # 특정 스택 프레임 검사
(gdb) print *ctrlr                 # 구조체 내용 출력
(gdb) watch ctrlr->state           # 필드에 워치포인트 설정
```

멀티 스레드 SPDK 애플리케이션에서는 각 리액터가 자체 스레드에서 실행됩니다. 크래시를 디버깅할 때 `info threads`와 `thread N`으로 각 리액터의 상태를 검사할 수 있습니다.

### 10.5 AddressSanitizer로 디버깅

메모리 버그의 경우 AddressSanitizer가 GDB보다 더 효과적입니다:

```bash
# ASan으로 빌드
./configure --enable-debug --enable-asan
make clean && make -j$(nproc)

# 정상적으로 실행 (ASan이 자동으로 잘못된 메모리 접근을 가로챔)
sudo ./build/examples/nvme/hello_world

# use-after-free 시 ASan 출력:
# ==12345==ERROR: AddressSanitizer: heap-use-after-free
# READ of size 8 at 0x602000000010 thread T0
# ...
```

참고: ASan은 상당한 오버헤드(~2배 느려짐)를 추가합니다. 성능 측정에는 사용하지 마세요.

### 10.6 Perf 및 트레이싱

```bash
# perf로 SPDK 애플리케이션 프로파일링
sudo perf record -g -p $(pgrep spdk_tgt) -- sleep 30
sudo perf report

# 특정 함수에 대한 CPU 사이클 기록
sudo perf stat -e cache-misses,cache-references,instructions \
    ./build/examples/nvme/hello_world

# SPDK는 --with-usdt로 빌드 시 USDT 프로브 지원
./configure --with-usdt
make -j$(nproc)

# bpftrace로 SPDK 이벤트 트레이싱 (--with-usdt 빌드 필요)
sudo bpftrace -e 'usdt:/path/to/app:spdk:*{ printf("%s\n", probe); }'
```

---

## 11. 일반적인 빌드 문제 및 문제 해결

### 11.1 서브모듈 미초기화

**증상**: `make` 시 dpdk/ 또는 isa-l/ 디렉터리의 헤더 누락 오류.

```
fatal error: rte_config.h: No such file or directory
```

**해결**:
```bash
git submodule update --init
```

### 11.2 Python pyelftools 누락

**증상**: DPDK 빌드 시 Python 임포트 오류.

```
ModuleNotFoundError: No module named 'elftools'
```

**해결**:
```bash
pip3 install pyelftools
# 또는 패키지 매니저를 통해:
sudo apt-get install python3-pyelftools
```

### 11.3 nasm을 찾을 수 없음

**증상**: ISA-L 또는 DPDK crypto 코드 컴파일 시 빌드 실패.

```
/bin/sh: 1: nasm: not found
```

**해결**:
```bash
sudo apt-get install nasm   # Ubuntu
sudo dnf install nasm       # Fedora
```

### 11.4 사용 가능한 휴지페이지 없음

**증상**: SPDK 애플리케이션이 즉시 종료.

```
EAL: Not enough memory available on socket 0! Requested: 2048MB
DPDK init failed with error -12
```

**해결**:
```bash
# 휴지페이지 할당
sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'

# 할당 확인 (Free가 Total과 같아야 함)
grep HugePages /proc/meminfo

# 할당 실패 시 (충분한 연속 메모리 없음), 재부팅 후 다시 시도
```

### 11.5 휴지페이지가 할당되었지만 Free가 아님

**증상**: 휴지페이지가 할당되었지만 애플리케이션 시작에 실패.

```
grep HugePages /proc/meminfo
HugePages_Total:    1024
HugePages_Free:        0   # 모든 페이지가 다른 프로세스에서 사용 중
```

**해결**: 휴지페이지를 사용하고 있는 프로세스를 찾아 종료:
```bash
# 휴지페이지를 사용하는 프로세스 찾기
sudo fuser /mnt/huge/*

# 또는 SPDK 프로세스 찾기
pgrep -a spdk
pgrep -a dpdk
```

### 11.6 VFIO 디바이스 권한 거부

**증상**: 애플리케이션이 `/dev/vfio/N`을 열 수 없음.

```
Failed to open /dev/vfio/1: Permission denied
```

**해결**:
```bash
# 옵션 1: sudo로 실행
sudo ./build/examples/nvme/hello_world

# 옵션 2: TARGET_USER로 소유권 설정
sudo TARGET_USER=$USER scripts/setup.sh

# 옵션 3: 사용자를 vfio 그룹에 추가 (그룹이 존재하는 경우)
sudo usermod -aG vfio $USER
newgrp vfio
```

### 11.7 디바이스가 사용 중 (마운트된 파일 시스템)

**증상**: `setup.sh`가 NVMe 디바이스를 건너뜀.

```
NVMe 0000:01:00.0 has active mountpoints, will not bind
```

**해결**:
```bash
# 디바이스의 파일 시스템을 찾아 마운트 해제
lsblk /dev/nvme0n1
sudo umount /dev/nvme0n1p1

# 그 다음 setup 다시 실행
sudo scripts/setup.sh
```

마운트된 파일 시스템이 있는 NVMe 디바이스를 절대 바인딩하지 마세요. 커널 드라이버가 관리하고 있으며, 강제 리바인딩은 데이터를 손상시킵니다.

### 11.8 IOMMU 미활성화

**증상**: `setup.sh`가 `vfio-pci` 대신 `uio_pci_generic`으로 폴백.

```
IOMMU not available, falling back to uio_pci_generic
```

**해결**: BIOS에서 IOMMU를 활성화하고 7.2절에 설명된 대로 커널 부트 매개변수를 추가합니다. 활성화 후 확인:

```bash
dmesg | grep -i "iommu enabled"
```

### 11.9 SPDK에 대해 애플리케이션 빌드 시 링커 오류

**증상**: 커스텀 애플리케이션 링킹 시 정의되지 않은 심볼 오류.

```
undefined reference to `spdk_nvme_probe'
```

**해결**: SPDK 라이브러리에는 올바른 순서로 나열해야 하는 내부 의존성이 있습니다. `pkg-config` 또는 SPDK 링커 플래그 헬퍼를 사용하세요:

```bash
# 올바른 링커 플래그 가져오기
SPDK_ROOT=/path/to/spdk
$(SPDK_ROOT)/scripts/pkgdep/get_ldflags.sh

# 또는 라이브러리가 설치된 경우 pkg-config 사용
pkg-config --libs spdk_nvme spdk_env_dpdk
```

---

## 12. 가상 머신 및 컨테이너 환경

### 12.1 VM에서 개발

물리적 NVMe 하드웨어를 사용할 수 없는 경우 VM에서도 SPDK 개발이 가능합니다. 두 가지 접근 방식이 있습니다:

**접근 방식 1: Null/AIO bdev (하드웨어 패스스루 없음)**

대부분의 SPDK 서브시스템은 null bdev 또는 AIO bdev(파일이나 블록 디바이스를 래핑)를 사용하여 실제 NVMe 하드웨어 없이 테스트할 수 있습니다:

```bash
# VM 내부: 특별한 설정 불필요
sudo scripts/setup.sh        # 휴지페이지 할당
make -j$(nproc)              # 정상 빌드

# 테스트에서 null bdev 사용 (하드웨어 불필요)
sudo ./build/examples/bdev/bdevperf/bdevperf -z -q 128 -o 4096 -w randread -t 5
```

**접근 방식 2: NVMe 패스스루 (실제 하드웨어 성능)**

VFIO를 사용하여 호스트에서 VM으로 NVMe 디바이스를 직접 전달:

```bash
# 호스트에서: NVMe를 vfio-pci에 바인드
sudo PCI_ALLOWED="0000:01:00.0" scripts/setup.sh

# NVMe 패스스루를 포함한 QEMU 실행
sudo qemu-system-x86_64 \
    -device vfio-pci,host=01:00.0 \
    -m 8G \
    -cpu host \
    -enable-kvm \
    ...

# VM 내부: SPDK가 디바이스를 네이티브로 인식
sudo scripts/setup.sh
sudo ./build/examples/nvme/hello_world
```

### 12.2 Docker에서 SPDK

Docker 컨테이너에서 개발 및 CI를 위해 SPDK를 실행할 수 있지만, 몇 가지 제약이 있습니다: 휴지페이지가 호스트에서 사용 가능하고 컨테이너와 공유되어야 합니다.

```dockerfile
# SPDK 개발 환경용 Dockerfile
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    git gcc g++ make python3 python3-pip \
    pkg-config nasm libaio-dev libssl-dev \
    libjson-c-dev libcunit1-dev uuid-dev \
    python3-pyelftools \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /spdk
COPY . .
RUN git submodule update --init
RUN ./configure --enable-debug
RUN make -j$(nproc)
```

휴지페이지 및 디바이스 접근 권한으로 컨테이너 실행:

```bash
# 휴지페이지 및 VFIO 접근 권한으로 컨테이너 실행
docker run -it \
    --privileged \
    --cap-add SYS_ADMIN \
    -v /dev/hugepages:/dev/hugepages \
    -v /dev/vfio:/dev/vfio \
    spdk-dev:latest \
    bash

# 컨테이너 내부: 휴지페이지는 이미 호스트에서 할당되어 있어야 함
sudo scripts/setup.sh
./build/examples/nvme/hello_world
```

`--privileged` 플래그는 PCI 디바이스 접근에 필요합니다. CI 환경에서는 특정 capability 플래그와 필요한 디바이스만 bind-mount하는 더 제한적인 접근 방식을 사용합니다.

### 12.3 Vagrant 개발 환경

SPDK에는 재현 가능한 개발 VM을 위한 Vagrant 설정이 포함되어 있습니다:

```bash
# Vagrant 및 VirtualBox 또는 libvirt 설치
# 그 다음 SPDK 소스 디렉터리에서:
cd scripts/vagrant
vagrant up

# VM에 연결
vagrant ssh

# VM 내부: SPDK 소스는 /spdk에 위치
cd /spdk
sudo scripts/setup.sh
make -j$(nproc)
./build/examples/nvme/hello_world
```

Vagrant 환경은 휴지페이지를 사전 설정하고 SPDK CI와 일치하는 안정적인 Ubuntu 기본 이미지를 제공합니다.

---

## 13. 환경 체크리스트

모듈 11로 진행하기 전에 이 체크리스트를 사용하여 환경이 완전히 구성되었는지 확인하세요.

```
시스템 요구 사항
  [ ] x86_64 또는 ARM64 CPU
  [ ] 8+ GB RAM
  [ ] Linux 커널 4.0+ (5.x 권장)
  [ ] GCC 7+ 설치됨

저장소
  [ ] git clone https://github.com/spdk/spdk
  [ ] git submodule update --init 성공 완료

의존성
  [ ] sudo scripts/pkgdep.sh 오류 없이 완료
  [ ] python3 -c "import elftools" 오류 없이 종료
  [ ] nasm --version 작동

빌드
  [ ] ./configure 완료 (mk/config.mk 존재 확인)
  [ ] make -j$(nproc) 오류 없이 완료
  [ ] ls build/lib/libspdk_nvme.a 파일 반환

휴지페이지
  [ ] grep HugePages_Free /proc/meminfo가 0보다 큰 값 표시

디바이스 접근 (실제 NVMe 사용 시)
  [ ] sudo scripts/setup.sh status가 vfio-pci 또는 uio_pci_generic에 바인딩된 디바이스 표시

검증
  [ ] sudo ./build/examples/nvme/hello_world 오류 없이 실행
```

---

## 요약

SPDK 설정은 대부분의 사용자 공간 라이브러리 설정에서 다루지 않는 시스템의 여러 레이어에 주의를 기울여야 합니다:

- **휴지페이지(Hugepage)**는 SPDK의 폴링 드라이버가 필요로 하는 DMA 가능하고 고정된 메모리를 제공합니다
- **IOMMU + VFIO**는 사용자 공간 코드에 안전하고 격리된 PCI 하드웨어 접근을 제공합니다
- **setup.sh**는 디바이스 바인딩과 휴지페이지 할당을 한 단계로 조율합니다
- **configure 플래그**를 통해 컴파일 시간 및 바이너리 크기와 디버그 기능 및 선택적 기능 간의 트레이드오프를 조절할 수 있습니다
- **bear + compile_commands.json**은 SPDK와 같은 복잡한 C 코드베이스에서 정확한 IDE 지원을 얻는 핵심입니다

동작하는 환경이 준비되면 모듈 11에서 SPDK 애플리케이션을 직접 작성하기 시작합니다.

**다음**: [11-S2-Hello-World.md](./11-S2-Hello-World.md)

---

*모듈 10 끝*

# 실습 09: NVMe-oF 이니시에이터 (Initiator)

## 개요

| 항목 | 내용 |
|-------|-------|
| **목표** | SPDK NVMe 드라이버를 사용하여 원격 NVMe-oF 타겟에 연결하고 읽기/쓰기 I/O를 수행하는 동작하는 NVMe-oF 이니시에이터(Initiator)를 구축합니다 |
| **예상 소요시간** | 3-4시간 |
| **난이도** | 중급 |
| **사전 요구사항** | EX01 (SPDK 환경 초기화), EX02 (NVMe Hello World), EX08 (NVMe-oF 타겟) 또는 이에 준하는 이해도 |

---

## 학습 목표

이 실습을 마치면 다음을 수행할 수 있게 됩니다:

1. TCP(및 선택적으로 RDMA) 전송(transport)을 위한 `spdk_nvme_transport_id`를 구성합니다.
2. `spdk_nvme_probe()`를 사용하여 원격 NVMe-oF 컨트롤러를 검색하고 연결합니다.
3. I/O QPair를 할당하고 비동기 읽기/쓰기 명령을 제출합니다.
4. 타이트 루프(tight loop)에서 완료(completion)를 올바르게 폴링합니다.
5. 디스커버리 서비스(Discovery Service) 로그 페이지를 조회하여 사용 가능한 서브시스템을 열거합니다.
6. `spdk_nvme_detach_async()`를 사용하여 연결을 깔끔하게 해제합니다.

---

## 배경

NVMe over Fabrics(NVMe-oF)는 TCP, RDMA/RoCE, 파이버 채널(Fibre Channel) 등의 네트워크 전송을 통해 NVMe 프로토콜을 확장합니다. SPDK 드라이버 관점에서 동일한 `spdk_nvme_probe()` / `spdk_nvme_connect()` API가 로컬 PCIe 장치와 원격 NVMe-oF 타겟 모두에 동작합니다 - 유일한 차이점은 제공하는 전송 ID(`trid`)입니다.

핵심 개념:

- **전송 ID (Transport ID, `spdk_nvme_transport_id`)** — 전송 유형, 주소, 포트, 서브시스템 NQN을 인코딩합니다.
- **디스커버리 서브시스템(Discovery Subsystem)** — 모든 NVMe-oF 타겟은 사용 가능한 서브시스템을 나열하는 잘 알려진 NQN(`nqn.2014-08.org.nvmexpress.discovery`)을 노출합니다.
- **QPair** — 각 I/O 큐 페어(Queue Pair)는 네임스페이스별로 독립적으로 할당됩니다. SPDK는 잠금이 없으므로(lock-free) 하나의 스레드가 하나의 QPair를 소유합니다.
- **폴링(Polling)** — SPDK는 I/O에 인터럽트를 사용하지 않습니다. 완료 콜백을 구동하려면 `spdk_nvme_qpair_process_completions()`를 호출해야 합니다.

---

## 사전 요구사항 설정

### 시스템 요구사항

```
- 휴지페이지(hugepages)가 설정된 Linux 호스트 (>= 512 MB)
- 소스에서 빌드된 SPDK (EX01 참조)
- TCP 전송용: 표준 커널 TCP 스택이면 충분
- RDMA 전송용: RDMA 지원 NIC + libibverbs 설치
```

### 휴지페이지 설정

```bash
# 2MB 휴지페이지 512 MB 분량
echo 256 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 확인
grep HugePages /proc/meminfo
```

### SPDK 빌드 (아직 수행하지 않은 경우)

```bash
cd /path/to/spdk
git submodule update --init
./configure --with-nvme-cuse   # TCP는 항상 활성화됨
make -j$(nproc)
```

---

## 파트 1: NVMe-oF 타겟 설정

이니시에이터를 테스트하려면 먼저 실행 중인 타겟이 필요합니다. 이 파트에서는 SPDK의 `nvmf_tgt` 애플리케이션을 RAM 지원 bdev와 함께 사용합니다. EX08에서 이미 타겟이 실행 중이면 파트 2로 건너뛰세요.

### 1.1 nvmf_tgt 시작

```bash
# 터미널 1 — 타겟 실행
cd /path/to/spdk
sudo ./build/bin/nvmf_tgt --wait-for-rpc &
```

### 1.2 RPC를 통한 타겟 설정

다음을 `setup_target.sh`로 저장하고 실행합니다:

```bash
#!/usr/bin/env bash
# setup_target.sh — 최소 NVMe-oF TCP 타겟 설정

SPDK_DIR="$(pwd)"
RPC="${SPDK_DIR}/scripts/rpc.py"

# 1. 64 MB malloc bdev 생성 (RAM 지원, 실제 디스크 불필요)
${RPC} bdev_malloc_create -b Malloc0 64 512

# 2. TCP 전송 생성
${RPC} nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192

# 3. NVMe-oF 서브시스템 생성
${RPC} nvmf_create_subsystem \
    nqn.2024-01.io.spdk:cnode1 \
    -a \
    -s SPDK00000000000001

# 4. bdev를 서브시스템에 연결
${RPC} nvmf_subsystem_add_ns \
    nqn.2024-01.io.spdk:cnode1 \
    Malloc0

# 5. TCP 포트 4420에 리스너 추가 (로컬 테스트용 루프백)
${RPC} nvmf_subsystem_add_listener \
    nqn.2024-01.io.spdk:cnode1 \
    -t tcp \
    -a 127.0.0.1 \
    -s 4420

echo "타겟 설정 완료. 서브시스템: nqn.2024-01.io.spdk:cnode1"
echo "127.0.0.1:4420에서 리스닝 중 (TCP)"
```

```bash
chmod +x setup_target.sh
./setup_target.sh
```

### 1.3 타겟이 리스닝 중인지 확인

```bash
# 디스커버리 서비스는 항상 동일한 전송에서 리스닝
./scripts/rpc.py nvmf_get_subsystems | python3 -m json.tool
```

디스커버리 서브시스템과 `nqn.2024-01.io.spdk:cnode1` 서브시스템 모두 표시되어야 합니다.

---

## 파트 2: NVMe-oF 이니시에이터 구축

### 2.1 애플리케이션 디렉토리 생성

```bash
mkdir -p ~/spdk_nvmf_initiator
cd ~/spdk_nvmf_initiator
```

### 2.2 이니시에이터 소스 작성

다음 내용으로 `nvmf_initiator.c`를 생성합니다:

```c
/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * EX09: NVMe-oF Initiator
 *
 * TCP(또는 RDMA)를 통해 원격 NVMe-oF 타겟에 연결하고, LBA 0에
 * 패턴을 쓴 후 다시 읽어서 정확성을 검증합니다.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/string.h"
#include "spdk/log.h"

/* ------------------------------------------------------------------ */
/*  데이터 구조체                                                       */
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

/* 쓰기 → 읽기 → 검증 체인을 위한 시퀀스 상태 */
struct io_sequence {
	struct ns_entry *ns_entry;
	char		*buf;
	int		 is_completed;  /* 0=대기중, 1=성공, 2=오류 */
};

static TAILQ_HEAD(, ctrlr_entry) g_controllers =
	TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces =
	TAILQ_HEAD_INITIALIZER(g_namespaces);

/* 명령줄 인수로부터 채워지는 전송 ID */
static struct spdk_nvme_transport_id g_trid = {};

/* 왕복 정확성을 검증하는 데 사용하는 쓰기 패턴 */
#define WRITE_PATTERN "SPDK-NVMf-EX09"

/* ------------------------------------------------------------------ */
/*  I/O 콜백                                                            */
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

	/* 데이터가 쓴 것과 일치하는지 검증 */
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
	 * 쓰기 성공. 버퍼를 제로화하고 데이터가 타겟에
	 * 도달했는지 검증하기 위해 읽기를 제출합니다.
	 */
	memset(seq->buf, 0, 0x1000);

	rc = spdk_nvme_ns_cmd_read(seq->ns_entry->ns,
				   seq->ns_entry->qpair,
				   seq->buf,
				   0,   /* LBA 시작 */
				   1,   /* LBA 수 */
				   read_complete, seq, 0);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_ns_cmd_read() failed: %d\n", rc);
		seq->is_completed = 2;
	}
}

/* ------------------------------------------------------------------ */
/*  네임스페이스 및 컨트롤러 관리                                        */
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
 * probe_cb: 검색된 컨트롤러마다 한 번 호출됩니다.
 * true를 반환하면 연결하고, false를 반환하면 건너뜁니다.
 */
static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Probe: found controller at %s (transport=%s)\n",
	       trid->traddr,
	       spdk_nvme_transport_id_trtype_str(trid->trtype));

	/*
	 * 필요시 여기서 컨트롤러 옵션을 조정합니다. 예:
	 *   opts->keep_alive_timeout_ms = 10000;
	 *   opts->num_io_queues = 4;
	 */

	return true;  /* 검색된 모든 컨트롤러에 연결 */
}

/*
 * attach_cb: 드라이버가 컨트롤러 초기화를 완료한 후 호출됩니다.
 * 모든 활성 네임스페이스를 등록합니다.
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

	/* 이 컨트롤러의 모든 활성 네임스페이스 열거 */
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
/*  I/O 실행                                                            */
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
		 * 1단계: I/O QPair 할당.
		 *
		 * 각 QPair는 정확히 하나의 스레드가 소유합니다. SPDK는
		 * 내부 잠금을 제공하지 않습니다 - 이것이 잠금 없는
		 * I/O 경로를 가능하게 합니다. NVMe-oF의 경우 QPair는
		 * 타겟 컨트롤러 큐로의 패브릭 연결에 매핑됩니다.
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
		 * 2단계: DMA 안전 버퍼 할당.
		 *
		 * spdk_zmalloc()는 고정된(pinned), 휴지페이지 지원
		 * 메모리를 반환합니다. NVMe-oF에서는 필수입니다:
		 * RDMA/TCP 스택이 커널 개입 없이 이 버퍼에 직접
		 * DMA합니다.
		 */
		seq.buf = spdk_zmalloc(0x1000,        /* 4 KB */
				       0x1000,        /* 4 KB 정렬 */
				       NULL,
				       SPDK_ENV_NUMA_ID_ANY,
				       SPDK_MALLOC_DMA);
		if (seq.buf == NULL) {
			fprintf(stderr, "spdk_zmalloc() failed\n");
			spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
			continue;
		}

		/*
		 * 3단계: LBA 0에 쓰기 제출.
		 *
		 * spdk_nvme_ns_cmd_write()는 논블로킹입니다. 명령을
		 * 큐잉하고 즉시 반환합니다. 타겟이 완료를 보내면
		 * write_complete()가 호출됩니다.
		 */
		snprintf(seq.buf, 0x1000, "%s", WRITE_PATTERN);
		seq.ns_entry    = ns_entry;
		seq.is_completed = 0;

		rc = spdk_nvme_ns_cmd_write(ns_entry->ns,
					    ns_entry->qpair,
					    seq.buf,
					    0,   /* LBA 시작 */
					    1,   /* LBA 수 */
					    write_complete, &seq, 0);
		if (rc != 0) {
			fprintf(stderr, "spdk_nvme_ns_cmd_write() failed: "
				"%d\n", rc);
			spdk_free(seq.buf);
			spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
			continue;
		}

		/*
		 * 4단계: 완료 폴링.
		 *
		 * spdk_nvme_qpair_process_completions()는 절대
		 * 블로킹하지 않습니다. 완료 큐를 확인하고, 완료된
		 * 명령에 대해 콜백을 실행한 후 반환합니다. 쓰기→읽기
		 * 체인이 완료를 알릴 때까지 루프합니다.
		 *
		 * 두 번째 인수(0)는 "사용 가능한 모든 완료를 처리"를
		 * 의미합니다. 양의 정수를 전달하면 호출당 처리 수를
		 * 제한합니다(지연에 민감한 루프에 유용).
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
		 * 5단계: 리소스 해제.
		 *
		 * QPair를 해제하기 전에 모든 진행 중인 I/O가 완료되어야
		 * 합니다. 여기서는 위 폴링 루프에 의해 보장됩니다.
		 */
		spdk_free(seq.buf);
		spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
	}
}

/* ------------------------------------------------------------------ */
/*  정리(Cleanup)                                                       */
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
		 * spdk_nvme_detach_async()는 블로킹 없이 분리 프로세스를
		 * 시작합니다. spdk_nvme_detach_poll()은 모든 대기 중인
		 * 분리가 완료될 때까지 기다립니다.
		 */
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

/* ------------------------------------------------------------------ */
/*  인수 파싱                                                           */
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
	 * 기본값: PCIe 전송. 패브릭 타겟에 연결할 때
	 * -r로 재정의됩니다.
	 */
	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s",
		 SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "r:h")) != -1) {
		switch (op) {
		case 'r':
			/*
			 * spdk_nvme_transport_id_parse()는 공백으로 구분된
			 * key:value 문자열을 이해합니다:
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
	 * 단계 A: SPDK 환경 초기화.
	 *
	 * 휴지페이지 지원 메모리, DPDK EAL, PCI 접근을 설정합니다.
	 * 다른 SPDK 함수보다 먼저 호출해야 합니다.
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
	 * 단계 B: 컨트롤러 탐색(probe).
	 *
	 * spdk_nvme_probe()는 NVMe Identify 명령(또는 NVMe-oF의 경우
	 * Fabrics Connect 명령)을 보내고 발견된 각 컨트롤러에 대해
	 * probe_cb를 호출합니다. probe_cb가 true를 반환하면 컨트롤러를
	 * 초기화하고 attach_cb를 호출합니다.
	 *
	 * 패브릭 전송의 경우 trid가 전체 연결 엔드포인트를 인코딩합니다.
	 * PCIe의 경우 trid를 NULL로 전달하여 모든 로컬 SSD를 열거할 수
	 * 있습니다.
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
	 * 단계 C: 등록된 모든 네임스페이스에서 I/O 수행.
	 */
	run_io();

	printf("\nAll I/O completed successfully.\n");

cleanup:
	cleanup();
	spdk_env_fini();
	return rc;
}
```

### 2.3 Makefile 작성

`~/spdk_nvmf_initiator/`에 `Makefile`을 생성합니다:

```makefile
# SPDK_DIR을 SPDK 소스 트리에 맞게 조정
SPDK_DIR ?= /path/to/spdk

APP = nvmf_initiator

# NVMe-oF 이니시에이터에 필요한 SPDK 라이브러리 수집
SPDK_LIB_LIST = nvme env_dpdk log util

include $(SPDK_DIR)/mk/spdk.app.mk
```

> **참고:** 이 디렉토리를 SPDK 트리 내부(예: `examples/nvme/nvmf_initiator/`)에 배치한 경우, 하드코딩된 경로 대신 `SPDK_DIR := $(SPDK_ROOT_DIR)`을 사용하세요.

### 2.4 빌드

```bash
cd ~/spdk_nvmf_initiator
make SPDK_DIR=/path/to/spdk
```

예상 출력:

```
  CC    nvmf_initiator.c
  LINK  nvmf_initiator
```

---

## 파트 3: 이니시에이터 실행

### 3.1 기본 TCP 실행

파트 1의 타겟이 아직 실행 중인 상태에서:

```bash
sudo ./nvmf_initiator \
  -r "trtype:tcp adrfam:ipv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2024-01.io.spdk:cnode1"
```

예상 출력:

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

### 3.2 대안: 단일 컨트롤러에 spdk_nvme_connect() 사용

정확히 하나의 컨트롤러가 존재한다는 것을 알고 더 간단한 API를 원하는 경우:

```c
/* main()의 probe 경로를 대체하는 드롭인 대체 코드 */
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

`spdk_nvme_connect()`는 비동기 탐색 메커니즘의 동기 래퍼입니다 - 단일 타겟 시나리오에서는 더 간단하지만, 여러 타겟에 동시에 연결할 때는 컨트롤러 리셋을 병렬화하므로 `spdk_nvme_probe()`가 선호됩니다.

---

## 파트 4: 선택 사항 - RDMA 전송

RDMA 지원 NIC가 있고 타겟에 RDMA 리스너가 설정된 경우, trid 문자열의 전송 유형만 변경하면 됩니다:

### 4.1 타겟에 RDMA 리스너 추가

```bash
./scripts/rpc.py nvmf_create_transport -t RDMA -u 131072
./scripts/rpc.py nvmf_subsystem_add_listener \
    nqn.2024-01.io.spdk:cnode1 \
    -t rdma \
    -a 192.168.1.10 \
    -s 4420
```

### 4.2 RDMA를 통한 연결

```bash
sudo ./nvmf_initiator \
  -r "trtype:rdma adrfam:ipv4 traddr:192.168.1.10 trsvcid:4420 subnqn:nqn.2024-01.io.spdk:cnode1"
```

코드 변경이 필요 없습니다 - 전송 계층은 SPDK NVMe 드라이버에 의해 완전히 추상화됩니다.

---

## 보너스 A: 디스커버리 서비스 조회

NVMe-oF 디스커버리 서브시스템(`nqn.2014-08.org.nvmexpress.discovery`)은 타겟의 모든 사용 가능한 서브시스템을 나열하는 로그 페이지를 노출합니다. 이니시에이터에 다음 함수를 추가하여 출력합니다:

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

	/* 디스커버리 서브시스템을 가리키는 trid 구성 */
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

	/* 비동기 호출이 admin 명령을 큐잉; 완료될 때까지 폴링 */
	bool done = false;
	/* 필요시 done 플래그를 작은 클로저 유사 구조체로 래핑 */
	spdk_nvme_ctrlr_get_discovery_log_page(disc_ctrlr,
					       discovery_log_cb, &done);
	while (!done) {
		spdk_nvme_ctrlr_process_admin_completions(disc_ctrlr);
	}

	spdk_nvme_detach(disc_ctrlr);
}
```

메인 탐색 루프 전에 `query_discovery_service("127.0.0.1", "4420", SPDK_NVME_TRANSPORT_TCP)`를 호출하여 사용 가능한 모든 서브시스템을 출력합니다.

---

## 보너스 B: 다중 네임스페이스 지원

서브시스템이 둘 이상의 네임스페이스를 노출할 때, `run_io()`의 `TAILQ_FOREACH` 루프가 이미 이를 처리합니다. 테스트하려면 타겟에 두 번째 bdev를 추가합니다:

```bash
./scripts/rpc.py bdev_malloc_create -b Malloc1 32 512
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2024-01.io.spdk:cnode1 Malloc1
```

이니시에이터를 재연결합니다. 두 네임스페이스(NS 1과 NS 2) 모두 등록되어 순차적으로 실행됩니다.

모든 네임스페이스에 동시에 I/O를 제출하려면 각 네임스페이스를 자체 스레드에서 실행(스레드당 하나의 QPair)하거나 SPDK의 리액터(reactor) 프레임워크를 사용해야 합니다. 해당 패턴은 EX11(Blobstore와 Reactor)에서 다룹니다.

---

## 흔한 실수와 해결 방법

### 실수 1: 전송 유형 불일치

**증상:** `spdk_nvme_probe() failed` 또는 즉시 연결 거부.

**원인:** 이니시에이터는 `trtype:tcp`를 사용하지만 타겟 리스너는 `-t rdma`로 추가되었습니다(또는 그 반대).

**해결:** 타겟의 리스너를 확인합니다:
```bash
./scripts/rpc.py nvmf_get_subsystems | python3 -m json.tool | grep -A5 listen_addresses
```
`-r` 인수의 `trtype`을 정확히 일치시킵니다.

---

### 실수 2: 잘못된 주소 또는 포트

**증상:** `spdk_nvme_probe() failed`와 함께 타임아웃, 또는 "connection refused".

**원인:** `traddr` 또는 `trsvcid`가 타겟이 실제로 리스닝하는 것과 일치하지 않습니다.

**해결:**
```bash
# 타겟이 리스닝 중인지 확인
ss -tlnp | grep 4420      # TCP용
# 또는
./scripts/rpc.py nvmf_get_subsystems
```

루프백 테스트에서는 항상 `traddr:127.0.0.1`을 사용합니다. 같은 머신에서 호스트명이나 외부 IP를 사용하면 추가 라우팅 설정이 필요할 수 있습니다.

---

### 실수 3: 잘못된 SubNQN

**증상:** 연결은 성공했지만 네임스페이스가 나타나지 않음.

**원인:** trid의 `subnqn`이 존재하지 않는 서브시스템을 가리키거나, NVM 서브시스템에 연결하려는데 `SPDK_NVMF_DISCOVERY_NQN`을 사용했습니다.

**해결:** 타겟의 RPC 출력에서 정확한 서브시스템 NQN을 복사합니다:
```bash
./scripts/rpc.py nvmf_get_subsystems | python3 -m json.tool | grep '"nqn"'
```

---

### 실수 4: I/O에 비DMA 버퍼 사용

**증상:** 세그멘테이션 폴트 또는 무음 데이터 손상.

**원인:** I/O 버퍼에 `spdk_zmalloc()` 대신 `malloc()`을 사용. NVMe 드라이버가 버퍼에 직접 DMA하므로 고정된 휴지페이지 메모리여야 합니다.

**해결:** I/O 데이터 버퍼에는 항상 `SPDK_MALLOC_DMA`와 함께 `spdk_zmalloc()`(또는 `spdk_malloc()`)을 사용합니다. `free()`가 아닌 `spdk_free()`로 해제합니다.

---

### 실수 5: 완료 폴링 누락

**증상:** `while (!seq.is_completed)` 루프에서 프로그램이 영원히 멈춤.

**원인:** `spdk_nvme_qpair_process_completions()`가 호출되지 않아 완료 콜백이 절대 실행되지 않음.

**해결:** 폴링 루프에서 I/O를 제출한 것과 동일한 QPair에, 동일한 스레드에서 `spdk_nvme_qpair_process_completions()`를 호출하는지 확인합니다.

---

### 실수 6: I/O 진행 중에 QPair 해제

**증상:** SPDK 드라이버 내부에서 크래시 또는 어서션 실패.

**원인:** 제출된 모든 명령이 완료되기 전에 `spdk_nvme_ctrlr_free_io_qpair()`를 호출.

**해결:** QPair를 해제하기 전에 항상 `is_completed != 0`까지 완료를 폴링합니다.

---

## 검증 체크리스트

이 실습을 완료로 표시하기 전에 다음 각 항목을 확인합니다:

- [ ] 타겟이 시작되고 `nvmf_get_subsystems`가 리스너가 있는 NVM 서브시스템을 표시함.
- [ ] 이니시에이터가 오류 없이 빌드됨.
- [ ] 이니시에이터가 연결되고 컨트롤러 모델/시리얼과 함께 `Attached:`를 출력함.
- [ ] 최소 하나의 네임스페이스가 등록되어 출력됨.
- [ ] 모든 네임스페이스에 대해 `[WRITE OK]`와 `[READ OK]`가 모두 나타남.
- [ ] 프로세스가 깔끔하게 종료됨 (크래시 없음, ASAN 오류 없음).
- [ ] (보너스 A) 디스커버리 로그 페이지가 올바른 서브시스템을 나열함.
- [ ] (보너스 B) 두 번째 네임스페이스를 추가하면 두 개의 I/O 시퀀스가 실행됨.

---

## 추가 참고자료

- `examples/nvme/hello_world/hello_world.c` - 이 실습의 기반으로 사용된 SPDK NVMe I/O 정식 예제.
- `app/spdk_nvme_discover/discovery_aer.c` - AER(비동기 이벤트 요청) 지원이 포함된 디스커버리 서비스 클라이언트.
- `include/spdk/nvme.h` - 전체 API 참조: `spdk_nvme_probe()`, `spdk_nvme_connect()`, `spdk_nvme_ctrlr_alloc_io_qpair()`, `spdk_nvme_ns_cmd_write()`, `spdk_nvme_ns_cmd_read()`.
- NVMe-oF 1.1 사양, 섹션 3 - Fabrics Connect 명령 및 디스커버리 서비스 프로토콜.
- SPDK 문서: https://spdk.io/doc/nvme.html

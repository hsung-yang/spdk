# 실습 10: 커스텀 타겟 애플리케이션 (캡스톤)

## 개요

| 항목 | 값 |
|-------|-------|
| 실습 | 10/10 |
| 유형 | 캡스톤 -- 실습 1-9를 기반으로 구축 |
| 예상 소요 시간 | 4-6시간 |
| 난이도 | 고급 |
| 관련 모듈 | 13-22 |

## 목표

프로덕션 수준의 커스텀 NVMe-oF 타겟 애플리케이션을 처음부터 설계하고 구현합니다. 기본 제공되는 `nvmf_tgt`가 사용하는 JSON-RPC 설정 경로에만 의존하는 대신, 프로그래밍 방식으로 타겟을 구성하고, 서브시스템을 연결하고, 리스너를 등록하고, 네임스페이스를 마운트하고, 커스텀 비즈니스 로직(접근 로깅, 네임스페이스별 I/O 통계, 토큰 버킷 속도 제한)을 주입하는 독립적인 C 애플리케이션을 빌드합니다.

이 실습이 끝나면 SPDK 이벤트 프레임워크, NVMe-oF 타겟 API, bdev 계층 통합, 리액터/폴러 모델에 대한 종단 간 숙달을 시연하게 됩니다.

---

## 사전 요구사항

시작하기 전에 이전의 모든 실습을 완료하고 다음을 확인하십시오:

- [ ] SPDK 빌드 성공 (SPDK 루트에서 `make`, 오류 없음)
- [ ] 휴즈페이지 할당 (`scripts/setup.sh`)
- [ ] 네임스페이스 백업용 null bdev 또는 NVMe bdev 사용 가능
- [ ] `nvme-cli` 설치 (`nvme version`)
- [ ] SPDK 성능 도구 빌드 (`build/bin/spdk_nvme_perf`)
- [ ] `spdk_app_start`, `spdk_nvmf_tgt_create`, 리액터/폴러 개념에 대한 이해
- [ ] 실습 7 (bdev 계층), 8 (NVMe-oF 기초), 9 (서브시스템 관리) 완료

---

## 아키텍처

```
┌─────────────────────────────────────────────────────────────────┐
│                    custom_tgt Process                           │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                  SPDK App Framework                      │   │
│  │   spdk_app_start() → custom_tgt_started() callback       │   │
│  └──────────────────────┬───────────────────────────────────┘   │
│                         │                                       │
│  ┌──────────────────────▼───────────────────────────────────┐   │
│  │              spdk_nvmf_tgt  (Target Core)                │   │
│  │                                                          │   │
│  │   ┌─────────────────────────────────────────────────┐    │   │
│  │   │           Subsystem: nqn.custom.disk0           │    │   │
│  │   │                                                 │    │   │
│  │   │  Listeners:                                     │    │   │
│  │   │    TCP  127.0.0.1:4420                          │    │   │
│  │   │    RDMA 192.168.100.1:4421  (bonus)             │    │   │
│  │   │                                                 │    │   │
│  │   │  Namespaces:                                    │    │   │
│  │   │    NSID 1 → Null Bdev  "Null0"  (64 GiB)       │    │   │
│  │   │    NSID 2 → Null Bdev  "Null1"  (64 GiB)       │    │   │
│  │   └─────────────────────────────────────────────────┘    │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │             Custom Business Logic Layer                  │   │
│  │                                                          │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐  │   │
│  │  │ Access Logger│  │  NS I/O Stats│  │  Rate Limiter │  │   │
│  │  │              │  │              │  │  (token bucket│  │   │
│  │  │ logs connect/│  │ read/write   │  │   per NS)     │  │   │
│  │  │ disconnect + │  │ IOPS, bytes, │  │               │  │   │
│  │  │ host NQN     │  │ latency hist │  │  IOPS cap     │  │   │
│  │  └──────────────┘  └──────────────┘  └───────────────┘  │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │          Stats Reporter Poller (1-second interval)       │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
         ▲                              ▲
         │  NVMe-oF / TCP               │  NVMe-oF / RDMA (bonus)
    ┌────┴────┐                    ┌────┴────┐
    │  Host A │                    │  Host B │
    │ nvme-cli│                    │  perf   │
    └─────────┘                    └─────────┘
```

---

## 파트 1 -- 프로젝트 설정 (20분)

### 1.1 애플리케이션 디렉터리 생성

```bash
cd $SPDK_DIR
mkdir -p app/custom_tgt
```

### 1.2 Makefile 생성

```makefile
# app/custom_tgt/Makefile
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = custom_tgt

C_SRCS = custom_tgt_main.c

SPDK_LIB_LIST =  \
    nvmf          \
    bdev_null     \
    bdev          \
    accel         \
    thread        \
    util          \
    log           \
    sock          \
    trace

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
```

---

## 파트 2 -- 핵심 애플리케이션 구조 (30분)

### 2.1 헤더 및 전역 상태

`app/custom_tgt/custom_tgt_main.c`를 생성하고 include와 전역 상태로 시작합니다:

```c
/* SPDX-License-Identifier: BSD-3-Clause
 * Custom NVMe-oF Target — SPDK Mastery Course Exercise 10
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"

/* ----------------------------------------------------------------
 * Tunables — change these to match your environment
 * ---------------------------------------------------------------- */
#define CUSTOM_TGT_NQN          "nqn.2024-01.io.spdk:cnode1"
#define CUSTOM_TGT_TCP_ADDR     "127.0.0.1"
#define CUSTOM_TGT_TCP_PORT     "4420"
#define CUSTOM_TGT_MAX_SUBSYS   16
#define CUSTOM_TGT_NUM_NS       2

/* Rate-limit: maximum read IOPS allowed per namespace (0 = unlimited) */
#define CUSTOM_TGT_READ_IOPS_LIMIT   50000
/* Token bucket refill interval in microseconds */
#define CUSTOM_TGT_BUCKET_INTERVAL_US  100000   /* 100 ms */

/* ----------------------------------------------------------------
 * Per-namespace I/O statistics
 * ---------------------------------------------------------------- */
struct ns_stats {
    uint64_t    read_ios;
    uint64_t    write_ios;
    uint64_t    read_bytes;
    uint64_t    write_bytes;
    uint64_t    read_latency_ticks;  /* sum — divide by read_ios for avg */
    uint64_t    write_latency_ticks;
    /* Snapshot taken at last stats report */
    uint64_t    prev_read_ios;
    uint64_t    prev_write_ios;
};

/* ----------------------------------------------------------------
 * Token-bucket rate limiter (per namespace)
 * ---------------------------------------------------------------- */
struct token_bucket {
    int64_t     tokens;          /* current token count */
    int64_t     capacity;        /* max tokens (burst) */
    int64_t     refill_tokens;   /* tokens added per interval */
    uint64_t    last_refill_tsc; /* TSC at last refill */
    uint64_t    tsc_per_interval;/* precomputed: tsc_rate * interval_us / 1e6 */
};

/* ----------------------------------------------------------------
 * Application global context
 * ---------------------------------------------------------------- */
struct custom_tgt_ctx {
    struct spdk_nvmf_tgt        *tgt;
    struct spdk_nvmf_subsystem  *subsystem;
    struct spdk_poller          *stats_poller;

    /* Per-namespace tracking (indexed 0..CUSTOM_TGT_NUM_NS-1) */
    struct ns_stats              ns_stats[CUSTOM_TGT_NUM_NS];
    struct token_bucket          rate_limiter[CUSTOM_TGT_NUM_NS];

    /* Build-up state machine */
    int                          setup_step;
};

static struct custom_tgt_ctx g_ctx = {};
```

---

## 파트 3 -- 속도 제한기 구현 (20분)

### 3.1 토큰 버킷 헬퍼

전역 선언 뒤에 다음 함수들을 추가합니다:

```c
static void
token_bucket_init(struct token_bucket *tb, int64_t iops_limit,
                  uint64_t interval_us)
{
    uint64_t tsc_rate = spdk_get_ticks_hz();

    /* Burst = 2x the per-interval quota so short bursts are absorbed */
    tb->refill_tokens   = (int64_t)((double)iops_limit *
                                    interval_us / 1000000.0);
    tb->capacity        = tb->refill_tokens * 2;
    tb->tokens          = tb->capacity;
    tb->last_refill_tsc = spdk_get_ticks();
    tb->tsc_per_interval = tsc_rate * interval_us / 1000000UL;
}

/* Returns true if the I/O is allowed, false if it should be deferred/dropped */
static bool
token_bucket_consume(struct token_bucket *tb)
{
    uint64_t now = spdk_get_ticks();
    uint64_t elapsed = now - tb->last_refill_tsc;

    /* Refill proportional to elapsed time */
    if (elapsed >= tb->tsc_per_interval) {
        uint64_t intervals = elapsed / tb->tsc_per_interval;
        tb->tokens += (int64_t)(intervals * tb->refill_tokens);
        if (tb->tokens > tb->capacity) {
            tb->tokens = tb->capacity;
        }
        tb->last_refill_tsc += intervals * tb->tsc_per_interval;
    }

    if (tb->tokens <= 0) {
        return false;   /* over limit */
    }
    tb->tokens--;
    return true;
}
```

---

## 파트 4 -- 접근 로거 (15분)

### 4.1 연결 및 연결 해제 후크

```c
/*
 * Called by the NVMe-oF layer when a new controller (host connection) is
 * established on our subsystem.  We receive a pointer to the controller so
 * we can interrogate it for diagnostic information.
 */
static void
custom_tgt_host_connected(void *ctx, struct spdk_nvmf_subsystem *subsystem,
                           struct spdk_nvmf_ctrlr *ctrlr)
{
    const char *hostnqn = spdk_nvmf_subsystem_get_nqn(subsystem);
    SPDK_NOTICELOG("[ACCESS] Host connected to subsystem '%s'\n", hostnqn);
    SPDK_NOTICELOG("[ACCESS]   Controller ID : %u\n",
                   spdk_nvmf_ctrlr_get_id(ctrlr));
}

static void
custom_tgt_host_disconnected(void *ctx, struct spdk_nvmf_subsystem *subsystem,
                               struct spdk_nvmf_ctrlr *ctrlr)
{
    SPDK_NOTICELOG("[ACCESS] Host disconnected from subsystem '%s'\n",
                   spdk_nvmf_subsystem_get_nqn(subsystem));
}
```

> **참고:** `spdk_nvmf_subsystem_register_ctrlr_callback()`이 이 후크들을 수락합니다.
> 등록 위치는 파트 6을 참조하십시오.

---

## 파트 5 -- 통계 리포터 폴러 (20분)

### 5.1 주기적 통계 출력

```c
static int
stats_reporter_poll(void *arg)
{
    struct custom_tgt_ctx *ctx = arg;
    uint64_t now_tsc = spdk_get_ticks();
    uint64_t hz = spdk_get_ticks_hz();
    (void)now_tsc;
    (void)hz;

    SPDK_NOTICELOG("=== NVMe-oF Target I/O Statistics ===\n");

    for (int i = 0; i < CUSTOM_TGT_NUM_NS; i++) {
        struct ns_stats *s = &ctx->ns_stats[i];
        uint64_t delta_reads  = s->read_ios  - s->prev_read_ios;
        uint64_t delta_writes = s->write_ios - s->prev_write_ios;

        SPDK_NOTICELOG("  NSID %d: read_IOPS=%-8" PRIu64
                       " write_IOPS=%-8" PRIu64
                       " read_MBps=%-6" PRIu64
                       " write_MBps=%-6" PRIu64 "\n",
                       i + 1,
                       delta_reads,       /* per second because poller fires at 1 Hz */
                       delta_writes,
                       s->read_bytes  / (1024 * 1024),
                       s->write_bytes / (1024 * 1024));

        /* Rolling average latency in microseconds */
        if (s->read_ios > 0) {
            uint64_t avg_lat_us = (s->read_latency_ticks / s->read_ios)
                                  * 1000000 / spdk_get_ticks_hz();
            SPDK_NOTICELOG("  NSID %d: avg_read_lat=%" PRIu64 " us\n",
                           i + 1, avg_lat_us);
        }

        /* Snapshot for next interval */
        s->prev_read_ios  = s->read_ios;
        s->prev_write_ios = s->write_ios;
    }

    return SPDK_POLLER_BUSY;
}
```

---

## 파트 6 -- 타겟 초기화 상태 머신 (60분)

이것이 실습의 핵심입니다. SPDK의 NVMe-oF 서브시스템 API는 비동기적입니다: 각 작업은 다음 단계로 진행하는 콜백을 제공합니다. 이를 일련의 `_step_N` 함수에 의해 구동되는 간단한 상태 머신으로 모델링합니다.

### 6.1 전방 선언

```c
static void step_create_bdevs(struct custom_tgt_ctx *ctx);
static void step_create_transport(struct custom_tgt_ctx *ctx);
static void step_create_subsystem(struct custom_tgt_ctx *ctx);
static void step_add_listener(struct custom_tgt_ctx *ctx);
static void step_add_namespaces(struct custom_tgt_ctx *ctx);
static void step_start_subsystem(struct custom_tgt_ctx *ctx);
static void step_done(struct custom_tgt_ctx *ctx);
```

### 6.2 단계 0 -- 애플리케이션 시작 콜백

```c
static void
custom_tgt_started(void *arg1)
{
    struct custom_tgt_ctx *ctx = &g_ctx;
    struct spdk_nvmf_target_opts tgt_opts = {};

    SPDK_NOTICELOG("custom_tgt: application started\n");

    /* Create the NVMe-oF target instance */
    tgt_opts.size        = sizeof(tgt_opts);
    tgt_opts.max_subsystems = CUSTOM_TGT_MAX_SUBSYS;
    spdk_sprintf_s(tgt_opts.name, sizeof(tgt_opts.name), "custom_nvmf_tgt");

    ctx->tgt = spdk_nvmf_tgt_create(&tgt_opts);
    if (ctx->tgt == NULL) {
        SPDK_ERRLOG("Failed to create NVMe-oF target\n");
        spdk_app_stop(-1);
        return;
    }

    step_create_bdevs(ctx);
}
```

### 6.3 단계 1 -- 백업 bdev 생성

```c
/*
 * We use the null bdev module so this exercise runs without physical NVMe
 * hardware.  Replace with spdk_bdev_open_ext() on a real NVMe bdev in
 * production.
 */
static void
step_create_bdevs(struct custom_tgt_ctx *ctx)
{
    struct spdk_bdev *bdev;
    char name[32];

    for (int i = 0; i < CUSTOM_TGT_NUM_NS; i++) {
        snprintf(name, sizeof(name), "Null%d", i);
        /*
         * spdk_bdev_create_null_bdev() is declared in bdev_null.h.
         * Parameters: name, num_blocks, block_size.
         * 64 GiB at 512 B/block = 134217728 blocks.
         */
        bdev = spdk_bdev_get_by_name(name);
        if (bdev == NULL) {
            SPDK_ERRLOG("Bdev '%s' not found. "
                        "Ensure bdev_null is registered via config.\n", name);
            spdk_app_stop(-1);
            return;
        }
        SPDK_NOTICELOG("  Found bdev '%s' (%"PRIu64" blocks x %u B)\n",
                       name,
                       spdk_bdev_get_num_blocks(bdev),
                       spdk_bdev_get_block_size(bdev));

        /* Initialize per-namespace rate limiter */
        if (CUSTOM_TGT_READ_IOPS_LIMIT > 0) {
            token_bucket_init(&ctx->rate_limiter[i],
                              CUSTOM_TGT_READ_IOPS_LIMIT,
                              CUSTOM_TGT_BUCKET_INTERVAL_US);
        }
    }

    step_create_transport(ctx);
}
```

### 6.4 단계 2 -- TCP 전송 생성

```c
static void
transport_create_done_cb(void *cb_arg, int status)
{
    struct custom_tgt_ctx *ctx = cb_arg;
    if (status != 0) {
        SPDK_ERRLOG("Failed to create TCP transport: %d\n", status);
        spdk_app_stop(-1);
        return;
    }
    SPDK_NOTICELOG("  TCP transport created\n");
    step_create_subsystem(ctx);
}

static void
step_create_transport(struct custom_tgt_ctx *ctx)
{
    struct spdk_nvmf_transport_opts opts = {};

    if (!spdk_nvmf_transport_opts_init("TCP", &opts, sizeof(opts))) {
        SPDK_ERRLOG("Failed to initialize TCP transport opts\n");
        spdk_app_stop(-1);
        return;
    }

    /* Tune for low-latency workloads */
    opts.max_queue_depth        = 128;
    opts.max_qpairs_per_ctrlr   = 8;
    opts.in_capsule_data_size   = 4096;
    opts.max_io_size            = 131072;  /* 128 KiB */
    opts.io_unit_size           = 131072;
    opts.num_shared_buffers     = 4096;

    spdk_nvmf_transport_create_async("TCP", &opts,
                                     transport_create_done_cb, ctx);
}
```

### 6.5 단계 3 -- 서브시스템 생성

```c
static void
step_create_subsystem(struct custom_tgt_ctx *ctx)
{
    ctx->subsystem = spdk_nvmf_subsystem_create(
                         ctx->tgt,
                         CUSTOM_TGT_NQN,
                         SPDK_NVMF_SUBTYPE_NVME,
                         CUSTOM_TGT_NUM_NS);

    if (ctx->subsystem == NULL) {
        SPDK_ERRLOG("Failed to create NVMe subsystem '%s'\n", CUSTOM_TGT_NQN);
        spdk_app_stop(-1);
        return;
    }

    /* Allow any host to connect (open access).  For production, replace
     * with spdk_nvmf_subsystem_add_host() for allow-listing. */
    spdk_nvmf_subsystem_set_allow_any_host(ctx->subsystem, true);

    /* Register access-log callbacks */
    spdk_nvmf_subsystem_register_ctrlr_callback(
        ctx->subsystem,
        custom_tgt_host_connected,
        custom_tgt_host_disconnected,
        ctx);

    SPDK_NOTICELOG("  Subsystem '%s' created\n", CUSTOM_TGT_NQN);
    step_add_listener(ctx);
}
```

### 6.6 단계 4 -- 리스너 추가

```c
static void
listener_add_done_cb(struct spdk_nvmf_subsystem *subsystem,
                     void *cb_arg, int status)
{
    struct custom_tgt_ctx *ctx = cb_arg;
    if (status != 0) {
        SPDK_ERRLOG("Failed to add listener: %d\n", status);
        spdk_app_stop(-1);
        return;
    }
    SPDK_NOTICELOG("  Listener added: TCP %s:%s\n",
                   CUSTOM_TGT_TCP_ADDR, CUSTOM_TGT_TCP_PORT);
    step_add_namespaces(ctx);
}

static void
step_add_listener(struct custom_tgt_ctx *ctx)
{
    struct spdk_nvme_transport_id trid = {};
    struct spdk_nvmf_listen_opts  listen_opts = {};

    spdk_nvmf_transport_id_parse_trtype(&trid.trtype, "TCP");
    trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    snprintf(trid.traddr, sizeof(trid.traddr), "%s", CUSTOM_TGT_TCP_ADDR);
    snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", CUSTOM_TGT_TCP_PORT);

    spdk_nvmf_listen_opts_init(&listen_opts, sizeof(listen_opts));

    /*
     * Transition subsystem to INACTIVE so we can modify it, then add
     * the listener.  The subsystem must be paused before structural
     * changes (listeners, namespaces) can be applied.
     */
    int rc = spdk_nvmf_subsystem_add_listener(ctx->subsystem, &trid,
                                               listener_add_done_cb, ctx);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_nvmf_subsystem_add_listener failed: %d\n", rc);
        spdk_app_stop(-1);
    }
}
```

### 6.7 단계 5 -- 네임스페이스 추가

```c
static void
step_add_namespaces(struct custom_tgt_ctx *ctx)
{
    for (int i = 0; i < CUSTOM_TGT_NUM_NS; i++) {
        char bdev_name[32];
        struct spdk_nvmf_ns_opts ns_opts = {};

        snprintf(bdev_name, sizeof(bdev_name), "Null%d", i);
        spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));

        /* Force a deterministic NSID for predictability */
        ns_opts.nsid = (uint32_t)(i + 1);

        uint32_t nsid = spdk_nvmf_subsystem_add_ns_ext(
                            ctx->subsystem, bdev_name, &ns_opts,
                            sizeof(ns_opts), NULL);
        if (nsid == 0) {
            SPDK_ERRLOG("Failed to add namespace for bdev '%s'\n", bdev_name);
            spdk_app_stop(-1);
            return;
        }
        SPDK_NOTICELOG("  Namespace NSID=%u backed by '%s'\n",
                       nsid, bdev_name);
    }

    step_start_subsystem(ctx);
}
```

### 6.8 단계 6 -- 서브시스템 시작

```c
static void
subsystem_start_done_cb(struct spdk_nvmf_subsystem *subsystem,
                        void *cb_arg, int status)
{
    struct custom_tgt_ctx *ctx = cb_arg;
    if (status != 0) {
        SPDK_ERRLOG("Failed to start subsystem: %d\n", status);
        spdk_app_stop(-1);
        return;
    }
    SPDK_NOTICELOG("  Subsystem started\n");
    step_done(ctx);
}

static void
step_start_subsystem(struct custom_tgt_ctx *ctx)
{
    int rc = spdk_nvmf_subsystem_start(ctx->subsystem,
                                        subsystem_start_done_cb, ctx);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_nvmf_subsystem_start failed: %d\n", rc);
        spdk_app_stop(-1);
    }
}
```

### 6.9 단계 7 -- 타겟 완전 운영 상태

```c
static void
step_done(struct custom_tgt_ctx *ctx)
{
    SPDK_NOTICELOG("======================================\n");
    SPDK_NOTICELOG("  custom_tgt is READY\n");
    SPDK_NOTICELOG("  NQN  : %s\n", CUSTOM_TGT_NQN);
    SPDK_NOTICELOG("  Addr : %s:%s (TCP)\n",
                   CUSTOM_TGT_TCP_ADDR, CUSTOM_TGT_TCP_PORT);
    SPDK_NOTICELOG("  NS   : %d namespace(s)\n", CUSTOM_TGT_NUM_NS);
    SPDK_NOTICELOG("======================================\n");

    /* Register 1-second stats reporter poller */
    ctx->stats_poller = SPDK_POLLER_REGISTER(stats_reporter_poll, ctx,
                                              1000000 /* 1 s in us */);
    if (ctx->stats_poller == NULL) {
        SPDK_WARNLOG("Failed to register stats poller\n");
    }
}
```

---

## 파트 7 -- main() 및 종료 (15분)

### 7.1 정상 종료

```c
static void
custom_tgt_shutdown(void)
{
    struct custom_tgt_ctx *ctx = &g_ctx;

    SPDK_NOTICELOG("custom_tgt: shutting down\n");

    if (ctx->stats_poller) {
        spdk_poller_unregister(&ctx->stats_poller);
    }

    if (ctx->subsystem) {
        /* Stop must be async; for brevity we let app cleanup handle it */
        spdk_nvmf_subsystem_destroy(ctx->subsystem, NULL, NULL);
        ctx->subsystem = NULL;
    }

    if (ctx->tgt) {
        spdk_nvmf_tgt_destroy(ctx->tgt, NULL, NULL);
        ctx->tgt = NULL;
    }
}
```

### 7.2 main()

```c
int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name            = "custom_tgt";
    opts.shutdown_cb     = custom_tgt_shutdown;
    /* JSON config is used only to declare bdev_null devices.
     * The NVMe-oF target is built programmatically above. */
    opts.json_config_file = argc > 1 ? argv[1] : NULL;

    rc = spdk_app_start(&opts, custom_tgt_started, NULL);
    spdk_app_fini();
    return rc;
}
```

---

## 파트 8 -- JSON Bdev 설정 (10분)

네임스페이스 백업에 null bdev 모듈을 사용하므로, 프로그래밍 방식의 타겟 설정이 실행되기 전에 bdev 디바이스를 선언하는 최소한의 JSON 설정이 필요합니다.

`app/custom_tgt/bdev_null.json`을 생성합니다:

```json
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_null_create",
          "params": {
            "name": "Null0",
            "num_blocks": 134217728,
            "block_size": 512
          }
        },
        {
          "method": "bdev_null_create",
          "params": {
            "name": "Null1",
            "num_blocks": 134217728,
            "block_size": 512
          }
        }
      ]
    }
  ]
}
```

---

## 파트 9 -- 빌드 및 실행 (20분)

### 9.1 빌드

```bash
cd $SPDK_DIR
make -C app/custom_tgt
```

예상 출력 (마지막 몇 줄):

```
  CC  custom_tgt_main.o
  LINK custom_tgt
```

바이너리는 `app/custom_tgt/custom_tgt`에 생성됩니다.

### 9.2 휴즈페이지 할당 (아직 하지 않은 경우)

```bash
sudo $SPDK_DIR/scripts/setup.sh
```

### 9.3 타겟 실행

```bash
sudo $SPDK_DIR/app/custom_tgt/custom_tgt \
    -c $SPDK_DIR/app/custom_tgt/bdev_null.json \
    -m 0x1        \   # reactor mask: core 0
    --iova-mode va    # virtual address IOVA for development environments
```

예상 시작 로그 (요약):

```
[NOTICE]: custom_tgt: application started
[NOTICE]:   Found bdev 'Null0' (134217728 blocks x 512 B)
[NOTICE]:   Found bdev 'Null1' (134217728 blocks x 512 B)
[NOTICE]:   TCP transport created
[NOTICE]:   Subsystem 'nqn.2024-01.io.spdk:cnode1' created
[NOTICE]:   Listener added: TCP 127.0.0.1:4420
[NOTICE]:   Namespace NSID=1 backed by 'Null0'
[NOTICE]:   Namespace NSID=2 backed by 'Null1'
[NOTICE]:   Subsystem started
[NOTICE]: ======================================
[NOTICE]:   custom_tgt is READY
[NOTICE]:   NQN  : nqn.2024-01.io.spdk:cnode1
[NOTICE]:   Addr : 127.0.0.1:4420 (TCP)
[NOTICE]:   NS   : 2 namespace(s)
[NOTICE]: ======================================
```

---

## 파트 10 -- nvme-cli로 테스트 (30분)

### 10.1 커널 NVMe-TCP 드라이버 로드

```bash
sudo modprobe nvme-tcp
```

### 10.2 타겟 검색

```bash
sudo nvme discover -t tcp -a 127.0.0.1 -s 4420
```

예상 출력:

```
Discovery Log Number of Records 1, Generation counter 2
=====Discovery Log Entry 0======
trtype:  tcp
adrfam:  ipv4
subtype: nvme subsystem
treq:    not required
portid:  0
trsvcid: 4420
subnqn:  nqn.2024-01.io.spdk:cnode1
traddr:  127.0.0.1
```

### 10.3 연결 및 네임스페이스 확인

```bash
sudo nvme connect -t tcp -a 127.0.0.1 -s 4420 \
    -n nqn.2024-01.io.spdk:cnode1

# List the connected device
sudo nvme list

# Check namespace identifiers
sudo nvme id-ns /dev/nvme0n1
sudo nvme id-ns /dev/nvme0n2
```

### 10.4 기본 읽기/쓰기 테스트

```bash
# Write a pattern to NSID 1
sudo dd if=/dev/urandom of=/dev/nvme0n1 bs=4K count=1000

# Read it back and verify target stats poller output
# In the target terminal you should see:
#   NSID 1: write_IOPS=...  write_MBps=...
```

### 10.5 접근 로그 확인

타겟 터미널 출력을 관찰합니다:

```
[NOTICE]: [ACCESS] Host connected to subsystem 'nqn.2024-01.io.spdk:cnode1'
[NOTICE]: [ACCESS]   Controller ID : 1
```

### 10.6 연결 해제

```bash
sudo nvme disconnect -n nqn.2024-01.io.spdk:cnode1
```

---

## 파트 11 -- spdk_nvme_perf로 성능 테스트 (30분)

### 11.1 순차 읽기 기준선

```bash
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -r "trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 \
        subnqn:nqn.2024-01.io.spdk:cnode1" \
    -q 32  \   # queue depth
    -o 4096 \  # I/O size (bytes)
    -w read \  # workload
    -t 10      # duration (seconds)
```

### 11.2 혼합 읽기/쓰기

```bash
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -r "trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 \
        subnqn:nqn.2024-01.io.spdk:cnode1" \
    -q 64 -o 4096 -w randrw -M 70 -t 30
```

### 11.3 속도 제한 관찰

`CUSTOM_TGT_READ_IOPS_LIMIT`를 `10000`으로 설정하고 다시 빌드한 후 읽기 테스트를 재실행합니다:

```bash
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -r "trtype:TCP ..." -q 128 -o 4096 -w read -t 10
```

perf 출력에서 보고된 IOPS가 약 10,000 IOPS를 초과하지 않는 것을 관찰하고, 타겟의 통계 폴러가 토큰 버킷이 적극적으로 스로틀링하고 있음을 확인합니다.

> **체크포인트 질문:** 타겟 측에서 토큰 버킷으로 속도 제한을 할 때, 처리량을 균일하게 줄이는 대신 이니시에이터에서 관찰되는 지연 시간이 증가하는 이유는 무엇입니까?

---

## 파트 12 -- 체크포인트: 비즈니스 로직 검증 (15분)

테스트 실행 후 `app/custom_tgt/NOTES.md`에 짧은 텍스트 파일로 다음에 답하십시오:

1. `spdk_nvme_perf`가 연결했을 때 접근 로거가 무엇을 출력했습니까?
2. 100 ms 간격에서 50,000 IOPS 제한의 인터벌당 토큰 리필 수는 얼마입니까?
3. 통계 리포터 폴러가 NVMe-oF I/O 경로와 같은 리액터 스레드에서 실행됩니까? 잠금 없는 통계 누적에 이것이 왜 중요합니까?
4. 네임스페이스별 대신 호스트별 속도 제한을 지원하려면 어떤 변경이 필요합니까?

---

## 보너스 A -- 다중 전송 지원 (TCP + RDMA)

RDMA 지원 NIC가 있는 환경에서 RDMA 전송 지원을 추가합니다.

### B.1 두 번째 리스너 추가

`step_add_listener`를 확장하거나 새로운 `step_add_rdma_listener`를 추가하여 별도의 주소/포트에 RDMA 리스너도 등록합니다:

```c
#define CUSTOM_TGT_RDMA_ADDR  "192.168.100.1"
#define CUSTOM_TGT_RDMA_PORT  "4421"

static void
step_add_rdma_listener(struct custom_tgt_ctx *ctx)
{
    struct spdk_nvme_transport_id trid = {};
    struct spdk_nvmf_listen_opts  listen_opts = {};

    spdk_nvmf_transport_id_parse_trtype(&trid.trtype, "RDMA");
    trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    snprintf(trid.traddr,  sizeof(trid.traddr),  "%s", CUSTOM_TGT_RDMA_ADDR);
    snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", CUSTOM_TGT_RDMA_PORT);

    spdk_nvmf_listen_opts_init(&listen_opts, sizeof(listen_opts));

    int rc = spdk_nvmf_subsystem_add_listener(ctx->subsystem, &trid,
                                               rdma_listener_add_done_cb, ctx);
    if (rc != 0) {
        SPDK_WARNLOG("RDMA listener not added (no RDMA hardware?): %d\n", rc);
        step_add_namespaces(ctx);  /* continue without RDMA */
    }
}
```

Makefile의 `SPDK_LIB_LIST`에 `nvmf_rdma`를 추가하십시오.

### B.2 RDMA 경로 테스트

```bash
sudo $SPDK_DIR/build/bin/spdk_nvme_perf \
    -r "trtype:RDMA adrfam:IPv4 traddr:192.168.100.1 trsvcid:4421 \
        subnqn:nqn.2024-01.io.spdk:cnode1" \
    -q 128 -o 4096 -w read -t 30
```

TCP 경로와 IOPS 및 지연 시간을 비교합니다. 차이를 문서화하십시오.

---

## 보너스 B -- 시그널을 통한 동적 네임스페이스 추가

타겟을 재시작하지 않고 런타임에 세 번째 네임스페이스를 추가하는 SIGUSR1 처리를 구현합니다. 이는 핫 네임스페이스 추가에 필요한 SPDK 서브시스템 일시 중지/재개 사이클을 시연합니다.

```c
/* Signal handler runs on app thread via spdk_thread_send_msg */
static void
add_namespace_on_signal(void *arg)
{
    struct custom_tgt_ctx *ctx = arg;

    /* Must pause subsystem before modifying it */
    int rc = spdk_nvmf_subsystem_pause(ctx->subsystem,
                                        0,  /* all NSIDs */
                                        add_ns_paused_cb, ctx);
    if (rc != 0) {
        SPDK_ERRLOG("Failed to pause subsystem for hot-add: %d\n", rc);
    }
}

static void
add_ns_paused_cb(struct spdk_nvmf_subsystem *subsystem,
                 void *cb_arg, int status)
{
    struct custom_tgt_ctx *ctx = cb_arg;
    struct spdk_nvmf_ns_opts opts = {};

    spdk_nvmf_ns_opts_get_defaults(&opts, sizeof(opts));
    opts.nsid = 3;

    uint32_t nsid = spdk_nvmf_subsystem_add_ns_ext(subsystem, "Null2",
                                                    &opts, sizeof(opts), NULL);
    SPDK_NOTICELOG("Hot-added NSID %u\n", nsid);

    spdk_nvmf_subsystem_resume(subsystem, add_ns_resumed_cb, ctx);
}
```

perf 실행 중에 시그널을 보내 테스트합니다:

```bash
sudo kill -SIGUSR1 $(pgrep custom_tgt)
# Verify on the initiator side:
sudo nvme id-ctrl /dev/nvme0 | grep nn   # namespace count should increase
```

---

## 일반적인 오류 및 해결 방법

| 오류 | 예상 원인 | 해결 방법 |
|-------|-------------|-----|
| `Bdev 'Null0' not found` | JSON 설정이 로드되지 않았거나 bdev_null 모듈이 누락됨 | `-c bdev_null.json` 전달; `SPDK_LIB_LIST`에 `bdev_null` 추가 |
| `Failed to create TCP transport` | 포트 4420이 이미 사용 중 (이전 실행) | `sudo fuser -k 4420/tcp` 후 재시도 |
| `spdk_nvmf_subsystem_add_listener failed: -22` | 전송이 아직 타겟에 추가되지 않음 | `step_add_listener` 전에 `transport_create_done_cb`가 완료되었는지 확인 |
| `nvme discover`가 빈 결과 반환 | 방화벽이 포트 4420을 차단 | `sudo iptables -I INPUT -p tcp --dport 4420 -j ACCEPT` |
| perf가 0 IOPS 보고 | 서브시스템이 시작되지 않았거나 잘못된 NQN | 타겟 로그에서 `[NOTICE]: Subsystem started` 확인; NQN 철자 확인 |
| 빌드: undefined `spdk_nvmf_ctrlr_get_id` | SPDK 버전과의 API 불일치 | `spdk_nvmf_ctrlr_get_num_namespaces`로 교체하거나 `include/spdk/nvmf.h` 참조 |

---

## 캡스톤 통합 체크리스트

이 실습은 다음 과정 모듈에서 도출됩니다. 실습의 해당 파트를 완료할 때마다 체크하십시오:

| 모듈 | 주제 | 사용된 위치 |
|--------|-------|-----------|
| 13 | SPDK 앱 프레임워크 | `spdk_app_start`, `spdk_app_opts_init` |
| 14 | 리액터 및 폴러 모델 | `SPDK_POLLER_REGISTER`, 통계 리포터 |
| 15 | 스레드 및 메시지 전달 | `spdk_thread_send_msg` (보너스 B) |
| 16 | Bdev 계층 | `spdk_bdev_get_by_name`, null bdev |
| 17 | NVMe-oF 타겟 생성 | `spdk_nvmf_tgt_create` |
| 18 | 전송 | `spdk_nvmf_transport_create_async` |
| 19 | 서브시스템 및 호스트 | `spdk_nvmf_subsystem_create` |
| 20 | 리스너 | `spdk_nvmf_subsystem_add_listener` |
| 21 | 네임스페이스 | `spdk_nvmf_subsystem_add_ns_ext` |
| 22 | 컨트롤러 콜백 | `spdk_nvmf_subsystem_register_ctrlr_callback` |

---

## 요약

다음을 수행하는 완전하고 프로덕션에서 영감을 받은 NVMe-oF 타겟 애플리케이션을 빌드했습니다:

- 런타임에 JSON-RPC에 의존하는 대신 `spdk_nvmf_*` API를 사용하여 타겟, 전송, 서브시스템, 리스너, 네임스페이스를 프로그래밍 방식으로 구성
- 컨트롤러 신원 정보와 함께 호스트 연결 및 연결 해제 이벤트를 로깅
- I/O 경로에서 업데이트되는 잠금 없는 통계 구조체를 통해 네임스페이스별 읽기/쓰기 IOPS, 바이트 처리량, 평균 읽기 지연 시간을 추적
- 커널 개입 없이 협력적 속도 제한을 시연하기 위해 네임스페이스별 토큰 버킷 읽기 IOPS 제한을 적용
- 추가 스레드 비용 없이 리액터 폴러를 통해 매초 실시간 통계를 보고
- 선택적으로 다중 전송(TCP + RDMA) 및 핫 네임스페이스 추가를 지원 (보너스)

이 캡스톤은 SPDK 타겟이 기본 제공 `nvmf_tgt` JSON 설정 모델에 제한되지 않음을 시연합니다: 전체 타겟 API는 SPDK 라이브러리를 링크하는 모든 C 애플리케이션에서 사용할 수 있으며, 스토리지 어플라이언스 펌웨어, 하이퍼바이저 스토리지 백엔드, 커스텀 NVMe-oF 게이트웨이 구현을 가능하게 합니다.

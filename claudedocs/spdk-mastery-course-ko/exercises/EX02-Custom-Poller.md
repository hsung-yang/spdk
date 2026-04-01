# 실습 02: 커스텀 폴러

**단계**: 2 (구현)
**난이도**: 중급
**예상 소요 시간**: 1-2시간
**사전 요구사항**: 모듈 12 (이벤트 프레임워크), 실습 01 (Hello SPDK App)

---

## 목표

두 개의 폴러(poller)를 등록하는 독립 실행형 SPDK 애플리케이션을 작성합니다:

1. 매 초마다 실행되어 CPU 틱 카운트와 시뮬레이션된 메모리 사용량 통계를 로깅하는 **타이머 폴러(timed poller)**.
2. 매 리액터 반복마다 실행(주기 = 0)되어 호출 횟수를 세고 정해진 사이클 수 이후 스스로를 해제하는 **바쁜 폴러(busy poller)**.

이 실습을 마치면 다음을 이해하게 됩니다:
- 폴러의 등록, 일시 중지, 재개, 해제 방법
- 타이머 폴러와 바쁜 폴러의 차이점
- `SPDK_POLLER_IDLE`과 `SPDK_POLLER_BUSY`를 올바르게 반환하는 방법
- 폴러 컨텍스트 데이터의 수명 규칙

---

## 배경 지식: 폴러 유형

SPDK 폴러는 SPDK 스레드에 등록된 콜백입니다. 리액터는 매 루프 반복마다(또는 최소 간격 후에) 폴러를 호출합니다. 두 가지 유형이 있습니다:

| 유형 | `period_microseconds` | 동작 |
|------|-----------------------|------|
| **바쁜 폴러(Busy poller)** | `0` | 매 리액터 루프 반복마다 호출됨. 지연 시간이 중요한 핫 패스에 사용. |
| **타이머 폴러(Timed poller)** | `> 0` | 지정된 간격보다 더 자주 호출되지 않음. 백그라운드 유지보수 작업에 사용. |

두 유형 모두 `enum spdk_thread_poller_rc`를 반환합니다:

```c
SPDK_POLLER_IDLE  // No work was done this invocation
SPDK_POLLER_BUSY  // Work was done; hint to reactor that we are active
```

반환 값은 리액터의 인터럽트 모드(interrupt mode) 로직에 대한 힌트입니다. 실제 작업을 수행했을 때 `SPDK_POLLER_BUSY`를 반환하면 SPDK가 인터럽트 모드로 전환하지 않고 폴링 모드를 유지할 수 있습니다.

---

## 단계별 안내

### 1단계: 소스 파일 생성

```bash
mkdir -p /path/to/your/spdk-exercises/ex02
cd /path/to/your/spdk-exercises/ex02
```

아래 해답 섹션에 표시된 내용으로 `custom_poller.c`를 생성하세요.

### 2단계: 통계 폴러 콜백 작성

통계 폴러(stats poller)는 1,000,000마이크로초(1초)마다 실행됩니다. 내부에서:
- `spdk_get_ticks()`를 호출하여 현재 CPU 틱 카운터를 얻습니다.
- `spdk_get_ticks_hz()`를 호출하여 틱 주파수를 얻어 경과 시간(초)을 계산합니다.
- 메모리 사용량 값을 시뮬레이션합니다 (실제 애플리케이션에서는 `/proc/self/status`를 읽을 것입니다).
- `SPDK_NOTICELOG`로 값을 로깅합니다.
- 실제 작업을 수행했으므로 `SPDK_POLLER_BUSY`를 반환합니다.

### 3단계: 바쁜 폴러 콜백 작성

바쁜 폴러(busy poller)는 매 리액터 반복마다 실행됩니다. 내부에서:
- 컨텍스트 구조체에 저장된 카운터를 증가시킵니다.
- 카운터가 임계값(예: 1,000,000회 반복)에 도달하면 메시지를 로깅하고 `spdk_poller_unregister(&ctx->busy_poller)`를 호출하여 스스로를 해제합니다.
- 카운팅 중에는 `SPDK_POLLER_BUSY`를, 더 이상 할 일이 없으면 `SPDK_POLLER_IDLE`을 반환합니다.

### 4단계: 앱 시작 콜백에서 폴러 연결

`app_started()` 내부에서 두 폴러를 모두 등록합니다:

```c
ctx->stats_poller = SPDK_POLLER_REGISTER(stats_poller_cb, ctx, 1000000);
ctx->busy_poller  = SPDK_POLLER_REGISTER(busy_poller_cb,  ctx, 0);
```

`SPDK_POLLER_REGISTER`는 `spdk_poller_register_named`를 호출하면서 자동으로 함수 이름을 폴러 이름으로 사용하는 편의 매크로입니다 -- `spdk_top`이나 리액터 통계를 읽을 때 유용합니다.

### 5단계: 정상적인 종료

시그널 핸들러를 등록하거나 타이머 폴러를 사용하여 몇 초 후에 `spdk_app_stop(0)`을 호출하면 실습이 깔끔하게 종료됩니다.

---

## 해답 코드

```c
/*
 * custom_poller.c - EX02: Custom Poller
 *
 * Demonstrates timed pollers and busy pollers in an SPDK application.
 * Build with the SPDK app framework (see Makefile below).
 */

#include "spdk/stdinc.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/util.h"

/* How many reactor iterations the busy poller runs before self-unregistering */
#define BUSY_POLLER_MAX_ITERS  1000000

/* How many seconds the stats poller runs before the app stops */
#define STATS_POLLER_MAX_TICKS 10

struct poller_ctx {
    struct spdk_poller *stats_poller;
    struct spdk_poller *busy_poller;

    /* Busy-poller state */
    uint64_t busy_count;

    /* Stats-poller state */
    uint64_t stats_fire_count;
    uint64_t start_ticks;
};

/* ------------------------------------------------------------------ */
/* Timed poller: fires once per second                                  */
/* ------------------------------------------------------------------ */
static int
stats_poller_cb(void *arg)
{
    struct poller_ctx *ctx = arg;
    uint64_t now      = spdk_get_ticks();
    uint64_t hz       = spdk_get_ticks_hz();
    uint64_t elapsed  = (now - ctx->start_ticks);      /* in ticks       */
    double   elapsed_s = (double)elapsed / (double)hz; /* in seconds     */

    ctx->stats_fire_count++;

    /* Simulate a memory reading; replace with a real /proc/self/status
     * parse in production code.                                          */
    uint64_t simulated_rss_kb = 4096 + (ctx->stats_fire_count * 128);

    SPDK_NOTICELOG("[stats #%lu] elapsed=%.2fs  ticks=%lu  hz=%lu  "
                   "sim_rss=%lu kB\n",
                   ctx->stats_fire_count, elapsed_s,
                   now, hz, simulated_rss_kb);

    /* Stop the application after STATS_POLLER_MAX_TICKS seconds */
    if (ctx->stats_fire_count >= STATS_POLLER_MAX_TICKS) {
        SPDK_NOTICELOG("Stats poller reached limit, stopping application.\n");
        spdk_app_stop(0);
    }

    return SPDK_POLLER_BUSY;
}

/* ------------------------------------------------------------------ */
/* Busy poller: runs every reactor iteration (period = 0)              */
/* ------------------------------------------------------------------ */
static int
busy_poller_cb(void *arg)
{
    struct poller_ctx *ctx = arg;

    ctx->busy_count++;

    if (ctx->busy_count >= BUSY_POLLER_MAX_ITERS) {
        SPDK_NOTICELOG("[busy] Reached %lu iterations — unregistering.\n",
                       ctx->busy_count);
        /*
         * Self-unregister: passing a pointer-to-pointer causes
         * spdk_poller_unregister to also NULL out ctx->busy_poller,
         * preventing double-free.
         */
        spdk_poller_unregister(&ctx->busy_poller);
        return SPDK_POLLER_IDLE;
    }

    /* Log a progress message every 100,000 iterations */
    if (ctx->busy_count % 100000 == 0) {
        SPDK_NOTICELOG("[busy] iteration %lu / %d\n",
                       ctx->busy_count, BUSY_POLLER_MAX_ITERS);
    }

    return SPDK_POLLER_BUSY;
}

/* ------------------------------------------------------------------ */
/* Application entry point (called by SPDK after subsystem init)       */
/* ------------------------------------------------------------------ */
static void
app_started(void *arg)
{
    struct poller_ctx *ctx = arg;

    ctx->start_ticks = spdk_get_ticks();
    ctx->busy_count  = 0;
    ctx->stats_fire_count = 0;

    SPDK_NOTICELOG("Application started. Registering pollers...\n");

    /*
     * SPDK_POLLER_REGISTER is preferred over spdk_poller_register because
     * it records the function name as the poller name, making it visible in
     * spdk_top and the JSON-RPC thread_get_stats output.
     */
    ctx->stats_poller = SPDK_POLLER_REGISTER(stats_poller_cb, ctx,
                                              1000000 /* 1 second */);
    if (ctx->stats_poller == NULL) {
        SPDK_ERRLOG("Failed to register stats poller\n");
        spdk_app_stop(1);
        return;
    }

    ctx->busy_poller = SPDK_POLLER_REGISTER(busy_poller_cb, ctx, 0);
    if (ctx->busy_poller == NULL) {
        SPDK_ERRLOG("Failed to register busy poller\n");
        spdk_poller_unregister(&ctx->stats_poller);
        spdk_app_stop(1);
        return;
    }

    SPDK_NOTICELOG("Both pollers registered successfully.\n");
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    struct poller_ctx    ctx  = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name        = "custom_poller";
    opts.reactor_mask = "0x1"; /* Use only CPU 0 to keep output readable */

    rc = spdk_app_parse_args(argc, argv, &opts, "", NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    rc = spdk_app_start(&opts, app_started, &ctx);

    spdk_app_fini();
    return rc;
}
```

---

## Makefile

같은 디렉토리에 `Makefile`로 저장하세요. `SPDK_DIR`을 SPDK 빌드 루트로 수정하세요.

```makefile
SPDK_DIR ?= $(HOME)/spdk

APP = custom_poller
SRCS = custom_poller.c

include $(SPDK_DIR)/mk/spdk.common.mk
include $(SPDK_DIR)/mk/spdk.app.mk

SPDK_LIB_LIST = event log thread util env_dpdk

include $(SPDK_DIR)/mk/spdk.app_vars.mk
include $(SPDK_DIR)/mk/spdk.deps.mk
```

---

## 빌드 및 실행

```bash
# 빌드 (실습 디렉토리에서)
make SPDK_DIR=/path/to/spdk

# 실행 -- root이거나 hugepage 권한이 필요
sudo ./custom_poller --no-pci -m 256 --huge-dir /dev/hugepages

# 또는 최소 DPDK 설정으로:
sudo ./custom_poller --no-pci --iova-mode=pa
```

---

## 예상 출력

```
[2024-01-15 10:00:00.001] NOTICE: Application started. Registering pollers...
[2024-01-15 10:00:00.001] NOTICE: Both pollers registered successfully.
[2024-01-15 10:00:00.001] NOTICE: [busy] iteration 100000 / 1000000
[2024-01-15 10:00:00.002] NOTICE: [busy] iteration 200000 / 1000000
...
[2024-01-15 10:00:00.004] NOTICE: [busy] iteration 1000000 / 1000000
[2024-01-15 10:00:00.004] NOTICE: [busy] Reached 1000000 iterations — unregistering.
[2024-01-15 10:00:01.001] NOTICE: [stats #1] elapsed=1.00s  ticks=2400000000  hz=2400000000  sim_rss=4224 kB
[2024-01-15 10:00:02.001] NOTICE: [stats #2] elapsed=2.00s  ticks=4800000000  hz=2400000000  sim_rss=4352 kB
...
[2024-01-15 10:00:10.001] NOTICE: [stats #10] elapsed=10.00s  ticks=24000000000  hz=2400000000  sim_rss=5376 kB
[2024-01-15 10:00:10.001] NOTICE: Stats poller reached limit, stopping application.
```

주요 관찰 사항:
- 바쁜 폴러는 첫 번째 통계 폴러 틱 전에 수천 번 실행됩니다.
- 바쁜 폴러는 깔끔하게 자체 해제됩니다 (크래시 없음, 메모리 누수 없음).
- 통계 폴러는 약 1초 간격으로 실행됩니다.
- `spdk_get_ticks_hz()`는 틱 주파수를 제공합니다 -- 틱을 hz로 나누면 초가 됩니다.

---

## 보너스: 주기적으로 bdev I/O 통계 수집

세 번째 폴러를 추가하여 라이브 bdev I/O 통계를 쿼리하는 것으로 실습을 확장하세요. 이를 위해서는 bdev가 존재해야 합니다 (테스트용으로 `null` bdev를 사용하세요).

### JSON 설정으로 null bdev 추가 (`config.json`)

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
            "num_blocks": 102400,
            "block_size": 512
          }
        }
      ]
    }
  ]
}
```

### bdev 통계 폴러 추가 사항

다음 헤더를 추가하세요:

```c
#include "spdk/bdev.h"
```

`poller_ctx`에 추가하세요:

```c
    struct spdk_poller *bdev_stats_poller;
    struct spdk_io_channel *bdev_ch;
    struct spdk_bdev_desc *bdev_desc;
```

폴러 콜백을 추가하세요:

```c
static int
bdev_stats_poller_cb(void *arg)
{
    struct poller_ctx *ctx = arg;

    if (ctx->bdev_ch == NULL) {
        return SPDK_POLLER_IDLE;
    }

    struct spdk_bdev_io_stat stat = {};
    /*
     * spdk_bdev_get_io_stat() is asynchronous in newer SPDK versions;
     * for local stats on a single channel use spdk_bdev_io_stat_add()
     * after polling the channel.  The pattern below uses the synchronous
     * channel-local stat that is always safe to read on the same thread
     * that owns the channel.
     */
    spdk_bdev_get_device_stat(spdk_bdev_desc_get_bdev(ctx->bdev_desc),
                              &stat, NULL, NULL);

    SPDK_NOTICELOG("[bdev_stats] bytes_read=%lu  bytes_written=%lu  "
                   "num_read_ops=%lu  num_write_ops=%lu\n",
                   stat.bytes_read, stat.bytes_written,
                   stat.num_read_ops, stat.num_write_ops);

    return SPDK_POLLER_BUSY;
}
```

`app_started()`에서 등록하세요:

```c
    /* Open the null bdev (created via JSON config) */
    int open_rc = spdk_bdev_open_ext("Null0", false, NULL, NULL,
                                     &ctx->bdev_desc);
    if (open_rc == 0) {
        ctx->bdev_ch = spdk_bdev_get_io_channel(ctx->bdev_desc);
        ctx->bdev_stats_poller = SPDK_POLLER_REGISTER(bdev_stats_poller_cb,
                                                       ctx, 2000000 /* 2s */);
    } else {
        SPDK_WARNLOG("Null0 bdev not found — skipping bdev stats poller\n");
    }
```

설정 파일과 함께 실행하세요:

```bash
sudo ./custom_poller --no-pci -c config.json
```

---

## 흔한 실수

### 1. SPDK 스레드 외부에서 폴러 등록

`spdk_poller_register()`와 `SPDK_POLLER_REGISTER`는 SPDK 스레드에서 실행되는 함수에서 호출해야 합니다 (예: `spdk_app_start`에 전달된 콜백 내부 또는 `spdk_thread_send_msg`를 통해 전송된 메시지 내부). `main()`에서 직접 호출하면 assert가 발생하거나 조용히 실패합니다.

```c
/* 잘못된 예 — SPDK 리액터가 실행되기 전에 호출 */
int main(...) {
    spdk_app_opts_init(&opts, sizeof(opts));
    my_poller = spdk_poller_register(cb, NULL, 1000000); /* 크래시/assert */
    spdk_app_start(&opts, app_started, NULL);
}

/* 올바른 예 — app_started 콜백 내부에서 호출 */
static void app_started(void *arg) {
    my_poller = SPDK_POLLER_REGISTER(cb, arg, 1000000); /* 안전 */
}
```

### 2. 다른 스레드에서 폴러 해제

폴러는 등록한 것과 동일한 SPDK 스레드에서 해제해야 합니다. 다른 스레드에서 `spdk_poller_unregister`를 호출하면 정의되지 않은 동작이 발생합니다. 필요한 경우 `spdk_thread_send_msg`를 사용하여 올바른 스레드로 해제 호출을 디스패치하세요.

### 3. 등록 후 NULL 검사를 잊음

`SPDK_POLLER_REGISTER`와 `spdk_poller_register`는 실패 시 `NULL`을 반환합니다. 포인터를 사용하기 전에 항상 반환 값을 확인하세요.

### 4. 해제 후 해제된 컨텍스트 접근

폴러 컨텍스트(`arg`)는 사용자 코드가 소유합니다. `spdk_poller_unregister`를 호출한 후에는 콜백이 더 이상 호출되지 않지만, 진행 중인 호출보다 컨텍스트가 더 오래 존재하도록 해야 합니다. 가장 안전한 패턴은 폴러 자체가 아닌 종료 콜백에서만 컨텍스트를 해제하는 것입니다.

### 5. 잘못된 poller_rc 값 반환

의미 있는 작업을 수행했을 때는 항상 `SPDK_POLLER_BUSY`를, 할 일이 없었을 때는 `SPDK_POLLER_IDLE`을 반환하세요. 대부분 유휴 상태인 폴러에서 무조건 `SPDK_POLLER_BUSY`를 반환하면 리액터가 인터럽트 모드로 진입하지 못하고 CPU 사이클을 낭비합니다.

### 6. 폴러 내부에서 벽시계 슬립 사용

폴러 내부에서 절대로 `sleep()`, `usleep()` 또는 블로킹 함수를 호출하지 마세요. 이는 전체 리액터 스레드를 차단하여 이벤트 기반 모델의 목적을 무력화합니다. 대신 원하는 간격의 타이머 폴러를 사용하세요.

---

## 주요 API 레퍼런스

| 함수 | 설명 |
|------|------|
| `SPDK_POLLER_REGISTER(fn, arg, period_us)` | 현재 스레드에 이름이 지정된 폴러 등록 |
| `spdk_poller_register(fn, arg, period_us)` | 폴러 등록 (자동 이름 없음) |
| `spdk_poller_register_named(fn, arg, period_us, name)` | 명시적 이름으로 폴러 등록 |
| `spdk_poller_unregister(struct spdk_poller **)` | 해제하고 포인터를 NULL로 설정 |
| `spdk_poller_pause(struct spdk_poller *)` | 폴러를 일시적으로 중지 |
| `spdk_poller_resume(struct spdk_poller *)` | 일시 중지된 폴러 재개 |
| `spdk_get_ticks()` | 현재 CPU 틱 카운터 |
| `spdk_get_ticks_hz()` | CPU 틱 주파수 (초당 틱 수) |

---

## 요약

이 실습에서 수행한 내용:

1. `SPDK_POLLER_REGISTER`를 `period_microseconds = 1000000`으로 사용하여 1초 간격으로 시스템 통계를 로깅하는 타이머 폴러를 생성했습니다.
2. 정해진 반복 횟수 이후 자체 해제하는 바쁜 폴러(`period_microseconds = 0`)를 생성했습니다.
3. `spdk_get_ticks()`와 `spdk_get_ticks_hz()`를 사용하여 블로킹 없이 경과 시간을 계산했습니다.
4. 올바른 반환 값(`SPDK_POLLER_BUSY` / `SPDK_POLLER_IDLE`)과 이것이 리액터 효율성에 중요한 이유를 배웠습니다.
5. (보너스) 라이브 bdev 채널에서 bdev I/O 통계를 수집하는 패턴으로 확장했습니다.

여기서 연습한 폴러 패턴은 모든 SPDK 서브시스템의 기반입니다 -- bdev 드라이버, NVMf 타겟, 네트워크 스택 모두 완료 폴링 루프에 동일한 `spdk_poller_register` 메커니즘을 사용합니다.

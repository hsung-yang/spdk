# 실습 03: 간단한 I/O 애플리케이션

**단계**: 2 (구현)
**난이도**: 중급
**예상 소요 시간**: 2-3시간
**사전 요구사항**: 모듈 01-13, 실습 01, 실습 02

---

## 목표

bdev에 대해 순차(sequential) 및 랜덤(random) I/O 작업을 수행하는 완전한 SPDK 애플리케이션을 작성합니다. bdev를 열고, DMA 가능 버퍼를 할당하고, 데이터를 쓰고, 다시 읽어서 정확성을 검증합니다. 이 실습을 마치면 SPDK 전체에서 사용되는 비동기 콜백 모델(async callback model)을 보여주는 동작하는 I/O 루프를 갖게 됩니다.

---

## 학습 내용

- `spdk_bdev_open_ext()`로 bdev 디스크립터(descriptor) 열기
- `spdk_bdev_get_io_channel()`로 스레드별 I/O 채널(I/O channel) 얻기
- `spdk_dma_malloc()`으로 DMA 가능 버퍼 할당
- `spdk_bdev_write_blocks()` / `spdk_bdev_read_blocks()`로 쓰기 및 읽기 I/O 제출
- `spdk_bdev_io_completion_cb`으로 비동기 I/O 완료 처리
- `spdk_bdev_free_io()`로 `spdk_bdev_io` 객체 해제
- 깔끔한 정리 (채널, 디스크립터, 앱)

---

## 배경 지식

SPDK bdev 계층은 기반 전송 방식(NVMe, AIO, Malloc 등)에 관계없이 균일한 블록 디바이스 인터페이스를 제공합니다. 모든 I/O는 비동기적으로 제출되며, I/O가 완료되면 동일한 리액터 스레드에서 콜백이 호출됩니다. 이는 **콜백 내에서 절대 블로킹해서는 안 된다**는 것을 의미합니다; 완료된 I/O에 의존하는 모든 작업은 콜백 자체에서 구동되어야 합니다.

모든 I/O 작업의 패턴은 다음과 같습니다:

```
spdk_bdev_open_ext()        -> descriptor
spdk_bdev_get_io_channel()  -> io_channel   (per-thread)
spdk_dma_malloc()           -> buf          (DMA-safe memory)

spdk_bdev_write_blocks(desc, ch, buf, lba, num_blocks, write_cb, ctx)
  -- write_cb called on completion --
spdk_bdev_read_blocks(desc, ch, buf, lba, num_blocks, read_cb, ctx)
  -- read_cb called on completion --

spdk_bdev_free_io()         (inside every callback)
spdk_put_io_channel()
spdk_bdev_close()
spdk_app_stop()
```

---

## 사전 요구사항 체크리스트

시작 전에 확인하세요:

- [ ] SPDK가 빌드됨: `ls $SPDK_DIR/build/lib/libspdk_bdev.so` (또는 `.a`)
- [ ] Hugepage가 설정됨: `cat /proc/meminfo | grep HugePages_Total`
- [ ] SPDK 리액터 스레드를 이해함 (모듈 07)
- [ ] 모듈 13 (Bdev 계층)을 읽음

---

## Part 1: 순차 I/O (쓰기 후 읽기-검증)

### 1단계: 애플리케이션 디렉토리 생성

```bash
cd $SPDK_DIR
mkdir -p app/ex03_simple_io
cd app/ex03_simple_io
```

### 2단계: 소스 파일 작성

아래 내용으로 `ex03_simple_io.c`를 생성하세요. 복사하기 전에 모든 주석을 주의 깊게 읽으세요.

```c
/*
 * EX03: Simple I/O Application
 *
 * Demonstrates sequential write -> read -> verify using the SPDK bdev API.
 * Uses a Malloc bdev so no physical hardware is required.
 */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

#define BDEV_NAME        "Malloc0"
#define IO_SIZE_BLOCKS   4          /* blocks per I/O */
#define TOTAL_IOS        8          /* sequential I/Os to perform    */

/* ------------------------------------------------------------------ */
/* Application context                                                  */
/* ------------------------------------------------------------------ */

struct io_ctx {
    struct spdk_bdev_desc  *desc;
    struct spdk_io_channel *ch;

    struct spdk_bdev       *bdev;
    uint32_t                block_size;
    uint64_t                num_blocks;

    void                   *write_buf;
    void                   *read_buf;
    uint32_t                buf_size;    /* bytes */

    uint64_t                current_lba; /* next LBA to operate on */
    uint32_t                io_count;    /* completed I/O pairs    */
    int                     errors;
};

static struct io_ctx g_ctx = {};

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static void submit_write(struct io_ctx *ctx);
static void write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void cleanup(struct io_ctx *ctx);

/* ------------------------------------------------------------------ */
/* I/O helpers                                                          */
/* ------------------------------------------------------------------ */

/*
 * submit_write -- fill write_buf with a pattern and issue spdk_bdev_write_blocks.
 *
 * The pattern encodes the LBA so verification can confirm the correct data
 * was read back from the correct location.
 */
static void
submit_write(struct io_ctx *ctx)
{
    int rc;

    /* Fill buffer with a recognisable pattern: upper 32 bits = LBA,
     * lower 32 bits = sequential byte index within the block.           */
    uint32_t *p = ctx->write_buf;
    uint32_t  words = ctx->buf_size / sizeof(uint32_t);

    for (uint32_t i = 0; i < words; i++) {
        p[i] = (uint32_t)(ctx->current_lba & 0xFFFFFFFF) ^ i;
    }

    SPDK_NOTICELOG("Writing %u blocks at LBA %lu\n",
                   IO_SIZE_BLOCKS, ctx->current_lba);

    rc = spdk_bdev_write_blocks(ctx->desc, ctx->ch,
                                ctx->write_buf,
                                ctx->current_lba,
                                IO_SIZE_BLOCKS,
                                write_complete, ctx);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_bdev_write_blocks failed: %d\n", rc);
        ctx->errors++;
        cleanup(ctx);
    }
    /* On success, write_complete will be called asynchronously. */
}

/*
 * write_complete -- callback invoked when a write I/O finishes.
 *
 * IMPORTANT: Always call spdk_bdev_free_io() before returning from
 * any completion callback. Failure to do so leaks the bdev_io object.
 */
static void
write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_ctx *ctx = cb_arg;
    int rc;

    /* Free the bdev_io object - this MUST happen in every callback path. */
    spdk_bdev_free_io(bdev_io);

    if (!success) {
        SPDK_ERRLOG("Write at LBA %lu failed\n", ctx->current_lba);
        ctx->errors++;
        cleanup(ctx);
        return;
    }

    SPDK_NOTICELOG("Write complete at LBA %lu, issuing read-back\n",
                   ctx->current_lba);

    /* Immediately issue a read-back to verify. */
    rc = spdk_bdev_read_blocks(ctx->desc, ctx->ch,
                               ctx->read_buf,
                               ctx->current_lba,
                               IO_SIZE_BLOCKS,
                               read_complete, ctx);
    if (rc != 0) {
        SPDK_ERRLOG("spdk_bdev_read_blocks failed: %d\n", rc);
        ctx->errors++;
        cleanup(ctx);
    }
}

/*
 * read_complete -- callback invoked when a read I/O finishes.
 *
 * Verifies that the data read back matches what was written, then
 * either advances to the next sequential I/O or finishes the test.
 */
static void
read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_ctx *ctx = cb_arg;

    spdk_bdev_free_io(bdev_io);

    if (!success) {
        SPDK_ERRLOG("Read at LBA %lu failed\n", ctx->current_lba);
        ctx->errors++;
        cleanup(ctx);
        return;
    }

    /* Verify data integrity. */
    uint32_t *wp = ctx->write_buf;
    uint32_t *rp = ctx->read_buf;
    uint32_t  words = ctx->buf_size / sizeof(uint32_t);
    bool      mismatch = false;

    for (uint32_t i = 0; i < words; i++) {
        if (wp[i] != rp[i]) {
            SPDK_ERRLOG("Data mismatch at word %u: wrote 0x%08x, read 0x%08x\n",
                        i, wp[i], rp[i]);
            mismatch = true;
            ctx->errors++;
            break;
        }
    }

    if (!mismatch) {
        SPDK_NOTICELOG("Read-back verified at LBA %lu [PASS]\n",
                       ctx->current_lba);
    }

    ctx->io_count++;
    ctx->current_lba += IO_SIZE_BLOCKS;

    if (ctx->io_count < TOTAL_IOS && ctx->current_lba + IO_SIZE_BLOCKS <= ctx->num_blocks) {
        /* Submit the next sequential I/O. */
        submit_write(ctx);
    } else {
        SPDK_NOTICELOG("Sequential I/O complete: %u I/Os, %d errors\n",
                       ctx->io_count, ctx->errors);
        cleanup(ctx);
    }
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                              */
/* ------------------------------------------------------------------ */

static void
cleanup(struct io_ctx *ctx)
{
    if (ctx->write_buf) {
        spdk_dma_free(ctx->write_buf);
        ctx->write_buf = NULL;
    }
    if (ctx->read_buf) {
        spdk_dma_free(ctx->read_buf);
        ctx->read_buf = NULL;
    }
    if (ctx->ch) {
        spdk_put_io_channel(ctx->ch);
        ctx->ch = NULL;
    }
    if (ctx->desc) {
        spdk_bdev_close(ctx->desc);
        ctx->desc = NULL;
    }

    spdk_app_stop(ctx->errors);
}

/* ------------------------------------------------------------------ */
/* Bdev event callback                                                  */
/* ------------------------------------------------------------------ */

/*
 * bdev_event_cb -- required by spdk_bdev_open_ext().
 *
 * Called on asynchronous bdev events (e.g., hot removal). For this
 * exercise we just log the event; a real application should handle
 * SPDK_BDEV_EVENT_REMOVE by closing the descriptor.
 */
static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
              void *event_ctx)
{
    SPDK_NOTICELOG("Bdev event: type=%d bdev=%s\n", type,
                   spdk_bdev_get_name(bdev));
}

/* ------------------------------------------------------------------ */
/* Application start                                                    */
/* ------------------------------------------------------------------ */

static void
app_start(void *arg)
{
    struct io_ctx *ctx = arg;
    int rc;

    /* Step 1: Open the bdev for read/write access. */
    rc = spdk_bdev_open_ext(BDEV_NAME, true /* write */, bdev_event_cb,
                            NULL, &ctx->desc);
    if (rc != 0) {
        SPDK_ERRLOG("Failed to open bdev '%s': %d\n", BDEV_NAME, rc);
        spdk_app_stop(rc);
        return;
    }

    /* Step 2: Retrieve the bdev pointer and its geometry. */
    ctx->bdev       = spdk_bdev_desc_get_bdev(ctx->desc);
    ctx->block_size = spdk_bdev_get_block_size(ctx->bdev);
    ctx->num_blocks = spdk_bdev_get_num_blocks(ctx->bdev);

    SPDK_NOTICELOG("Opened '%s': block_size=%u, num_blocks=%lu\n",
                   BDEV_NAME, ctx->block_size, ctx->num_blocks);

    if (ctx->num_blocks < (uint64_t)(IO_SIZE_BLOCKS * TOTAL_IOS)) {
        SPDK_ERRLOG("Bdev too small for this exercise\n");
        spdk_bdev_close(ctx->desc);
        spdk_app_stop(-ENOSPC);
        return;
    }

    /* Step 3: Get a per-thread I/O channel.
     * The channel must be obtained on the same thread that will submit I/O.
     * The reactor thread is that thread here (we are inside app_start).    */
    ctx->ch = spdk_bdev_get_io_channel(ctx->desc);
    if (ctx->ch == NULL) {
        SPDK_ERRLOG("Failed to get I/O channel\n");
        spdk_bdev_close(ctx->desc);
        spdk_app_stop(-ENOMEM);
        return;
    }

    /* Step 4: Allocate DMA-capable buffers.
     *
     * spdk_dma_malloc(size, alignment, phys_addr_out)
     *
     * - alignment must be at least the bdev block size.
     * - phys_addr_out can be NULL if you don't need the physical address.
     * - The returned pointer is virtually contiguous and DMA-safe.        */
    ctx->buf_size = ctx->block_size * IO_SIZE_BLOCKS;

    ctx->write_buf = spdk_dma_malloc(ctx->buf_size, ctx->block_size, NULL);
    if (ctx->write_buf == NULL) {
        SPDK_ERRLOG("Failed to allocate write buffer\n");
        cleanup(ctx);
        return;
    }

    ctx->read_buf = spdk_dma_malloc(ctx->buf_size, ctx->block_size, NULL);
    if (ctx->read_buf == NULL) {
        SPDK_ERRLOG("Failed to allocate read buffer\n");
        cleanup(ctx);
        return;
    }

    /* Step 5: Begin sequential I/O from LBA 0. */
    ctx->current_lba = 0;
    ctx->io_count    = 0;
    ctx->errors      = 0;

    submit_write(ctx);
    /* Execution continues asynchronously through write_complete / read_complete. */
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "ex03_simple_io";

    /*
     * The JSON config tells SPDK to create a Malloc bdev named "Malloc0"
     * with 256 blocks of 4096 bytes each (1 MiB total).
     * Pass it on the command line: --json malloc.json
     */

    rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    rc = spdk_app_start(&opts, app_start, &g_ctx);

    spdk_app_fini();
    return rc;
}
```

### 3단계: JSON 설정 작성

Malloc bdev를 정의하는 `malloc.json`을 생성하세요. 물리적 디바이스가 필요하지 않습니다.

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
            "num_blocks": 4096,
            "block_size": 512
          }
        }
      ]
    }
  ]
}
```

> 참고: 여기서 블록 크기는 512입니다. 시스템 bdev가 4096바이트 블록이면 `block_size`를 4096으로, `num_blocks`를 256으로 변경하여 동등한 용량을 사용하세요.

### 4단계: Makefile 작성

`Makefile`을 생성하세요:

```makefile
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = ex03_simple_io
SRCS = ex03_simple_io.c

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
```

### 5단계: 빌드

```bash
cd $SPDK_DIR/app/ex03_simple_io
make
```

예상 출력:

```
  CC ex03_simple_io.c
  LINK ex03_simple_io
```

누락된 심볼에 대한 링커 오류가 발생하면 `SPDK_ROOT_DIR`이 올바른 위치를 가리키는지, SPDK가 `./configure && make`로 빌드되었는지 확인하세요.

### 6단계: 실행

```bash
sudo ./ex03_simple_io --json malloc.json
```

### 예상 출력

```
[2024-01-15 10:23:01.123456] NOTICE: Opened 'Malloc0': block_size=512, num_blocks=4096
[2024-01-15 10:23:01.123500] NOTICE: Writing 4 blocks at LBA 0
[2024-01-15 10:23:01.123510] NOTICE: Write complete at LBA 0, issuing read-back
[2024-01-15 10:23:01.123520] NOTICE: Read-back verified at LBA 0 [PASS]
[2024-01-15 10:23:01.123530] NOTICE: Writing 4 blocks at LBA 4
[2024-01-15 10:23:01.123540] NOTICE: Write complete at LBA 4, issuing read-back
[2024-01-15 10:23:01.123550] NOTICE: Read-back verified at LBA 4 [PASS]
...
[2024-01-15 10:23:01.123900] NOTICE: Sequential I/O complete: 8 I/Os, 0 errors
```

---

## Part 2: 보너스 - 지연 시간 측정이 포함된 랜덤 I/O

애플리케이션을 확장하여 랜덤 I/O를 수행하고 I/O당 지연 시간(latency)을 측정하세요.

### 1단계: 컨텍스트에 지연 시간 추적 추가

`struct io_ctx`에 다음 필드를 추가하세요:

```c
    /* Bonus: random I/O and latency */
    bool                    random_mode;
    uint64_t                io_start_tsc;   /* TSC at I/O submission  */
    uint64_t                total_latency_ns;
    uint64_t                min_latency_ns;
    uint64_t                max_latency_ns;
    uint32_t                random_seed;
```

### 2단계: 랜덤 LBA 생성기 추가

```c
static uint64_t
random_aligned_lba(struct io_ctx *ctx)
{
    /* Generate a random LBA aligned to IO_SIZE_BLOCKS. */
    uint64_t max_lba = (ctx->num_blocks / IO_SIZE_BLOCKS) - 1;
    uint64_t idx = (uint64_t)rand_r(&ctx->random_seed) % (max_lba + 1);
    return idx * IO_SIZE_BLOCKS;
}
```

### 3단계: 제출 시 TSC 기록

`submit_write()`에서 I/O 호출 직전에 TSC를 캡처하세요:

```c
    ctx->io_start_tsc = spdk_get_ticks();

    rc = spdk_bdev_write_blocks(ctx->desc, ctx->ch,
                                ctx->write_buf,
                                ctx->current_lba,
                                IO_SIZE_BLOCKS,
                                write_complete, ctx);
```

### 4단계: 읽기 콜백에서 지연 시간 계산

`read_complete()` 상단, `spdk_bdev_free_io()` 이후:

```c
    uint64_t tsc_diff = spdk_get_ticks() - ctx->io_start_tsc;
    uint64_t ticks_hz = spdk_get_ticks_hz();
    uint64_t latency_ns = tsc_diff * 1000000000ULL / ticks_hz;

    ctx->total_latency_ns += latency_ns;
    if (latency_ns < ctx->min_latency_ns || ctx->min_latency_ns == 0) {
        ctx->min_latency_ns = latency_ns;
    }
    if (latency_ns > ctx->max_latency_ns) {
        ctx->max_latency_ns = latency_ns;
    }

    SPDK_NOTICELOG("LBA %lu round-trip: %lu ns\n", ctx->current_lba, latency_ns);
```

### 5단계: cleanup()에서 요약 출력

`spdk_app_stop()` 이전에:

```c
    if (ctx->io_count > 0) {
        SPDK_NOTICELOG("=== I/O Summary ===\n");
        SPDK_NOTICELOG("  Total I/Os : %u\n", ctx->io_count);
        SPDK_NOTICELOG("  Errors     : %d\n", ctx->errors);
        SPDK_NOTICELOG("  Avg latency: %lu ns\n",
                       ctx->total_latency_ns / ctx->io_count);
        SPDK_NOTICELOG("  Min latency: %lu ns\n", ctx->min_latency_ns);
        SPDK_NOTICELOG("  Max latency: %lu ns\n", ctx->max_latency_ns);

        uint64_t total_bytes = (uint64_t)ctx->io_count
                               * IO_SIZE_BLOCKS * ctx->block_size * 2; /* read+write */
        uint64_t elapsed_ns  = ctx->total_latency_ns;
        if (elapsed_ns > 0) {
            uint64_t throughput_mbs = total_bytes * 1000ULL / elapsed_ns;
            SPDK_NOTICELOG("  Throughput : ~%lu MB/s (cumulative)\n", throughput_mbs);
        }
    }
```

### 6단계: 랜덤 모드 활성화

`app_start()`에서 `ctx->random_mode = true`와 `ctx->random_seed = 42`를 설정하세요. `read_complete()`에서 LBA 전진 방식을 다음과 같이 변경하세요:

```c
    if (ctx->random_mode) {
        ctx->current_lba = random_aligned_lba(ctx);
    } else {
        ctx->current_lba += IO_SIZE_BLOCKS;
    }
```

### 보너스 예상 출력

```
[...] NOTICE: LBA 256 round-trip: 1842 ns
[...] NOTICE: LBA 512 round-trip: 1791 ns
[...] NOTICE: LBA 128 round-trip: 1823 ns
...
[...] NOTICE: === I/O Summary ===
[...] NOTICE:   Total I/Os : 8
[...] NOTICE:   Errors     : 0
[...] NOTICE:   Avg latency: 1820 ns
[...] NOTICE:   Min latency: 1791 ns
[...] NOTICE:   Max latency: 1842 ns
[...] NOTICE:   Throughput : ~17 MB/s (cumulative)
```

> Malloc bdev는 소프트웨어로 실행되므로 지연 시간은 실제 스토리지 지연이 아닌 리액터 오버헤드를 반영합니다. 실제 NVMe SSD에서는 50-200 us가 나타날 것입니다.

---

## 흔한 실수

### 실수 1: DMA 메모리 대신 스택 또는 힙 버퍼 사용

```c
/* 잘못된 예 - 실제 하드웨어에서 정의되지 않은 동작 또는 assert 발생 */
char buf[4096];
spdk_bdev_write_blocks(desc, ch, buf, 0, 1, cb, ctx);

/* 올바른 예 */
void *buf = spdk_dma_malloc(4096, 512, NULL);
spdk_bdev_write_blocks(desc, ch, buf, 0, 1, cb, ctx);
```

**이유**: SPDK는 버퍼 포인터를 DMA 엔진에 직접 전달합니다. 스택 버퍼는 물리 메모리에 고정되어 있지 않으며 디바이스 요구사항에 맞게 정렬되어 있지 않습니다.

### 실수 2: 콜백에서 spdk_bdev_free_io() 호출을 잊음

```c
/* 잘못된 예 - 매 I/O마다 bdev_io 객체 누수 */
static void
my_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    /* 누락: spdk_bdev_free_io(bdev_io); */
    do_next_io(cb_arg);
}

/* 올바른 예 - 모든 반환 경로 전에 해제 */
static void
my_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    spdk_bdev_free_io(bdev_io);   /* always, first */
    if (!success) { ... return; }
    do_next_io(cb_arg);
}
```

**이유**: 각 `spdk_bdev_io` 객체는 고정 크기 풀에서 가져옵니다. 하나라도 누수되면 결국 풀이 고갈되어 이후 모든 I/O가 `-ENOMEM`으로 실패합니다.

### 실수 3: 콜백 내에서 블로킹

```c
/* 잘못된 예 - 리액터를 차단하여 다른 모든 폴러를 고갈시킴 */
static void
my_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    spdk_bdev_free_io(bdev_io);
    sleep(1);           /* 절대 하지 마세요 */
    pthread_mutex_lock(&some_lock); /* 이것도 절대 하지 마세요 */
}

/* 올바른 예 - 콜백에서 다음 비동기 작업 체이닝 */
static void
my_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    spdk_bdev_free_io(bdev_io);
    submit_next_io(cb_arg);  /* non-blocking, returns immediately */
}
```

**이유**: SPDK는 코어당 단일 스레드 리액터에서 실행됩니다. 콜백 내의 블로킹 호출은 타이머 만료와 다른 폴러를 포함한 전체 리액터를 정지시킵니다.

### 실수 4: 잘못된 스레드에서 I/O 채널 사용

```c
/* 잘못된 예 - 채널이 스레드 A에서 얻어졌지만 스레드 B에서 사용됨 */
/* Thread A */
ctx->ch = spdk_bdev_get_io_channel(ctx->desc);

/* Thread B - RACE / undefined behavior */
spdk_bdev_write_blocks(ctx->desc, ctx->ch, ...);

/* 올바른 예 - 같은 리액터 스레드에서 채널 획득 및 사용 */
```

**이유**: I/O 채널은 스레드 안전하지 않습니다. 각 리액터 스레드는 `spdk_bdev_get_io_channel()`을 통해 자신만의 채널을 얻어야 합니다.

### 실수 5: spdk_bdev_close() 이후 I/O 제출

```c
/* 잘못된 예 */
spdk_bdev_close(ctx->desc);
spdk_bdev_write_blocks(ctx->desc, ctx->ch, ...);  /* use-after-free */

/* 올바른 예 - 모든 진행 중인 I/O가 완료된 후에만 닫기 */
```

---

## 검증 체크리스트

실습을 완료한 후 확인하세요:

- [ ] 애플리케이션이 경고 없이 컴파일됨
- [ ] 8개의 순차 쓰기+읽기 쌍이 모두 `[PASS]`로 완료됨
- [ ] 최종 메시지가 `0 errors`를 표시
- [ ] (보너스) 지연 시간 요약이 min/avg/max 값을 출력
- [ ] Valgrind 또는 ASAN으로 실행하여 메모리 누수 없음을 확인:
      `sudo valgrind --leak-check=full ./ex03_simple_io --json malloc.json`
- [ ] `TOTAL_IOS`를 64로 변경하고 여전히 통과하는지 확인

---

## 주요 API 레퍼런스

| 함수 | 용도 |
|---|---|
| `spdk_bdev_open_ext(name, write, event_cb, ctx, &desc)` | bdev 열기, 디스크립터 얻기 |
| `spdk_bdev_desc_get_bdev(desc)` | 디스크립터에서 `spdk_bdev*` 얻기 |
| `spdk_bdev_get_block_size(bdev)` | 블록 크기(바이트) |
| `spdk_bdev_get_num_blocks(bdev)` | 전체 논리 블록 수 |
| `spdk_bdev_get_io_channel(desc)` | 스레드별 I/O 채널 할당 |
| `spdk_bdev_write_blocks(desc, ch, buf, lba, n, cb, arg)` | 비동기 쓰기 제출 |
| `spdk_bdev_read_blocks(desc, ch, buf, lba, n, cb, arg)` | 비동기 읽기 제출 |
| `spdk_bdev_free_io(bdev_io)` | I/O 객체 해제 (모든 콜백에서 호출) |
| `spdk_put_io_channel(ch)` | I/O 채널 해제 |
| `spdk_bdev_close(desc)` | bdev 디스크립터 닫기 |
| `spdk_dma_malloc(size, align, NULL)` | DMA 안전 버퍼 할당 |
| `spdk_dma_free(buf)` | DMA 버퍼 해제 |
| `spdk_get_ticks()` | 지연 시간 측정을 위한 TSC 읽기 |
| `spdk_get_ticks_hz()` | ns 변환을 위한 TSC 주파수 |

---

## 요약

이 실습에서 수행한 내용:

1. `spdk_bdev_open_ext()`로 bdev를 열고 형상(geometry)을 조회함
2. 스레드별 I/O 채널과 DMA 버퍼를 할당함
3. 블로킹 호출 없이 콜백으로만 구동되는 체인된 비동기 쓰기 -> 읽기 -> 검증 루프를 구현함
4. (보너스) TSC 타임스탬프를 사용하여 I/O당 지연 시간을 측정하고 처리량을 보고함

동일한 패턴이 모든 SPDK bdev에 적용됩니다: JSON에서 `"Malloc0"`을 `"NVMe0n1"`로 교체하면 애플리케이션이 변경 없이 실제 NVMe 드라이브에 대해 실행됩니다.

# 실습 04: Null Bdev 모듈

## 개요

| 항목 | 값 |
|-------|-------|
| 예상 소요 시간 | 3-4시간 |
| 난이도 | 중급 |
| 모듈 경로 | `module/bdev/` |
| 참조 | `module/bdev/null/bdev_null.c` |

## 목표

`my_null`이라는 최소한의 블록 디바이스 모듈을 처음부터 구현합니다. 이 디바이스는 모든 쓰기를 버리고 읽기 시 0으로 채워진 버퍼를 반환합니다 -- 실제 스토리지 로직을 계층화하기 전에 bdev 모듈 인터페이스의 모든 필수 훅(hook)을 이해하기 위한 깔끔한 기반입니다.

이 실습을 마치면 다음을 할 수 있게 됩니다:

- `SPDK_BDEV_MODULE_REGISTER`로 bdev 모듈 등록
- `submit_request`, `io_type_supported`, `get_io_channel`, `destruct` 구현
- JSON-RPC를 통한 디바이스 인스턴스 생성 및 삭제
- SPDK와 함께 공유 라이브러리로 모듈 빌드
- `bdevperf`로 모듈에 대한 I/O 처리량 측정
- 채널별 I/O 카운터와 설정 가능한 지연 주입 추가 (보너스)

## 사전 요구사항

- 실습 01 (SPDK 빌드), 실습 02 (리액터/스레드 모델), 실습 03 (I/O 채널 패턴) 완료
- SPDK의 폴링 I/O 모델과 `spdk_poller`에 대한 친숙함
- C99, `TAILQ`, 기본적인 포인터 산술

## 배경 지식: bdev 모듈 인터페이스

모든 bdev 모듈은 SPDK bdev 계층에 두 개의 구조체를 노출합니다.

**`struct spdk_bdev_module`** -- 모듈당 한 번 등록되며, 수명주기 훅을 기술합니다:

```c
struct spdk_bdev_module {
    const char *name;
    int  (*module_init)(void);       /* called at subsystem startup        */
    void (*module_fini)(void);       /* called after all bdevs unregistered */
    int  (*get_ctx_size)(void);      /* per-I/O private storage size        */
    int  (*config_json)(struct spdk_json_write_ctx *w); /* config snapshot */
    bool async_fini;                 /* set true if module_fini is async    */
    /* ... additional optional fields ... */
};
```

**`struct spdk_bdev_fn_table`** -- 디바이스 인스턴스당 등록되며, I/O별 디스패치를 기술합니다:

```c
struct spdk_bdev_fn_table {
    void (*submit_request)(struct spdk_io_channel *ch,
                           struct spdk_bdev_io *bdev_io);
    bool (*io_type_supported)(void *ctx,
                              enum spdk_bdev_io_type io_type);
    struct spdk_io_channel *(*get_io_channel)(void *ctx);
    int  (*destruct)(void *ctx);
    void (*write_config_json)(struct spdk_bdev *bdev,
                              struct spdk_json_write_ctx *w);
};
```

bdev 계층은 I/O 채널을 소유한 스레드에서 `submit_request`를 호출합니다. 핸들러는 `spdk_bdev_io_complete()`를 통해 I/O를 동기적으로 완료하거나, 폴러를 통한 지연 완료를 위해 큐에 넣어야 합니다.

---

## 1단계 -- 디렉토리 구조

기존 null bdev 옆에 모듈 디렉토리를 생성하세요:

```
module/bdev/my_null/
    my_null_bdev.h        # .c 파일 간 공유되는 내부 타입
    my_null_bdev.c        # 모듈 등록, I/O 디스패치, 채널 관리
    my_null_bdev_rpc.c    # JSON-RPC 핸들러 (생성 / 삭제)
    Makefile
```

```bash
mkdir -p /path/to/spdk/module/bdev/my_null
```

---

## 2단계 -- 내부 헤더 (`my_null_bdev.h`)

```c
/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"

/* Options passed to bdev_my_null_create() from the RPC handler. */
struct my_null_opts {
    char     *name;
    uint64_t  num_blocks;
    uint32_t  block_size;
};

/*
 * Create a new my_null bdev.
 *
 * On success *bdev_out is set to the registered bdev and 0 is returned.
 * The caller owns nothing; the bdev layer owns the lifetime.
 */
int bdev_my_null_create(struct spdk_bdev **bdev_out,
                        const struct my_null_opts *opts);

/* Asynchronous delete — cb is called on the calling thread once done. */
void bdev_my_null_delete(const char *name,
                         spdk_delete_callback cb, void *cb_arg);
```

---

## 3단계 -- 핵심 구현 (`my_null_bdev.c`)

### 3.1 인클루드 및 전방 선언

```c
/* SPDX-License-Identifier: BSD-3-Clause */
#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/bdev_module.h"

#include "my_null_bdev.h"

/* Forward declarations required by the module struct. */
static int  bdev_my_null_initialize(void);
static void bdev_my_null_finish(void);
```

### 3.2 I/O별 컨텍스트

bdev 계층은 진행 중인 I/O당 고정 크기의 개인 스토리지 블록을 할당합니다. 모듈은 `get_ctx_size`를 통해 해당 블록의 크기를 선언합니다. 지연 완료에 필요한 것을 저장하는 데 사용하세요 (이 경우 리스트 링크).

```c
/*
 * Per-I/O private storage allocated by the bdev layer.
 * We embed a TAILQ link so the channel poller can walk pending I/Os.
 */
struct my_null_bdev_io {
    TAILQ_ENTRY(my_null_bdev_io) link;
};

static int
bdev_my_null_get_ctx_size(void)
{
    return sizeof(struct my_null_bdev_io);
}
```

### 3.3 디바이스별 구조체

`struct spdk_bdev`를 첫 번째 멤버로 포함시키세요. bdev 계층은 `ctx` (`void *`)를 다시 사용자의 구조체로 캐스팅하므로, 기본 주소가 동일해야 합니다.

```c
struct my_null_bdev {
    struct spdk_bdev         bdev;   /* MUST be first */
    TAILQ_ENTRY(my_null_bdev) tailq;
};

static TAILQ_HEAD(, my_null_bdev) g_my_null_bdev_head =
    TAILQ_HEAD_INITIALIZER(g_my_null_bdev_head);
```

### 3.4 채널별 구조체 및 폴러

각 I/O 채널은 자체 개인 상태를 갖습니다. 폴러는 매 리액터 루프 반복마다 대기 큐를 비웁니다 (제로 지연 완료 -- 실제 지연을 추가하려면 보너스 섹션을 참조하세요).

```c
struct my_null_io_channel {
    struct spdk_poller               *poller;
    TAILQ_HEAD(, my_null_bdev_io)     pending_io;
};

static int
bdev_my_null_channel_poll(void *arg)
{
    struct my_null_io_channel *ch = arg;
    struct my_null_bdev_io    *io;
    TAILQ_HEAD(, my_null_bdev_io) completed =
        TAILQ_HEAD_INITIALIZER(completed);

    /* Splice the whole queue atomically to avoid iterator invalidation. */
    TAILQ_SWAP(&completed, &ch->pending_io, my_null_bdev_io, link);

    if (TAILQ_EMPTY(&completed)) {
        return SPDK_POLLER_IDLE;
    }

    while ((io = TAILQ_FIRST(&completed)) != NULL) {
        TAILQ_REMOVE(&completed, io, link);
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io),
                              SPDK_BDEV_IO_STATUS_SUCCESS);
    }

    return SPDK_POLLER_BUSY;
}

static int
bdev_my_null_create_channel_cb(void *io_device, void *ctx_buf)
{
    struct my_null_io_channel *ch = ctx_buf;

    TAILQ_INIT(&ch->pending_io);
    ch->poller = SPDK_POLLER_REGISTER(bdev_my_null_channel_poll, ch, 0);
    if (!ch->poller) {
        SPDK_ERRLOG("Failed to register channel poller\n");
        return -ENOMEM;
    }

    return 0;
}

static void
bdev_my_null_destroy_channel_cb(void *io_device, void *ctx_buf)
{
    struct my_null_io_channel *ch = ctx_buf;

    spdk_poller_unregister(&ch->poller);
    /* Drain any remaining I/Os with failure so callers are not stuck. */
    struct my_null_bdev_io *io;
    while ((io = TAILQ_FIRST(&ch->pending_io)) != NULL) {
        TAILQ_REMOVE(&ch->pending_io, io, link);
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io),
                              SPDK_BDEV_IO_STATUS_FAILED);
    }
}
```

### 3.5 모듈 수준 수명주기

`module_init`은 채널 생성의 기반이 되는 io_device를 등록합니다. `module_fini`는 이를 해제합니다.

```c
static int
bdev_my_null_initialize(void)
{
    /*
     * Register the io_device keyed on the global list head.
     * Any unique, stable address works — the list head is convenient.
     */
    spdk_io_device_register(&g_my_null_bdev_head,
                            bdev_my_null_create_channel_cb,
                            bdev_my_null_destroy_channel_cb,
                            sizeof(struct my_null_io_channel),
                            "my_null");
    return 0;
}

static void
bdev_my_null_finish(void)
{
    spdk_io_device_unregister(&g_my_null_bdev_head, NULL);
    spdk_bdev_module_fini_done();
}
```

### 3.6 모듈 등록

`SPDK_BDEV_MODULE_REGISTER`는 모듈 구조체를 링커 세트에 삽입하여 bdev 서브시스템이 시작 시 순회합니다 -- 애플리케이션 설정에서 명시적 호출이 필요하지 않습니다.

```c
static struct spdk_bdev_module my_null_if = {
    .name        = "my_null",
    .module_init = bdev_my_null_initialize,
    .module_fini = bdev_my_null_finish,
    .async_fini  = true,     /* module_fini calls spdk_bdev_module_fini_done() */
    .get_ctx_size = bdev_my_null_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(my_null, &my_null_if)
```

### 3.7 I/O 디스패치 (`submit_request`)

`submit_request`는 채널을 소유한 리액터 스레드에서 호출됩니다. null 디바이스의 경우 지원하는 모든 I/O 유형은 단순히 큐에 넣어지고, 폴러가 다음 폴링 반복에서 완료합니다. 지원되지 않는 유형은 즉시 실패로 완료됩니다.

```c
static void
bdev_my_null_submit_request(struct spdk_io_channel *_ch,
                            struct spdk_bdev_io    *bdev_io)
{
    struct my_null_bdev_io    *io  = (struct my_null_bdev_io *)bdev_io->driver_ctx;
    struct my_null_io_channel *ch  = spdk_io_channel_get_ctx(_ch);

    switch (bdev_io->type) {
    case SPDK_BDEV_IO_TYPE_READ:
        /*
         * If the caller supplied a NULL iov_base the bdev layer expects
         * us to point it at a zeroed buffer.  We use the iov already
         * allocated by the bdev layer and zero-fill it here.
         */
        if (bdev_io->u.bdev.iovs[0].iov_base != NULL) {
            uint64_t len = bdev_io->u.bdev.num_blocks *
                           bdev_io->bdev->blocklen;
            memset(bdev_io->u.bdev.iovs[0].iov_base, 0, len);
        }
        /* fall through — queue for deferred completion */
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
    case SPDK_BDEV_IO_TYPE_RESET:
        TAILQ_INSERT_TAIL(&ch->pending_io, io, link);
        break;

    default:
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        break;
    }
}
```

### 3.8 기능 광고 (`io_type_supported`)

`submit_request`가 성공적으로 처리하는 I/O 유형에 대해서만 `true`를 반환하세요. bdev 계층은 지원되지 않는 유형에 대한 요청을 핸들러에 도달하기 전에 거부합니다.

```c
static bool
bdev_my_null_io_type_supported(void *ctx,
                               enum spdk_bdev_io_type io_type)
{
    switch (io_type) {
    case SPDK_BDEV_IO_TYPE_READ:
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
    case SPDK_BDEV_IO_TYPE_RESET:
        return true;
    default:
        return false;
    }
}
```

### 3.9 채널 획득

동일한 io_device를 공유하는 모든 bdev는 스레드당 단일 채널을 얻습니다. `module_init`에서 등록된 전역 리스트 헤드를 키로 하는 채널을 반환합니다.

```c
static struct spdk_io_channel *
bdev_my_null_get_io_channel(void *ctx)
{
    return spdk_get_io_channel(&g_my_null_bdev_head);
}
```

### 3.10 디바이스 파괴

`destruct`는 bdev 계층이 더 이상 I/O가 제출되지 않음을 보장한 후 호출됩니다. 전역 리스트에서 제거하고 해제합니다. 동기 해제의 경우 0을 반환합니다.

```c
static int
bdev_my_null_destruct(void *ctx)
{
    struct my_null_bdev *bdev = ctx;

    TAILQ_REMOVE(&g_my_null_bdev_head, bdev, tailq);
    free(bdev->bdev.name);
    free(bdev);

    return 0;
}
```

### 3.11 설정 JSON 직렬화

이 콜백은 SPDK가 실행 중인 설정을 저장할 때 호출됩니다. 디바이스를 재생성하는 데 필요한 정확한 RPC 호출과 매개변수를 출력합니다.

```c
static void
bdev_my_null_write_config_json(struct spdk_bdev        *bdev,
                               struct spdk_json_write_ctx *w)
{
    spdk_json_write_object_begin(w);
    spdk_json_write_named_string(w, "method", "bdev_my_null_create");
    spdk_json_write_named_object_begin(w, "params");
    spdk_json_write_named_string(w,  "name",       bdev->name);
    spdk_json_write_named_uint64(w,  "num_blocks",  bdev->blockcnt);
    spdk_json_write_named_uint32(w,  "block_size",  bdev->blocklen);
    spdk_json_write_object_end(w);
    spdk_json_write_object_end(w);
}
```

### 3.12 함수 테이블 및 `bdev_my_null_create`

모든 것을 연결합니다:

```c
static const struct spdk_bdev_fn_table my_null_fn_table = {
    .destruct          = bdev_my_null_destruct,
    .submit_request    = bdev_my_null_submit_request,
    .io_type_supported = bdev_my_null_io_type_supported,
    .get_io_channel    = bdev_my_null_get_io_channel,
    .write_config_json = bdev_my_null_write_config_json,
};

int
bdev_my_null_create(struct spdk_bdev **bdev_out,
                    const struct my_null_opts *opts)
{
    struct my_null_bdev *disk;
    int rc;

    if (!opts || !opts->name || opts->num_blocks == 0 ||
            opts->block_size == 0 || opts->block_size % 512 != 0) {
        SPDK_ERRLOG("Invalid options for my_null bdev\n");
        return -EINVAL;
    }

    disk = calloc(1, sizeof(*disk));
    if (!disk) {
        return -ENOMEM;
    }

    disk->bdev.name = strdup(opts->name);
    if (!disk->bdev.name) {
        free(disk);
        return -ENOMEM;
    }

    disk->bdev.product_name = "My Null Disk";
    disk->bdev.blocklen      = opts->block_size;
    disk->bdev.blockcnt      = opts->num_blocks;
    disk->bdev.ctxt          = disk;
    disk->bdev.fn_table      = &my_null_fn_table;
    disk->bdev.module        = &my_null_if;

    rc = spdk_bdev_register(&disk->bdev);
    if (rc != 0) {
        SPDK_ERRLOG("Failed to register my_null bdev '%s': %d\n",
                    opts->name, rc);
        free(disk->bdev.name);
        free(disk);
        return rc;
    }

    TAILQ_INSERT_TAIL(&g_my_null_bdev_head, disk, tailq);
    *bdev_out = &disk->bdev;
    return 0;
}
```

### 3.13 `bdev_my_null_delete`

```c
void
bdev_my_null_delete(const char *name,
                    spdk_delete_callback cb, void *cb_arg)
{
    int rc = spdk_bdev_unregister_by_name(name, &my_null_if, cb, cb_arg);
    if (rc != 0) {
        cb(cb_arg, rc);
    }
}
```

---

## 4단계 -- JSON-RPC 핸들러 (`my_null_bdev_rpc.c`)

```c
/* SPDX-License-Identifier: BSD-3-Clause */
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "my_null_bdev.h"

/* ---- bdev_my_null_create ---- */

static const struct spdk_json_object_decoder rpc_create_decoders[] = {
    {"name",       offsetof(struct my_null_opts, name),       spdk_json_decode_string},
    {"num_blocks", offsetof(struct my_null_opts, num_blocks), spdk_json_decode_uint64},
    {"block_size", offsetof(struct my_null_opts, block_size), spdk_json_decode_uint32},
};

static void
rpc_bdev_my_null_create(struct spdk_jsonrpc_request *request,
                        const struct spdk_json_val  *params)
{
    struct my_null_opts        req = {};
    struct spdk_bdev          *bdev;
    struct spdk_json_write_ctx *w;
    int rc;

    if (spdk_json_decode_object(params, rpc_create_decoders,
                                SPDK_COUNTOF(rpc_create_decoders), &req)) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "Invalid parameters");
        goto cleanup;
    }

    rc = bdev_my_null_create(&bdev, &req);
    if (rc != 0) {
        spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
        goto cleanup;
    }

    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_string(w, bdev->name);
    spdk_jsonrpc_end_result(request, w);

cleanup:
    free(req.name);
}
SPDK_RPC_REGISTER("bdev_my_null_create", rpc_bdev_my_null_create,
                  SPDK_RPC_RUNTIME)

/* ---- bdev_my_null_delete ---- */

struct rpc_delete_req {
    char *name;
};

static const struct spdk_json_object_decoder rpc_delete_decoders[] = {
    {"name", offsetof(struct rpc_delete_req, name), spdk_json_decode_string},
};

static void
rpc_bdev_my_null_delete_cb(void *cb_arg, int bdeverrno)
{
    struct spdk_jsonrpc_request *request = cb_arg;

    if (bdeverrno == 0) {
        struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
        spdk_json_write_bool(w, true);
        spdk_jsonrpc_end_result(request, w);
    } else {
        spdk_jsonrpc_send_error_response(request, bdeverrno,
                                         spdk_strerror(-bdeverrno));
    }
}

static void
rpc_bdev_my_null_delete(struct spdk_jsonrpc_request *request,
                        const struct spdk_json_val  *params)
{
    struct rpc_delete_req req = {};

    if (spdk_json_decode_object(params, rpc_delete_decoders,
                                SPDK_COUNTOF(rpc_delete_decoders), &req)) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "Invalid parameters");
        return;
    }

    bdev_my_null_delete(req.name, rpc_bdev_my_null_delete_cb, request);
    free(req.name);
}
SPDK_RPC_REGISTER("bdev_my_null_delete", rpc_bdev_my_null_delete,
                  SPDK_RPC_RUNTIME)
```

---

## 5단계 -- Makefile

기존 null bdev Makefile을 그대로 따라하되, 라이브러리 이름을 교체하세요:

```makefile
#  SPDX-License-Identifier: BSD-3-Clause

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER   := 1
SO_MINOR := 0

C_SRCS  = my_null_bdev.c my_null_bdev_rpc.c
LIBNAME = bdev_my_null

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
```

---

## 6단계 -- 빌드 시스템에 연결

SPDK의 최상위 `CONFIG` 및 `mk/` 파일이 어떤 bdev 모듈을 컴파일할지 제어합니다. 가장 간단한 방법은 기존 bdev 모듈 목록에 라이브러리를 추가하는 것입니다.

`mk/spdk.lib_deps.mk`를 편집하세요 -- bdev 모듈을 나열하는 줄을 찾아 `bdev_my_null`을 추가하세요:

```makefile
# Before:
DEPDIRS-bdev_null = ...

# After (add your entry in alphabetical order):
DEPDIRS-bdev_my_null = log bdev
```

그런 다음 `module/bdev/Makefile`에 서브디렉토리를 추가하세요:

```makefile
# In the DIRS list, add:
my_null \
```

다시 빌드하세요:

```bash
cd /path/to/spdk
make -j$(nproc)
```

성공적인 빌드는 `build/lib/libspdk_bdev_my_null.so`를 생성합니다.

---

## 7단계 -- bdevperf로 런타임 테스트

### 7.1 SPDK 설정 JSON 생성

`my_null_test.json`으로 저장하세요:

```json
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_my_null_create",
          "params": {
            "name": "MyNull0",
            "num_blocks": 131072,
            "block_size": 4096
          }
        }
      ]
    }
  ]
}
```

이것은 512 MiB 디바이스(131072 블록 * 4096 바이트)를 생성합니다.

### 7.2 bdevperf 실행

```bash
sudo ./build/examples/bdevperf \
    --json my_null_test.json \
    -q 128 \
    -o 4096 \
    -t 10 \
    -w randread \
    -m 0x1
```

| 플래그 | 의미 |
|------|------|
| `-q 128` | 스레드당 큐 깊이 |
| `-o 4096` | I/O 크기(바이트) |
| `-t 10` | 지속 시간(초) |
| `-w randread` | 워크로드: `randread`, `randwrite`, `randrw` |
| `-m 0x1` | CPU 마스크 -- 코어 하나 |

예상 출력 (숫자는 하드웨어에 따라 다름):

```
MyNull0 : 0.10 seconds (  0.10 total)    Read= 1250.00 MiB/s (  320000 IOPS)
```

null bdev는 CPU 바운드입니다. 최신 하드웨어에서 수백만 IOPS가 나와야 합니다.

### 7.3 런타임에 RPC로 검증

`--rpc-socket /tmp/spdk.sock`으로 SPDK 애플리케이션을 시작한 후, 다른 터미널에서:

```bash
# 등록된 모든 bdev 목록
./scripts/rpc.py bdev_get_bdevs

# 런타임에 두 번째 디바이스 생성
./scripts/rpc.py bdev_my_null_create --name MyNull1 --num-blocks 65536 --block-size 512

# 삭제
./scripts/rpc.py bdev_my_null_delete --name MyNull1
```

---

## 8단계 -- 검증 체크리스트

보너스 섹션으로 넘어가기 전에 각 항목을 확인하세요:

- [ ] `make`가 수정된 파일에 대해 경고 없이 완료
- [ ] `bdev_get_bdevs`에 올바른 `block_size`와 `num_blocks`로 `MyNull0`이 표시
- [ ] bdevperf가 10초 randread 실행을 오류 없이 완료
- [ ] bdevperf가 10초 randwrite 실행을 오류 없이 완료
- [ ] RPC를 통한 두 번째 디바이스 생성 성공
- [ ] RPC를 통한 디바이스 삭제 성공 후 `bdev_get_bdevs`에서 사라짐
- [ ] SPDK가 깔끔하게 종료 (Valgrind 또는 ASAN에서 use-after-free 없음)

---

## 보너스 A -- 채널별 I/O 통계

`struct my_null_io_channel`에 카운터를 추가하고 새 RPC를 통해 노출하세요.

### A.1 채널 구조체 확장

```c
struct my_null_io_channel {
    struct spdk_poller               *poller;
    TAILQ_HEAD(, my_null_bdev_io)     pending_io;

    /* Statistics — updated on the owning reactor, no locking needed. */
    uint64_t  reads_completed;
    uint64_t  writes_completed;
    uint64_t  read_bytes;
    uint64_t  write_bytes;
};
```

### A.2 완료 전 I/O를 분류하도록 폴러 업데이트

```c
static int
bdev_my_null_channel_poll(void *arg)
{
    struct my_null_io_channel *ch = arg;
    struct my_null_bdev_io    *io;
    TAILQ_HEAD(, my_null_bdev_io) done = TAILQ_HEAD_INITIALIZER(done);

    TAILQ_SWAP(&done, &ch->pending_io, my_null_bdev_io, link);
    if (TAILQ_EMPTY(&done)) {
        return SPDK_POLLER_IDLE;
    }

    while ((io = TAILQ_FIRST(&done)) != NULL) {
        struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(io);
        uint64_t bytes = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

        if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
            ch->reads_completed++;
            ch->read_bytes += bytes;
        } else {
            ch->writes_completed++;
            ch->write_bytes += bytes;
        }

        TAILQ_REMOVE(&done, io, link);
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
    }

    return SPDK_POLLER_BUSY;
}
```

### A.3 `spdk_for_each_channel`로 채널 간 집계

통계는 채널별(스레드별)로 존재합니다. 합계를 얻으려면 각 리액터에 자체 채널을 읽도록 요청해야 합니다. SPDK는 정확히 이 패턴을 위해 `spdk_for_each_channel`을 제공합니다.

```c
struct my_null_stats_ctx {
    struct spdk_jsonrpc_request *request;
    uint64_t reads_completed;
    uint64_t writes_completed;
    uint64_t read_bytes;
    uint64_t write_bytes;
};

static void
_gather_stats(struct spdk_io_channel_iter *i)
{
    struct my_null_stats_ctx  *ctx = spdk_io_channel_iter_get_ctx(i);
    struct spdk_io_channel    *ch  = spdk_io_channel_iter_get_channel(i);
    struct my_null_io_channel *nch = spdk_io_channel_get_ctx(ch);

    ctx->reads_completed  += nch->reads_completed;
    ctx->writes_completed += nch->writes_completed;
    ctx->read_bytes        += nch->read_bytes;
    ctx->write_bytes       += nch->write_bytes;

    spdk_for_each_channel_continue(i, 0);
}

static void
_stats_done(struct spdk_io_channel_iter *i, int status)
{
    struct my_null_stats_ctx  *ctx = spdk_io_channel_iter_get_ctx(i);
    struct spdk_json_write_ctx *w   = spdk_jsonrpc_begin_result(ctx->request);

    spdk_json_write_object_begin(w);
    spdk_json_write_named_uint64(w, "reads_completed",  ctx->reads_completed);
    spdk_json_write_named_uint64(w, "writes_completed", ctx->writes_completed);
    spdk_json_write_named_uint64(w, "read_bytes",       ctx->read_bytes);
    spdk_json_write_named_uint64(w, "write_bytes",      ctx->write_bytes);
    spdk_json_write_object_end(w);
    spdk_jsonrpc_end_result(ctx->request, w);

    free(ctx);
}

static void
rpc_bdev_my_null_get_stats(struct spdk_jsonrpc_request *request,
                           const struct spdk_json_val  *params)
{
    struct my_null_stats_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        spdk_jsonrpc_send_error_response(request, -ENOMEM,
                                         spdk_strerror(ENOMEM));
        return;
    }

    ctx->request = request;
    spdk_for_each_channel(&g_my_null_bdev_head,
                          _gather_stats,
                          ctx,
                          _stats_done);
}
SPDK_RPC_REGISTER("bdev_my_null_get_stats", rpc_bdev_my_null_get_stats,
                  SPDK_RPC_RUNTIME)
```

테스트:

```bash
./scripts/rpc.py bdev_my_null_get_stats
# Output:
# {
#   "reads_completed": 3200000,
#   "writes_completed": 0,
#   "read_bytes": 13107200000,
#   "write_bytes": 0
# }
```

---

## 보너스 B -- 설정 가능한 지연 주입

제로 지연 null bdev는 처리량 벤치마크에 유용하지만, 때로는 실제 디바이스를 시뮬레이션하고 싶을 수 있습니다. 채널 폴러가 타이머를 사용하여 적용하는 디바이스별 목표 지연 시간을 추가하세요.

### B.1 디바이스 구조체와 opts에 지연 추가

```c
/* In my_null_bdev.h */
struct my_null_opts {
    char     *name;
    uint64_t  num_blocks;
    uint32_t  block_size;
    uint64_t  latency_us;   /* 0 means complete immediately */
};

/* In my_null_bdev.c */
struct my_null_bdev {
    struct spdk_bdev         bdev;
    TAILQ_ENTRY(my_null_bdev) tailq;
    uint64_t                 latency_us;
};
```

### B.2 각 I/O에 마감 시한 태깅

```c
struct my_null_bdev_io {
    TAILQ_ENTRY(my_null_bdev_io) link;
    uint64_t complete_tsc;   /* TSC value at or after which I/O may complete */
};
```

### B.3 `submit_request`에서 마감 시한 설정

```c
static void
bdev_my_null_submit_request(struct spdk_io_channel *_ch,
                            struct spdk_bdev_io    *bdev_io)
{
    struct my_null_bdev_io *io   = (struct my_null_bdev_io *)bdev_io->driver_ctx;
    struct my_null_io_channel *ch = spdk_io_channel_get_ctx(_ch);
    struct my_null_bdev   *disk  = (struct my_null_bdev *)bdev_io->bdev->ctxt;

    switch (bdev_io->type) {
    case SPDK_BDEV_IO_TYPE_READ:
        if (bdev_io->u.bdev.iovs[0].iov_base != NULL) {
            memset(bdev_io->u.bdev.iovs[0].iov_base, 0,
                   bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
        }
        /* fall through */
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
    case SPDK_BDEV_IO_TYPE_RESET:
        if (disk->latency_us == 0) {
            io->complete_tsc = 0;
        } else {
            io->complete_tsc = spdk_get_ticks() +
                (spdk_get_ticks_hz() * disk->latency_us) / SPDK_SEC_TO_USEC;
        }
        TAILQ_INSERT_TAIL(&ch->pending_io, io, link);
        break;
    default:
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        break;
    }
}
```

### B.4 폴러에서 마감 시한으로 완료 게이팅

```c
static int
bdev_my_null_channel_poll(void *arg)
{
    struct my_null_io_channel *ch  = arg;
    struct my_null_bdev_io    *io;
    uint64_t now = spdk_get_ticks();
    int completed = 0;

    /* Walk in FIFO order; stop at the first I/O not yet due. */
    while ((io = TAILQ_FIRST(&ch->pending_io)) != NULL) {
        if (io->complete_tsc != 0 && now < io->complete_tsc) {
            break;
        }
        TAILQ_REMOVE(&ch->pending_io, io, link);
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io),
                              SPDK_BDEV_IO_STATUS_SUCCESS);
        completed++;
    }

    return completed > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}
```

100 us 시뮬레이션 지연으로 테스트:

```json
{
  "method": "bdev_my_null_create",
  "params": {
    "name": "SlowNull0",
    "num_blocks": 131072,
    "block_size": 4096,
    "latency_us": 100
  }
}
```

100 us 지연과 큐 깊이 128에서 이론적 최대 IOPS는 `128 / 0.0001 = 1,280,000`입니다. bdevperf는 이 수치에 가까운 값을 보고하여 지연 주입이 작동하고 있음을 확인해야 합니다.

---

## 흔한 실수

### 1. `struct spdk_bdev`를 첫 번째 멤버로 포함하지 않음

bdev 계층은 `(struct my_null_bdev *)bdev->ctxt`를 수행합니다. `spdk_bdev`가 오프셋 0에 없으면 캐스팅이 잘못된 포인터를 생성합니다.

```c
/* 잘못된 예 */
struct my_null_bdev {
    int                      some_field;
    struct spdk_bdev         bdev;   /* not at offset 0 */
};

/* 올바른 예 */
struct my_null_bdev {
    struct spdk_bdev         bdev;   /* must be first */
    int                      some_field;
};
```

### 2. `async_fini = true`일 때 `spdk_bdev_module_fini_done()` 호출을 잊음

모듈 구조체에서 `async_fini = true`를 설정했지만 `spdk_bdev_module_fini_done()`을 호출하지 않으면, SPDK가 모듈 종료를 기다리며 셧다운 시 중단됩니다.

```c
static void
bdev_my_null_finish(void)
{
    spdk_io_device_unregister(&g_my_null_bdev_head, NULL);
    spdk_bdev_module_fini_done();   /* 반드시 호출해야 함 */
}
```

### 3. 잘못된 스레드에서 I/O 완료

`spdk_bdev_io_complete()`는 `submit_request`를 호출한 것과 동일한 스레드에서 호출해야 합니다. 완료를 다른 리액터로 지연시키는 경우(예: 메시지를 통해), `spdk_bdev_io_complete`를 호출하기 전에 `spdk_thread_send_msg`를 사용하여 올바른 스레드로 돌아가세요.

### 4. 등록 실패 시 메모리 누수

`bdev_my_null_create`는 `spdk_bdev_register`를 호출하기 전에 디바이스를 할당합니다. 등록이 실패하면 오류를 반환하기 전에 이름과 디바이스 구조체를 모두 해제해야 합니다.

```c
rc = spdk_bdev_register(&disk->bdev);
if (rc != 0) {
    free(disk->bdev.name);  /* strdup된 이름을 잊지 마세요 */
    free(disk);
    return rc;
}
```

### 5. `io_type_supported`에서 `false`를 반환하면서 해당 유형을 처리

bdev 계층은 지원하지 않는다고 광고한 I/O 유형을 차단합니다. 나중에 `submit_request`에서 `false`를 반환한 유형을 처리하면 해당 코드 경로에 절대 도달할 수 없습니다. 두 함수를 동기화 상태로 유지하세요.

### 6. 잘못된 컨텍스트 크기로 io_device 등록

`spdk_io_device_register`는 네 번째 인자로 채널 컨텍스트 크기를 받습니다. `sizeof(struct my_null_io_channel)`과 일치하지 않으면 채널 폴러가 메모리 범위를 벗어나 읽고 쓸 것입니다.

```c
spdk_io_device_register(&g_my_null_bdev_head,
                        bdev_my_null_create_channel_cb,
                        bdev_my_null_destroy_channel_cb,
                        sizeof(struct my_null_io_channel), /* exact size */
                        "my_null");
```

---

## 요약

완전한 bdev 모듈을 구축했습니다:

- `SPDK_BDEV_MODULE_REGISTER`와 수명주기 훅이 있는 **모듈 구조체**
- `struct spdk_bdev`를 래핑하는 **디바이스별 구조체**
- 폴링 완료 큐가 있는 **채널별 구조체**
- 지원되는 모든 I/O 유형을 디스패치하는 **submit_request**
- 생성 및 삭제를 위한 **JSON-RPC 핸들러**
- SPDK 빌드 시스템과 통합되는 **Makefile**
- 수백만 IOPS 처리량을 보여주는 **bdevperf** 검증
- **보너스**: `spdk_for_each_channel`로 집계되는 채널별 통계
- **보너스**: 디바이스 시뮬레이션을 위한 TSC 기반 지연 주입

동일한 스켈레톤이 모든 스토리지 백엔드에 적용됩니다 -- `submit_request` 본문을 실제 DMA, NVMe 명령 제출, 또는 네트워크 I/O로 교체하면 나머지 인프라는 동일하게 유지됩니다.

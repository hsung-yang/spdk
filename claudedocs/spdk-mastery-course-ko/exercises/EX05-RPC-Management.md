# 실습 05: RPC 관리

## 개요

| 항목 | 값 |
|---|---|
| **목표** | SPDK 애플리케이션에 사용자 정의 JSON-RPC 메서드를 추가하여 런타임 설정 및 통계를 노출 |
| **예상 소요 시간** | 2-3시간 |
| **사전 요구사항** | 실습 01 (SPDK 빌드), 실습 02 (Hello World 앱), C 구조체 및 JSON에 대한 친숙함 |
| **주요 API** | `SPDK_RPC_REGISTER`, `spdk_json_decode_object`, `spdk_jsonrpc_begin_result`, `spdk_jsonrpc_end_result` |

---

## 배경 지식

SPDK는 유닉스 도메인 소켓(Unix domain socket)을 통해 JSON-RPC 2.0 서버를 노출합니다. 모든 SPDK 서브시스템은 시작 시 `SPDK_RPC_REGISTER` 매크로를 사용하여 RPC 메서드를 등록하며, 이 매크로는 등록 함수가 `main()` 이전에 C 생성자로 실행되도록 배치합니다. 런타임에 애플리케이션은 폴러 루프에서 `spdk_rpc_server_accept()`를 호출하여 들어오는 요청을 디스패치합니다.

메서드가 호출 가능한 시점을 제어하는 두 가지 상태 마스크가 있습니다:

| 마스크 | 호출 가능 시점 |
|---|---|
| `SPDK_RPC_STARTUP` | 앱 초기화 완료 전에만 |
| `SPDK_RPC_RUNTIME` | 초기화 완료 후에만 |
| `SPDK_RPC_STARTUP \| SPDK_RPC_RUNTIME` | 항상 |

핸들러 함수의 시그니처는 다음과 같습니다:

```c
static void my_rpc_handler(struct spdk_jsonrpc_request *request,
                            const struct spdk_json_val *params);
```

핸들러 내에서 다음 중 하나를 수행합니다:
- `spdk_json_decode_object()`로 `params`를 디코딩하고 작업을 수행한 후 응답을 전송하거나,
- `spdk_jsonrpc_send_error_response()`로 오류를 전송합니다.

---

## 과제 설명

간단한 설정과 런타임 카운터를 유지하는 `rpc_demo`라는 소규모 SPDK 애플리케이션을 빌드합니다. 세 개의 RPC 메서드를 노출합니다:

| 메서드 | 상태 | 설명 |
|---|---|---|
| `rpc_demo_get_config` | RUNTIME | 현재 설정을 JSON 객체로 반환 |
| `rpc_demo_set_config` | RUNTIME | 하나 이상의 설정 필드 업데이트 |
| `rpc_demo_get_stats` | RUNTIME | 런타임 카운터 반환 (처리된 요청 수, 가동 시간) |

**보너스 과제** (섹션 8):
- 잘못된 값에 대한 설명이 포함된 입력 유효성 검사
- `rpc_demo_reset_stats` -- 런타임 카운터를 0으로 초기화

---

## 1단계: 프로젝트 구조

`app/rpc_demo/` 아래에 다음 파일을 생성하세요:

```
app/rpc_demo/
├── Makefile
├── rpc_demo.c      # 앱 스켈레톤, 폴러 루프
└── rpc_demo_rpc.c  # 모든 RPC 핸들러 등록
```

---

## 2단계: 애플리케이션 상태

`rpc_demo.c`에 RPC 메서드가 읽고 쓸 전역 상태를 정의하세요. 간단하게 유지합니다: 설정 구조체와 통계 구조체 -- SPDK 앱 스레드에 의해 보호됩니다 (폴러 루프에서 호출되는 핸들러에 대해 단일 스레드 접근이 보장됨).

```c
/* rpc_demo.c */
#include "spdk/stdinc.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/string.h"

/* ------------------------------------------------------------------ */
/* Application state shared with rpc_demo_rpc.c via this header-like  */
/* extern declarations (or a small internal header).                   */
/* ------------------------------------------------------------------ */

struct rpc_demo_config {
    uint32_t queue_depth;   /* max outstanding I/Os */
    uint32_t timeout_us;    /* per-request timeout in microseconds */
    bool     debug_mode;    /* verbose logging toggle */
    char     target_name[64];
};

struct rpc_demo_stats {
    uint64_t requests_served;
    uint64_t errors;
    uint64_t uptime_ticks; /* updated by the poller */
};

/* Globals – only accessed on the SPDK app thread. */
struct rpc_demo_config g_config = {
    .queue_depth  = 128,
    .timeout_us   = 10000,
    .debug_mode   = false,
    .target_name  = "default",
};

struct rpc_demo_stats g_stats = {0};

static struct spdk_rpc_server *g_rpc_server;
static struct spdk_poller     *g_poller;

/* Forward declarations */
static int  demo_poller(void *arg);
static void demo_start(void *arg1);
static void demo_shutdown(void);

/* ------------------------------------------------------------------ */
/* Poller – called by SPDK reactor every ~1 ms                         */
/* ------------------------------------------------------------------ */
static int
demo_poller(void *arg)
{
    g_stats.uptime_ticks++;

    /* Accept pending RPC connections and dispatch requests. */
    if (g_rpc_server) {
        spdk_rpc_server_accept(g_rpc_server);
    }

    return SPDK_POLLER_BUSY;
}

/* ------------------------------------------------------------------ */
/* App start callback                                                  */
/* ------------------------------------------------------------------ */
static void
demo_start(void *arg1)
{
    const char *rpc_addr = "/var/tmp/rpc_demo.sock";

    SPDK_NOTICELOG("rpc_demo starting\n");

    g_rpc_server = spdk_rpc_server_listen(rpc_addr);
    if (!g_rpc_server) {
        SPDK_ERRLOG("Failed to start RPC server on %s\n", rpc_addr);
        spdk_app_stop(-1);
        return;
    }

    /* Allow runtime RPCs now that we are fully initialized. */
    spdk_rpc_set_state(SPDK_RPC_RUNTIME);

    g_poller = spdk_poller_register(demo_poller, NULL, 1000 /* 1 ms */);
    if (!g_poller) {
        SPDK_ERRLOG("Failed to register poller\n");
        spdk_app_stop(-1);
    }

    SPDK_NOTICELOG("RPC server listening on %s\n", rpc_addr);
}

/* ------------------------------------------------------------------ */
/* App shutdown                                                        */
/* ------------------------------------------------------------------ */
static void
demo_shutdown(void)
{
    spdk_poller_unregister(&g_poller);
    if (g_rpc_server) {
        spdk_rpc_server_close(g_rpc_server);
        g_rpc_server = NULL;
    }
    spdk_app_stop(0);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name            = "rpc_demo";
    opts.shutdown_cb     = demo_shutdown;
    /* Disable the default SPDK RPC server; we start our own. */
    opts.rpc_addr        = NULL;

    rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    rc = spdk_app_start(&opts, demo_start, NULL);
    spdk_app_fini();
    return rc;
}
```

---

## 3단계: 내부 헤더

`rpc_demo_rpc.c`가 전역 변수를 참조할 수 있도록 `rpc_demo_internal.h`를 생성하세요:

```c
/* rpc_demo_internal.h */
#pragma once

#include "spdk/stdinc.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"
#include "spdk/log.h"
#include "spdk/util.h"   /* SPDK_COUNTOF */

struct rpc_demo_config {
    uint32_t queue_depth;
    uint32_t timeout_us;
    bool     debug_mode;
    char     target_name[64];
};

struct rpc_demo_stats {
    uint64_t requests_served;
    uint64_t errors;
    uint64_t uptime_ticks;
};

extern struct rpc_demo_config g_config;
extern struct rpc_demo_stats  g_stats;
```

> **참고:** 실제 SPDK 모듈에서는 이 헤더를 모듈의 private include 디렉토리에 놓을 것입니다. 이 실습에서는 소스 파일과 같은 수준의 단일 헤더로 충분합니다.

---

## 4단계: `rpc_demo_get_config` 구현

이 메서드는 매개변수를 받지 않고 현재 설정을 반환합니다.

```c
/* rpc_demo_rpc.c */
#include "rpc_demo_internal.h"

/* ------------------------------------------------------------------ */
/* rpc_demo_get_config                                                 */
/*   No parameters.                                                    */
/*   Returns: { queue_depth, timeout_us, debug_mode, target_name }    */
/* ------------------------------------------------------------------ */
static void
rpc_demo_get_config(struct spdk_jsonrpc_request *request,
                    const struct spdk_json_val *params)
{
    struct spdk_json_write_ctx *w;

    /*
     * Methods that accept no parameters should reject any params the
     * caller passes.  This is a JSON-RPC 2.0 best practice.
     */
    if (params != NULL) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "rpc_demo_get_config requires no parameters");
        return;
    }

    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_object_begin(w);

    spdk_json_write_named_uint32(w, "queue_depth", g_config.queue_depth);
    spdk_json_write_named_uint32(w, "timeout_us",  g_config.timeout_us);
    spdk_json_write_named_bool  (w, "debug_mode",  g_config.debug_mode);
    spdk_json_write_named_string(w, "target_name", g_config.target_name);

    spdk_json_write_object_end(w);
    spdk_jsonrpc_end_result(request, w);

    g_stats.requests_served++;
}

SPDK_RPC_REGISTER("rpc_demo_get_config", rpc_demo_get_config, SPDK_RPC_RUNTIME)
```

**이 패턴이 하는 일:**

1. `spdk_jsonrpc_begin_result()`가 JSON-RPC 응답 봉투에서 `"result"` 키를 열고 쓰기 컨텍스트를 반환합니다.
2. `spdk_json_write_object_begin/end()`가 필드를 JSON 객체 `{}`로 감쌉니다.
3. `spdk_json_write_named_*()`가 `"key": value` 쌍을 출력합니다.
4. `spdk_jsonrpc_end_result()`가 응답을 완성하고 전송합니다.

---

## 5단계: `rpc_demo_set_config` 구현

이 메서드는 부분 업데이트를 받습니다 -- 모든 필드가 선택적입니다. 디코더 배열의 마지막 열(`true`)은 필드를 선택적으로 표시합니다.

```c
/* ------------------------------------------------------------------ */
/* rpc_demo_set_config                                                 */
/*   Parameters (all optional):                                        */
/*     queue_depth  uint32                                             */
/*     timeout_us   uint32                                             */
/*     debug_mode   bool                                               */
/*     target_name  string                                             */
/*   Returns: true on success                                          */
/* ------------------------------------------------------------------ */

/*
 * Intermediate struct for decoding.  We decode into this first, then
 * copy valid fields into g_config so the live state is never partially
 * updated if decoding fails halfway through.
 */
struct rpc_set_config_req {
    uint32_t queue_depth;
    uint32_t timeout_us;
    bool     debug_mode;
    char    *target_name; /* heap-allocated by spdk_json_decode_string */
};

/*
 * Decoder table.  Each entry maps a JSON key to a struct field.
 * Format: { "json_key", offsetof(struct, field), decoder_fn, optional }
 * optional = true  -> field may be absent in the request
 * optional = false -> field is required; decode fails if absent
 */
static const struct spdk_json_object_decoder rpc_set_config_decoders[] = {
    {
        "queue_depth",
        offsetof(struct rpc_set_config_req, queue_depth),
        spdk_json_decode_uint32,
        true   /* optional */
    },
    {
        "timeout_us",
        offsetof(struct rpc_set_config_req, timeout_us),
        spdk_json_decode_uint32,
        true
    },
    {
        "debug_mode",
        offsetof(struct rpc_set_config_req, debug_mode),
        spdk_json_decode_bool,
        true
    },
    {
        "target_name",
        offsetof(struct rpc_set_config_req, target_name),
        spdk_json_decode_string, /* allocates heap memory */
        true
    },
};

static void
free_rpc_set_config_req(struct rpc_set_config_req *req)
{
    free(req->target_name);
}

static void
rpc_demo_set_config(struct spdk_jsonrpc_request *request,
                    const struct spdk_json_val *params)
{
    struct rpc_set_config_req req = {
        .queue_depth  = g_config.queue_depth,
        .timeout_us   = g_config.timeout_us,
        .debug_mode   = g_config.debug_mode,
        .target_name  = NULL,
    };

    if (params == NULL) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "Parameters required");
        return;
    }

    if (spdk_json_decode_object(params,
                                 rpc_set_config_decoders,
                                 SPDK_COUNTOF(rpc_set_config_decoders),
                                 &req)) {
        SPDK_ERRLOG("spdk_json_decode_object failed for rpc_demo_set_config\n");
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "Invalid parameters");
        free_rpc_set_config_req(&req);
        return;
    }

    /* Apply to live config. */
    g_config.queue_depth = req.queue_depth;
    g_config.timeout_us  = req.timeout_us;
    g_config.debug_mode  = req.debug_mode;

    if (req.target_name != NULL) {
        snprintf(g_config.target_name, sizeof(g_config.target_name),
                 "%s", req.target_name);
    }

    free_rpc_set_config_req(&req);
    g_stats.requests_served++;

    spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("rpc_demo_set_config", rpc_demo_set_config, SPDK_RPC_RUNTIME)
```

---

## 6단계: `rpc_demo_get_stats` 구현

```c
/* ------------------------------------------------------------------ */
/* rpc_demo_get_stats                                                  */
/*   No parameters.                                                    */
/*   Returns: { requests_served, errors, uptime_ticks }               */
/* ------------------------------------------------------------------ */
static void
rpc_demo_get_stats(struct spdk_jsonrpc_request *request,
                   const struct spdk_json_val *params)
{
    struct spdk_json_write_ctx *w;

    if (params != NULL) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "rpc_demo_get_stats requires no parameters");
        return;
    }

    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_object_begin(w);

    spdk_json_write_named_uint64(w, "requests_served", g_stats.requests_served);
    spdk_json_write_named_uint64(w, "errors",          g_stats.errors);
    spdk_json_write_named_uint64(w, "uptime_ticks",    g_stats.uptime_ticks);

    spdk_json_write_object_end(w);
    spdk_jsonrpc_end_result(request, w);

    g_stats.requests_served++;
}

SPDK_RPC_REGISTER("rpc_demo_get_stats", rpc_demo_get_stats, SPDK_RPC_RUNTIME)
```

---

## 7단계: Makefile

```makefile
# app/rpc_demo/Makefile
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = rpc_demo

C_SRCS = rpc_demo.c rpc_demo_rpc.c

SPDK_LIB_LIST = event log util jsonrpc json rpc thread

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
```

SPDK 루트에서 빌드:

```bash
cd /path/to/spdk
make -C app/rpc_demo
```

---

## 8단계: 빌드 및 실행

### 8.1 SPDK 설정 및 빌드 (아직 하지 않은 경우)

```bash
cd /path/to/spdk
./configure --with-shared
make -j$(nproc)
```

### 8.2 데모 앱 빌드

```bash
make -C app/rpc_demo
```

### 8.3 앱 실행

```bash
sudo ./app/rpc_demo/rpc_demo \
    --no-shconf \
    -m 0x1 \
    --log-level=rpc_demo:DEBUG
```

앱이 출력합니다:
```
rpc_demo: RPC server listening on /var/tmp/rpc_demo.sock
```

---

## 9단계: `scripts/rpc.py`로 테스트

두 번째 터미널을 엽니다. SPDK는 표준 RPC 클라이언트로 `scripts/rpc.py`를 제공합니다.

### 9.1 현재 설정 가져오기

```bash
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_get_config
```

예상 응답:
```json
{
  "queue_depth": 128,
  "timeout_us": 10000,
  "debug_mode": false,
  "target_name": "default"
}
```

### 9.2 설정 업데이트 (부분 업데이트)

```bash
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock \
    rpc_demo_set_config \
    --queue-depth 256 \
    --debug-mode true
```

> `rpc.py`는 `--queue-depth`를 자동으로 `"queue_depth"`로 변환합니다.

변경 사항 확인:

```bash
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_get_config
```

```json
{
  "queue_depth": 256,
  "timeout_us": 10000,
  "debug_mode": true,
  "target_name": "default"
}
```

### 9.3 런타임 통계 가져오기

```bash
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_get_stats
```

```json
{
  "requests_served": 3,
  "errors": 0,
  "uptime_ticks": 12847
}
```

### 9.4 `curl`로 테스트 (원시 JSON-RPC 2.0)

`rpc.py`를 사용할 수 없는 경우 유닉스 소켓에 대해 `curl`을 사용할 수 있습니다:

```bash
curl --unix-socket /var/tmp/rpc_demo.sock \
     -X POST \
     -H "Content-Type: application/json" \
     -d '{
           "jsonrpc": "2.0",
           "id": 1,
           "method": "rpc_demo_get_config"
         }' \
     http://localhost/
```

`curl`로 설정 업데이트:

```bash
curl --unix-socket /var/tmp/rpc_demo.sock \
     -X POST \
     -H "Content-Type: application/json" \
     -d '{
           "jsonrpc": "2.0",
           "id": 2,
           "method": "rpc_demo_set_config",
           "params": {
             "queue_depth": 64,
             "target_name": "nvme0"
           }
         }' \
     http://localhost/
```

예상:
```json
{"jsonrpc":"2.0","id":2,"result":true}
```

### 9.5 오류 처리 테스트

매개변수가 없는 메서드에 예상치 못한 매개변수를 전달:

```bash
curl --unix-socket /var/tmp/rpc_demo.sock \
     -X POST \
     -H "Content-Type: application/json" \
     -d '{
           "jsonrpc": "2.0",
           "id": 3,
           "method": "rpc_demo_get_config",
           "params": {"bad_field": 1}
         }' \
     http://localhost/
```

예상 오류 응답:
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "error": {
    "code": -32602,
    "message": "rpc_demo_get_config requires no parameters"
  }
}
```

---

## 보너스 과제

### 보너스 A: `rpc_demo_set_config`의 매개변수 유효성 검사

디코딩 후 `g_config`에 적용하기 전에 범위 검사를 추가하세요:

```c
/* Inside rpc_demo_set_config, after spdk_json_decode_object succeeds */

if (req.queue_depth == 0 || req.queue_depth > 4096) {
    spdk_jsonrpc_send_error_response(request,
                                     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                     "queue_depth must be between 1 and 4096");
    free_rpc_set_config_req(&req);
    g_stats.errors++;
    return;
}

if (req.timeout_us < 100 || req.timeout_us > 1000000) {
    spdk_jsonrpc_send_error_response(request,
                                     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                     "timeout_us must be between 100 and 1000000");
    free_rpc_set_config_req(&req);
    g_stats.errors++;
    return;
}

if (req.target_name != NULL && strlen(req.target_name) == 0) {
    spdk_jsonrpc_send_error_response(request,
                                     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                     "target_name must not be empty");
    free_rpc_set_config_req(&req);
    g_stats.errors++;
    return;
}
```

유효성 검사 테스트:

```bash
curl --unix-socket /var/tmp/rpc_demo.sock \
     -X POST \
     -H "Content-Type: application/json" \
     -d '{"jsonrpc":"2.0","id":4,"method":"rpc_demo_set_config",
          "params":{"queue_depth": 0}}' \
     http://localhost/
```

예상:
```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "error": {
    "code": -32602,
    "message": "queue_depth must be between 1 and 4096"
  }
}
```

### 보너스 B: `rpc_demo_reset_stats` 구현

통계 카운터를 0으로 초기화하는 메서드를 추가하세요.

```c
/* ------------------------------------------------------------------ */
/* rpc_demo_reset_stats                                                */
/*   No parameters.                                                    */
/*   Returns: true                                                     */
/* ------------------------------------------------------------------ */
static void
rpc_demo_reset_stats(struct spdk_jsonrpc_request *request,
                     const struct spdk_json_val *params)
{
    if (params != NULL) {
        spdk_jsonrpc_send_error_response(request,
                                         SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                         "rpc_demo_reset_stats requires no parameters");
        return;
    }

    memset(&g_stats, 0, sizeof(g_stats));
    SPDK_NOTICELOG("Stats reset via RPC\n");

    spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("rpc_demo_reset_stats", rpc_demo_reset_stats, SPDK_RPC_RUNTIME)
```

전체 흐름 테스트:

```bash
# 리셋 전 통계 확인
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_get_stats

# 리셋
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_reset_stats

# 0으로 초기화되었는지 확인
python3 scripts/rpc.py -s /var/tmp/rpc_demo.sock rpc_demo_get_stats
```

---

## 흔한 실수

### 1. `spdk_json_decode_string`에서 할당된 힙 메모리를 해제하지 않음

`spdk_json_decode_string`은 `malloc`을 호출합니다. 모든 조기 반환 오류 경로에서 해제 함수를 호출해야 합니다. 그렇지 않으면 잘못된 요청마다 메모리가 누수됩니다.

```c
/* 잘못된 예: 디코딩 성공 후 유효성 검사 실패 시 req.target_name 누수 */
if (spdk_json_decode_object(...)) { return; }
if (req.queue_depth > 4096)      { return; } /* 누수! */
free(req.target_name);

/* 올바른 예: 반환 전 항상 해제 */
if (spdk_json_decode_object(...)) { goto invalid; }
if (req.queue_depth > 4096)      { goto invalid; }
free_rpc_set_config_req(&req);
spdk_jsonrpc_send_bool_response(request, true);
return;
invalid:
    free_rpc_set_config_req(&req);
    spdk_jsonrpc_send_error_response(...);
```

### 2. `spdk_rpc_set_state(SPDK_RPC_RUNTIME)` 전에 RPC 메서드 호출

`SPDK_RPC_RUNTIME` 메서드는 애플리케이션이 `spdk_rpc_set_state(SPDK_RPC_RUNTIME)`을 호출할 때까지 조용히 거부됩니다. 초기화가 완료된 후, 그 이전이 아니라 항상 상태를 설정하세요.

### 3. 초기화 전용 메서드에 잘못된 상태 마스크 사용

정적 설정(예: 수신 주소)을 수정하는 메서드는 `SPDK_RPC_RUNTIME`이 아닌 `SPDK_RPC_STARTUP`을 사용해야 합니다. 시작 후 호출하면 효과가 없거나 오류가 발생합니다. 각 메서드가 어떤 마스크를 받아야 하는지 신중하게 생각하세요.

### 4. `spdk_jsonrpc_begin_result` 이후 `spdk_jsonrpc_end_result`를 호출하지 않음

`spdk_jsonrpc_begin_result()`를 호출한 후에는 모든 코드 경로에서 **반드시** `spdk_jsonrpc_end_result()`를 호출해야 합니다. 그렇지 않으면 요청이 매달려 있고 클라이언트가 영원히 대기합니다. C에는 RAII가 없으므로 -- 모든 분기를 추적하세요.

### 5. `SPDK_COUNTOF`와 `sizeof` 혼동

디코더 배열 인자는 `sizeof(decoder_array)`가 아닌 `SPDK_COUNTOF(decoder_array)` (요소 수)를 사용해야 합니다. `sizeof`를 전달하면 가비지 필드를 디코딩하는 런타임 버그가 됩니다.

```c
/* 잘못된 예 */
spdk_json_decode_object(params, decoders, sizeof(decoders), &req);

/* 올바른 예 */
spdk_json_decode_object(params, decoders, SPDK_COUNTOF(decoders), &req);
```

### 6. 디코딩 완료 전에 전역 상태 수정

`spdk_json_decode_object`가 성공을 반환하기 전에 절대로 부분 설정을 적용하지 마세요. 임시 구조체로 디코딩하고, 유효성을 검사한 후, 라이브 전역에 복사하세요.

### 7. 앱 스레드가 아닌 곳에서 RPC 핸들러 호출

SPDK RPC 서버는 `spdk_rpc_server_accept()` 내에서 앱 스레드에서 디스패치합니다. 앱 스레드가 아닌 다른 리액터 스레드에서 `spdk_rpc_server_accept()`를 호출하면 데이터 경쟁이 발생합니다. accept 호출을 메인 스레드 폴러에 유지하세요.

---

## 요약

이 실습에서 수행한 내용:

- `SPDK_RPC_REGISTER`를 사용하여 세 개의 사용자 정의 RPC 메서드를 등록함
- `spdk_json_decode_object`와 디코더 테이블로 JSON 매개변수를 디코딩함
- `spdk_jsonrpc_begin_result` / `spdk_json_write_named_*` / `spdk_jsonrpc_end_result`를 사용하여 JSON 응답을 구성함
- `spdk_jsonrpc_send_bool_response`와 `spdk_jsonrpc_send_error_response`로 불리언 및 오류 응답을 전송함
- `scripts/rpc.py`와 원시 `curl` 모두로 모든 메서드를 테스트함
- (보너스) 입력 유효성 검사와 `reset_stats` 메서드를 추가함

동일한 패턴 -- 디코더 테이블, 디코딩, 유효성 검사, 응답 -- 은 모든 SPDK 서브시스템의 모든 RPC 메서드에 적용됩니다. 이를 마스터하면 모든 SPDK 애플리케이션에 런타임 관측성과 제어를 추가할 수 있는 능력이 열립니다.

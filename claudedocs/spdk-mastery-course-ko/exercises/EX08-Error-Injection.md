# 실습 08: 오류 주입 (Error Injection)

## 개요

| 항목 | 내용 |
|-------|---------|
| **목표** | null bdev 모듈을 확장하여 설정 가능한 오류 주입(Error Injection)을 구현하고, 하드웨어 장애를 시뮬레이션하며, 애플리케이션 계층으로의 오류 전파를 측정하고, SPDK bdev 완료(completion)가 실패를 어떻게 알리는지 이해합니다. |
| **사전 요구사항** | 실습 04 (커스텀 Bdev 모듈), `spdk_bdev_io_complete`, `SPDK_BDEV_IO_STATUS_*` 상수, SPDK RPC 시스템에 대한 이해 |
| **예상 소요시간** | 2-3시간 |
| **난이도** | 중급 |

---

## 배경

실제 스토리지 장치는 다양한 방식으로 장애가 발생합니다: 임의 비율의 I/O가 오류를 반환하거나, 특정 작업 유형(읽기 vs 쓰기)이 더 오류에 취약하거나, 잠재적 장애가 추가 지연으로 모델링될 수 있습니다. SPDK의 bdev 레이어는 `spdk_bdev_io_complete(bdev_io, status)`를 유일한 완료 경로로 노출하므로, bdev 드라이버 내부에서 어떤 장애 상태든 간단하게 주입할 수 있습니다.

기존 null bdev(`module/bdev/null/bdev_null.c`)가 이상적인 기반입니다: 모든 I/O를 수락하고, 채널별 목록에 큐잉한 후, 폴러(poller)에서 목록을 비우면서 모든 I/O에 대해 `spdk_bdev_io_complete(..., SPDK_BDEV_IO_STATUS_SUCCESS)`를 호출합니다. 여기에 설정 가능한 규칙에 따라 `SPDK_BDEV_IO_STATUS_FAILED`를 발생시키는 두 번째 완료 경로를 추가하게 됩니다.

이 실습에서 사용하는 주요 완료 상태 값:

```c
SPDK_BDEV_IO_STATUS_SUCCESS   /*  0 - 정상 완료               */
SPDK_BDEV_IO_STATUS_FAILED    /* -1 - 일반 실패               */
SPDK_BDEV_IO_STATUS_ABORTED   /* -2 - I/O가 중단됨            */
SPDK_BDEV_IO_STATUS_NOMEM     /* -4 - 메모리 부족             */
```

---

## 작업 설명

`bdev_null`을 확장하여 bdev별 설정 구조체로 제어되는 세 가지 독립적인 주입 모드를 구현합니다:

1. **오류 비율 주입(Error Rate Injection)** - 설정 가능한 비율(0-100%)의 모든 I/O를 실패 처리합니다.
2. **I/O 유형 필터(I/O-Type Filter)** - 주입을 읽기, 쓰기 또는 양쪽 모두로 제한합니다.
3. **지연 주입(Latency Injection)** - 대상 I/O의 완료를 설정 가능한 마이크로초만큼 지연시킵니다.

세 모드 모두 SPDK를 재시작하지 않고 새로운 JSON-RPC 메서드(`bdev_null_set_error_injection`)를 통해 런타임에 제어할 수 있어야 합니다.

---

## 단계별 지침

### 1단계 - 기존 null bdev 구조 이해

코드를 작성하기 전에, 핵심 구조체와 완료 폴러를 읽어봅니다:

```c
/* module/bdev/null/bdev_null.c - 수정할 구조체들 */

struct null_bdev_io {
    TAILQ_ENTRY(null_bdev_io) link;
    /* 추가 예정: uint64_t delay_tsc; */
};

struct null_bdev {
    struct spdk_bdev    bdev;
    TAILQ_ENTRY(null_bdev) tailq;
    /* 여기에 오류 주입 설정을 추가할 예정 */
};

struct null_io_channel {
    struct spdk_poller              *poller;
    TAILQ_HEAD(, null_bdev_io)       io;          /* 대기 중인 I/O */
    /* 여기에 지연된 I/O 목록을 추가할 예정 */
};
```

폴러 콜백은 `ch->io`를 순회하면서, 각 항목에 대해 `spdk_bdev_io_complete(..., SPDK_BDEV_IO_STATUS_SUCCESS)`를 호출하고 목록을 비웁니다. 주입 로직은 이 폴러에서 완료를 가로챕니다.

### 2단계 - 오류 주입 설정 구조체 추가

`module/bdev/null/bdev_null.c`를 엽니다. 기존 구조체 정의 바로 뒤(약 27번째 줄)에 다음 구조체를 추가하고 `null_bdev`를 확장합니다:

```c
/* 오류를 주입할 I/O 유형의 비트마스크 */
#define NULL_INJECT_IO_READ        (1u << SPDK_BDEV_IO_TYPE_READ)
#define NULL_INJECT_IO_WRITE       (1u << SPDK_BDEV_IO_TYPE_WRITE)
#define NULL_INJECT_IO_ALL         (NULL_INJECT_IO_READ | NULL_INJECT_IO_WRITE)

struct null_error_inject_config {
    bool     enabled;          /* 마스터 스위치                           */
    uint32_t error_rate_pct;   /* 0-100: 실패시킬 I/O의 비율              */
    uint32_t io_type_mask;     /* 비트마스크: 영향을 줄 I/O 유형           */
    uint64_t latency_us;       /* 추가 지연 시간(마이크로초, 0 = 없음)      */
    uint64_t io_counter;       /* 결정론적 비율을 위한 순환 카운터          */
};
```

`null_bdev`를 확장하여 이 설정을 포함합니다:

```c
struct null_bdev {
    struct spdk_bdev                  bdev;
    TAILQ_ENTRY(null_bdev)            tailq;
    struct null_error_inject_config   inject;    /* <-- 새 필드 */
};
```

`null_bdev_io`를 확장하여 지연 완료를 위한 절대 TSC 마감 시한을 포함합니다:

```c
struct null_bdev_io {
    TAILQ_ENTRY(null_bdev_io) link;
    uint64_t                  complete_tsc;  /* 0 = 즉시 완료 */
};
```

`null_io_channel`을 확장하여 지연된 I/O를 위한 별도 목록을 추가합니다:

```c
struct null_io_channel {
    struct spdk_poller              *poller;
    TAILQ_HEAD(, null_bdev_io)       io;          /* 완료 준비됨        */
    TAILQ_HEAD(, null_bdev_io)       delayed_io;  /* 마감 시한 대기 중  */
};
```

`bdev_null_create_cb`(io_channel 생성 콜백)에서 새 목록을 초기화합니다. `TAILQ_INIT(&ch->io)`를 검색하고 다음을 추가합니다:

```c
TAILQ_INIT(&ch->delayed_io);
```

### 3단계 - 주입 판단 헬퍼 구현

구조체 정의 뒤, `bdev_null_submit_request` 앞에 주어진 I/O를 실패시킬지 결정하는 정적 헬퍼를 추가합니다:

```c
/*
 * 이 I/O에 오류를 주입해야 하는지 여부를 반환합니다.
 * rand()에 의존하지 않고 단순한 모듈러 카운터를 사용하여 오류 비율이
 * 결정론적이므로 테스트의 재현성이 보장됩니다.
 */
static bool
null_should_inject_error(struct null_bdev *nbdev, enum spdk_bdev_io_type io_type)
{
    struct null_error_inject_config *cfg = &nbdev->inject;

    if (!cfg->enabled) {
        return false;
    }

    /* I/O 유형 필터 확인 */
    if (!(cfg->io_type_mask & (1u << io_type))) {
        return false;
    }

    if (cfg->error_rate_pct == 0) {
        return false;
    }

    /* 결정론적: 매 (100 / error_rate_pct)번째 I/O를 실패 */
    cfg->io_counter++;
    return ((cfg->io_counter * cfg->error_rate_pct) / 100) >
           (((cfg->io_counter - 1) * cfg->error_rate_pct) / 100);
}
```

### 4단계 - `bdev_null_submit_request` 수정으로 오류 및 지연 주입

제출(submit) 경로는 I/O를 `ch->io`에 큐잉합니다. `TAILQ_INSERT_TAIL(&ch->io, null_io, link)` 호출(READ, WRITE, WRITE_ZEROES, RESET 케이스)을 지연이 설정되었을 때 `ch->delayed_io`로 라우팅하거나, 오류 판단 후 즉시 완료를 위해 `complete_tsc = 0`을 표시하는 헬퍼로 교체합니다:

```c
static void
null_enqueue_io(struct null_io_channel *ch, struct null_bdev *nbdev,
                struct null_bdev_io *null_io, struct spdk_bdev_io *bdev_io)
{
    struct null_error_inject_config *cfg = &nbdev->inject;

    if (null_should_inject_error(nbdev, bdev_io->type)) {
        /*
         * 오류 경로: 지연 주입도 설정된 경우, 오류 신호를 보내기 전에
         * 지연시킵니다(느린 장애 장치 시뮬레이션).
         * 그렇지 않으면 FAILED 상태로 즉시 완료합니다.
         */
        if (cfg->latency_us > 0) {
            null_io->complete_tsc = spdk_get_ticks() +
                                    cfg->latency_us * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
            /* 상위 플래그의 비트를 재사용; 오류-지연으로 표시 */
            null_io->complete_tsc |= (1ULL << 63);  /* 오류 센티넬 비트 */
            TAILQ_INSERT_TAIL(&ch->delayed_io, null_io, link);
        } else {
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        }
        return;
    }

    /* 성공 경로 */
    if (cfg->latency_us > 0 && (cfg->io_type_mask & (1u << bdev_io->type))) {
        null_io->complete_tsc = spdk_get_ticks() +
                                cfg->latency_us * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
        TAILQ_INSERT_TAIL(&ch->delayed_io, null_io, link);
    } else {
        null_io->complete_tsc = 0;
        TAILQ_INSERT_TAIL(&ch->io, null_io, link);
    }
}
```

모든 큐잉 케이스에서 `bdev_null_submit_request`를 업데이트합니다:

```c
case SPDK_BDEV_IO_TYPE_READ:
    /* ... 기존 DIF 처리 ... */
    null_enqueue_io(ch, spdk_io_channel_get_ctx(spdk_bdev_io_get_io_channel(bdev_io)),
                    null_io, bdev_io);
    /* 직접 TAILQ_INSERT_TAIL 호출을 위 호출로 교체 */
    break;
case SPDK_BDEV_IO_TYPE_WRITE:
    /* ... 기존 DIF 처리 ... */
    null_enqueue_io(ch, ..., null_io, bdev_io);
    break;
case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
case SPDK_BDEV_IO_TYPE_RESET:
    null_enqueue_io(ch, ..., null_io, bdev_io);
    break;
```

### 5단계 - 지연된 I/O를 드레인하도록 폴러 업데이트

기존 폴러는 `ch->io`를 한 번에 드레인합니다. 마감 시한이 지난 `ch->delayed_io`도 드레인하도록 확장합니다:

```c
static int
bdev_null_poll(void *arg)
{
    struct null_io_channel *ch = arg;
    uint32_t count;
    TAILQ_HEAD(, null_bdev_io) completed;
    struct null_bdev_io *null_io, *tmp;
    uint64_t now = spdk_get_ticks();

    TAILQ_INIT(&completed);
    TAILQ_SWAP(&completed, &ch->io, null_bdev_io, link);

    count = 0;
    TAILQ_FOREACH_SAFE(null_io, &completed, link, tmp) {
        TAILQ_REMOVE(&completed, null_io, link);
        spdk_bdev_io_complete(spdk_bdev_io_from_ctx(null_io),
                              SPDK_BDEV_IO_STATUS_SUCCESS);
        count++;
    }

    /* 마감 시한이 지난 지연된 I/O 드레인 */
    TAILQ_FOREACH_SAFE(null_io, &ch->delayed_io, link, tmp) {
        uint64_t deadline = null_io->complete_tsc & ~(1ULL << 63);
        bool is_error     = !!(null_io->complete_tsc & (1ULL << 63));

        if (now >= deadline) {
            TAILQ_REMOVE(&ch->delayed_io, null_io, link);
            spdk_bdev_io_complete(spdk_bdev_io_from_ctx(null_io),
                                  is_error ? SPDK_BDEV_IO_STATUS_FAILED
                                           : SPDK_BDEV_IO_STATUS_SUCCESS);
            count++;
        }
    }

    return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}
```

### 6단계 - RPC 메서드 `bdev_null_set_error_injection` 추가

새 파일 `module/bdev/null/bdev_null_rpc.c`를 생성합니다(기존 RPC 파일이 있으면 거기에 추가):

```c
/* module/bdev/null/bdev_null_rpc.c */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "bdev_null.h"   /* bdev_null_set_error_injection()을 노출 */

struct rpc_bdev_null_set_error_injection {
    char     *name;
    bool      enabled;
    uint32_t  error_rate_pct;
    bool      inject_reads;
    bool      inject_writes;
    uint64_t  latency_us;
};

static const struct spdk_json_object_decoder
rpc_bdev_null_set_error_injection_decoders[] = {
    {"name",            offsetof(struct rpc_bdev_null_set_error_injection, name),
     spdk_json_decode_string},
    {"enabled",         offsetof(struct rpc_bdev_null_set_error_injection, enabled),
     spdk_json_decode_bool, true},
    {"error_rate_pct",  offsetof(struct rpc_bdev_null_set_error_injection, error_rate_pct),
     spdk_json_decode_uint32, true},
    {"inject_reads",    offsetof(struct rpc_bdev_null_set_error_injection, inject_reads),
     spdk_json_decode_bool, true},
    {"inject_writes",   offsetof(struct rpc_bdev_null_set_error_injection, inject_writes),
     spdk_json_decode_bool, true},
    {"latency_us",      offsetof(struct rpc_bdev_null_set_error_injection, latency_us),
     spdk_json_decode_uint64, true},
};

static void
rpc_bdev_null_set_error_injection(struct spdk_jsonrpc_request *request,
                                   const struct spdk_json_val *params)
{
    struct rpc_bdev_null_set_error_injection req = {
        .enabled        = true,
        .error_rate_pct = 10,
        .inject_reads   = true,
        .inject_writes  = true,
        .latency_us     = 0,
    };
    struct null_error_inject_opts opts = {};
    int rc;

    if (spdk_json_decode_object(params,
                                rpc_bdev_null_set_error_injection_decoders,
                                SPDK_COUNTOF(rpc_bdev_null_set_error_injection_decoders),
                                &req)) {
        SPDK_ERRLOG("spdk_json_decode_object failed\n");
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                         "spdk_json_decode_object failed");
        goto cleanup;
    }

    opts.enabled        = req.enabled;
    opts.error_rate_pct = req.error_rate_pct;
    opts.io_type_mask   = 0;
    if (req.inject_reads)  { opts.io_type_mask |= NULL_INJECT_IO_READ;  }
    if (req.inject_writes) { opts.io_type_mask |= NULL_INJECT_IO_WRITE; }
    opts.latency_us     = req.latency_us;

    rc = bdev_null_set_error_injection(req.name, &opts);
    if (rc != 0) {
        spdk_jsonrpc_send_error_response_fmt(request, rc,
                                             "Failed to set error injection: %s",
                                             spdk_strerror(-rc));
        goto cleanup;
    }

    spdk_jsonrpc_send_bool_response(request, true);

cleanup:
    free(req.name);
}

SPDK_RPC_REGISTER("bdev_null_set_error_injection",
                  rpc_bdev_null_set_error_injection,
                  SPDK_RPC_RUNTIME)
```

`module/bdev/null/bdev_null.h`에 공개 API 선언을 추가합니다:

```c
struct null_error_inject_opts {
    bool     enabled;
    uint32_t error_rate_pct;   /* 0-100                                   */
    uint32_t io_type_mask;     /* NULL_INJECT_IO_READ | NULL_INJECT_IO_WRITE */
    uint64_t latency_us;       /* 0 = 추가 지연 없음                       */
};

/* RPC 레이어에서 사용하기 위해 내보내는 매크로 */
#define NULL_INJECT_IO_READ   (1u << SPDK_BDEV_IO_TYPE_READ)
#define NULL_INJECT_IO_WRITE  (1u << SPDK_BDEV_IO_TYPE_WRITE)

int bdev_null_set_error_injection(const char *bdev_name,
                                   const struct null_error_inject_opts *opts);
```

`bdev_null.c`에서 `bdev_null_set_error_injection`을 구현합니다:

```c
int
bdev_null_set_error_injection(const char *bdev_name,
                               const struct null_error_inject_opts *opts)
{
    struct null_bdev *nbdev;

    if (!opts || opts->error_rate_pct > 100) {
        return -EINVAL;
    }

    TAILQ_FOREACH(nbdev, &g_null_bdev_head, tailq) {
        if (strcmp(nbdev->bdev.name, bdev_name) == 0) {
            nbdev->inject.enabled        = opts->enabled;
            nbdev->inject.error_rate_pct = opts->error_rate_pct;
            nbdev->inject.io_type_mask   = opts->io_type_mask;
            nbdev->inject.latency_us     = opts->latency_us;
            nbdev->inject.io_counter     = 0;  /* 설정 변경 시 카운터 리셋 */
            return 0;
        }
    }

    return -ENODEV;
}
```

### 7단계 - 빌드 시스템 연결

`module/bdev/null/CMakeLists.txt`(또는 해당 `Makefile`)을 편집하여 새 RPC 소스를 포함합니다:

```cmake
# CMakeLists.txt (CMake 기반 빌드를 사용하는 경우)
target_sources(bdev_null PRIVATE
    bdev_null.c
    bdev_null_rpc.c   # <-- 이 줄 추가
)
```

기존 SPDK Makefile로 빌드하는 경우, `module/bdev/null/Makefile`을 찾아 `C_SRCS` 변수에 `bdev_null_rpc.c`를 추가합니다:

```makefile
C_SRCS = bdev_null.c bdev_null_rpc.c
```

---

## 전체 솔루션 코드

아래는 `module/bdev/null/bdev_null.c`에 대한 최소한의, 자체 완결적인 diff로 추가 부분만 보여줍니다. `+` 접두사가 있는 줄은 새로운 것이고, 컨텍스트 줄에는 접두사가 없습니다.

```diff
--- a/module/bdev/null/bdev_null.c
+++ b/module/bdev/null/bdev_null.c
+#define NULL_INJECT_IO_READ   (1u << SPDK_BDEV_IO_TYPE_READ)
+#define NULL_INJECT_IO_WRITE  (1u << SPDK_BDEV_IO_TYPE_WRITE)
+#define NULL_INJECT_IO_ALL    (NULL_INJECT_IO_READ | NULL_INJECT_IO_WRITE)
+
+struct null_error_inject_config {
+    bool     enabled;
+    uint32_t error_rate_pct;
+    uint32_t io_type_mask;
+    uint64_t latency_us;
+    uint64_t io_counter;
+};

 struct null_bdev_io {
     TAILQ_ENTRY(null_bdev_io) link;
+    uint64_t complete_tsc;   /* 0 = 즉시 완료; bit63 = 오류 플래그 */
 };

 struct null_bdev {
     struct spdk_bdev    bdev;
     TAILQ_ENTRY(null_bdev) tailq;
+    struct null_error_inject_config inject;
 };

 struct null_io_channel {
     struct spdk_poller              *poller;
     TAILQ_HEAD(, null_bdev_io)       io;
+    TAILQ_HEAD(, null_bdev_io)       delayed_io;
 };

+static bool
+null_should_inject_error(struct null_bdev *nbdev, enum spdk_bdev_io_type io_type)
+{
+    struct null_error_inject_config *cfg = &nbdev->inject;
+    if (!cfg->enabled || cfg->error_rate_pct == 0) { return false; }
+    if (!(cfg->io_type_mask & (1u << io_type)))     { return false; }
+    cfg->io_counter++;
+    return ((cfg->io_counter * cfg->error_rate_pct) / 100) >
+           (((cfg->io_counter - 1) * cfg->error_rate_pct) / 100);
+}
+
+static void
+null_enqueue_io(struct null_io_channel *ch, struct null_bdev *nbdev,
+                struct null_bdev_io *null_io, struct spdk_bdev_io *bdev_io)
+{
+    struct null_error_inject_config *cfg = &nbdev->inject;
+    bool inject_err = null_should_inject_error(nbdev, bdev_io->type);
+
+    if (inject_err && cfg->latency_us == 0) {
+        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
+        return;
+    }
+
+    if (cfg->latency_us > 0) {
+        uint64_t deadline = spdk_get_ticks() +
+                            cfg->latency_us * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
+        null_io->complete_tsc = inject_err ? (deadline | (1ULL << 63)) : deadline;
+        TAILQ_INSERT_TAIL(&ch->delayed_io, null_io, link);
+    } else {
+        null_io->complete_tsc = 0;
+        TAILQ_INSERT_TAIL(&ch->io, null_io, link);
+    }
+}

 /* bdev_null_poll에서 - 기존 드레인 루프 확장: */
+    TAILQ_FOREACH_SAFE(null_io, &ch->delayed_io, link, tmp) {
+        uint64_t deadline = null_io->complete_tsc & ~(1ULL << 63);
+        bool     is_error = !!(null_io->complete_tsc & (1ULL << 63));
+        if (spdk_get_ticks() >= deadline) {
+            TAILQ_REMOVE(&ch->delayed_io, null_io, link);
+            spdk_bdev_io_complete(spdk_bdev_io_from_ctx(null_io),
+                                  is_error ? SPDK_BDEV_IO_STATUS_FAILED
+                                           : SPDK_BDEV_IO_STATUS_SUCCESS);
+            count++;
+        }
+    }

+int
+bdev_null_set_error_injection(const char *bdev_name,
+                               const struct null_error_inject_opts *opts)
+{
+    struct null_bdev *nbdev;
+    if (!opts || opts->error_rate_pct > 100) { return -EINVAL; }
+    TAILQ_FOREACH(nbdev, &g_null_bdev_head, tailq) {
+        if (strcmp(nbdev->bdev.name, bdev_name) == 0) {
+            nbdev->inject.enabled        = opts->enabled;
+            nbdev->inject.error_rate_pct = opts->error_rate_pct;
+            nbdev->inject.io_type_mask   = opts->io_type_mask;
+            nbdev->inject.latency_us     = opts->latency_us;
+            nbdev->inject.io_counter     = 0;
+            return 0;
+        }
+    }
+    return -ENODEV;
+}
```

---

## 빌드 지침

```bash
# SPDK 루트 디렉토리에서:
cd /path/to/spdk

# 설정 (아직 수행하지 않은 경우):
./configure --with-shared

# bdev_null 모듈과 그 의존성만 빌드:
make -j$(nproc) DPDKDIR=/path/to/dpdk

# 또는 전체 트리 빌드:
make -j$(nproc)
```

예상 출력: `module/bdev/null/`에서 오류 없음. 미사용 변수에 대한 경고는 switch 케이스 중 하나에서 `null_enqueue_io`를 호출하지 않았음을 나타냅니다.

---

## 실행 지침

### 1 - null bdev 애플리케이션으로 SPDK 시작

```bash
sudo ./build/bin/spdk_tgt -c /path/to/config.json &
```

최소 JSON 설정 예시 (`null_ei_test.json`):

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
            "num_blocks": 204800,
            "block_size": 512
          }
        }
      ]
    }
  ]
}
```

### 2 - 읽기에만 20% 오류 주입 활성화, 지연 없음

```bash
./scripts/rpc.py bdev_null_set_error_injection \
    --name Null0 \
    --enabled true \
    --error-rate-pct 20 \
    --inject-reads true \
    --inject-writes false \
    --latency-us 0
```

### 3 - bdevperf를 실행하여 실패 관찰

```bash
sudo ./build/examples/bdevperf \
    -c null_ei_test.json \
    -q 64 -o 4096 -w read -t 10 \
    -r /var/tmp/spdk.sock
```

예상 결과: bdevperf가 0이 아닌 `io_errors`를 보고합니다. 읽기 I/O의 약 20%가 실패합니다.

### 4 - 쓰기에 지연 주입 (500 us) 활성화

```bash
./scripts/rpc.py bdev_null_set_error_injection \
    --name Null0 \
    --enabled true \
    --error-rate-pct 0 \
    --inject-reads false \
    --inject-writes true \
    --latency-us 500
```

예상 결과: 쓰기 IOPS가 크게 감소하고, 읽기 IOPS는 영향 없음.

### 5 - 주입 비활성화

```bash
./scripts/rpc.py bdev_null_set_error_injection \
    --name Null0 \
    --enabled false
```

---

## 테스트: 애플리케이션 계층으로의 오류 전파 검증

### 단위 테스트 접근법

SPDK는 `test/unit/` 하위에 단위 테스트 프레임워크를 제공합니다. `test/unit/lib/bdev/null/bdev_null_error_inject.c`를 생성합니다:

```c
#include "spdk_cunit.h"
#include "spdk/bdev.h"
#include "bdev_null.c"   /* 화이트박스 테스트: .c 파일을 직접 포함 */

static int g_last_status;

static void
test_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    g_last_status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
    spdk_bdev_free_io(bdev_io);
}

static void
test_error_rate_100pct(void)
{
    struct null_bdev nbdev = {};
    struct null_error_inject_opts opts = {
        .enabled        = true,
        .error_rate_pct = 100,
        .io_type_mask   = NULL_INJECT_IO_READ | NULL_INJECT_IO_WRITE,
        .latency_us     = 0,
    };

    /* bdev에 설정 적용 */
    snprintf(nbdev.bdev.name, sizeof(nbdev.bdev.name), "TestNull");
    TAILQ_INSERT_TAIL(&g_null_bdev_head, &nbdev, tailq);
    CU_ASSERT(bdev_null_set_error_injection("TestNull", &opts) == 0);

    /* 대상 유형의 모든 I/O에 대해 should_inject가 true를 반환해야 함 */
    for (int i = 0; i < 100; i++) {
        CU_ASSERT(null_should_inject_error(&nbdev, SPDK_BDEV_IO_TYPE_READ) == true);
    }

    TAILQ_REMOVE(&g_null_bdev_head, &nbdev, tailq);
}

static void
test_error_rate_0pct(void)
{
    struct null_bdev nbdev = {};
    struct null_error_inject_opts opts = {
        .enabled        = true,
        .error_rate_pct = 0,
        .io_type_mask   = NULL_INJECT_IO_ALL,
        .latency_us     = 0,
    };

    snprintf(nbdev.bdev.name, sizeof(nbdev.bdev.name), "TestNull2");
    TAILQ_INSERT_TAIL(&g_null_bdev_head, &nbdev, tailq);
    CU_ASSERT(bdev_null_set_error_injection("TestNull2", &opts) == 0);

    for (int i = 0; i < 100; i++) {
        CU_ASSERT(null_should_inject_error(&nbdev, SPDK_BDEV_IO_TYPE_READ) == false);
    }

    TAILQ_REMOVE(&g_null_bdev_head, &nbdev, tailq);
}

static void
test_io_type_filter(void)
{
    struct null_bdev nbdev = {};
    struct null_error_inject_opts opts = {
        .enabled        = true,
        .error_rate_pct = 100,
        .io_type_mask   = NULL_INJECT_IO_READ,  /* 읽기만 */
        .latency_us     = 0,
    };

    snprintf(nbdev.bdev.name, sizeof(nbdev.bdev.name), "TestNull3");
    TAILQ_INSERT_TAIL(&g_null_bdev_head, &nbdev, tailq);
    CU_ASSERT(bdev_null_set_error_injection("TestNull3", &opts) == 0);

    CU_ASSERT(null_should_inject_error(&nbdev, SPDK_BDEV_IO_TYPE_READ)  == true);
    nbdev.inject.io_counter = 0;
    CU_ASSERT(null_should_inject_error(&nbdev, SPDK_BDEV_IO_TYPE_WRITE) == false);

    TAILQ_REMOVE(&g_null_bdev_head, &nbdev, tailq);
}

int
main(int argc, char **argv)
{
    CU_pSuite suite = NULL;
    unsigned int num_failures;

    CU_initialize_registry();
    suite = CU_add_suite("bdev_null_error_inject", NULL, NULL);
    CU_ADD_TEST(suite, test_error_rate_100pct);
    CU_ADD_TEST(suite, test_error_rate_0pct);
    CU_ADD_TEST(suite, test_io_type_filter);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    num_failures = CU_get_number_of_failures();
    CU_cleanup_registry();
    return num_failures;
}
```

단위 테스트 실행:

```bash
make -C test/unit/lib/bdev/null
./test/unit/lib/bdev/null/bdev_null_error_inject
```

### bdevperf를 이용한 통합 검증

0이 아닌 오류 수를 확인하는 스크립트:

```bash
#!/usr/bin/env bash
set -e

SPDK_ROOT="$(pwd)"
SOCK="/var/tmp/spdk.sock"

# 타겟 시작
sudo "${SPDK_ROOT}/build/bin/spdk_tgt" -c null_ei_test.json &
SPDK_PID=$!
sleep 2

# 50% 오류 주입 활성화
"${SPDK_ROOT}/scripts/rpc.py" -s "${SOCK}" bdev_null_set_error_injection \
    --name Null0 --enabled true --error-rate-pct 50 \
    --inject-reads true --inject-writes true --latency-us 0

# 5초간 bdevperf 실행
OUTPUT=$(sudo "${SPDK_ROOT}/build/examples/bdevperf" \
    -r "${SOCK}" -q 32 -o 4096 -w read -t 5 2>&1)

# 오류가 보고되었는지 확인
if echo "${OUTPUT}" | grep -q "io_errors.*[1-9]"; then
    echo "PASS: 오류가 애플리케이션 계층에 전파됨"
else
    echo "FAIL: 오류가 관찰되지 않음"
    kill "${SPDK_PID}"
    exit 1
fi

kill "${SPDK_PID}"
```

---

## 보너스 과제

### 보너스 A - 비트 부패 시뮬레이션 (Bit-Rot, 읽기 데이터 손상)

실제 SSD는 성공을 반환하면서 데이터를 조용히 손상시킬 수 있습니다(비트 부패). `null_error_inject_config`에 `corrupt_reads` 플래그를 추가합니다. 설정 시, 읽기 버퍼를 복사한 후 설정 가능한 오프셋의 단일 바이트를 `0xFF`로 XOR합니다:

```c
if (cfg->corrupt_reads && bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
    uint8_t *buf = bdev_io->u.bdev.iovs[0].iov_base;
    if (buf && bdev_io->u.bdev.iovs[0].iov_len > cfg->corrupt_offset) {
        buf[cfg->corrupt_offset] ^= 0xFF;
    }
    /* 여전히 SUCCESS로 완료 - 이것이 비트 부패를 교활하게 만드는 점 */
    spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
    return;
}
```

RPC 디코더에 `"corrupt_reads"` (bool)와 `"corrupt_offset"` (uint64) 필드를 확장합니다.

검증하려면 애플리케이션이 알려진 패턴을 쓰고, 비트 부패를 활성화한 후 다시 읽어서 체크섬을 비교합니다.

### 보너스 B - 타임아웃 주입 (I/O가 완료되지 않음)

멈춘 장치는 I/O 완료를 완전히 중단합니다. `timeout_inject_pct` 필드를 추가합니다. 트리거되면 I/O를 `complete_tsc = UINT64_MAX`로 `ch->delayed_io`에 삽입합니다 - 폴러가 절대 드레인하지 않습니다. 이는 중단 경로를 테스트합니다: 애플리케이션의 타임아웃이 발생한 후 `spdk_bdev_abort`를 호출하면, `delayed_io`에서 멈춘 I/O를 찾아 드레인해야 합니다.

```c
if (null_should_inject_timeout(nbdev, bdev_io->type)) {
    null_io->complete_tsc = UINT64_MAX;  /* 자연적으로 만료되지 않음 */
    TAILQ_INSERT_TAIL(&ch->delayed_io, null_io, link);
    return;
}
```

다음으로 검증합니다:
```bash
./scripts/rpc.py bdev_null_set_error_injection \
    --name Null0 --enabled true --error-rate-pct 5 \
    --timeout-inject-pct 5 --latency-us 0
```

그런 다음 bdevperf 중단 카운터가 증가하는 것을 확인하여 `spdk_bdev_abort`가 멈춘 I/O를 성공적으로 복구하는지 확인합니다.

---

## 흔한 실수

| 실수 | 증상 | 해결 방법 |
|---------|---------|-----|
| 동일한 I/O에 대해 `spdk_bdev_io_complete`를 두 번 호출 | bdev 레이어에서 세그폴트 또는 어서션 실패 | 모든 코드 경로가 정확히 하나의 완료 호출에 도달하도록 합니다. `spdk_bdev_io_complete` 후 즉시 `return`을 사용합니다. |
| `TAILQ_INIT(&ch->delayed_io)` 초기화 누락 | 첫 번째 지연 I/O가 큐잉될 때 랜덤 크래시 | 기존 `TAILQ_INIT(&ch->io)` 옆의 io_channel 생성 콜백에 `TAILQ_INIT` 호출을 추가합니다. |
| 오류 비율에 `rand()` 사용 | 불안정한 테스트, 실패 비율 재현 어려움 | `null_should_inject_error`에서 보여준 결정론적 카운터 기반 접근법을 사용합니다. |
| 이미 비트 63이 설정된 TSC 값에 오류 센티넬 비트 설정 | I/O가 실패해야 할 때 성공으로 처리되거나, 마감 시한이 손상됨 | TSC 값을 확인합니다: 일반적인 x86 하드웨어에서 비트 63은 수백 년 동안 설정되지 않지만, 어서션을 추가합니다: `assert(deadline < (1ULL << 63))`. |
| 설정 변경 후 `io_counter`를 리셋하지 않음 | `rpc.py` 업데이트 후 오류 비율이 예기치 않게 변동 | `bdev_null_set_error_injection` 내부에서 항상 `io_counter = 0`으로 리셋합니다. |
| bdev를 소유한 SPDK 스레드 외부에서 `null_bdev.inject` 수정 | 데이터 경쟁, 간헐적 오동작 | RPC 핸들러에서 `spdk_thread_is_app_thread()`로 SPDK 앱 스레드에서 실행 중인지 확인하거나 `spdk_thread_send_msg`를 사용하여 설정 업데이트를 마샬링합니다. |
| `delayed_io`에 있는 I/O에 대한 중단 처리 누락 | 중단 RPC가 완료되지 않음; 메모리 누수 | `bdev_null_abort_io`를 확장하여 `ch->io`와 `ch->delayed_io` 모두를 검색합니다. |
| RESET 유형 I/O에 오류 주입 활성화 | 장치가 리셋할 수 없어 이후 모든 I/O가 실패 | 기본 `io_type_mask`에서 `SPDK_BDEV_IO_TYPE_RESET`을 제외합니다. 리셋 실패 시나리오를 명시적으로 테스트할 때만 RESET을 마스크에 추가합니다. |

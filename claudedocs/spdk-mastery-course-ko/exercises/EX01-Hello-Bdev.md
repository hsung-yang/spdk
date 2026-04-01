# 실습 01: Hello Bdev

**모듈**: 13 - Bdev 계층
**단계**: 2 (구현)
**난이도**: 초급-중급
**예상 소요 시간**: 1-2시간
**사전 요구사항**: 모듈 01-12, SPDK 개발 환경 구성 완료

---

## 목표

bdev 서브시스템을 초기화하고, 등록된 모든 블록 디바이스(block device)를 순회하며 주요 속성(이름, 전체 크기(바이트), 블록 크기, 제품명, UUID)을 출력하는 독립 실행형 SPDK 애플리케이션을 작성합니다. 이 실습을 마치면 SPDK의 bdev 추상화 계층 구조, 런타임에 디바이스를 열거하는 방법, SPDK 이벤트 프레임워크(event framework) 내에서 애플리케이션 로직을 실행하는 방법을 이해하게 됩니다.

---

## 배경 지식

### Bdev 추상화

SPDK의 블록 디바이스(bdev) 계층은 애플리케이션과 스토리지 백엔드(storage backend) 사이에 위치합니다. 백엔드가 NVMe SSD이든, 커널 AIO 파일이든, 인메모리 malloc 디바이스이든, RAID 배열이든 관계없이 애플리케이션 코드는 동일합니다 -- 항상 같은 bdev API를 호출합니다.

```
Application
    |
    v
[ Bdev API ]          <-- uniform interface
    |
    +-- NVMe bdev  --> NVMe SSD
    +-- AIO bdev   --> Kernel file / block device
    +-- Malloc bdev --> RAM buffer
    +-- RAID bdev  --> Multiple backing bdevs
```

### 이 실습에서 사용하는 주요 API 함수

| 함수 | 용도 |
|---|---|
| `spdk_bdev_first()` | 첫 번째 등록된 bdev 반환 (없으면 NULL) |
| `spdk_bdev_next(prev)` | `prev` 다음의 bdev 반환 |
| `spdk_bdev_get_name(bdev)` | bdev 이름 문자열 반환 |
| `spdk_bdev_get_product_name(bdev)` | 제품/모듈 이름 문자열 반환 |
| `spdk_bdev_get_block_size(bdev)` | 논리 블록 크기(바이트) 반환 |
| `spdk_bdev_get_num_blocks(bdev)` | 전체 논리 블록 수 반환 |
| `spdk_bdev_get_uuid(bdev)` | bdev UUID 포인터 반환 |
| `spdk_uuid_fmt_lower(buf, sz, uuid)` | UUID를 소문자 문자열로 포맷 |

### SPDK 애플리케이션 프레임워크

SPDK 애플리케이션은 리액터/폴러(reactor/poller) 모델 내에서 실행됩니다. 표준 진입점은 `spdk_app_start()`이며, 시작 콜백(startup callback)을 인자로 받습니다. 모든 bdev 관련 호출은 SPDK 스레드에서 이루어져야 합니다(즉, 해당 콜백 내부 또는 콜백이 호출하는 함수 내부에서).

---

## 과제 설명

다음 기능을 수행하는 C 프로그램 `hello_bdev.c`를 작성하세요:

1. SPDK 애플리케이션 인자(JSON 설정 파일, 리액터 마스크 등)를 파싱합니다.
2. `spdk_app_start()`를 호출하여 SPDK를 초기화하고 이벤트 루프에 진입합니다.
3. 시작 콜백 내에서 `spdk_bdev_first()` / `spdk_bdev_next()`를 사용하여 등록된 모든 bdev를 순회합니다.
4. 각 bdev에 대해 다음을 출력합니다:
   - 이름
   - 전체 용량(바이트) (`num_blocks * block_size`)
   - 블록 크기(바이트)
   - 제품명
   - UUID (소문자 하이픈 구분 문자열로 포맷)
5. 출력 후 `spdk_app_stop(0)`을 호출하여 깔끔하게 종료합니다.

---

## 단계별 안내

### 1단계 - 프로젝트 디렉토리 생성

```bash
mkdir -p ~/spdk-exercises/ex01-hello-bdev
cd ~/spdk-exercises/ex01-hello-bdev
```

### 2단계 - 스켈레톤 작성

아래 구조로 `hello_bdev.c`를 생성하세요. 주석을 읽으세요 -- 각 `TODO`는 코드를 채워 넣을 위치를 표시합니다.

```c
/* hello_bdev.c - Exercise 01: Enumerate all bdevs and print their properties */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/uuid.h"

/* Called by spdk_app_start() once SPDK has initialized. */
static void
hello_bdev_start(void *arg)
{
    struct spdk_bdev *bdev;
    uint32_t count = 0;

    SPDK_NOTICELOG("=== Bdev Enumeration ===\n");

    /* TODO 1: spdk_bdev_first()를 사용하여 첫 번째 bdev를 얻으세요.
     * 결과를 `bdev`에 대입하세요. */

    /* TODO 2: `bdev`가 NULL이 아닌 동안 반복하세요.
     * 루프 내부에서:
     *   a) UUID 문자열용 37바이트 char 배열을 선언하세요.
     *   b) spdk_uuid_fmt_lower()로 UUID를 포맷하세요.
     *   c) total_bytes = num_blocks * block_size를 계산하세요.
     *   d) 다섯 가지 속성을 SPDK_NOTICELOG 또는 printf로 출력하세요.
     *   e) spdk_bdev_next()로 다음 bdev로 이동하세요.
     *   f) count를 증가시키세요.
     */

    SPDK_NOTICELOG("=== Total bdevs found: %u ===\n", count);

    /* TODO 3: spdk_app_stop(0)을 호출하여 깔끔하게 종료하세요. */
}

int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    /* Initialize opts with defaults. */
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "hello_bdev";

    /* Parse SPDK standard arguments (-c <config>, -m <mask>, etc.). */
    rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    /* Start the SPDK application framework. hello_bdev_start is called
     * after all subsystems (including bdev) have been initialized. */
    rc = spdk_app_start(&opts, hello_bdev_start, NULL);

    spdk_app_fini();
    return rc;
}
```

### 3단계 - TODO 구현

세 개의 TODO 섹션을 채우세요. 막히면 다음 섹션에 해답이 있지만, 먼저 직접 작성해 보세요.

**힌트:**

- `spdk_bdev_first()`는 `struct spdk_bdev *`를 반환합니다 (bdev가 없으면 NULL).
- `spdk_bdev_next(bdev)`는 현재 bdev를 받아 다음 bdev를 반환합니다.
- UUID 문자열 버퍼는 최소 37바이트여야 합니다 (`xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\0`).
- `spdk_bdev_get_uuid()`는 문자열이 아닌 `const struct spdk_uuid *`를 반환합니다.
- `spdk_uuid_fmt_lower()` 시그니처: `int spdk_uuid_fmt_lower(char *uuid_str, size_t uuid_str_size, const struct spdk_uuid *uuid)`.

### 4단계 - Makefile 작성

같은 디렉토리에 `Makefile`을 생성하세요:

```makefile
# Makefile for Exercise 01: Hello Bdev

SPDK_PATH ?= $(HOME)/spdk

# pkg-config provides the correct compiler and linker flags for SPDK.
SPDK_CFLAGS  := $(shell PKG_CONFIG_PATH=$(SPDK_PATH)/build/lib/pkgconfig \
                    pkg-config --cflags spdk_event spdk_event_bdev)
SPDK_LDFLAGS := $(shell PKG_CONFIG_PATH=$(SPDK_PATH)/build/lib/pkgconfig \
                    pkg-config --libs spdk_event spdk_event_bdev)

CC      = gcc
CFLAGS  = -g -Wall -Wextra $(SPDK_CFLAGS)
LDFLAGS = $(SPDK_LDFLAGS) -ldpdk -lnuma -luuid -pthread

TARGET  = hello_bdev

$(TARGET): hello_bdev.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
```

### 5단계 - 최소 JSON 설정 파일 준비

SPDK는 어떤 bdev를 생성할지 알려주는 설정 파일이 필요합니다. 이 실습에서는 malloc bdev 하나를 생성합니다 (하드웨어 불필요):

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
        },
        {
          "method": "bdev_malloc_create",
          "params": {
            "name": "Malloc1",
            "num_blocks": 8192,
            "block_size": 4096
          }
        }
      ]
    }
  ]
}
```

`bdev.json`으로 저장하세요.

### 6단계 - 빌드 및 실행

```bash
# 빌드 (SPDK 트리가 ~/spdk에 없으면 SPDK_PATH를 설정하세요)
make SPDK_PATH=/path/to/your/spdk

# 실행 (hugepage 필요; sudo 사용 또는 권한 설정)
sudo ./hello_bdev -c bdev.json
```

환경에서 NVMe에 vfio 또는 UIO를 사용하는 경우, 일반적인 SPDK DPDK 마스크 플래그를 추가하세요:

```bash
sudo ./hello_bdev -c bdev.json -m 0x1
```

---

## 완전한 해답

```c
/* hello_bdev.c - Exercise 01: SOLUTION */

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/uuid.h"

#define UUID_STR_LEN 37   /* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\0" */

static void
hello_bdev_start(void *arg)
{
    struct spdk_bdev *bdev;
    uint32_t count = 0;

    SPDK_NOTICELOG("=== Bdev Enumeration ===\n");

    /* Iterate over every registered bdev. */
    for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
        char uuid_str[UUID_STR_LEN];
        uint64_t total_bytes;

        /* Format UUID to lowercase hyphenated string. */
        spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), spdk_bdev_get_uuid(bdev));

        /* Total capacity in bytes. */
        total_bytes = spdk_bdev_get_num_blocks(bdev) *
                      (uint64_t)spdk_bdev_get_block_size(bdev);

        SPDK_NOTICELOG(
            "  [%u] name=%-20s  size=%10" PRIu64 " B  "
            "block_size=%5u B  product=%-20s  uuid=%s\n",
            count,
            spdk_bdev_get_name(bdev),
            total_bytes,
            spdk_bdev_get_block_size(bdev),
            spdk_bdev_get_product_name(bdev),
            uuid_str);

        count++;
    }

    SPDK_NOTICELOG("=== Total bdevs found: %u ===\n", count);

    spdk_app_stop(0);
}

int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "hello_bdev";

    rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    rc = spdk_app_start(&opts, hello_bdev_start, NULL);

    spdk_app_fini();
    return rc;
}
```

---

## 예상 출력

`bdev.json`에 정의된 두 개의 malloc bdev에 대해 실행하면 다음과 유사한 출력이 생성됩니다:

```
[2026-03-30 10:12:01.543] NOTICE: ==> Bdev Enumeration <==
[2026-03-30 10:12:01.543] NOTICE:   [0] name=Malloc0               size=   2097152 B  block_size=  512 B  product=Malloc Disk            uuid=a4e1bc3f-22d0-4f71-9c7a-1b3e8d905f20
[2026-03-30 10:12:01.543] NOTICE:   [1] name=Malloc1               size= 33554432 B  block_size= 4096 B  product=Malloc Disk            uuid=f7d3a012-8b4c-4e61-adf3-09c7e5820b11
[2026-03-30 10:12:01.543] NOTICE: ==> Total bdevs found: 2 <==
```

참고: UUID는 디바이스 생성 시 무작위로 생성되므로 매 실행마다 다릅니다. 설정 파일에서 `uuid` 매개변수를 통해 명시적으로 지정하지 않는 한 동일하지 않습니다.

---

## 보너스 도전 과제

### 도전 A: 제품 유형별 필터링

루프를 수정하여 선택적 `-P <product_prefix>` 인자를 받고, 제품명이 해당 접두사로 시작하는 bdev만 출력하도록 하세요 (예: `-P Malloc` 또는 `-P NVMe`).

힌트: `spdk_app_parse_args()`의 `getopt_str` 및 `parse_arg` 콜백 매개변수를 사용하여 사용자 정의 옵션을 추가하고, `strncmp()`로 비교하세요.

### 도전 B: 사람이 읽기 쉬운 단위로 용량 표시

원시 바이트 대신 크기에 따라 KB, MB, GB로 용량을 포맷하세요:

```c
static void
fmt_bytes(uint64_t bytes, char *buf, size_t len)
{
    if (bytes >= (1ULL << 30)) {
        snprintf(buf, len, "%.2f GiB", (double)bytes / (1ULL << 30));
    } else if (bytes >= (1ULL << 20)) {
        snprintf(buf, len, "%.2f MiB", (double)bytes / (1ULL << 20));
    } else if (bytes >= (1ULL << 10)) {
        snprintf(buf, len, "%.2f KiB", (double)bytes / (1ULL << 10));
    } else {
        snprintf(buf, len, "%" PRIu64 " B", bytes);
    }
}
```

### 도전 C: 제품 유형별 bdev 수 집계

열거 후 각 제품 유형별로 발견된 bdev 수를 보여주는 요약 테이블을 출력하세요. 간단한 `{product_name, count}` 구조체 배열을 사용합니다.

### 도전 D: spdk_bdev_first_leaf() / spdk_bdev_next_leaf() 사용

`spdk_bdev_first()` / `spdk_bdev_next()`를 `spdk_bdev_first_leaf()` / `spdk_bdev_next_leaf()`로 교체하세요. Malloc bdev 위에 RAID 또는 Crypto bdev가 계층화된 설정으로 실행하고, 각 열거 모드에서 어떤 bdev가 나타나는지 관찰하세요.

`spdk_bdev_first_leaf()`는 리프(비가상) 디바이스만 반환합니다 -- 논리적 스택이 아닌 물리적 백엔드 스토어에 직접 작업하고 싶을 때 유용합니다.

---

## 흔한 실수

### 실수 1: `spdk_app_stop()` 호출을 잊음

```c
/* 잘못된 예 - 애플리케이션이 이벤트를 계속 기다리며 영원히 중단됨 */
static void hello_bdev_start(void *arg) {
    /* ... bdev 열거 ... */
    /* 누락: spdk_app_stop(0); */
}
```

`spdk_app_start()`는 `spdk_app_stop()`이 호출될 때까지 호출 스레드를 차단합니다. 호출을 잊으면 콜백이 반환된 후에도 프로세스가 무한히 유휴 상태로 남습니다.

### 실수 2: `spdk_bdev_get_uuid()`를 문자열로 직접 사용

```c
/* 잘못된 예 - spdk_bdev_get_uuid()는 char *가 아닌 const struct spdk_uuid *를 반환 */
printf("uuid=%s\n", spdk_bdev_get_uuid(bdev));  /* 정의되지 않은 동작 */

/* 올바른 예 */
char uuid_str[37];
spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), spdk_bdev_get_uuid(bdev));
printf("uuid=%s\n", uuid_str);
```

### 실수 3: 전체 바이트 계산 시 정수 오버플로

```c
/* 잘못된 예 - 대용량 디바이스에서 문제 발생 - num_blocks는 uint64_t이지만 block_size는 uint32_t;
 * block_size가 uint32_t로 먼저 캐스팅되면 곱셈이 오버플로될 수 있음 */
uint32_t total = spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev);

/* 올바른 예 - 곱하기 전에 uint64_t로 캐스팅 */
uint64_t total_bytes = spdk_bdev_get_num_blocks(bdev) *
                       (uint64_t)spdk_bdev_get_block_size(bdev);
```

### 실수 4: SPDK 초기화 전에 bdev API 호출

```c
/* 잘못된 예 - spdk_app_start() 이전에 main()에서 직접 호출 */
int main(int argc, char **argv) {
    spdk_app_opts_init(&opts, sizeof(opts));
    struct spdk_bdev *bdev = spdk_bdev_first(); /* NULL, 서브시스템 미준비 */
    ...
}
```

모든 bdev API 호출은 SPDK 스레드 내에서 이루어져야 합니다 -- 즉, `spdk_app_start()`에 전달된 콜백 내부 또는 해당 콜백이 디스패치하는 함수 내부에서 호출해야 합니다.

### 실수 5: UUID 버퍼 크기가 너무 작음

```c
/* 잘못된 예 - UUID 문자열은 36자 + NUL = 최소 37바이트 */
char uuid_str[36];  /* 1바이트 부족; NUL 종료 문자를 위한 공간 없음 */
spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), spdk_bdev_get_uuid(bdev));
/* 결과: 잘린 UUID 또는 스택 손상 */

/* 올바른 예 */
char uuid_str[37];
```

### 실수 6: 열거 중 bdev 상태 수정

`spdk_bdev_first()` / `spdk_bdev_next()` 루프 내에서 bdev를 열거나, 닫거나, 생성하지 마세요. 목록이 변경되어 이터레이터가 디바이스를 건너뛰거나 해제된 메모리에 접근할 수 있습니다. 먼저 디바이스 이름을 수집한 후, 루프가 끝난 후에 작업하세요.

---

## 핵심 정리

- bdev 계층은 백엔드에 관계없이 균일한 블록 디바이스 인터페이스를 제공합니다.
- `spdk_bdev_first()`와 `spdk_bdev_next()`는 등록된 모든 디바이스에 대한 간단한 순방향 이터레이터를 제공합니다.
- 디바이스 속성(이름, 크기, 제품명, UUID)은 읽기 전용이며 디스크립터를 열지 않고도 사용할 수 있습니다.
- 모든 bdev API 호출은 SPDK 스레드에서 실행되어야 합니다 -- `spdk_app_start()` 콜백 내부 또는 해당 콜백이 호출하는 함수 내부에서 실행해야 합니다.
- 리액터 루프를 깔끔하게 종료하려면 반드시 `spdk_app_stop()`을 호출해야 합니다.

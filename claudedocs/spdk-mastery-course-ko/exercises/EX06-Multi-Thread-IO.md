# 실습 06: 멀티 스레드 I/O

## 개요

| 항목 | 내용 |
|-------|-------|
| 실습 | EX06 |
| 주제 | 스레드별 채널을 활용한 멀티 스레드 I/O (Multi-Thread I/O with Per-Thread Channels) |
| 예상 소요 시간 | 2-3시간 |
| 난이도 | 중급 |

## 목표

각 SPDK 스레드가 자체 전용 I/O 채널(I/O channel)을 사용하여 동일한 bdev에 블록 I/O를 수행하는 멀티 스레드 SPDK 애플리케이션을 구축합니다. SPDK의 핵심 스레딩 모델인 스레드 생성, 스레드별 채널 획득, 스레드 간 메시지 전달, 코디네이터 스레드(coordinator thread)를 통한 결과 집계를 실습합니다.

이 실습을 완료하면 다음을 수행할 수 있습니다:

- 특정 CPU 코어에 고정된 이름 있는 SPDK 스레드 생성
- 각 스레드가 자체 I/O 채널을 획득해야 하는 이유 이해
- `spdk_thread_send_msg`를 사용한 스레드 간 메시지 전송
- 코디네이터 스레드를 사용한 작업 분배 및 스레드별 결과 수집
- `/proc/self/status`를 확인하여 스레드가 서로 다른 코어에서 실행되는지 확인

## 사전 요구 사항

- EX01~EX05 완료 (또는 SPDK bdev I/O에 대한 동등한 수준의 이해)
- `libbdev`와 `null` bdev 모듈이 포함된 SPDK 빌드 환경
- EX04에서 다룬 리액터/폴러 모델(reactor/poller model)에 대한 이해

## 배경 지식

### SPDK 스레드 모델

SPDK 스레드(`struct spdk_thread`)는 OS 스레드(일반적으로 DPDK lcore 또는 POSIX pthread) 내에서 실행되는 경량의 스택 없는 협력적 실행 단위(lightweight, stackless cooperative unit)입니다. 핵심 규약은 다음과 같습니다:

- 하나의 SPDK 스레드는 한 번에 정확히 하나의 OS 스레드에서 실행됩니다.
- SPDK 스레드의 모든 작업은 호스팅 OS 스레드에서 `spdk_thread_poll()`을 타이트 루프로 호출하여 구동됩니다.
- SPDK 스레드는 절대 블로킹해서는 안 됩니다. 긴 작업은 폴러(poller)와 메시지 콜백으로 분리해야 합니다.

### I/O 채널은 스레드 로컬

`struct spdk_io_channel`은 bdev I/O를 제출하는 데 사용되는 객체입니다. bdev 계층은 각 채널 내부에 스레드별 상태를 할당합니다. 이로 인해:

- **I/O를 제출하는 모든 SPDK 스레드는 자체 스레드 컨텍스트에서 `spdk_bdev_get_io_channel()`을 호출하여** 전용 채널을 획득해야 합니다.
- 스레드 A에서 획득한 채널은 스레드 B에서 **사용해서는 안 됩니다**. 그렇게 하면 내부 큐 상태가 손상되어 무음 데이터 오류(silent data error)나 크래시가 발생합니다.
- 채널은 획득한 것과 동일한 스레드에서 `spdk_put_io_channel()`로 해제합니다.

### 스레드 간 통신

```
Thread A                     Thread B
--------                     --------
spdk_thread_send_msg(B, fn, ctx)
  -- enqueues fn+ctx into B's message ring -->
                             spdk_thread_poll() drains ring
                             fn(ctx) executes on B
```

`spdk_thread_send_msg`는 다른 스레드의 상태에 접근하는 유일한 안전한 방법입니다. 이 호출은 비동기적(asynchronous)입니다: `fn`이 실행되기 전에 호출이 반환됩니다.

---

## 과제 설명

다음과 같이 동작하는 `ex06_mt_io`를 구현합니다:

1. SPDK 환경과 bdev 계층을 초기화합니다.
2. `null` bdev를 엽니다 (제로 카피, 항상 성공, 실제 디스크 불필요).
3. **N개의 워커 SPDK 스레드**를 생성합니다 (기본 N=2, 설정 가능). 각 스레드는 서로 다른 CPU 코어에 고정됩니다.
4. **메인 스레드**가 코디네이터 역할을 합니다: 각 워커에게 "I/O 시작" 메시지를 보냅니다.
5. 각 워커는:
   a. `spdk_bdev_get_io_channel`을 통해 자체 I/O 채널을 획득합니다.
   b. 설정 가능한 수의 읽기 I/O를 발행합니다 (기본 워커당 64개).
   c. 완료 횟수를 메시지를 통해 코디네이터에 보고합니다.
6. 코디네이터는 모든 보고를 수집하고, 요약을 출력한 다음, 올바른 순서로 모든 것을 해제합니다 (채널, 스레드, bdev).

---

## 단계별 지침

### 단계 1: 프로젝트 스켈레톤

디렉토리와 소스 파일을 생성합니다:

```
ex06/
  Makefile
  ex06_mt_io.c
```

EX05(또는 `examples/bdev/bdev_hello_world`)에서 Makefile을 복사하고 `APP = ex06_mt_io`로 변경합니다.

### 단계 2: 인클루드와 전역 상태

```c
#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"

#define NUM_WORKERS     2
#define IOS_PER_WORKER  64
#define BDEV_NAME       "Null0"

/* Per-worker state - one instance per SPDK worker thread */
struct worker_ctx {
    int                     id;
    struct spdk_thread     *thread;       /* the worker's SPDK thread */
    struct spdk_io_channel *ch;           /* thread-local bdev channel */
    struct spdk_bdev_desc  *desc;         /* shared bdev descriptor (read-only use) */
    uint64_t                ios_submitted;
    uint64_t                ios_completed;
    uint64_t                ios_target;
    bool                    done;
};

/* Coordinator state */
struct coordinator_ctx {
    struct spdk_thread     *thread;       /* coordinator's SPDK thread (app thread) */
    struct spdk_bdev_desc  *desc;
    struct spdk_bdev       *bdev;
    struct worker_ctx       workers[NUM_WORKERS];
    int                     workers_done; /* count of workers that reported back */
};

static struct coordinator_ctx g_coordinator;
```

### 단계 3: bdev 이벤트 콜백

bdev 디스크립터(descriptor)에는 이벤트 콜백이 필요합니다. 최소한의 구현을 작성합니다:

```c
static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
    SPDK_NOTICELOG("Bdev event: type %d on %s\n", type, spdk_bdev_get_name(bdev));
}
```

### 단계 4: I/O 완료 콜백

```c
struct io_req {
    struct worker_ctx *worker;
    void              *buf;
};

static void
read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_req     *req    = cb_arg;
    struct worker_ctx *worker = req->worker;

    spdk_bdev_free_io(bdev_io);
    spdk_dma_free(req->buf);
    free(req);

    worker->ios_completed++;

    if (worker->ios_completed == worker->ios_target) {
        worker->done = true;
        /* Notify coordinator - send message back to coordinator thread */
        spdk_thread_send_msg(g_coordinator.thread, worker_report_done, worker);
    }
}
```

### 단계 5: I/O 제출 (워커 스레드에서 실행)

```c
static void
submit_one_read(struct worker_ctx *worker)
{
    struct io_req *req;
    uint32_t       block_size;
    uint64_t       num_blocks;
    int            rc;

    block_size = spdk_bdev_get_block_size(g_coordinator.bdev);
    num_blocks = spdk_bdev_get_num_blocks(g_coordinator.bdev);

    req = calloc(1, sizeof(*req));
    assert(req != NULL);
    req->worker = worker;
    req->buf    = spdk_dma_zmalloc(block_size, block_size, NULL);
    assert(req->buf != NULL);

    /* Offset cycles through bdev blocks, one block at a time */
    uint64_t offset = (worker->ios_submitted % num_blocks);

    rc = spdk_bdev_read_blocks(worker->desc, worker->ch,
                               req->buf, offset, 1,
                               read_complete, req);
    if (rc != 0) {
        SPDK_ERRLOG("Worker %d: spdk_bdev_read_blocks failed: %d\n", worker->id, rc);
        spdk_dma_free(req->buf);
        free(req);
        return;
    }
    worker->ios_submitted++;
}
```

### 단계 6: 워커 시작 핸들러 (워커 스레드에서 실행)

이 함수는 코디네이터에서 `spdk_thread_send_msg`를 통해 호출됩니다. 이 시점에서 워커는 자신의 SPDK 스레드에서 실행 중이므로, `spdk_bdev_get_io_channel`을 호출해도 안전합니다.

```c
static void
worker_start(void *arg)
{
    struct worker_ctx *worker = arg;
    uint64_t           i;

    /* Acquire a thread-local I/O channel. MUST be called from this thread. */
    worker->ch = spdk_bdev_get_io_channel(worker->desc);
    if (worker->ch == NULL) {
        SPDK_ERRLOG("Worker %d: failed to get I/O channel\n", worker->id);
        spdk_thread_send_msg(g_coordinator.thread, worker_report_done, worker);
        return;
    }

    SPDK_NOTICELOG("Worker %d: channel acquired, submitting %lu I/Os "
                   "(lcore %u)\n",
                   worker->id, worker->ios_target,
                   spdk_env_get_current_core());

    worker->ios_submitted = 0;
    worker->ios_completed = 0;

    for (i = 0; i < worker->ios_target; i++) {
        submit_one_read(worker);
    }
}
```

### 단계 7: 워커 완료 핸들러 (코디네이터 스레드에서 실행)

```c
static void
worker_report_done(void *arg)
{
    struct worker_ctx     *worker = arg;
    struct coordinator_ctx *coord = &g_coordinator;

    SPDK_NOTICELOG("Coordinator: worker %d finished (%lu/%lu I/Os completed)\n",
                   worker->id, worker->ios_completed, worker->ios_submitted);

    /* Release the worker's channel - must be done from that worker's thread */
    spdk_thread_send_msg(worker->thread, worker_put_channel, worker);

    coord->workers_done++;
    if (coord->workers_done == NUM_WORKERS) {
        print_summary(coord);
        cleanup(coord);
    }
}

static void
worker_put_channel(void *arg)
{
    struct worker_ctx *worker = arg;

    if (worker->ch != NULL) {
        spdk_put_io_channel(worker->ch);
        worker->ch = NULL;
    }
    /* Signal the thread to exit after channel teardown drains */
    spdk_thread_exit(worker->thread);
}
```

### 단계 8: 요약 출력과 정리

```c
static void
print_summary(struct coordinator_ctx *coord)
{
    int i;
    uint64_t total = 0;

    printf("\n=== EX06 Multi-Thread I/O Summary ===\n");
    for (i = 0; i < NUM_WORKERS; i++) {
        printf("  Worker %d: %lu I/Os completed\n",
               coord->workers[i].id,
               coord->workers[i].ios_completed);
        total += coord->workers[i].ios_completed;
    }
    printf("  Total   : %lu I/Os\n", total);
    printf("======================================\n\n");
}

static void
cleanup(struct coordinator_ctx *coord)
{
    /* Close the bdev descriptor from the coordinator (app) thread */
    spdk_bdev_close(coord->desc);

    /* spdk_app_stop tears down the reactor and exits */
    spdk_app_stop(0);
}
```

### 단계 9: 애플리케이션 진입점

```c
static void
app_start(void *arg1)
{
    struct coordinator_ctx *coord = &g_coordinator;
    struct spdk_cpuset      cpumask;
    int                     rc, i;
    char                    name[32];

    /* Record the coordinator (app) thread */
    coord->thread = spdk_get_thread();

    /* Open the bdev once; workers share the descriptor but each gets its own channel */
    rc = spdk_bdev_open_ext(BDEV_NAME, false, bdev_event_cb, NULL, &coord->desc);
    if (rc != 0) {
        SPDK_ERRLOG("Failed to open bdev '%s': %d\n", BDEV_NAME, rc);
        spdk_app_stop(rc);
        return;
    }
    coord->bdev = spdk_bdev_desc_get_bdev(coord->desc);

    SPDK_NOTICELOG("Bdev '%s' opened: %u-byte blocks, %lu blocks total\n",
                   BDEV_NAME,
                   spdk_bdev_get_block_size(coord->bdev),
                   spdk_bdev_get_num_blocks(coord->bdev));

    /* Create worker threads, each pinned to its own core */
    for (i = 0; i < NUM_WORKERS; i++) {
        struct worker_ctx *w = &coord->workers[i];

        w->id         = i;
        w->desc       = coord->desc;
        w->ios_target = IOS_PER_WORKER;
        w->done       = false;

        /* Pin to core (i + 1) so that core 0 stays for the coordinator */
        spdk_cpuset_zero(&cpumask);
        spdk_cpuset_set_cpu(&cpumask, (i + 1) % spdk_env_get_core_count(), true);

        snprintf(name, sizeof(name), "worker_%d", i);
        w->thread = spdk_thread_create(name, &cpumask);
        if (w->thread == NULL) {
            SPDK_ERRLOG("Failed to create SPDK thread for worker %d\n", i);
            spdk_app_stop(-ENOMEM);
            return;
        }
    }

    /* Dispatch work - each message runs worker_start() on the target thread */
    for (i = 0; i < NUM_WORKERS; i++) {
        spdk_thread_send_msg(coord->workers[i].thread,
                             worker_start, &coord->workers[i]);
    }
}

int
main(int argc, char **argv)
{
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name    = "ex06_mt_io";
    opts.reactor_mask = "0x7";   /* cores 0, 1, 2 */

    rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    rc = spdk_app_start(&opts, app_start, NULL);
    spdk_app_fini();
    return rc;
}
```

---

## 전체 솔루션

위 섹션들이 전체 솔루션을 구성합니다. `ex06_mt_io.c`에 다음 순서로 조립하세요:

```
1. 인클루드와 매크로 정의
2. struct worker_ctx
3. struct coordinator_ctx + g_coordinator 전역 변수
4. 전방 선언 (worker_report_done, worker_put_channel, cleanup, print_summary)
5. bdev_event_cb
6. read_complete
7. submit_one_read
8. worker_start
9. worker_put_channel
10. worker_report_done
11. print_summary
12. cleanup
13. app_start
14. main
```

정의되기 전에 호출되는 함수는 전방 선언(forward declaration)합니다. 예:

```c
static void worker_report_done(void *arg);
static void worker_put_channel(void *arg);
static void cleanup(struct coordinator_ctx *coord);
static void print_summary(struct coordinator_ctx *coord);
```

---

## 빌드 지침

```bash
# From the SPDK root
cd /path/to/spdk

# Minimal Makefile for the exercise
cat > ex06/Makefile << 'EOF'
APP = ex06_mt_io
SRCS = ex06_mt_io.c

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SPDK_LIB_LIST = event event_bdev bdev bdev_null log env_dpdk
LIBS += $(SPDK_LIB_LIST:%=-lspdk_%)
LIBS += $(SPDK_DPDK_LIB_LIST:%=-l%)

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
EOF

make -C ex06 -j$(nproc)
```

---

## 실행 지침

먼저 huge page를 설정하고 null bdev 모듈을 바인딩합니다:

```bash
# Bind huge pages (once per reboot)
sudo scripts/setup.sh

# Run with a JSON config that creates a Null bdev
# Create ex06/bdev.json:
cat > ex06/bdev.json << 'EOF'
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
EOF

sudo ./ex06/ex06_mt_io \
    --json ex06/bdev.json \
    -m 0x7 \
    --no-pci
```

예상 출력 (워커 라인의 순서는 다를 수 있음):

```
[NOTICE]: Bdev 'Null0' opened: 512-byte blocks, 102400 blocks total
[NOTICE]: Worker 0: channel acquired, submitting 64 I/Os (lcore 1)
[NOTICE]: Worker 1: channel acquired, submitting 64 I/Os (lcore 2)
[NOTICE]: Coordinator: worker 0 finished (64/64 I/Os completed)
[NOTICE]: Coordinator: worker 1 finished (64/64 I/Os completed)

=== EX06 Multi-Thread I/O Summary ===
  Worker 0: 64 I/Os completed
  Worker 1: 64 I/Os completed
  Total   : 128 I/Os
======================================
```

---

## 스레드가 서로 다른 코어에서 실행되는지 확인하기

### 방법 1: spdk_env_get_current_core()

`worker_start` 콜백은 이미 lcore 번호를 로그에 기록합니다. 각 워커가 서로 다른 값을 출력하는지 확인합니다.

### 방법 2: /proc/self/task 확인

프로세스가 실행 중일 때 스레드와 CPU 어피니티(affinity)를 나열합니다:

```bash
# Find the PID
PID=$(pgrep ex06_mt_io)

# List threads and their current CPU
for TID in /proc/$PID/task/*; do
    TNAME=$(cat $TID/comm 2>/dev/null)
    CPU=$(cat $TID/status 2>/dev/null | grep -i 'cpusallowed:')
    echo "TID $(basename $TID) ($TNAME): $CPU"
done
```

### 방법 3: taskset

```bash
PID=$(pgrep ex06_mt_io)
for TID in $(ls /proc/$PID/task/); do
    echo -n "TID $TID: "
    taskset -p $TID 2>/dev/null
done
```

각 워커 스레드는 고정된 코어에 해당하는 단일 비트 CPU 마스크를 표시해야 합니다.

### 방법 4: perf 또는 htop

두 번째 터미널에서 `htop`을 실행하고 `F2 -> Display -> Show custom thread names`를 누릅니다. 각 SPDK 스레드가 별도의 행으로 표시됩니다. 서로 다른 코어에 고정된 워커 스레드는 `CPU` 열의 값이 다르게 나타납니다.

---

## 보너스: 코디네이터 주도 작업 분배

코디네이터가 각 워커에 보낼 I/O 수를 정확히 제어하고, 워커가 고정 배치를 미리 제출하는 대신 추가 작업을 요청하도록 실습을 확장합니다.

### 추가 구조체

```c
struct work_request {
    struct worker_ctx      *worker;
    uint64_t                start_block;
    uint64_t                num_ios;
};

struct coordinator_ctx {
    /* ... existing fields ... */
    uint64_t total_ios_requested; /* coordinator tracks grand total */
    pthread_mutex_t results_lock; /* only needed if results are written off-thread */
};
```

### 코디네이터가 배치를 디스패치

```c
static void
coordinator_dispatch_batch(struct coordinator_ctx *coord, int worker_id,
                           uint64_t start_block, uint64_t num_ios)
{
    struct work_request *req = calloc(1, sizeof(*req));
    assert(req != NULL);

    req->worker      = &coord->workers[worker_id];
    req->start_block = start_block;
    req->num_ios     = num_ios;

    spdk_thread_send_msg(coord->workers[worker_id].thread,
                         worker_execute_batch, req);
}
```

### 워커가 배치를 실행하고 추가 작업 요청

```c
static void
worker_execute_batch(void *arg)
{
    struct work_request *req    = arg;
    struct worker_ctx   *worker = req->worker;
    uint64_t             i;

    for (i = 0; i < req->num_ios; i++) {
        submit_read_at_block(worker, req->start_block + i);
    }
    free(req);
}
```

배치의 모든 I/O가 완료되면, 워커는 코디네이터에게 다른 배치를 요청하거나 유휴 상태임을 알리는 메시지를 보냅니다. 코디네이터는 남은 작업을 워커 간에 동적으로 재분배하여 부하를 균형 있게 조절합니다.

이 패턴은 SPDK의 NVMe-oF 타겟이 수신 명령을 I/O 큐와 CPU 코어에 분배하는 방식을 모델링합니다.

---

## 흔한 실수

### 1. 스레드 간 I/O 채널 공유

**잘못된 예:**
```c
/* Coordinator gets one channel, passes it to all workers */
g_shared_ch = spdk_bdev_get_io_channel(desc);
spdk_thread_send_msg(worker->thread, worker_start, g_shared_ch); /* BUG */
```

**실패 원인:** bdev 계층은 채널에서 소유 스레드로의 역참조 포인터(back-pointer)를 저장합니다. 다른 스레드에서 I/O를 제출하면 채널의 내부 큐가 손상되어 어서션 실패(assertion failure) 또는 무음 데이터 손상이 발생합니다.

**올바른 방법:** 각 워커는 자신의 스레드 컨텍스트 내에서(즉, 해당 스레드에서 실행되는 메시지 콜백이나 폴러 안에서) `spdk_bdev_get_io_channel`을 호출해야 합니다.

---

### 2. 스레드 시작 전에 spdk_bdev_get_io_channel 호출

**잘못된 예:**
```c
/* In app_start(), before sending any message to the worker thread */
worker->ch = spdk_bdev_get_io_channel(desc); /* runs on coordinator thread */
worker->thread = spdk_thread_create(...);
spdk_thread_send_msg(worker->thread, worker_start, worker); /* ch was created on wrong thread */
```

**올바른 방법:** 의도된 스레드에서 실행되는 첫 번째 메시지나 폴러 안에서 항상 채널을 획득합니다.

---

### 3. 잘못된 스레드에서 채널 해제

**잘못된 예:**
```c
/* Coordinator tears down the channel on behalf of a worker */
spdk_put_io_channel(worker->ch); /* runs on coordinator thread - BUG */
```

**올바른 방법:** 소유 스레드로 해제를 라우팅합니다:
```c
spdk_thread_send_msg(worker->thread, worker_put_channel, worker);
```

---

### 4. 채널 해제 전에 스레드 파괴

SPDK는 내부적으로 채널 파괴를 지연시키지만, `spdk_put_io_channel`이 호출된 후에야 `spdk_thread_exit`를 호출해야 합니다. 올바른 순서는 다음과 같습니다:

```
worker thread:
  1. spdk_put_io_channel(ch)
  2. spdk_thread_exit(thread)   /* only after put */
```

모든 채널을 해제하기 전에 `spdk_thread_exit`를 호출하면, 스레드의 소멸자(destructor)가 미해결 채널 참조를 기다리며 어서트하거나 행(hang)됩니다.

---

### 5. 워커 스레드 폴링 누락

SPDK 스레드는 `spdk_thread_poll()`이 호출될 때만 진행됩니다. `spdk_app_start` 리액터는 앱 스레드를 자동으로 폴링합니다. `spdk_thread_create`로 생성된 워커 스레드는 cpumask를 통해 할당된 DPDK lcore에 의해 폴링됩니다. 워커를 고정하려는 코어가 포함되지 않은 리액터 마스크(`-m`/`reactor_mask`)를 사용하면, 해당 워커는 절대 실행되지 않습니다.

항상 다음을 보장하세요: **리액터 마스크의 lcore 수 >= 워커 스레드 수 + 1** (코디네이터용).

---

### 6. 완료 콜백과 스레드 종료 간의 경쟁 조건

I/O가 아직 진행 중(in-flight)일 때 스레드를 종료하지 마세요. 안전한 패턴은 다음과 같습니다:

```
worker: submit all I/Os
  -> read_complete increments ios_completed each time
  -> when ios_completed == ios_target, send "done" message to coordinator
coordinator: receives "done" message
  -> sends "put channel" message back to worker
worker: puts channel, then calls spdk_thread_exit
```

이렇게 하면 채널이 해제될 때 진행 중인 I/O가 없음을 보장합니다.

---

## 주요 API 참조

| API | 스레드 | 용도 |
|-----|--------|---------|
| `spdk_thread_create(name, cpumask)` | 아무 스레드 | 새 SPDK 스레드 생성 |
| `spdk_get_thread()` | 아무 스레드 | 현재 SPDK 스레드 포인터 획득 |
| `spdk_thread_send_msg(thread, fn, ctx)` | 아무 스레드 | `thread`에서 `fn(ctx)`를 비동기적으로 예약 |
| `spdk_thread_exit(thread)` | 소유 스레드 | 스레드 정상 종료 시작 |
| `spdk_thread_destroy(thread)` | 아무 스레드 (exit 후) | 스레드 리소스 해제 |
| `spdk_bdev_get_io_channel(desc)` | 소유 스레드 | 스레드별 I/O 채널 획득 |
| `spdk_put_io_channel(ch)` | 소유 스레드 | 스레드별 I/O 채널 해제 |
| `spdk_bdev_read_blocks(desc, ch, ...)` | 소유 스레드 | 비동기 블록 읽기 제출 |
| `spdk_bdev_free_io(bdev_io)` | 완료 콜백 | I/O를 프리 풀(free pool)에 반환 |
| `spdk_env_get_current_core()` | 아무 스레드 | 현재 실행 중인 lcore 조회 |
| `spdk_env_get_core_count()` | 아무 스레드 | 사용 가능한 총 lcore 수 |

---

## 복습 질문

1. `spdk_bdev_get_io_channel`을 편리한 아무 스레드가 아닌, 채널을 사용할 스레드에서 호출해야 하는 이유는 무엇입니까?

2. `spdk_thread_send_msg`는 "비동기적"이라고 설명됩니다. 동일한 소스 스레드에서 동일한 대상 스레드로 전송된 메시지의 순서에 대해 이는 무엇을 의미합니까?

3. 두 워커가 거의 동시에 완료되어 둘 다 코디네이터에게 메시지를 보내면, `workers_done++`에 경쟁 조건(race condition)이 있습니까? 그 이유는 무엇입니까?

4. `reactor_mask = "0x1"` (코어 0만)로 설정했지만 cpumask가 코어 1과 2를 대상으로 하는 두 개의 워커 스레드를 생성하면 어떻게 됩니까?

5. `spdk_app_parse_args`를 사용하여 커맨드라인 인수로 전달되는 설정 가능한 워커 수를 지원하도록 솔루션을 수정하세요.

---

## 다음 실습

EX07: 폴러 기반 속도 제한(Poller-Based Rate Limiting) - SPDK 폴러를 사용하여 토큰 버킷 속도 제한기를 구현하고, 이 실습의 멀티 스레드 애플리케이션이 발행하는 IOPS를 제한합니다.

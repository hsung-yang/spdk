# SPDK 오류 코드 레퍼런스

**소스 파일**: `include/spdk/bdev_module.h`, `include/spdk/nvme_spec.h`, `include/spdk/string.h`

---

## 목차

1. [Bdev I/O 상태 코드](#1-bdev-io-상태-코드)
2. [NVMe 상태 코드 유형 (SCT)](#2-nvme-상태-코드-유형-sct)
3. [NVMe 일반 명령 상태 코드](#3-nvme-일반-명령-상태-코드)
4. [NVMe 명령별 상태 코드](#4-nvme-명령별-상태-코드)
5. [NVMe 미디어 오류 상태 코드](#5-nvme-미디어-오류-상태-코드)
6. [NVMe 경로 상태 코드](#6-nvme-경로-상태-코드)
7. [SPDK에서 사용되는 POSIX errno 값](#7-spdk에서-사용되는-posix-errno-값)
8. [오류 유틸리티 함수](#8-오류-유틸리티-함수)
9. [일반적인 오류 시나리오](#9-일반적인-오류-시나리오)
10. [빠른 조회 테이블](#10-빠른-조회-테이블)
11. [오류 범주별 디버깅 팁](#11-오류-범주별-디버깅-팁)

---

## 1. Bdev I/O 상태 코드

`include/spdk/bdev_module.h`에 `enum spdk_bdev_io_status`로 정의됩니다.

이 값들은 `struct spdk_bdev_io`의 `status` 필드에 나타나며 bdev I/O 완료 콜백(`spdk_bdev_io_completion_cb`)에 전달됩니다.

| 상수 | 값 | 의미 |
|---|---|---|
| `SPDK_BDEV_IO_STATUS_SUCCESS` | `1` | I/O가 성공적으로 완료됨 |
| `SPDK_BDEV_IO_STATUS_PENDING` | `0` | I/O가 아직 진행 중 (최종 상태가 아님) |
| `SPDK_BDEV_IO_STATUS_FAILED` | `-1` | 일반 실패 — 더 구체적인 코드가 적용되지 않을 때 사용 |
| `SPDK_BDEV_IO_STATUS_NVME_ERROR` | `-2` | NVMe 수준 오류; SCT/SC를 위해 `bdev_io->internal.error.nvme` 확인 |
| `SPDK_BDEV_IO_STATUS_SCSI_ERROR` | `-3` | SCSI 수준 오류; 센스 데이터를 위해 `bdev_io->internal.error.scsi` 확인 |
| `SPDK_BDEV_IO_STATUS_NOMEM` | `-4` | 리소스 부족 (DMA 버퍼, 큐 깊이 등) — I/O가 자동으로 재시도됨 |
| `SPDK_BDEV_IO_STATUS_MISCOMPARE` | `-5` | 비교 후 쓰기(compare-and-write) 또는 검증 작업 중 데이터 불일치 |
| `SPDK_BDEV_IO_STATUS_FIRST_FUSED_FAILED` | `-6` | 융합 작업(fused operation)의 첫 번째 명령이 실패 |
| `SPDK_BDEV_IO_STATUS_ABORTED` | `-7` | I/O가 명시적으로 중단됨 (예: 큐 드레인, 리셋) |
| `SPDK_BDEV_IO_STATUS_AIO_ERROR` | `-8` | Linux AIO 작업이 오류를 반환 |
| `SPDK_MIN_BDEV_IO_STATUS` | `-8` | 센티넬 — 유효한 최저 상태 값 |

### 핵심 규칙

- `PENDING`이 아닌 `<= 0` 값은 오류를 나타냅니다.
- `NOMEM`은 영구적 실패가 **아닙니다**. bdev 레이어가 동일 채널의 다른 I/O가 완료되어 리소스를 해제한 후 자동으로 I/O를 재시도합니다. bdev 모듈은 `RESET` 유형 I/O에 대해 `NOMEM`을 반환하면 안 됩니다.
- `NVME_ERROR`와 `SCSI_ERROR`는 `bdev_io->internal.error` 공용체(union)에 추가 세부 정보를 담고 있습니다. 일반 오류 메시지를 로깅하기 전에 항상 해당 공용체를 검사하세요.
- `ABORTED`는 진행 중인 I/O에 대해 `spdk_bdev_abort()`를 호출할 때 예상되는 상태입니다.

### 완료 콜백 패턴

```c
static void
my_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    if (!success) {
        enum spdk_bdev_io_status status = bdev_io->internal.status;
        switch (status) {
        case SPDK_BDEV_IO_STATUS_NVME_ERROR:
            /* bdev_io->internal.error.nvme.sct 및 .sc 검사 */
            break;
        case SPDK_BDEV_IO_STATUS_NOMEM:
            /* 여기에 도달하면 안 됨 — bdev 레이어가 자동 재시도 */
            break;
        case SPDK_BDEV_IO_STATUS_ABORTED:
            /* 리셋 또는 종료 중 예상되는 상태 */
            break;
        default:
            break;
        }
    }
    spdk_bdev_free_io(bdev_io);
}
```

---

## 2. NVMe 상태 코드 유형 (SCT)

`include/spdk/nvme_spec.h`에 `enum spdk_nvme_status_code_type`으로 정의됩니다.

SCT 필드는 NVMe 완료 큐 항목(Completion Queue Entry) 상태 필드의 3비트이며, SC 필드가 속하는 상태 코드의 네임스페이스를 선택합니다.

| 상수 | 값 | 설명 |
|---|---|---|
| `SPDK_NVME_SCT_GENERIC` | `0x0` | 일반 명령 상태 — 모든 명령에 적용 |
| `SPDK_NVME_SCT_COMMAND_SPECIFIC` | `0x1` | 발행된 명령 유형에 특정한 상태 |
| `SPDK_NVME_SCT_MEDIA_ERROR` | `0x2` | 미디어 및 데이터 무결성 오류 |
| `SPDK_NVME_SCT_PATH` | `0x3` | 경로 관련 오류 (멀티패스, ANA) |
| `SPDK_NVME_SCT_VENDOR_SPECIFIC` | `0x7` | 벤더 정의 상태 코드 |

### 완료 확인 매크로

```c
/* 완료에 오류가 포함되어 있으면 true */
spdk_nvme_cpl_is_error(cpl)

/* 완료가 성공이면 true */
spdk_nvme_cpl_is_success(cpl)

/* 오류가 미디어/무결성 오류이면 true */
spdk_nvme_cpl_is_media_error(cpl)

/* 오류가 경로 관련 오류이면 true */
spdk_nvme_cpl_is_path_error(cpl)

/* 오류가 네임스페이스 미준비를 나타내면 true */
spdk_nvme_cpl_is_ns_not_ready(cpl)
```

`sct == SPDK_NVME_SCT_GENERIC`이고 `sc == SPDK_NVME_SC_SUCCESS`일 때만 완료가 성공으로 간주됩니다. 다른 모든 조합은 오류입니다.

---

## 3. NVMe 일반 명령 상태 코드

`enum spdk_nvme_generic_command_status_code` (`SCT = 0x0`)로 정의됩니다.

### 성공

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_SUCCESS` | `0x00` | 명령이 오류 없이 완료됨 |

### 명령 매개변수 오류

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_INVALID_OPCODE` | `0x01` | 옵코드(opcode)가 지원되지 않거나 예약됨 |
| `SPDK_NVME_SC_INVALID_FIELD` | `0x02` | 명령 필드에 유효하지 않은 값이 포함됨 |
| `SPDK_NVME_SC_COMMAND_ID_CONFLICT` | `0x03` | CID가 이미 제출 큐(Submission Queue)에서 사용 중 |
| `SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT` | `0x0b` | NSID가 유효하지 않거나 네임스페이스 형식이 지원되지 않음 |
| `SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR` | `0x0c` | 필요한 순서를 벗어나 명령이 발행됨 |
| `SPDK_NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED` | `0x14` | 쓰기가 원자적 경계를 넘음 |
| `SPDK_NVME_SC_OPERATION_DENIED` | `0x15` | 정책 또는 예약에 의해 작업이 허용되지 않음 |

### 데이터 전송 및 SGL 오류

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_DATA_TRANSFER_ERROR` | `0x04` | 호스트-장치 또는 장치-호스트 데이터 전송 실패 |
| `SPDK_NVME_SC_INVALID_SGL_SEG_DESCRIPTOR` | `0x0d` | SGL 세그먼트 디스크립터가 유효하지 않음 |
| `SPDK_NVME_SC_INVALID_NUM_SGL_DESCIRPTORS` | `0x0e` | SGL 디스크립터 수가 한도를 초과 |
| `SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID` | `0x0f` | 데이터 SGL 길이가 전송 길이와 일치하지 않음 |
| `SPDK_NVME_SC_METADATA_SGL_LENGTH_INVALID` | `0x10` | 메타데이터 SGL 길이가 유효하지 않음 |
| `SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID` | `0x11` | SGL 디스크립터 유형이 지원되지 않음 |
| `SPDK_NVME_SC_INVALID_CONTROLLER_MEM_BUF` | `0x12` | 컨트롤러 메모리 버퍼 주소가 유효하지 않음 |
| `SPDK_NVME_SC_INVALID_PRP_OFFSET` | `0x13` | PRP 항목 오프셋이 유효하지 않음 |
| `SPDK_NVME_SC_INVALID_SGL_OFFSET` | `0x16` | SGL 오프셋이 지원되지 않음 |
| `SPDK_NVME_SC_SGL_DATA_BLOCK_GRANULARITY_INVALID` | `0x1e` | SGL 데이터 블록 세분화(granularity) 위반 |

### 중단(Abort) 및 전원 이벤트

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_ABORTED_POWER_LOSS` | `0x05` | 전원 손실 알림으로 인해 명령이 중단됨 |
| `SPDK_NVME_SC_INTERNAL_DEVICE_ERROR` | `0x06` | 내부 장치 오류; 데이터가 전송되지 않음 |
| `SPDK_NVME_SC_ABORTED_BY_REQUEST` | `0x07` | Abort 명령에 의해 명령이 중단됨 |
| `SPDK_NVME_SC_ABORTED_SQ_DELETION` | `0x08` | SQ가 삭제되어 명령이 중단됨 |
| `SPDK_NVME_SC_ABORTED_FAILED_FUSED` | `0x09` | 다른 절반이 실패하여 융합 명령(fused command)이 중단됨 |
| `SPDK_NVME_SC_ABORTED_MISSING_FUSED` | `0x0a` | 동반 명령이 누락되어 융합 명령이 중단됨 |
| `SPDK_NVME_SC_ABORTED_PREEMPT` | `0x1b` | 예약 선점 및 중단으로 인해 명령이 중단됨 |
| `SPDK_NVME_SC_COMMAND_INTERRUPTED` | `0x21` | 명령이 인터럽트됨 |

### 패브릭 / Keep-Alive / 호스트

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_HOSTID_INCONSISTENT_FORMAT` | `0x18` | 호스트 식별자 형식이 일관되지 않음 |
| `SPDK_NVME_SC_KEEP_ALIVE_EXPIRED` | `0x19` | Keep-alive 타이머가 만료됨 |
| `SPDK_NVME_SC_KEEP_ALIVE_INVALID` | `0x1a` | Keep-alive 타이머 값이 유효하지 않거나 지원되지 않음 |
| `SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR` | `0x22` | 일시적 전송 오류; 재시도 시 성공할 수 있음 |
| `SPDK_NVME_SC_COMMAND_PROHIBITED_BY_LOCKDOWN` | `0x23` | 현재 잠금 모드에서 명령이 허용되지 않음 |
| `SPDK_NVME_SC_ADMIN_COMMAND_MEDIA_NOT_READY` | `0x24` | 관리 명령이 발행되었지만 미디어가 준비되지 않음 |

### 삭제(Sanitize)

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_SANITIZE_FAILED` | `0x1c` | 삭제 작업 실패 |
| `SPDK_NVME_SC_SANITIZE_IN_PROGRESS` | `0x1d` | 삭제 진행 중; 대부분의 명령이 차단됨 |

### 네임스페이스 / 용량 오류 (SC >= 0x80)

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_LBA_OUT_OF_RANGE` | `0x80` | LBA가 네임스페이스 크기를 초과 |
| `SPDK_NVME_SC_CAPACITY_EXCEEDED` | `0x81` | 네임스페이스 용량 초과 (씬 프로비저닝) |
| `SPDK_NVME_SC_NAMESPACE_NOT_READY` | `0x82` | 네임스페이스 미준비; 짧은 지연 후 재시도 |
| `SPDK_NVME_SC_RESERVATION_CONFLICT` | `0x83` | 다른 호스트가 보유한 예약이 충돌 |
| `SPDK_NVME_SC_FORMAT_IN_PROGRESS` | `0x84` | 이 네임스페이스에서 Format NVM 진행 중 |

### FDP (유연한 데이터 배치, Flexible Data Placement)

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_FDP_DISABLED` | `0x29` | 이 네임스페이스에서 FDP가 활성화되지 않음 |
| `SPDK_NVME_SC_INVALID_PLACEMENT_HANDLE_LIST` | `0x2a` | 배치 핸들 목록이 유효하지 않음 |

---

## 4. NVMe 명령별 상태 코드

`enum spdk_nvme_command_specific_status_code` (`SCT = 0x1`)로 정의됩니다.

특정 관리(admin) 또는 I/O 명령에만 적용됩니다. SC 값의 의미는 어떤 옵코드가 제출되었는지에 따라 달라집니다.

### 큐 관리 (I/O SQ/CQ 생성/삭제)

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_COMPLETION_QUEUE_INVALID` | `0x00` | 지정된 CQ가 존재하지 않음 |
| `SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER` | `0x01` | 큐 ID가 유효하지 않거나 이미 사용 중 |
| `SPDK_NVME_SC_INVALID_QUEUE_SIZE` | `0x02` | 큐 크기가 컨트롤러 최대값을 초과 |
| `SPDK_NVME_SC_ABORT_COMMAND_LIMIT_EXCEEDED` | `0x03` | 미완료 중단 수가 초과됨 |
| `SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED` | `0x05` | AER 한도 도달 |
| `SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR` | `0x08` | 인터럽트 벡터가 유효하지 않음 |
| `SPDK_NVME_SC_INVALID_QUEUE_DELETION` | `0x0c` | 현재 상태에서 큐를 삭제할 수 없음 |

### 펌웨어 및 로그

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_INVALID_FIRMWARE_SLOT` | `0x06` | 펌웨어 슬롯 번호가 유효하지 않음 |
| `SPDK_NVME_SC_INVALID_FIRMWARE_IMAGE` | `0x07` | 펌웨어 이미지 검증 실패 |
| `SPDK_NVME_SC_INVALID_LOG_PAGE` | `0x09` | 로그 페이지 식별자가 지원되지 않음 |
| `SPDK_NVME_SC_FIRMWARE_REQ_CONVENTIONAL_RESET` | `0x0b` | 펌웨어 활성화에 일반 리셋이 필요 |
| `SPDK_NVME_SC_FIRMWARE_REQ_NVM_RESET` | `0x10` | 펌웨어 활성화에 NVM 서브시스템 리셋이 필요 |
| `SPDK_NVME_SC_FIRMWARE_REQ_RESET` | `0x11` | 펌웨어 활성화에 컨트롤러 리셋이 필요 |
| `SPDK_NVME_SC_FIRMWARE_REQ_MAX_TIME_VIOLATION` | `0x12` | 펌웨어 활성화가 시간 제한을 초과할 수 있음 |
| `SPDK_NVME_SC_FIRMWARE_ACTIVATION_PROHIBITED` | `0x13` | 현 시점에서 펌웨어 활성화가 허용되지 않음 |

### 기능(Feature) 관리

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_INVALID_FORMAT` | `0x0a` | Format NVM 매개변수가 유효하지 않음 |
| `SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE` | `0x0d` | 기능 ID가 저장(Save)을 지원하지 않음 |
| `SPDK_NVME_SC_FEATURE_NOT_CHANGEABLE` | `0x0e` | 기능을 변경할 수 없음 |
| `SPDK_NVME_SC_FEATURE_NOT_NAMESPACE_SPECIFIC` | `0x0f` | 기능이 네임스페이스가 아닌 컨트롤러에 적용됨 |

### 네임스페이스 관리

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_OVERLAPPING_RANGE` | `0x14` | 디렉티브 범위가 기존 범위와 겹침 |
| `SPDK_NVME_SC_NAMESPACE_INSUFFICIENT_CAPACITY` | `0x15` | 새 네임스페이스를 위한 NVM 용량 부족 |
| `SPDK_NVME_SC_NAMESPACE_ID_UNAVAILABLE` | `0x16` | 할당 가능한 NSID 없음 |
| `SPDK_NVME_SC_NAMESPACE_ALREADY_ATTACHED` | `0x18` | 네임스페이스가 이미 이 컨트롤러에 연결됨 |
| `SPDK_NVME_SC_NAMESPACE_IS_PRIVATE` | `0x19` | 네임스페이스가 프라이빗이어서 공유할 수 없음 |
| `SPDK_NVME_SC_NAMESPACE_NOT_ATTACHED` | `0x1a` | 네임스페이스가 이 컨트롤러에 연결되지 않음 |
| `SPDK_NVME_SC_THINPROVISIONING_NOT_SUPPORTED` | `0x1b` | 씬 프로비저닝(Thin Provisioning)이 지원되지 않음 |
| `SPDK_NVME_SC_CONTROLLER_LIST_INVALID` | `0x1c` | 컨트롤러 목록에 유효하지 않은 항목이 있음 |
| `SPDK_NVME_SC_NAMESPACE_ATTACH_LIMIT_EXCEEDED` | `0x27` | 네임스페이스에 연결된 컨트롤러가 너무 많음 |

### ANA (비대칭 네임스페이스 접근, Asymmetric Namespace Access)

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_ANA_GROUP_IDENTIFIER_INVALID` | `0x24` | ANA 그룹 ID가 유효하지 않음 |
| `SPDK_NVME_SC_ANA_ATTACH_FAILED` | `0x25` | ANA 연결 실패 |

### I/O 명령 특정

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_CONFLICTING_ATTRIBUTES` | `0x80` | 명령에 충돌하는 속성이 있음 |
| `SPDK_NVME_SC_INVALID_PROTECTION_INFO` | `0x81` | 보호 정보(Protection Information) 설정이 유효하지 않음 |
| `SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE` | `0x82` | 읽기 전용 LBA 범위에 쓰기 시도 |

### ZNS (Zoned Namespace)

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_ZONED_BOUNDARY_ERROR` | `0xb8` | 쓰기가 존(Zone) 경계를 넘음 |
| `SPDK_NVME_SC_ZONE_IS_FULL` | `0xb9` | 대상 존이 가득 참 |
| `SPDK_NVME_SC_ZONE_IS_READ_ONLY` | `0xba` | 대상 존이 읽기 전용 |
| `SPDK_NVME_SC_ZONE_IS_OFFLINE` | `0xbb` | 대상 존이 오프라인 |
| `SPDK_NVME_SC_ZONE_INVALID_WRITE` | `0xbc` | 쓰기 주소가 존 쓰기 포인터(Write Pointer)와 일치하지 않음 |
| `SPDK_NVME_SC_TOO_MANY_ACTIVE_ZONES` | `0xbd` | 활성 존 한도 초과 |
| `SPDK_NVME_SC_TOO_MANY_OPEN_ZONES` | `0xbe` | 열린 존 한도 초과 |
| `SPDK_NVME_SC_INVALID_ZONE_STATE_TRANSITION` | `0xbf` | 요청된 존 상태 전이가 허용되지 않음 |

---

## 5. NVMe 미디어 오류 상태 코드

`enum spdk_nvme_media_error_status_code` (`SCT = 0x2`)로 정의됩니다.

데이터 무결성 또는 물리적 미디어 장애를 나타냅니다. 데이터 내구성 관점에서 가장 중요한 범주입니다.

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_WRITE_FAULTS` | `0x80` | 쓰기 작업에서 복구 불가능한 물리적 장애 발생 |
| `SPDK_NVME_SC_UNRECOVERED_READ_ERROR` | `0x81` | 읽기 데이터가 복구 불가능 (정정 불가능한 ECC) |
| `SPDK_NVME_SC_GUARD_CHECK_ERROR` | `0x82` | 종단간(end-to-end) 데이터의 가드(CRC) 검사 실패 |
| `SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR` | `0x83` | 보호 정보의 애플리케이션 태그 불일치 |
| `SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR` | `0x84` | 보호 정보의 참조 태그 불일치 |
| `SPDK_NVME_SC_COMPARE_FAILURE` | `0x85` | Compare 명령 데이터 불일치 (엄밀히 미디어 오류는 아님) |
| `SPDK_NVME_SC_ACCESS_DENIED` | `0x86` | LBA 범위에 대한 접근이 거부됨 |
| `SPDK_NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK` | `0x87` | 읽기가 할당 해제되었거나 기록되지 않은 논리 블록 데이터를 반환 |
| `SPDK_NVME_SC_END_TO_END_STORAGE_TAG_CHECK_ERROR` | `0x88` | 종단간 보호의 저장소 태그 검사 오류 |

### 미디어 오류 처리 참고

`SPDK_NVME_SC_UNRECOVERED_READ_ERROR` 또는 `SPDK_NVME_SC_WRITE_FAULTS`의 미디어 오류는 일반적으로 NAND 장애를 나타냅니다. 드라이브가 AER(비동기 이벤트 요청, Asynchronous Event Request)을 통해 위치를 보고할 수 있습니다. SPDK에서는 SC를 검사하기 전에 `spdk_nvme_cpl_is_media_error()`를 사용하여 이 범주를 빠르게 테스트하세요.

---

## 6. NVMe 경로 상태 코드

`enum spdk_nvme_path_status_code` (`SCT = 0x3`)로 정의됩니다.

경로 오류는 멀티패스(ANA) 구성이나 패브릭 전송이 실패할 때 나타납니다.

| 상수 | SC | 설명 |
|---|---|---|
| `SPDK_NVME_SC_INTERNAL_PATH_ERROR` | `0x00` | 일반 내부 경로 오류 |
| `SPDK_NVME_SC_ASYMMETRIC_ACCESS_PERSISTENT_LOSS` | `0x01` | ANA 상태가 Persistent Loss — 경로가 영구적으로 사용 불가 |
| `SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE` | `0x02` | ANA 상태가 Inaccessible — 다른 경로에서 재시도 |
| `SPDK_NVME_SC_ASYMMETRIC_ACCESS_TRANSITION` | `0x03` | ANA 상태가 전이 중 — 상태 안정화 후 재시도 |
| `SPDK_NVME_SC_CONTROLLER_PATH_ERROR` | `0x60` | 컨트롤러 경로 오류 |
| `SPDK_NVME_SC_HOST_PATH_ERROR` | `0x70` | 호스트 경로 오류 (예: 패브릭 전송) |
| `SPDK_NVME_SC_ABORTED_BY_HOST` | `0x71` | 호스트에 의해 명령이 중단됨 (호스트 발의) |

SC를 검사하기 전에 `SCT = 0x3`을 테스트하려면 `spdk_nvme_cpl_is_path_error()`를 사용하세요.

---

## 7. SPDK에서 사용되는 POSIX errno 값

SPDK API 함수는 실패 시 음수 errno 값(`-errno`)을 반환합니다. 표준 Linux 규칙을 따르지만 SPDK 코드에서 가장 흔히 접하는 값들은 다음과 같습니다.

| errno | 값 | 매크로 | SPDK 컨텍스트에서의 의미 |
|---|---|---|---|
| `EINVAL` | `22` | `-EINVAL` | 유효하지 않은 인수 — 잘못된 매개변수, 지원되지 않는 설정 또는 형식 오류 |
| `ENOMEM` | `12` | `-ENOMEM` | 메모리 부족 — 휴지페이지(Hugepage) 할당 실패, 풀 소진 |
| `ENODEV` | `19` | `-ENODEV` | 해당 장치 없음 — NVMe 컨트롤러를 찾을 수 없음, 장치 분리됨 |
| `EBUSY` | `16` | `-EBUSY` | 장치 또는 리소스 사용 중 — 큐 가득 참, 리액터 유휴 아님, 장치 리셋 중 |
| `EEXIST` | `17` | `-EEXIST` | 이미 존재함 — 중복 bdev 이름, 중복 엔드포인트 |
| `ENOTSUP` | `95` | `-ENOTSUP` | 지원되지 않음 — 이 백엔드에 대해 기능이 구현되지 않음 |
| `EIO` | `5` | `-EIO` | I/O 오류 — 전송 수준 실패, vhost 소켓 오류 |
| `EAGAIN` | `11` | `-EAGAIN` | 다시 시도 — 리소스가 일시적으로 사용 불가, 백프레셔(back-pressure) |
| `ETIMEDOUT` | `110` | `-ETIMEDOUT` | 시간 초과 — NVMe 관리 명령 타임아웃, keep-alive 만료 |
| `ENOENT` | `2` | `-ENOENT` | 해당 파일 또는 디렉토리 없음 — 설정 파일을 찾을 수 없음 |
| `EPERM` | `1` | `-EPERM` | 작업 허용되지 않음 — 권한 부족 |
| `ERANGE` | `34` | `-ERANGE` | 범위 초과 — LBA 범위 검사, 버퍼 너무 작음 |
| `EFAULT` | `14` | `-EFAULT` | 잘못된 주소 — null 포인터, 유효하지 않은 DMA 매핑 |
| `EACCES` | `13` | `-EACCES` | 권한 거부 — vfio 그룹 접근, 휴지페이지 mmap |
| `ENOSPC` | `28` | `-ENOSPC` | 공간 부족 — 씬 프로비저닝 용량 도달 |

### SPDK에서 errno 디코딩

```c
int rc = spdk_some_api_call(...);
if (rc != 0) {
    SPDK_ERRLOG("API failed: %s\n", spdk_strerror(-rc));
    /* 또는 스레드 안전 버전: */
    char errbuf[128];
    spdk_strerror_r(-rc, errbuf, sizeof(errbuf));
    SPDK_ERRLOG("API failed: %s\n", errbuf);
}
```

### 일반적인 errno 반환 패턴

```c
/* lib/event/reactor.c에서 */
if (core >= RTE_MAX_LCORE) {
    return -EINVAL;   /* 호출자가 잘못된 CPU 마스크를 전달 */
}
if (reactor == NULL) {
    return -ENOMEM;   /* rte_zmalloc이 NULL을 반환 */
}
if (reactor->in_interrupt) {
    return -EBUSY;    /* 리액터가 인터럽트 모드 */
}
if (!spdk_interrupt_mode_is_enabled()) {
    return -ENOTSUP;  /* 런타임에 기능 비활성화됨 */
}
```

---

## 8. 오류 유틸리티 함수

### `spdk_strerror()` / `spdk_strerror_r()`

`include/spdk/string.h`에 선언됩니다.

```c
/* 스레드 로컬 문자열에 대한 포인터를 반환 — 스레드 간 안전하지 않음 */
const char *spdk_strerror(int errnum);

/* 스레드 안전 버전 — 호출자가 제공한 버퍼에 기록 */
void spdk_strerror_r(int errnum, char *buf, size_t buflen);
```

**양수** errno 값을 전달합니다 (예: `spdk_strerror(ENOMEM)` 또는 `rc`가 음수일 때 `spdk_strerror(-rc)`).

### `spdk_nvme_cpl_get_status_string()`

`include/spdk/nvme.h`에 선언됩니다.

```c
const char *spdk_nvme_cpl_get_status_string(const struct spdk_nvme_status *status);
```

NVMe 완료 상태에 대한 사람이 읽을 수 있는 문자열을 반환합니다. 로깅에 유용합니다:

```c
if (spdk_nvme_cpl_is_error(&cpl)) {
    SPDK_ERRLOG("NVMe error: %s\n",
                spdk_nvme_cpl_get_status_string(&cpl.status));
}
```

### 완료 확인 매크로 (`nvme_spec.h`에서)

```c
/* 기본 오류 검사 — 오류가 있으면 true */
spdk_nvme_cpl_is_error(cpl)          /* sct != 0 || sc != 0 */

/* 반대 */
spdk_nvme_cpl_is_success(cpl)

/* 범주 검사 */
spdk_nvme_cpl_is_media_error(cpl)    /* SCT == 0x2 */
spdk_nvme_cpl_is_path_error(cpl)     /* SCT == 0x3 */
spdk_nvme_cpl_is_ns_not_ready(cpl)   /* SCT=0, SC=0x82 */
```

---

## 9. 일반적인 오류 시나리오

### 시나리오 1: 탐색(Probe) 시 NVMe 장치를 찾을 수 없음

**증상**: `spdk_nvme_probe()`가 0을 반환하지만 컨트롤러가 연결되지 않음.

**관련 코드**: `-ENODEV`, probe 콜백이 전혀 실행되지 않음.

**원인**:
- PCI 주소 문자열이 잘못됨 (전송 레이어에서 `-EINVAL` 트리거).
- VFIO가 설정되지 않음 — 장치가 여전히 커널 드라이버에 바인딩됨.
- probe와 attach 사이에 장치가 핫리무브(hot-remove)됨.

**디버그**:
```bash
# VFIO 바인딩 확인
ls /sys/bus/pci/devices/<BDF>/driver
# nvme가 아닌 vfio-pci가 표시되어야 함
```

---

### 시나리오 2: Bdev I/O가 NOMEM을 반환

**증상**: I/O 완료 콜백이 `success = false`를 수신, `bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM`.

**원인**:
- DMA 안전 메모리 풀(`spdk_mempool`)이 소진됨.
- NVMe 큐 페어(Queue Pair)가 가득 참 (제출 큐 깊이 초과).
- 설정된 큐 깊이에 대해 동시 I/O가 너무 많음.

**해결**: 자동으로 처리됩니다 — bdev 레이어가 I/O를 큐잉하고 진행 중인 I/O가 완료된 후 재시도합니다. `NOMEM`이 지속되면 큐 깊이를 늘리거나 (bdev NVMe 설정의 `num_io_queues`, `queue_depth`) 애플리케이션 동시성을 줄이세요.

---

### 시나리오 3: NVMe Keep-Alive 만료 (NVMf)

**증상**: 완료에서 `SPDK_NVME_SC_KEEP_ALIVE_EXPIRED` (SC `0x19`, SCT `0x0`) 또는 호스트 측 연결 해제.

**원인**:
- NVMf 타겟이 과부하; 폴링 루프가 정체됨.
- Keep-alive 타임아웃 (NVMf connect의 `-k`)이 RTT 대비 너무 낮게 설정됨.
- SPDK 리액터(reactor)가 너무 오래 일시 중지됨 (예: 휴지페이지 할당 경합).

**디버그**:
```bash
# 타겟 리액터 사용률 확인
rpc.py framework_get_reactors
# 100% busy에 근접한 리액터를 찾으세요
```

---

### 시나리오 4: LBA 범위 초과

**증상**: `SPDK_NVME_SC_LBA_OUT_OF_RANGE` (SC `0x80`, SCT `0x0`).

**원인**:
- 애플리케이션이 잘못된 오프셋을 계산 — 네임스페이스 끝을 지나는 오프바이원(off-by-one).
- 장치가 열린 후 네임스페이스 크기가 변경됨.
- 캐시된 `num_blocks` 값이 오래됨.

**디버그**: `spdk_nvme_ns_get_num_sectors()`를 호출하여 현재 네임스페이스 크기를 가져옵니다. 항상 검증: `start_lba + num_blocks <= spdk_nvme_ns_get_num_sectors(ns)`.

---

### 시나리오 5: Zoned Namespace 쓰기 포인터 불일치

**증상**: `SPDK_NVME_SC_ZONE_INVALID_WRITE` (SC `0xbc`, SCT `0x1`).

**원인**:
- 쓰기 LBA가 존 쓰기 포인터와 일치하지 않음 (SLBA가 WP와 같아야 함).
- 동기화 없이 동일 존에 여러 라이터가 경쟁.
- 존이 Open 또는 Implicit Open 상태가 아님.

**디버그**: `Zone Management Receive` 명령을 발행하거나 (`spdk_nvme_zns_report_zones()` 사용) 영향받는 존의 현재 쓰기 포인터를 읽으세요.

---

### 시나리오 6: 리액터가 `-ENOTSUP`을 반환

**증상**: `spdk_env_thread_launch_master()` 또는 스케줄러 API가 `-ENOTSUP`을 반환.

**원인**:
- 인터럽트 모드가 요청되었지만 컴파일에 포함되지 않음.
- 스케줄러 유형이 등록되지 않음 (플러그인 로드되지 않음).
- 존재하지 않는 DPDK 버전이 요구하는 기능.

**해결**: 인터럽트 모드 API를 호출하기 전에 `spdk_interrupt_mode_is_enabled()`를 확인합니다. 필요한 모든 공유 라이브러리가 로드되었는지 확인합니다.

---

### 시나리오 7: NVMf에서 ANA 경로 오류

**증상**: 경로 오류 완료 `SCT = 0x3`, `SC = 0x02` (`SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE`).

**원인**:
- Active-Optimized 경로가 실패; I/O가 Inaccessible 경로로 라우팅됨.
- 컨트롤러가 아직 새 ANA 상태를 보고하지 않음.

**해결**: NVMe 멀티패스 레이어가 자동으로 Optimized 경로에서 재시도해야 합니다. SPDK에서 `spdk_nvme_cpl_is_path_error()`가 true를 반환하면 호출자는 다른 컨트롤러의 다른 `qpair`로 페일오버(failover)해야 합니다.

---

## 10. 빠른 조회 테이블

### 증상별

| 증상 | 예상 코드 | 범주 |
|---|---|---|
| I/O가 데이터 없이 조용히 실패 | `SPDK_NVME_SC_INTERNAL_DEVICE_ERROR` (0x06) | 일반 |
| 잘못된 LBA로 쓰기 거부됨 | `SPDK_NVME_SC_LBA_OUT_OF_RANGE` (0x80) | 일반 |
| 읽기가 예기치 않게 0을 반환 | `SPDK_NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK` (0x87) | 미디어 |
| 데이터 손상 감지 | `SPDK_NVME_SC_GUARD_CHECK_ERROR` (0x82) | 미디어 |
| I/O가 자동으로 재시도됨 | `SPDK_BDEV_IO_STATUS_NOMEM` (-4) | Bdev |
| 리셋 중 I/O 취소됨 | `SPDK_BDEV_IO_STATUS_ABORTED` (-7) | Bdev |
| Linux AIO 실패 | `SPDK_BDEV_IO_STATUS_AIO_ERROR` (-8) | Bdev |
| 장치를 찾을 수 없음 | `-ENODEV` | errno |
| 잘못된 API 매개변수 | `-EINVAL` | errno |
| 풀/메모리 소진 | `-ENOMEM` | errno |
| 기능 사용 불가 | `-ENOTSUP` | errno |
| 장치 리셋 중 | `-EBUSY` | errno |
| 존 쓰기 포인터 불일치 | `SPDK_NVME_SC_ZONE_INVALID_WRITE` (0xbc) | 명령별 |
| 네임스페이스 연결되지 않음 | `SPDK_NVME_SC_NAMESPACE_NOT_ATTACHED` (0x1a) | 명령별 |
| 패브릭 연결 해제 | `SPDK_NVME_SC_KEEP_ALIVE_EXPIRED` (0x19) | 일반 |
| 멀티패스 페일오버 필요 | `SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE` (0x02) | 경로 |

### NVMe SC 값별 (일반 부분 집합)

| SC (hex) | SCT | 상수 |
|---|---|---|
| `0x00` | Generic | `SPDK_NVME_SC_SUCCESS` |
| `0x01` | Generic | `SPDK_NVME_SC_INVALID_OPCODE` |
| `0x02` | Generic | `SPDK_NVME_SC_INVALID_FIELD` |
| `0x04` | Generic | `SPDK_NVME_SC_DATA_TRANSFER_ERROR` |
| `0x06` | Generic | `SPDK_NVME_SC_INTERNAL_DEVICE_ERROR` |
| `0x07` | Generic | `SPDK_NVME_SC_ABORTED_BY_REQUEST` |
| `0x19` | Generic | `SPDK_NVME_SC_KEEP_ALIVE_EXPIRED` |
| `0x1c` | Generic | `SPDK_NVME_SC_SANITIZE_FAILED` |
| `0x80` | Generic | `SPDK_NVME_SC_LBA_OUT_OF_RANGE` |
| `0x82` | Generic | `SPDK_NVME_SC_NAMESPACE_NOT_READY` |
| `0x80` | Media | `SPDK_NVME_SC_WRITE_FAULTS` |
| `0x81` | Media | `SPDK_NVME_SC_UNRECOVERED_READ_ERROR` |
| `0x82` | Media | `SPDK_NVME_SC_GUARD_CHECK_ERROR` |
| `0x87` | Media | `SPDK_NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK` |
| `0x00` | Path | `SPDK_NVME_SC_INTERNAL_PATH_ERROR` |
| `0x02` | Path | `SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE` |
| `0xbc` | Cmd | `SPDK_NVME_SC_ZONE_INVALID_WRITE` |

---

## 11. 오류 범주별 디버깅 팁

### Bdev I/O 상태 오류

1. **`success` 불리언만이 아니라 항상 구체적인 상태 값을 검사하세요.** 불리언은 모든 오류에 대해 `false`입니다; 상태 코드가 어떤 오류인지 알려줍니다.

2. **`NVME_ERROR`의 경우**, 항상 `bdev_io->internal.error.nvme`에서 `sct`와 `sc`를 모두 읽으세요. 두 필드를 로깅하고 `spdk_nvme_cpl_get_status_string()`을 호출하세요.

3. **`SCSI_ERROR`의 경우**, `bdev_io->internal.error.scsi`에서 sense key, ASC, ASCQ를 읽으세요. SCSI 오류는 SCSI 백엔드를 구현하는 bdev 모듈(예: virtio-scsi, iSCSI)에만 관련됩니다.

4. **SPDK 트레이스 활성화**로 상세한 타임라인을 확인하세요:
   ```bash
   spdk_trace_record -s <shm_id> -o trace.out
   spdk_trace -f trace.out | grep BDEV
   ```

### NVMe 일반 오류

5. **`INVALID_FIELD` (0x02)**는 거의 항상 드라이버 버그를 의미합니다 — NVMe 명령 구조체의 필드가 지원되지 않는 값으로 설정되었습니다. 원시 명령 구조체를 로깅하려면 `SPDK_LOG_NVMe`를 DEBUG 수준으로 활성화하세요.

6. **`NAMESPACE_NOT_READY` (0x82)**는 일시적입니다. 백오프(back-off) 후 재시도하세요. 지속되면 Format NVM 또는 Sanitize가 진행 중인지 확인하세요.

7. **`KEEP_ALIVE_EXPIRED` (0x19)**가 NVMf 연결에서 발생하면 패브릭 RTT 또는 타겟 처리 지연이 KAT를 초과한 것입니다. `spdk_nvme_ctrlr_cmd_set_feature()` `KEEP_ALIVE_TIMER`로 조정하거나 `nvme connect`에 `-k <ms>`를 전달하세요.

### 미디어 오류

8. **미디어 오류는 장치 측 영구적 실패입니다.** NSID와 LBA 범위를 즉시 로깅하세요. 미디어 오류를 재시도하지 마세요 — 재시도는 동일한 결과를 반환하고 플래시를 불필요하게 마모시킵니다.

9. **`DEALLOCATED_OR_UNWRITTEN_BLOCK` (0x87)**은 실패가 아닙니다 — 할당 해제된 블록을 읽었고 결정론적 값이 반환되었음을 장치가 보고하는 것입니다 (Identify Namespace 데이터의 `DLFEAT` 필드로 제어). `spdk_nvme_ns_get_dealloc_logical_block_fill_value()`를 확인하세요.

### 경로 오류

10. **경로 오류는 애플리케이션에 I/O 오류로 노출되지 않고 페일오버를 트리거해야 합니다.** SPDK의 bdev_nvme 멀티패스 모듈에서 경로 오류는 다음 사용 가능한 경로에서 자동으로 I/O를 재시도하게 합니다.

11. **`ASYMMETRIC_ACCESS_INACCESSIBLE`**는 ANA 그룹이 Inaccessible 상태임을 의미합니다 — 컨트롤러 변경이 진행 중입니다. Optimized 경로를 사용할 수 있을 때까지 `spdk_nvme_ctrlr_get_ana_log_page()`를 폴링하세요.

### POSIX errno

12. **API 초기화 함수에서 `-ENOMEM`**은 보통 휴지페이지 할당 실패를 의미합니다. `--huge-unlink`가 설정되지 않았는지, 충분한 2 MB 또는 1 GB 페이지가 예약되었는지 확인하세요:
    ```bash
    cat /proc/meminfo | grep HugePage
    ```

13. **bdev 또는 NVMe API에서 `-EINVAL`** — `SPDK_LOG`를 DEBUG 수준으로 활성화하세요. SPDK는 대부분의 코드 경로에서 `-EINVAL`을 반환하기 전에 구체적인 유효하지 않은 필드를 로깅합니다.

14. **리액터 API에서 `-EBUSY`** — 리액터가 인터럽트 모드이거나 현재 처리 중입니다. 임의의 컨텍스트에서 리액터 API를 호출하는 대신 `spdk_thread_send_msg()`를 사용하여 올바른 스레드에서 작업을 스케줄링하세요.

15. **`-ENOTSUP`**는 컴파일 타임 또는 런타임 기능 게이트를 나타냅니다. 관련 기능 플래그(예: `--with-rdma`, `--with-fio-plugin`)에 대해 `./configure --help`를 확인하세요.

---

*레퍼런스는 SPDK 소스에서 작성: `include/spdk/bdev_module.h`, `include/spdk/nvme_spec.h`, `include/spdk/nvme.h`, `include/spdk/string.h`, 및 `lib/`의 대표적인 샘플.*

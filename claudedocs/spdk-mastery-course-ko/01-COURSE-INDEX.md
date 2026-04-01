# SPDK 마스터리 코스 - 전체 인덱스

**코스 버전**: 2.0
**최종 업데이트**: 2026-03-30
**대상 SPDK 버전**: 최신 (`git describe --tags`로 확인)

---

## 코스 구조 개요

이 코스는 **탑다운(Top-Down) 3단계 접근법**을 사용합니다:

- **스테이지 1: 기초** - 아키텍처와 원칙 (3주)
- **스테이지 2: 구현** - API와 코딩 (5주)
- **스테이지 3: 마스터리** - 고급 주제와 기여 (8주)

**총 기간**: ~16주 (4개월)
**시간 투자**: 주당 10-15시간

---

## 진행 상황 범례

- 📝 **미시작** - 기획 단계
- 🔄 **진행 중** - 적극적으로 작성 중
- ✏️ **초안 완료** - 검토 준비됨
- ✅ **검토 완료** - 내용 확인됨
- 🎯 **최종 완료** - 완성 및 다듬어짐

---

## 스테이지 1: 기초 (1-3주차)

### 모듈 01: SPDK가 존재하는 이유
**파일**: `stage-1-foundation/01-S1-Why-SPDK-Exists.md`
**상태**: ✅ 완료
**소요 시간**: 2시간

**주제**:
- 커널 I/O 병목 문제
- 인터럽트 기반(Interrupt-Driven) vs 폴링 I/O(Polled I/O)
- 컨텍스트 스위칭(Context Switching) 오버헤드
- 유저스페이스 드라이버(Userspace Driver)가 타당한 이유
- SPDK의 성능 장점
- 사용 사례 및 대상 워크로드

---

### 모듈 02: 핵심 원칙
**파일**: `stage-1-foundation/02-S1-Core-Principles.md`
**상태**: ✅ 완료
**소요 시간**: 2시간

**주제**:
- 유저스페이스 드라이버(Userspace Driver)
- 폴링 모드(Polled Mode) 동작
- 제로 카피(Zero-Copy) 데이터 이동
- 락프리 자료 구조(Lock-Free Data Structures)
- 완료까지 실행(Run-to-Completion) 스레딩
- 메시지 패싱(Message Passing) 아키텍처

---

### 모듈 03: 아키텍처 개요
**파일**: `stage-1-foundation/03-S1-Architecture-Overview.md`
**상태**: ✅ 완료
**소요 시간**: 3시간

**주제**:
- SPDK 컴포넌트 계층
- 라이브러리 구성 (`lib/` 구조)
- 모듈 시스템 (`module/` 구조)
- 애플리케이션 아키텍처 (`app/` 구조)
- 환경 추상화(Environment Abstraction, env)
- DPDK 통합
- 공개 API 설계

---

### 모듈 04: 스레딩 모델
**파일**: `stage-1-foundation/04-S1-Threading-Model.md`
**상태**: ✅ 완료
**소요 시간**: 3시간

**주제**:
- SPDK 스레드 vs OS 스레드
- 리액터 패턴(Reactor Pattern)
- 스레드 어피니티(Thread Affinity) 및 CPU 격리
- 폴러(Poller)와 이벤트
- 스레드 간 메시지 패싱(Message Passing)
- 락리스 통신(Lockless Communication)
- 스레드 라이프사이클

---

### 모듈 05: 메모리 관리
**파일**: `stage-1-foundation/05-S1-Memory-Management.md`
**상태**: ✅ 완료
**소요 시간**: 3시간

**주제**:
- 휴지페이지 할당(Hugepage Allocation)
- DMA 안전 메모리(DMA-Safe Memory)
- 메모리 풀(Memory Pool)
- 제로 카피(Zero-Copy) 기법
- 버퍼 관리
- DPDK 메모리 모델
- RDMA용 메모리 등록(Memory Registration)

---

### 모듈 06: 빌드 시스템 및 설정
**파일**: `stage-1-foundation/06-S1-Build-System.md`
**상태**: ✅ 완료
**소요 시간**: 2시간

**주제**:
- Autoconf 기반 빌드 시스템
- 설정 옵션
- 의존성 관리
- 라이브러리 버전 관리
- pkg-config 통합
- SPDK 기반 애플리케이션 빌드

---

### 모듈 07: 코드베이스 탐색
**파일**: `stage-1-foundation/07-S1-Codebase-Navigation.md`
**상태**: ✅ 완료
**소요 시간**: 2시간

**주제**:
- 디렉토리 구조 심층 분석
- API 및 구현체 찾기
- SPDK 코드를 효과적으로 읽기
- 공통 코딩 패턴
- 헤더 구성
- 문서 위치

---

**스테이지 1 총계**: ~17시간

---

## 스테이지 2: 구현 (4-8주차)

### 모듈 10: 환경 설정
**파일**: `stage-2-implementation/10-S2-Environment-Setup.md`
**상태**: ✅ 완료 (1,147줄)
**소요 시간**: 2시간

**주제**:
- 시스템 요구사항
- 의존성 설치
- SPDK 빌드
- 디바이스 설정 및 바인딩
- 휴지페이지 설정
- 테스트 실행
- 개발 환경 설정

---

### 모듈 11: Hello World
**파일**: `stage-2-implementation/11-S2-Hello-World.md`
**상태**: ✅ 완료 (1,107줄)
**소요 시간**: 3시간

**주제**:
- 첫 번째 SPDK 애플리케이션
- 초기화 및 정리
- 이벤트 프레임워크 기초
- 간단한 NVMe 열거
- 디바이스 정보 읽기
- 빌드 및 실행

---

### 모듈 12: 이벤트 프레임워크
**파일**: `stage-2-implementation/12-S2-Event-Framework.md`
**상태**: ✅ 완료 (651줄)
**소요 시간**: 4시간

**주제**:
- spdk_app 프레임워크
- 애플리케이션 시작/종료
- 이벤트 기반 프로그래밍
- 폴러(Poller) (타이머 및 비동기)
- 이벤트 제출
- 서브시스템 초기화
- JSON-RPC 서버 설정

---

### 모듈 13: Bdev 계층
**파일**: `stage-2-implementation/13-S2-Bdev-Layer.md`
**상태**: ✅ 완료 (720줄)
**소요 시간**: 4시간

**주제**:
- Bdev 추상화 개념
- bdev 열기 및 닫기
- I/O 동작 (읽기/쓰기)
- I/O 완료 처리
- Bdev 디스크립터 및 채널
- 에러 처리
- 공통 bdev 동작

---

### 모듈 14: NVMe 드라이버
**파일**: `stage-2-implementation/14-S2-NVMe-Driver.md`
**상태**: ✅ 완료 (538줄)
**소요 시간**: 4시간

**주제**:
- NVMe 드라이버 초기화
- 컨트롤러 및 네임스페이스 관리
- 큐 페어(Queue Pair) 할당
- 제출(Submission) 및 완료(Completion)
- NVMe 커맨드 API
- 에러 복구
- Admin 커맨드

---

### 모듈 15: JSON-RPC 인터페이스
**파일**: `stage-2-implementation/15-S2-JSON-RPC.md`
**상태**: ✅ 완료 (1,570줄)
**소요 시간**: 3시간

**주제**:
- RPC 프레임워크 개요
- RPC 메서드 등록
- 요청/응답 처리
- RPC를 통한 설정
- 관리 동작
- 에러 리포팅
- spdk_rpc.py 사용

---

### 모듈 16: 커스텀 Bdev 모듈
**파일**: `stage-2-implementation/16-S2-Custom-Bdev-Module.md`
**상태**: ✅ 완료 (1,425줄)
**소요 시간**: 5시간

**주제**:
- Bdev 모듈 인터페이스
- 필수 콜백 구현
- I/O 경로 구현
- 모듈 등록
- 설정 파싱
- 외부 모듈로 빌드
- 모듈 테스트

---

### 모듈 17: 애플리케이션 개발 패턴
**파일**: `stage-2-implementation/17-S2-Application-Development.md`
**상태**: ✅ 완료 (1,281줄)
**소요 시간**: 4시간

**주제**:
- 애플리케이션 구조
- 초기화 시퀀스
- 리소스 할당 패턴
- 종료 및 정리
- 에러 처리 전략
- 로깅 및 디버깅
- 설정 관리

---

### 모듈 18: 스레드 관리
**파일**: `stage-2-implementation/18-S2-Thread-Management.md`
**상태**: ✅ 완료 (1,446줄)
**소요 시간**: 3시간

**주제**:
- SPDK 스레드 생성
- 스레드 스케줄링
- 스레드 간 메시징
- 스레드 로컬 스토리지(Thread-Local Storage)
- CPU 코어 할당
- 스레드 라이프사이클 관리

---

### 모듈 19: 비동기 I/O 패턴
**파일**: `stage-2-implementation/19-S2-Async-IO-Patterns.md`
**상태**: ✅ 완료 (1,122줄)
**소요 시간**: 3시간

**주제**:
- 비동기 I/O 모델
- 완료 콜백(Completion Callback)
- 동작 체이닝
- 요청 배칭
- 에러 전파
- 비동기 컨텍스트에서의 리소스 정리

---

**스테이지 2 총계**: ~35시간

---

## 스테이지 3: 마스터리 (9-16주차)

### 모듈 20: NVMe-oF 타겟 아키텍처
**파일**: `stage-3-mastery/20-S3-NVMe-oF-Target-Architecture.md`
**상태**: ✅ 완료 (1,485줄)
**소요 시간**: 5시간

**주제**:
- NVMe-oF 프로토콜 개요
- 타겟 서브시스템 아키텍처
- 트랜스포트 추상화(Transport Abstraction)
- RDMA 트랜스포트 내부 구조
- TCP 트랜스포트 내부 구조
- 연결 관리
- 네임스페이스 공유

---

### 모듈 21: Vhost 타겟
**파일**: `stage-3-mastery/21-S3-Vhost-Target.md`
**상태**: ✅ 완료 (1,332줄)
**소요 시간**: 4시간

**주제**:
- Vhost 프로토콜
- Virtio 큐 관리
- Vhost-user vs vhost-kernel
- QEMU 통합
- SPDK vhost-blk 타겟
- SPDK vhost-scsi 타겟
- 성능 고려사항

---

### 모듈 22: iSCSI 타겟
**파일**: `stage-3-mastery/22-S3-iSCSI-Target.md`
**상태**: ✅ 완료 (1,337줄)
**소요 시간**: 4시간

**주제**:
- iSCSI 프로토콜 기초
- 타겟/LUN 아키텍처
- 연결 처리
- 인증
- 디스커버리 서비스
- PDU 처리
- 성능 튜닝

---

### 모듈 23: 성능 최적화
**파일**: `stage-3-mastery/23-S3-Performance-Optimization.md`
**상태**: ✅ 완료 (1,431줄)
**소요 시간**: 5시간

**주제**:
- SPDK 애플리케이션 프로파일링
- CPU 어피니티(Affinity) 튜닝
- 메모리 접근 패턴
- 캐시 최적화
- 락 경합(Lock Contention) 분석
- I/O 깊이 튜닝
- 배치 처리 전략
- 하드웨어 큐 설정

---

### 모듈 24: 고급 스레딩 패턴
**파일**: `stage-3-mastery/24-S3-Advanced-Threading-Patterns.md`
**상태**: ✅ 완료 (1,340줄)
**소요 시간**: 4시간

**주제**:
- 멀티 리액터(Multi-Reactor) 패턴
- 부하 분산(Load Balancing)
- 스레드 조정
- 락프리 알고리즘(Lock-Free Algorithm)
- 메모리 배리어(Memory Barrier)
- 인터럽트 모드
- 동적 스레드 생성

---

### 모듈 25: 메모리 풀 내부 구조
**파일**: `stage-3-mastery/25-S3-Memory-Pool-Internals.md`
**상태**: ✅ 완료 (1,155줄)
**소요 시간**: 3시간

**주제**:
- 메모리 풀 구현
- 링 버퍼(Ring Buffer) 구조
- 캐시 정렬 할당(Cache-Aligned Allocation)
- 풀 크기 조정 전략
- 프래그먼테이션(Fragmentation) 방지
- 풀 통계
- 커스텀 할당자

---

### 모듈 26: DPDK 통합 심층 분석
**파일**: `stage-3-mastery/26-S3-DPDK-Integration-Deep-Dive.md`
**상태**: ✅ 완료 (1,412줄)
**소요 시간**: 4시간

**주제**:
- DPDK 아키텍처 개요
- EAL 초기화
- Mempool 사용
- 링 버퍼(Ring Buffer)
- NIC 드라이버
- 크립토 디바이스
- 커스텀 DPDK 통합

---

### 모듈 27: 디버깅 기법
**파일**: `stage-3-mastery/27-S3-Debugging-Techniques.md`
**상태**: ✅ 완료 (1,279줄)
**소요 시간**: 4시간

**주제**:
- SPDK에서의 GDB 사용
- 트레이스 포인트(Trace Point)
- 로그 레벨 및 필터링
- 코어 덤프 분석
- 메모리 누수 탐지
- 레이스 컨디션(Race Condition) 디버깅
- 성능 회귀(Regression) 분석

---

### 모듈 28: 테스트 전략
**파일**: `stage-3-mastery/28-S3-Testing-Strategies.md`
**상태**: ✅ 완료 (1,351줄)
**소요 시간**: 3시간

**주제**:
- CUnit을 이용한 단위 테스트
- 목 객체(Mock Object)
- 통합 테스트
- 퍼즈 테스트(Fuzz Testing)
- 성능 테스트
- CI/CD 통합
- 테스트 커버리지

---

### 모듈 29: 고급 Bdev 주제
**파일**: `stage-3-mastery/29-S3-Advanced-Bdev-Topics.md`
**상태**: ✅ 완료 (1,292줄)
**소요 시간**: 4시간

**주제**:
- Bdev 계층화(Layering)
- 클레임(Claim)과 의존성
- RAID 모듈 내부 구조
- Crypto bdev
- Compress bdev
- 씬 프로비저닝(Thin Provisioning, lvol)
- 서비스 품질(QoS)

---

### 모듈 30: SPDK에 기여하기
**파일**: `stage-3-mastery/30-S3-Contributing-to-SPDK.md`
**상태**: ✅ 완료 (903줄)
**소요 시간**: 3시간

**주제**:
- 개발 워크플로
- 코딩 표준
- 패치 제출 프로세스
- 코드 리뷰 기대사항
- 지속적 통합(CI)
- 문서화 요구사항
- 커뮤니티 가이드라인

---

### 모듈 31: 프로덕션 배포
**파일**: `stage-3-mastery/31-S3-Production-Deployment.md`
**상태**: ✅ 완료 (1,560줄)
**소요 시간**: 4시간

**주제**:
- 시스템 요구사항
- 용량 계획
- 고가용성(High Availability) 설정
- 모니터링 및 알림
- 업데이트 절차
- 보안 고려사항
- 문제 해결 가이드

---

### 모듈 32: 고급 사용 사례
**파일**: `stage-3-mastery/32-S3-Advanced-Use-Cases.md`
**상태**: ✅ 완료 (1,447줄)
**소요 시간**: 3시간

**주제**:
- 스토리지 어플라이언스(Appliance) 구축
- 클라우드 스토리지 백엔드
- 데이터베이스 가속
- 컨테이너 통합
- Kubernetes CSI 플러그인
- 오브젝트 스토리지 백엔드
- 실제 아키텍처 사례

---

**스테이지 3 총계**: ~50시간

---

## 실습 과제

### 실습 01: Hello Bdev
**파일**: `exercises/EX01-Hello-Bdev.md`
**상태**: ✅ 완료 (471줄)
**관련**: 모듈 13

모든 bdev를 열거하고 속성을 출력하는 간단한 애플리케이션을 구축합니다.

---

### 실습 02: 커스텀 폴러
**파일**: `exercises/EX02-Custom-Poller.md`
**상태**: ✅ 완료 (477줄)
**관련**: 모듈 12

시스템 통계를 모니터링하는 주기적 폴러를 생성합니다.

---

### 실습 03: 간단한 I/O 애플리케이션
**파일**: `exercises/EX03-Simple-IO-App.md`
**상태**: ✅ 완료 (770줄)
**관련**: 모듈 13, 14

NVMe 디바이스에 순차 및 랜덤 I/O를 수행하는 애플리케이션을 구축합니다.

---

### 실습 04: Null Bdev 모듈
**파일**: `exercises/EX04-Null-Bdev.md`
**상태**: ✅ 완료 (1,038줄)
**관련**: 모듈 16

쓰기를 무시하고 읽기 시 0을 반환하는 간단한 null bdev를 구현합니다.

---

### 실습 05: RPC 관리
**파일**: `exercises/EX05-RPC-Management.md`
**상태**: ✅ 완료 (846줄)
**관련**: 모듈 15

애플리케이션 특화 설정을 관리하기 위한 커스텀 RPC 메서드를 추가합니다.

---

### 실습 06: 멀티 스레드 I/O
**파일**: `exercises/EX06-Multi-Thread-IO.md`
**상태**: ✅ 완료 (758줄)
**관련**: 모듈 18, 19

스레드 간 조율된 I/O를 수행하는 멀티 스레드 애플리케이션을 구축합니다.

---

### 실습 07: 성능 벤치마크
**파일**: `exercises/EX07-Performance-Benchmark.md`
**상태**: ✅ 완료 (843줄)
**관련**: 모듈 23

I/O 성능을 측정하고 최적화하는 벤치마크 도구를 만듭니다.

---

### 실습 08: 에러 주입
**파일**: `exercises/EX08-Error-Injection.md`
**상태**: ✅ 완료 (831줄)
**관련**: 모듈 16

테스트를 위해 커스텀 bdev 모듈에 에러 주입을 구현합니다.

---

### 실습 09: NVMe-oF 이니시에이터
**파일**: `exercises/EX09-NVMf-Initiator.md`
**상태**: ✅ 완료 (951줄)
**관련**: 모듈 20

원격 타겟에 연결하는 간단한 NVMe-oF 이니시에이터를 구축합니다.

---

### 실습 10: 커스텀 타겟 애플리케이션
**파일**: `exercises/EX10-Custom-Target.md`
**상태**: ✅ 완료 (1,058줄)
**관련**: 모듈 20, 21, 22

특정 비즈니스 로직을 갖춘 커스텀 스토리지 타겟을 개발합니다.

---

## 참고 자료

### API 빠른 참조
**파일**: `reference/REF-API-Quick-Reference.md`
**상태**: ✅ 완료 (1,594줄)

자주 사용하는 SPDK API와 사용법에 대한 빠른 조회.

---

### 공통 패턴 치트시트
**파일**: `reference/REF-Common-Patterns.md`
**상태**: ✅ 완료 (1,477줄)

자주 사용되는 코드 패턴과 관용구 모음.

---

### 디버깅 치트시트
**파일**: `reference/REF-Debugging-Cheatsheet.md`
**상태**: ✅ 완료 (797줄)

디버깅 도구 및 기법에 대한 빠른 참조.

---

### 성능 튜닝 가이드
**파일**: `reference/REF-Performance-Tuning.md`
**상태**: ✅ 완료 (792줄)

SPDK 애플리케이션 최적화를 위한 체계적 체크리스트.

---

### 용어집
**파일**: `reference/REF-Glossary.md`
**상태**: ✅ 완료 (833줄)

SPDK 용어 및 약어의 완전한 용어집.

---

### 에러 코드 참조
**파일**: `reference/REF-Error-Codes.md`
**상태**: ✅ 완료 (687줄)

자주 발생하는 에러 코드와 그 의미.

---

### 설정 참조
**파일**: `reference/REF-Configuration.md`
**상태**: ✅ 완료 (1,340줄)

SPDK 설정 옵션에 대한 완전한 참조.

---

## 진행 요약

### 전체 진행률

**총 항목**: 49
**완료**: 49 (100%)
**진행 중**: 0 (0%)
**남은 항목**: 0 (0%)

### 스테이지별 진행

| 스테이지 | 총계 | 완료 | 작성 분량 |
|----------|------|------|----------|
| 스테이지 1 | 7 | 7 | ~4,500줄 |
| 스테이지 2 | 10 | 10 | ~11,007줄 |
| 스테이지 3 | 13 | 13 | ~16,924줄 |
| 실습 과제 | 10 | 10 | ~8,043줄 |
| 참고 자료 | 7 | 7 | ~7,520줄 |
| **총계** | **47** | **47** | **~47,994줄** |

---

## 개정 이력

| 날짜 | 버전 | 변경사항 |
|------|------|----------|
| 2026-01-30 | 1.0 | 초기 코스 구조 생성 |
| 2026-03-30 | 2.0 | 모든 모듈, 실습 과제, 참고 자료 완료 |

---

*최종 업데이트: 2026-03-30*

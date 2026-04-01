# 모듈 30: SPDK에 기여하기(Contributing to SPDK)

**스테이지 3 - 마스터리** | 선수 과목: 모듈 1-29, 탄탄한 C 프로그래밍 경험, git 사용 경험

---

## 개요

SPDK에 기여하는 것은 전문성을 심화하는 가장 효과적인 방법 중 하나입니다. 이 프로젝트는 Linux Foundation을 통해 운영되며, 소수의 핵심 메인테이너(Core Maintainer) 팀이 검토를 수행하고, 잘 정의된 패치 제출 프로세스를 따릅니다. 이 모듈에서는 거버넌스 구조, 개발 환경 설정, 코딩 표준, 엔드투엔드 패치 워크플로우, CI 기대 사항, 문서화 요구 사항, 코드 리뷰 예절, 그리고 메인테이너가 되는 경로까지 알아야 할 모든 것을 다룹니다.

---

## 1. 프로젝트 거버넌스(Project Governance)

### 1.1 핵심 메인테이너

SPDK에는 일상적인 기술 감독을 수행하는 소규모 핵심 메인테이너 팀이 있습니다. 2024-2025년 기준 팀 구성원은 다음과 같습니다:

| 메인테이너 | 소속 |
|---|---|
| Jim Harris | NVIDIA |
| Jacek Kalwas | Intel/Solidigm |
| Mateusz Kozlowski | Intel/Solidigm |
| Changpeng Liu | Intel |
| Alexey Marchuk | NVIDIA |
| Shuhei Matsumoto | Fujitsu |
| Konrad Sztyber | Intel |
| Ben Walker | Intel/Solidigm |
| Tomek Zawadzki | Nutanix |

핵심 메인테이너의 책임:

- 패치 검토 및 승인
- 코드 리뷰 및 개발 가이드라인 설정
- 커뮤니티 프로세스에 대한 의사 결정
- 모범적인 개발 관행 시범
- 긍정적이고 생산적인 커뮤니티 조성
- 프로젝트 로드맵 정의 참여
- 개발 작업 식별 및 조직

### 1.2 기술 운영 위원회(Technical Steering Committee, TSC)

SPDK에는 전체적인 기술 감독을 위해 Linux Foundation을 통해 설립된 기술 운영 위원회(TSC)가 있습니다. TSC 투표 위원은 다음으로 구성됩니다:

- 위에 나열된 모든 핵심 메인테이너
- 각 참여 조직이 지명한 대표 1명

**참여 조직**: ARM, Dell, HPE, Nutanix, NVIDIA, Samsung, Solidigm, Starwind, Tencent

TSC 연락처: Tomek Zawadzki (tomasz.zawadzki@nutanix.com), Jim Harris (jim.harris@nvidia.com)

### 1.3 의사 결정

TSC는 상위 수준의 프로젝트 방향과 정책을 담당합니다. 패치 수락 여부, API 설계 방식, 릴리스에 포함될 기능 등 일상적인 기술적 결정은 핵심 메인테이너 팀이 패치 리뷰 프로세스를 통해 처리합니다.

---

## 2. 행동 강령(Code of Conduct)

SPDK는 Contributor Covenant 행동 강령(버전 2.1)을 사용합니다. 커뮤니티 참여 시 이 표준을 준수해야 합니다.

### 기대되는 긍정적 행동

- 다른 사람에 대한 공감과 친절함 보이기
- 다른 의견, 관점 및 경험 존중하기
- 건설적인 피드백 주고받기
- 실수에 대한 책임을 지고 영향받은 사람들에게 사과하며 경험에서 배우기
- 개인뿐만 아니라 전체 커뮤니티에 가장 좋은 것에 집중하기

### 용납할 수 없는 행동

- 성적인 언어나 이미지, 그리고 어떤 종류의 성적 관심이나 접근
- 트롤링, 모욕적이거나 비하하는 댓글, 개인적 또는 정치적 공격
- 공적 또는 사적 괴롭힘
- 명시적 허가 없이 다른 사람의 개인 정보(물리적 또는 이메일 주소) 공개
- 전문적 환경에서 부적절하다고 합리적으로 간주될 수 있는 기타 행위

### 집행

위반 사항은 SPDK 핵심 메인테이너에게 비공개로 보고할 수 있습니다. 메인테이너는 모든 불만 사항을 신속하고 공정하게 검토하고 조사합니다. 결과는 위반의 심각성과 패턴에 따라 비공개 서면 경고, 임시 차단, 영구 차단까지 다양합니다.

**핵심 사항**: 행동 강령은 모든 커뮤니티 공간(메일링 리스트, GitHub, IRC/Matrix, 컨퍼런스)과 개인이 공공 장소에서 공식적으로 커뮤니티를 대표할 때 적용됩니다.

---

## 3. 기여를 위한 개발 환경 설정

### 3.1 사전 요구 사항

```bash
# 핵심 빌드 도구
sudo apt-get install -y build-essential git python3 python3-pip meson ninja-build

# 포맷 검사 도구 (버전이 중요함)
sudo apt-get install -y astyle    # 버전 3.0.1 - 3.1 필요
pip3 install ruff mypy            # Python 스타일 및 타입 검사

# 셸 포맷 검사기
# shfmt v3.8.0이 지원되는 버전 - pkgdep.sh를 통해 설치
./scripts/pkgdep.sh -d

# 정적 분석용 (선택 사항이지만 유용함)
sudo apt-get install -y clang-tools  # scan-build 제공
```

### 3.2 초기 저장소 설정

```bash
# 저장소 복제
git clone https://github.com/spdk/spdk.git
cd spdk

# 모든 서브모듈 초기화 (필수 - SPDK는 많은 서브모듈을 사용)
git submodule update --init

# 변경하기 전에 빌드가 정상 작동하는지 확인
./configure
make -j$(nproc)

# 포맷 검사기를 실행하여 기준선 확인
./scripts/check_format.sh
```

### 3.3 기여를 위한 Git 설정

SPDK는 모든 커밋에 `Signed-off-by` 트레일러가 필요합니다(Developer Certificate of Origin). `git commit -s`가 올바른 트레일러를 자동으로 생성하도록 신원 정보를 설정하세요:

```bash
git config --global user.name  "Your Full Name"
git config --global user.email "your.email@example.com"

# SPDK 워크플로우에 유용한 별칭
git config --global alias.logone "log --oneline"
git config --global alias.fixup  "commit --fixup"
```

### 3.4 업스트림과 동기화 유지

```bash
# 포크에서 복제한 경우 업스트림 리모트 추가
git remote add upstream https://github.com/spdk/spdk.git

# 최신 변경 사항 가져오기
git fetch upstream

# master 브랜치를 최신 상태로 유지
git checkout master
git merge upstream/master
```

---

## 4. 코딩 표준 및 스타일 가이드

SPDK는 `scripts/check_format.sh`를 통해 코딩 스타일을 자동으로 적용합니다. 스크립트가 무엇을 검사하는지 이해하면 사후 반복 작업 대신 처음부터 규격에 맞는 코드를 작성할 수 있습니다.

### 4.1 C/C++ 스타일 (astyle)

SPDK는 저장소 루트의 `.astylerc`에 저장된 특정 설정으로 **astyle**을 사용합니다:

| 규칙 | 값 |
|---|---|
| 괄호 스타일 | K&R |
| 들여쓰기 | 탭 (force-tab=8) |
| 줄 길이 제한 | 100자 |
| 포인터 정렬 | `*`를 변수명 옆에 (`int *ptr`) |
| 연산자 패딩 | 연산자 주위에 공백 |
| 키워드 패딩 | `if`/`while`/`for`와 `(` 사이에 공백 |
| 괄호 패딩 | 괄호 안에 추가 공백 없음 |
| 중괄호 | 한 줄 조건문에 중괄호 추가 |
| 줄 끝 | LF만 (Linux) |

```c
/* 올바른 예 - K&R 괄호, 탭, 연산자 주위 공백 */
static int
my_function(struct spdk_bdev *bdev, uint64_t offset)
{
	if (bdev == NULL) {
		return -EINVAL;
	}

	bdev->offset = offset + 1;
	return 0;
}

/* 잘못된 예 - allman 괄호, 공백으로 들여쓰기, 연산자 주위 공백 없음 */
static int my_function(struct spdk_bdev* bdev, uint64_t offset){
    if(bdev==NULL){
        return -EINVAL;
    }
    bdev->offset=offset+1;
    return 0;
}
```

**C 스타일 검사 실행**:

```bash
# git이 추적하는 모든 C/C++ 파일 검사
./scripts/check_format.sh

# astyle 버전은 3.0.1과 3.1 사이여야 함
astyle --version

# 제자리 자동 수정 (주의: 파일을 수정함)
astyle --options=.astylerc --break-return-type --attach-return-type-decl <file.c>
```

### 4.2 Python 스타일 (ruff + mypy)

Python 코드는 `python/pyproject.toml`의 설정을 사용하여 `ruff`와 `mypy`로 검사됩니다:

```bash
# Python 스타일 검사 실행
ruff --config python/pyproject.toml check python/

# 타입 검사 실행
mypy --config-file python/pyproject.toml python/
```

주요 규칙:
- PEP 8 준수
- 새로운 Python 코드에 타입 어노테이션 필수
- 와일드카드 임포트 금지 (`from module import *`)

### 4.3 셸 스크립트 스타일 (shfmt)

셸 스크립트는 정확히 **v3.8.0** 버전의 `shfmt`로 포맷됩니다(다른 버전은 허용되지 않음):

```bash
# 지원되는 shfmt 버전 설치
./scripts/pkgdep.sh -d

# 검사는 check_format.sh 내부에서 자동으로 실행됨
# 사용되는 설정:
#   -i 0    탭으로 들여쓰기
#   -bn     이항 연산자가 다음 줄 시작
#   -ci     switch/case 들여쓰기
#   -ln bash  bash 변형
#   -sr     리다이렉트 연산자 뒤에 공백
```

### 4.4 SPDK의 일반적인 C 관습

astyle이 강제하는 것 이외에도 SPDK는 Linux 커널 스타일과 프로젝트 자체 관행에서 파생된 강력한 관습이 있습니다:

**오류 처리**:
```c
/* 오류에 대해 음수 errno 값 사용 */
if (rc != 0) {
    SPDK_ERRLOG("Operation failed: %s\n", spdk_strerror(-rc));
    return rc;
}

/* 포인터를 즉시 확인 */
if (ctx == NULL) {
    return -ENOMEM;
}
```

**명명 규칙**:
```c
/* 공개 API 함수: spdk_<module>_<verb>_<noun> */
int spdk_bdev_read(struct spdk_bdev_desc *desc, ...);

/* 내부 함수: <module>_<verb>_<noun> (spdk_ 접두사 없음) */
static int bdev_read_internal(struct spdk_bdev *bdev, ...);

/* 구조체: 공개는 spdk_<module>_<noun>, 내부는 <module>_<noun> */
struct spdk_bdev_io;      /* 공개 */
struct bdev_io_channel;   /* 내부 */

/* 매크로 및 상수: SPDK_<MODULE>_<NAME> */
#define SPDK_BDEV_LARGE_BUF_MAX_SIZE (64 * 1024)
```

**메모리 관리**:
```c
/* SPDK 관리 메모리에는 spdk_ 할당자 사용 */
buf = spdk_malloc(size, alignment, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
if (buf == NULL) {
    return -ENOMEM;
}

/* 대응하는 해제 */
spdk_free(buf);
```

**문서화 주석**:
```c
/**
 * Brief one-line description.
 *
 * Longer description if needed. Explain the "why" not just the "what".
 *
 * \param desc Bdev descriptor opened with spdk_bdev_open_ext().
 * \param buf  DMA-safe buffer allocated with spdk_malloc().
 * \param nbytes Number of bytes to read. Must be a multiple of block size.
 * \param cb Completion callback. Called on the same thread that issued the I/O.
 * \param cb_arg Opaque argument passed to \p cb.
 *
 * \return 0 on success, negative errno on failure. -ENOMEM if no resources.
 */
int spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
                   void *buf, uint64_t offset, uint64_t nbytes,
                   spdk_bdev_io_completion_cb cb, void *cb_arg);
```

모든 공개 API 함수(`include/spdk/`에 선언된 함수)에는 Doxygen 주석이 있어야 합니다.

### 4.5 헤더 파일 관습

```c
/* 모든 헤더에는 파일 이름을 사용한 인클루드 가드가 필요 */
#ifndef SPDK_BDEV_H
#define SPDK_BDEV_H

/* SPDK 공통 타입을 먼저 포함 */
#include "spdk/stdinc.h"

/* ... 선언 ... */

#ifdef __cplusplus
extern "C" {
#endif

/* ... C-linkage 선언 ... */

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_H */
```

---

## 5. SPDK 개발 워크플로우

SPDK는 **GitHub 풀 리퀘스트(Pull Request)**를 주요 코드 리뷰 메커니즘으로 사용하며, 각 PR에서 CI 시스템이 자동으로 실행됩니다.

### 5.1 단계별 패치 제출

**1단계: 집중된 브랜치 생성**

```bash
# 항상 최신 master에서 브랜치 생성
git fetch upstream
git checkout -b <module>/<short-description> upstream/master

# 좋은 브랜치 이름 예시:
#   bdev/add-uring-passthrough
#   nvmf/fix-tcp-reconnect-timeout
#   doc/update-bdev-api-guide
```

**2단계: 변경 사항 작성**

- 각 커밋을 하나의 논리적 변경에 집중
- 리팩토링과 버그 수정 또는 새 기능을 혼합하지 않기
- 구현과 테스트를 함께 작성

**3단계: 단위 테스트 작성**

```bash
# 단위 테스트는 test/unit/ 아래에 위치
# 모듈의 테스트 파일 찾기:
ls test/unit/lib/<module>/

# 단위 테스트를 실행하여 확인
./test/unit/unittest.sh

# 관련 테스트 스위트만 실행
./test/unit/unittest.sh <module>
```

**4단계: 포맷 검사기 실행**

```bash
# 제출 전에 오류 없이 통과해야 함
./scripts/check_format.sh

# astyle이 포맷 문제를 발견하면 보고합니다.
# 수동으로 수정하거나 astyle이 재포맷하도록 하세요 (차이점을 신중하게 검토).
git diff  # 포맷 변경 사항 검토
```

**5단계: 정적 분석 실행 (권장)**

```bash
# Clang 정적 분석기
scan-build make

# Address Sanitizer 빌드
./configure --enable-asan --enable-ubsan
make -j$(nproc)
```

**6단계: 좋은 커밋 메시지 작성** (섹션 6 참조)

```bash
git add <files>
git commit -s   # -s는 Signed-off-by를 자동으로 추가
```

**7단계: 제출 전 최신 master에 리베이스**

```bash
git fetch upstream
git rebase upstream/master

# 충돌 해결 후:
git rebase --continue
```

**8단계: 푸시 및 풀 리퀘스트 열기**

```bash
git push origin <your-branch>
# 그런 다음 https://github.com/spdk/spdk에서 PR 열기
```

**9단계: 리뷰 피드백에 응답**

```bash
# 리뷰어가 요청한 변경 사항 적용
git add <files>
git commit --fixup HEAD   # 또는 대화형 리베이스로 스쿼시

# 최종 병합 전 히스토리 정리
git rebase -i upstream/master  # fixup 커밋 스쿼시

# PR 브랜치를 업데이트하기 위해 강제 푸시
git push --force-with-lease origin <your-branch>
```

### 5.2 제출 후 진행 과정

1. CI가 자동으로 실행됩니다(빌드, 단위 테스트, 스타일 검사, 정적 분석)
2. 핵심 메인테이너 또는 커뮤니티 구성원이 코드를 검토합니다
3. 리뷰 코멘트가 GitHub에 게시됩니다 - 응답하고 PR을 업데이트합니다
4. 최소 한 명의 핵심 메인테이너가 승인하고 CI가 통과하면, 메인테이너가 병합합니다
5. 패치는 자신이 작성한 것을 자신이 병합해서는 안 됩니다

---

## 6. SPDK용 좋은 커밋 메시지 작성

SPDK 커밋 메시지는 엄격한 형식을 따릅니다. 이것을 제대로 하는 것이 중요합니다 - 프로젝트 히스토리는 주요 문서화 형태입니다.

### 6.1 형식

```
<module>: <72자 미만의 짧은 명령형 요약>

<빈 줄>

<본문: 무엇이 변경되었고 왜 변경되었는지 설명. 72자에서 줄 바꿈.
어떻게 변경했는지는 설명하지 마세요 — 그것은 코드의 역할입니다.
버그 수정의 경우, 버그와 발생 조건을 설명합니다. 기능 추가의 경우,
동기를 설명합니다.>

<빈 줄>

Signed-off-by: Your Name <your.email@example.com>
```

### 6.2 모듈 접두사 규칙

접두사는 변경이 속하는 서브시스템을 식별합니다. 소문자를 사용하고 구체적으로 작성하세요:

| 접두사 | 범위 |
|---|---|
| `bdev` | 블록 디바이스 레이어 |
| `nvme` | NVMe 드라이버 |
| `nvmf` | NVMe-oF 타겟 |
| `bdev/nvme` | NVMe bdev 모듈 한정 |
| `lib/env_dpdk` | DPDK 환경 라이브러리 |
| `test/bdev` | Bdev 테스트 |
| `doc` | 문서만 |
| `scripts` | 빌드 또는 유틸리티 스크립트 |
| `ci` | CI 설정 |
| `json` | JSON 파싱 라이브러리 |

### 6.3 좋은 커밋 메시지 vs 나쁜 커밋 메시지

```
# 좋은 예: 구체적 모듈, 명령형 동사, 본문에서 이유 설명
bdev/nvme: handle timeout during controller reset

When an NVMe controller reset is in progress, I/O commands submitted
by other threads can time out before the reset completes. Previously
these timeouts triggered an additional reset attempt, leading to a
reset storm. Fix this by checking the controller state in the timeout
handler and deferring the response until the reset completes.

Signed-off-by: Your Name <your@email.com>

# 나쁜 예: 모호함, 모듈 없음, 설명 없음
fix: fixed a bug in nvme code

This fixes the timeout issue.

Signed-off-by: Your Name <your@email.com>
```

### 6.4 개발자 인증서(Developer Certificate of Origin, DCO)

모든 커밋에는 git 신원 정보와 일치하는 이름과 이메일이 포함된 `Signed-off-by` 줄이 있어야 합니다. 이것은 개발자 인증서(DCO)입니다 - 코드를 직접 작성했거나 프로젝트의 오픈 소스 라이선스(BSD-3-Clause)에 따라 제출할 권리가 있음을 증명합니다.

```bash
# 매 커밋마다 자동으로 추가
git commit -s

# 깜빡한 경우, 마지막 커밋에 추가
git commit --amend -s

# 여러 커밋의 경우
git rebase --signoff HEAD~<N>
```

---

## 7. 지속적 통합(Continuous Integration) 시스템

SPDK의 CI는 모든 풀 리퀘스트에서 실행됩니다. 패치가 CI를 깨뜨려서는 안 됩니다. CI가 무엇을 검사하는지 이해하면 푸시하기 전에 로컬에서 실패를 잡을 수 있습니다.

### 7.1 CI 검사 항목

| 검사 | 로컬 동등 명령 |
|---|---|
| 포맷 검사 | `./scripts/check_format.sh` |
| 빌드 (GCC) | `./configure && make -j$(nproc)` |
| 빌드 (Clang) | `CC=clang ./configure && make -j$(nproc)` |
| 단위 테스트 | `./test/unit/unittest.sh` |
| 정적 분석 | `scan-build make` |
| Python 스타일 | `ruff check && mypy` |
| 셸 스타일 | `shfmt -d scripts/` |
| 자동 테스트 (기능) | `./test/autotest.sh` (하드웨어 필요) |

### 7.2 로컬에서 CI 검사 실행

```bash
# 제출 전 가장 중요한 검사
./scripts/check_format.sh

# 단위 테스트 스위트 (빠르게 실행, 하드웨어 불필요)
./test/unit/unittest.sh

# 전체 빌드 검증
./configure && make -j$(nproc) 2>&1 | tee build.log

# Python 검사 (Python 코드를 수정한 경우)
ruff --config python/pyproject.toml check python/
mypy --config-file python/pyproject.toml python/

# 일반적인 문제 확인 (정의되지 않은 심볼, 누락된 헤더)
./configure --enable-werror
make -j$(nproc)
```

### 7.3 CI 실패 이해하기

CI가 실패하면 첫 번째 단계는 로컬에서 실패를 재현하는 것입니다. 일반적인 문제들:

**포맷 실패**: `./scripts/check_format.sh`를 로컬에서 실행하고 보고된 줄을 수정합니다.

**`-Werror` 빌드 실패**: SPDK는 CI에서 경고를 오류로 처리합니다. 로컬에서 꺼져 있는 경고뿐만 아니라 모든 경고를 수정하세요.

**단위 테스트 실패**: `./test/unit/unittest.sh`를 실행하고 출력을 주의 깊게 확인합니다. 단위 테스트 실패는 항상 회귀(regression)입니다 - 프로덕션 코드를 수정하고, 테스트를 통과하도록 수정하지 마세요.

**Python 타입 오류**: mypy 타입 오류는 실제 문제를 나타냅니다. 적절한 타입 어노테이션을 추가하거나 로직을 수정하세요.

---

## 8. 문서화 요구 사항

SPDK는 문서화를 중요하게 여깁니다. 적절한 문서화가 없는 코드는 수락되지 않습니다.

### 8.1 문서화가 필요한 것

- `include/spdk/`의 모든 공개 API 함수 (Doxygen 주석, 섹션 4.4 참조)
- 새 모듈은 `doc/<module>.md` 파일이 있어야 함
- 새 RPC는 `doc/jsonrpc.md`에 문서화되어야 함
- 중요한 동작 변경은 기존 문서 업데이트 필요
- 새 기능은 `CHANGELOG.md`에 항목을 포함해야 함

### 8.2 CHANGELOG.md 형식

```markdown
## 25.01

### New Features

- **bdev**: Added `spdk_bdev_get_qd` to query current queue depth. See
  `include/spdk/bdev.h` for the API.

### Bug Fixes

- **nvmf/tcp**: Fixed connection teardown when the initiator closes the TCP
  connection before all in-flight I/Os complete.
```

### 8.3 문서 빌드

```bash
# Doxygen 설치
sudo apt-get install -y doxygen graphviz

# HTML 문서 빌드
make doc

# 브라우저에서 열기
xdg-open doc/output/html/index.html
```

### 8.4 JSON-RPC 문서화

패치가 RPC 메서드를 추가하거나 수정하는 경우, `doc/jsonrpc.md`를 다음 내용으로 업데이트하세요:
- 메서드 이름 및 설명
- 파라미터 (이름, 타입, 필수/선택, 설명)
- 응답 필드
- 요청 및 응답 예시

```markdown
### bdev_example_create {#rpc_bdev_example_create}

Create an Example bdev.

#### Parameters

Name               | Optional | Type   | Description
------------------ | -------- | ------ | -----------
name               | Required | string | Bdev name
size_in_mb         | Required | number | Size in MiB

#### Example

Example request:

~~~json
{
  "jsonrpc": "2.0",
  "method": "bdev_example_create",
  "id": 1,
  "params": {
    "name": "Example0",
    "size_in_mb": 64
  }
}
~~~
```

---

## 9. 코드 리뷰 기대 사항 및 예절

### 9.1 패치 작성자로서

**제출 전**:
- 엣지 케이스를 포함하여 패치를 철저히 테스트하세요
- 처음 보는 리뷰어의 입장에서 자신의 diff를 읽어보세요
- CI가 로컬에서 통과하는지 확인하세요 (`check_format.sh`, 단위 테스트, 빌드)
- 패치 시리즈를 작고 집중적으로 유지하세요 - 커밋당 하나의 논리적 변경

**리뷰 중**:
- 모든 리뷰 코멘트에 응답하세요, 확인만 하는 경우에도
- 설명 없이 피드백에 반대하지 마세요 - 존중하며 이유를 설명하세요
- 동의하지 않는 경우, 건설적으로 말하세요: "그 접근 방식을 고려했지만, Y 때문에 X 문제가 있습니다"
- 패치를 업데이트할 때, PR 설명이나 코멘트에 변경 사항을 요약하세요
- 리뷰어가 변경 사항에 만족할 때까지 커밋을 스쿼시하지 마세요 (재리뷰를 어렵게 만듦)

**예상할 것**:
- 리뷰에 메인테이너 대역폭에 따라 며칠에서 몇 주가 걸릴 수 있습니다
- 특히 크거나 복잡한 변경의 경우 여러 라운드의 피드백이 정상입니다
- "수정 필요" 결과는 거절이 아닙니다 - 메인테이너가 가치를 보지만 변경을 요구한다는 의미입니다

### 9.2 리뷰어로서

**기술적 피드백**:
- 하드웨어가 있다면 패치를 테스트하세요
- 스타일뿐만 아니라 정확성을 확인하세요
- 주의할 점: off-by-one 오류, 잠금 순서 문제, 메모리 누수, 누락된 오류 경로, 스레드 안전성
- 혼란스러운 부분이 있다면 말하세요 - 혼란스러운 코드는 설계 문제를 나타낼 수 있습니다

**커뮤니케이션**:
- 구체적으로 작성하세요. "이것은 잘못되었습니다"는 유용하지 않습니다. "Y 때문에 X일 때 실패합니다 - Z를 고려해 보세요"가 유용합니다.
- 차단 이슈와 제안을 구분하세요: 비차단 스타일 제안에는 "nit:" 접두사를 사용하세요
- 존중하세요. 작성자가 패치에 노력을 기울였습니다. 사람이 아닌 코드를 비평하세요.
- 패치가 좋으면 그렇게 말하세요. 긍정적 피드백은 가치 있고 종종 간과됩니다.

**리뷰 코멘트 패턴 예시**:
```
# 차단 이슈 (반드시 수정해야 함)
This can deadlock if spdk_thread_send_msg() fails and the callback
is never called. Need to handle the -ENOMEM return value.

# 비차단 제안
nit: Consider renaming `tmp` to `io_channel` for clarity.

# 긍정적 강화
The batching approach here is clever — good use of the ring buffer
to amortize submission overhead.
```

### 9.3 커뮤니티 채널

- **GitHub**: 패치 리뷰의 주요 장소 (https://github.com/spdk/spdk)
- **메일링 리스트**: spdk@lists.01.org — 설계 논의 및 공지에 사용
- **IRC/Matrix**: Libera.Chat의 #spdk — 빠른 질문에 적합
- **SPDK 개발 페이지**: https://spdk.io/development/ — 프로세스에 대한 공식 참조

---

## 10. 메인테이너가 되는 방법

메인테이너는 정해진 일정에 따라 임명되지 않습니다. 시간이 지남에 따라 지속적이고 높은 품질의 기여를 통해 얻는 길입니다.

### 10.1 메인테이너 팀이 찾는 것

**기술적 역량**: 코드베이스의 최소 한 영역에 대한 깊은 이해. 이는 첫 번째 또는 두 번째 제출에서 올바른 패치와 실제 문제를 잡는 코드 리뷰 코멘트를 통해 입증됩니다.

**판단력**: 트레이드오프를 평가하는 능력 - 성능 vs 복잡성, 정확성 vs 단순성. 메인테이너는 패치가 프로젝트에 속하는지 여부를 결정해야 합니다.

**커뮤니케이션**: 다른 사람의 패치에 대한 명확하고 전문적이며 건설적인 피드백. 메인테이너는 커뮤니티의 분위기를 조성합니다.

**신뢰성**: 몇 달 또는 몇 년에 걸친 일관된 참여. 한 번의 활동 폭발은 메인테이너를 만들지 않습니다.

**커뮤니티 지향성**: 단기적 목표나 고용주 이해보다 프로젝트의 장기적 건강을 우선시하는 것.

### 10.2 실질적 경로

1. **작은 것부터 시작하세요**: 버그를 수정하고, 문서를 개선하고, 단위 테스트를 추가하세요. 수락된 모든 패치는 여러분의 평판과 코드베이스에 대한 친숙도를 쌓습니다.

2. **다른 패치를 리뷰하세요**: 병합 권한이 없더라도 열린 PR에 코멘트를 달아보세요. 높은 품질의 리뷰 코멘트는 주목받습니다. 이것은 또한 여러분의 학습을 가속화합니다.

3. **서브시스템을 소유하세요**: 특정 모듈(bdev 레이어, NVMe 드라이버, NVMe-oF 타겟 등)의 전문가가 되세요. 한 영역의 깊은 전문성이 여러 영역의 얕은 지식보다 더 가치 있습니다.

4. **설계 논의에 참여하세요**: 아키텍처 결정이 이루어질 때 메일링 리스트와 GitHub 이슈에서 참여하세요. 설계 논의에 대한 사려 깊은 기여는 판단력을 입증합니다.

5. **시간이 지남에 따라 일관되게**: 6-12개월 이상에 걸쳐 꾸준한 기여를 목표로 하세요. 활동의 폭발보다는 꾸준함이 중요합니다. 메인테이너 팀은 여러분의 패치가 유지보수를 필요로 할 때 여전히 참여하고 있을 것이라는 신뢰가 필요합니다.

6. **지명받으세요**: 일반적으로 핵심 메인테이너가 관찰된 실적을 기반으로 기여자를 지명합니다. 공식적인 지원 프로세스는 없습니다.

---

## 11. 새 기여자의 흔한 실수

**실수 1: 하나의 패치에 관심사 혼합**

버그를 수정하고 주변 코드를 리팩토링하고 새 기능을 추가하는 패치는 리뷰하기 어렵고 나중에 이분 검색(bisect)하기도 어렵습니다. 별도의 커밋이나 별도의 PR로 분리하세요.

**실수 2: 스레드 모델 위반**

SPDK의 락프리(lockless) 모델은 주어진 객체에 대한 모든 작업이 동일한 `spdk_thread`에서 발생할 것을 요구합니다. 이를 잊으면 재현하기 어렵지만 프로덕션에서 심각한 경쟁 조건이 발생합니다.

**실수 3: I/O 경로에서 메모리 할당**

I/O 처리 중 메모리 할당(`spdk_malloc`, `calloc`)은 메모리가 단편화될 때 지연 시간 급증과 실패를 초래할 수 있습니다. 초기화 중에 리소스를 사전 할당하세요.

**실수 4: 오류 경로 정리 누락**

```c
/* 잘못된 예: 두 번째 할당 실패 시 buf1 누수 */
buf1 = spdk_malloc(size1, ...);
if (!buf1) return -ENOMEM;

buf2 = spdk_malloc(size2, ...);
if (!buf2) return -ENOMEM;  /* buf1 누수! */

/* 올바른 예: 각 실패 시 정리 */
buf1 = spdk_malloc(size1, ...);
if (!buf1) return -ENOMEM;

buf2 = spdk_malloc(size2, ...);
if (!buf2) {
    spdk_free(buf1);
    return -ENOMEM;
}
```

**실수 5: 서브모듈 업데이트 미수행**

```bash
# 업스트림이 서브모듈 포인터를 변경할 때, 업데이트하세요
git submodule update --init --recursive
```

**실수 6: check_format.sh 실행 없이 제출**

즉각적인 CI 실패의 가장 흔한 원인입니다. 모든 푸시 전에 실행하는 습관을 들이세요.

---

## 12. 실습 연습

### 연습 1: 환경 설정 및 첫 포맷 실행

1. SPDK를 복제하고 모든 서브모듈을 초기화합니다.
2. 깨끗한 트리에서 `./scripts/check_format.sh`를 실행합니다. 오류 없이 통과하는지 확인합니다.
3. 의도적으로 포맷 위반을 도입하고(잘못된 들여쓰기) 스크립트를 다시 실행합니다. 출력을 관찰합니다. 변경을 되돌립니다.

### 연습 2: 문서 수정 작성 및 제출

1. `include/spdk/`에서 불완전하거나 누락된 Doxygen 주석이 있는 공개 API 함수를 찾습니다.
2. 섹션 4.4의 형식에 따라 적절한 Doxygen 주석을 작성합니다.
3. 브랜치를 생성하고, 적절한 메시지(`doc:` 또는 모듈 접두사 사용)로 커밋하고, GitHub에서 드래프트 PR을 엽니다. CI가 통과하는지 확인합니다.

### 연습 3: 단위 테스트 추가

1. 불완전한 단위 테스트 커버리지를 가진 모듈을 선택합니다. `test/unit/lib/` 아래에서 해당 테스트 파일을 찾습니다.
2. 엣지 케이스 또는 오류 경로를 다루는 새로운 테스트 케이스 하나를 작성합니다.
3. 테스트가 실행되고 통과하는지 확인합니다: `./test/unit/unittest.sh`.
4. 섹션 5.1의 전체 워크플로우에 따라 패치로 제출합니다.

---

## 참고 자료

- https://spdk.io/development/ — 공식 SPDK 개발 프로세스 가이드
- https://github.com/spdk/spdk — 주요 저장소 및 PR 트래커
- `CONTRIBUTING.md` — SPDK 저장소 루트에 위치
- `GOVERNANCE.md` — 메인테이너 목록 및 TSC 구조
- `CODE_OF_CONDUCT.md` — Contributor Covenant v2.1
- `.astylerc` — C/C++ 포맷팅 설정
- `scripts/check_format.sh` — 전체 포맷 검사 스크립트
- `python/pyproject.toml` — Python 스타일 설정
- Linux 커널 코딩 스타일: https://www.kernel.org/doc/html/latest/process/coding-style.html
- Developer Certificate of Origin: https://developercertificate.org/

---

## 핵심 요약

- SPDK는 소규모 핵심 메인테이너 팀과 TSC를 갖춘 Linux Foundation을 통해 운영됩니다. 프로젝트를 누가 유지하고 어떻게 결정이 내려지는지 이해하면 효과적으로 기여하는 데 도움이 됩니다.

- `./scripts/check_format.sh`는 타협할 수 없습니다. 모든 푸시 전에 실행하세요. 그렇지 않으면 CI가 실패합니다.

- C 스타일은 K&R 괄호, 8칸 탭(강제), 100자 줄 제한, 변수명 옆에 `*`를 사용합니다 - `.astylerc` 설정 파일의 astyle에 의해 적용됩니다.

- 모든 커밋은 `Signed-off-by` 트레일러(DCO)를 포함해야 합니다. 항상 `git commit -s`를 사용하세요.

- 커밋 메시지는 `<module>: <명령형 요약>` 형식을 따르며, 본문에서 무엇이 변경되었고 왜 변경되었는지 설명합니다. 모듈 접두사는 필수이며 정확해야 합니다.

- 패치를 집중적으로 유지하세요. 커밋당 하나의 논리적 변경. 리팩토링과 기능 작업을 혼합하면 위험합니다.

- `include/spdk/`의 공개 API 함수에는 Doxygen 주석이 필요합니다. 새 RPC는 `doc/jsonrpc.md`에 문서화가 필요합니다. 새 기능은 CHANGELOG 항목이 필요합니다.

- 코드 리뷰는 대립적이 아니라 협력적입니다. 모든 코멘트에 응답하고, 의견 불일치를 존중하며 설명하고, 리뷰어가 만족할 때까지 스쿼시하지 마세요.

- 메인테이너 경로는 수개월 또는 수년에 걸쳐 지속적이고 높은 품질의 패치와 코드 리뷰를 통해 얻습니다. 하나의 서브시스템에 대한 깊이가 여러 곳의 넓이보다 낫습니다.

- SPDK의 락프리 폴링 모드 아키텍처는 스레딩 실수가 특히 비용이 많이 든다는 것을 의미합니다. I/O 경로를 수정하기 전에 스레드 모델을 이해하세요.

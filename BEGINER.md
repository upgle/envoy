# Envoy 빌드 가이드 (초보자용)

이 문서는 Envoy를 처음 빌드하는 분들을 위한 간단한 가이드입니다.

## 목차

- [시작하기 전에](#시작하기-전에)
- [방법 1: Docker로 빌드 (권장)](#방법-1-docker로-빌드-권장)
- [방법 2: 로컬에서 직접 빌드](#방법-2-로컬에서-직접-빌드)
- [빌드된 바이너리 찾기](#빌드된-바이너리-찾기)
- [빌드 최적화 팁](#빌드-최적화-팁)
- [문제 해결](#문제-해결)

---

## 시작하기 전에

### 시스템 요구사항

| 항목 | 권장 사양 |
|------|----------|
| 메모리 | CPU 코어당 2GB 이상 |
| 디스크 | 최소 50GB 여유 공간 |
| 운영체제 | Ubuntu 20.04+ / macOS / Windows (WSL2) |

### 빌드 시간 예상

| 빌드 유형 | 첫 빌드 | 증분 빌드 |
|----------|--------|----------|
| 전체 Envoy | ~30분 | ~19초 |
| 특정 필터만 | ~1분 | ~16초 |

---

## 방법 1: Docker로 빌드 (권장)

Docker를 사용하면 복잡한 의존성 설치 없이 바로 빌드할 수 있습니다.

### 1단계: 빌드 디렉토리 설정

```bash
# 빌드 캐시 저장 디렉토리 설정 (중요!)
export ENVOY_DOCKER_BUILD_DIR=~/envoy-build
mkdir -p $ENVOY_DOCKER_BUILD_DIR

# 매번 설정하기 귀찮다면 .bashrc에 추가
echo 'export ENVOY_DOCKER_BUILD_DIR=~/envoy-build' >> ~/.bashrc
```

### 2단계: 빌드 실행

```bash
# 개발용 빌드 (가장 빠름)
ENVOY_DOCKER_BUILD_DIR=~/envoy-build \
  ./ci/run_envoy_docker.sh \
  'bazel build -c fastbuild --config=clang //source/exe:envoy-static'

# 릴리즈 빌드 (최적화됨, 프로덕션용)
ENVOY_DOCKER_BUILD_DIR=~/envoy-build \
  ./ci/run_envoy_docker.sh \
  'bazel build -c opt --config=clang //source/exe:envoy-static'

# 디버그 빌드 (GDB 디버깅용)
ENVOY_DOCKER_BUILD_DIR=~/envoy-build \
  ./ci/run_envoy_docker.sh \
  'bazel build -c dbg --config=clang //source/exe:envoy-static'
```

### 빌드 모드 설명

| 모드 | 명령 옵션 | 용도 |
|-----|----------|-----|
| fastbuild | `-c fastbuild` | 개발 중 빠른 반복 빌드 |
| opt | `-c opt` | 프로덕션 배포용 (최적화됨) |
| dbg | `-c dbg` | GDB로 디버깅할 때 |

---

## 방법 2: 로컬에서 직접 빌드

Docker 없이 로컬에서 직접 빌드하려면 몇 가지 도구를 먼저 설치해야 합니다.

### 1단계: Bazelisk 설치

```bash
sudo wget -O /usr/local/bin/bazel \
  https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-$([ $(uname -m) = "aarch64" ] && echo "arm64" || echo "amd64")
sudo chmod +x /usr/local/bin/bazel
```

### 2단계: 필수 패키지 설치 (Ubuntu)

```bash
sudo apt-get install \
   autoconf \
   curl \
   libtool \
   patch \
   python3-pip \
   unzip \
   virtualenv
```

### 3단계: Clang 컴파일러 설정

Envoy는 Clang 18 이상을 권장합니다.

```bash
# Clang 설치 후 설정
bazel/setup_clang.sh <CLANG_설치_경로>

# Clang을 기본 컴파일러로 설정
echo "build --config=clang" >> user.bazelrc
```

### 4단계: 빌드 실행

```bash
# 개발용 빌드
bazel build -c fastbuild //source/exe:envoy-static

# 릴리즈 빌드
bazel build -c opt //source/exe:envoy-static

# 특정 필터만 빌드 (더 빠름)
bazel build -c fastbuild //source/extensions/filters/http/router:config
```

---

## 빌드된 바이너리 찾기

### Docker 빌드 시

```bash
# 최신 바이너리 찾기
find $ENVOY_DOCKER_BUILD_DIR -name "envoy-static" -type f 2>/dev/null | xargs ls -t | head -1
```

### 로컬 빌드 시

```bash
# 빌드된 바이너리 위치
bazel-bin/source/exe/envoy-static
```

### 바이너리 실행

```bash
# 설정 파일과 함께 실행
./envoy-static -c /path/to/config.yaml
```

---

## 빌드 최적화 팁

### 느린 빌드를 피하는 방법

```bash
# 피해야 할 방법 (30분 이상 소요)
./ci/run_envoy_docker.sh './ci/do_ci.sh dev'

# 권장 방법 (증분 빌드 시 16~19초)
ENVOY_DOCKER_BUILD_DIR=~/envoy-build \
  ./ci/run_envoy_docker.sh \
  'bazel build -c fastbuild --config=clang //source/exe:envoy-static'
```

### 핵심 원칙

1. **빌드 디렉토리 설정 필수**: `ENVOY_DOCKER_BUILD_DIR` 없으면 캐시 사용 불가
2. **CI 스크립트 사용 금지**: `do_ci.sh dev`는 전체 빌드 + 테스트 실행
3. **필요한 것만 빌드**: 전체(`//...`) 대신 특정 타겟만 지정
4. **일관된 빌드 옵션 사용**: 매번 같은 `-c`와 `--config` 사용

### 메모리가 부족할 때

```bash
# 병렬 작업 수 제한 (메모리 8GB일 때)
bazel build --jobs=4 //source/exe:envoy-static

# 또는 user.bazelrc에 영구 설정
echo "build --jobs=4" >> user.bazelrc
```

---

## 문제 해결

### 빌드가 너무 느려요

| 확인 사항 | 해결 방법 |
|----------|----------|
| `ENVOY_DOCKER_BUILD_DIR` 미설정 | 환경변수 설정 |
| CI 스크립트 사용 중 | 직접 bazel 명령 사용 |
| 전체 빌드 중 | 필요한 타겟만 지정 |

### 메모리 부족 오류

```bash
# 병렬 작업 수 줄이기
bazel build --jobs=2 //source/exe:envoy-static
```

### 캐시가 작동 안 해요

```bash
# Bazel 서버 재시작
bazel shutdown

# 최후의 수단: 캐시 정리 (다음 빌드가 오래 걸림)
bazel clean
```

### 빌드 결과물 삭제

```bash
# Envoy 빌드만 정리
bazel clean

# 모든 캐시 포함 정리 (완전 초기화)
bazel clean --expunge

# Bazel 캐시 전체 삭제
rm -rf ~/.cache/bazel
```

---

## 빠른 참조

### 가장 많이 쓰는 명령어

```bash
# 환경 설정
export ENVOY_DOCKER_BUILD_DIR=~/envoy-build

# 개발용 빌드 (Docker)
ENVOY_DOCKER_BUILD_DIR=~/envoy-build \
  ./ci/run_envoy_docker.sh \
  'bazel build -c fastbuild --config=clang //source/exe:envoy-static'

# 로컬 빌드
bazel build -c fastbuild //source/exe:envoy-static

# 특정 필터만 빌드
bazel build -c fastbuild //source/extensions/filters/http/YOUR_FILTER:all
```

### 빌드 시간 비교

```
CI 스크립트 사용:     30분 이상
최적화된 방법:        16~19초 (증분 빌드)

개선율: 최대 100배 이상!
```


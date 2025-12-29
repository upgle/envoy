# Single-Flight Pattern Test Guide

이 문서는 global_cache 필터의 single-flight 패턴이 제대로 동작하는지 테스트하는 방법을 설명합니다.

## Quick Start (자동 테스트)

```bash
chmod +x run_single_flight_test.sh
./run_single_flight_test.sh
```

이 스크립트는 자동으로:
1. Slow backend 시작 (각 요청 2초 지연)
2. Envoy 시작 (debug 로그 활성화)
3. 동시에 5개 요청 전송
4. Single-flight 패턴 검증

## Manual Test (수동 테스트)

더 세밀한 제어가 필요하면 수동으로 테스트할 수 있습니다.

### 1단계: Backend 서버 시작

터미널 1:
```bash
python3 slow_backend.py
```

출력 예시:
```
Slow backend server running on 127.0.0.1:8080
Each request will take 2 seconds to process
```

### 2단계: Envoy 빌드 및 시작

터미널 2:
```bash
# Envoy 빌드 (최초 1회만, 30분 소요)
ENVOY_DOCKER_BUILD_DIR=/home/seonghyun/envoy-build \
  ./ci/run_envoy_docker.sh \
  'bazel build -c fastbuild --config=clang //source/exe:envoy-static'

# Envoy 실행 (debug 로그로)
bazel-bin/source/exe/envoy-static -c test_global_cache.yaml -l debug
```

Envoy가 준비되면:
```
[info] all clusters initialized. initializing init manager
[info] starting main dispatch loop
```

### 3단계: 동시 요청 테스트

터미널 3:
```bash
python3 test_single_flight.py
```

## 예상되는 동작

### 클라이언트 출력

```
Testing single-flight pattern with 5 concurrent requests
======================================================================

Request 1: status=200, x-cache=MISS, duration=2.005s
Request 2: status=200, x-cache=HIT-COALESCED, duration=2.008s
Request 3: status=200, x-cache=HIT-COALESCED, duration=2.010s
Request 4: status=200, x-cache=HIT-COALESCED, duration=2.012s
Request 5: status=200, x-cache=HIT-COALESCED, duration=2.015s

Results Summary:
  Cache MISS (first upstream request): 1
  Cache HIT-COALESCED (waited for first): 4
  Cache HIT (from existing cache): 0
  Errors: 0

✅ SUCCESS: Single-flight pattern is working correctly!
```

**핵심 포인트:**
- 1개 요청만 `MISS` (첫 번째가 upstream으로 감)
- 4개 요청은 `HIT-COALESCED` (첫 번째를 기다렸다가 같은 응답 받음)
- 모든 요청이 거의 동시에 완료 (~2초, backend 지연 시간)

### Backend 서버 출력

```
Backend: Request #1 arrived at 1234567890.123
Backend: Request #1 completed at 1234567892.125
```

**핵심 포인트:**
- Backend는 **1개 요청만** 받음 (5개가 아님!)
- Single-flight가 없으면 5개 모두 backend로 전달됨

### Envoy Debug 로그

```
[debug] global_cache: checking cache for key: GET:localhost:10000:/test
[info] global_cache: cache MISS for key: GET:localhost:10000:/test - sending to upstream

[debug] global_cache: checking cache for key: GET:localhost:10000:/test
[info] global_cache: WAITING for in-flight request for key: GET:localhost:10000:/test

[debug] global_cache: checking cache for key: GET:localhost:10000:/test
[info] global_cache: WAITING for in-flight request for key: GET:localhost:10000:/test

[debug] global_cache: checking cache for key: GET:localhost:10000:/test
[info] global_cache: WAITING for in-flight request for key: GET:localhost:10000:/test

[debug] global_cache: checking cache for key: GET:localhost:10000:/test
[info] global_cache: WAITING for in-flight request for key: GET:localhost:10000:/test

[info] global_cache: cached response (44bytes) for key: GET:localhost:10000:/test
[debug] global_cache: notified waiting requests for key: GET:localhost:10000:/test

[info] global_cache: in-flight request completed for key: ... - serving cached response
[info] global_cache: in-flight request completed for key: ... - serving cached response
[info] global_cache: in-flight request completed for key: ... - serving cached response
[info] global_cache: in-flight request completed for key: ... - serving cached response
```

**핵심 포인트:**
1. 첫 번째 요청: `cache MISS ... - sending to upstream`
2. 2-5번째 요청: `WAITING for in-flight request`
3. 첫 번째 완료: `cached response` + `notified waiting requests`
4. 2-5번째 완료: `in-flight request completed ... - serving cached response`

## Single-Flight 패턴의 효과

### Without Single-Flight (이전 구현)
```
5 concurrent requests → 5 upstream requests → Backend overload
```

Backend 로그:
```
Request #1 arrived
Request #2 arrived  ← 중복!
Request #3 arrived  ← 중복!
Request #4 arrived  ← 중복!
Request #5 arrived  ← 중복!
```

### With Single-Flight (현재 구현)
```
5 concurrent requests → 1 upstream request → 4 requests wait → All get same response
```

Backend 로그:
```
Request #1 arrived   ← 단 1개만!
```

## 캐시 동작 확인

두 번째 테스트에서는 1초 후 다시 요청:

```
Request subsequent: status=200, x-cache=HIT, duration=0.003s

✅ SUCCESS: Cache is working correctly!
```

**핵심 포인트:**
- `HIT` (캐시에서 즉시 응답)
- Duration ~3ms (매우 빠름, backend 갈 필요 없음)
- Backend에 요청 가지 않음

## 문제 해결

### Backend에 여러 요청이 도착하는 경우

Single-flight가 동작하지 않는 것입니다. 확인 사항:
1. 코드가 제대로 컴파일되었는지
2. Envoy가 최신 바이너리를 사용 중인지 (`bazel-bin/source/exe/envoy-static`)
3. global_cache 필터가 설정에 포함되었는지 (`test_global_cache.yaml`)

### x-cache 헤더가 없는 경우

Global_cache 필터가 동작하지 않는 것입니다:
1. Envoy 로그에서 `global_cache` 관련 메시지 확인
2. 필터가 올바르게 등록되었는지 확인

### 모든 요청이 MISS인 경우

요청마다 다른 cache key가 생성되고 있습니다:
- URL이 정확히 같은지 확인
- Method, Host, Path가 모두 동일해야 함

## 성능 측정

### Thundering Herd 없이 (Single-Flight)
- Backend 요청: 1개
- Total duration: ~2초 (backend delay)
- Backend load: Minimal

### Thundering Herd 있을 때 (Without Single-Flight)
- Backend 요청: 5개
- Total duration: ~2초 (parallel)
- Backend load: 5배 증가

**결론**: Single-flight 패턴은 동일한 응답 시간을 유지하면서 backend 부하를 크게 줄입니다.

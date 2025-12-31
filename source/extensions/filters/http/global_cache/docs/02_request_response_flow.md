# 02. 필터 동작 흐름 (요청/응답)

## 2.1 요청 시작: `decodeHeaders`

요청이 들어오면 필터는 아래 순서로 동작합니다.

1. **라우트별 override 확인**
   - `disabled`, `default_ttl`, `include_query_params` 적용
   - `disabled`면 캐시 로직을 건너뜀

2. **캐시 키 생성**
   - `METHOD:HOST:PATH` 형식
   - `include_query_params`가 false면 쿼리 스트링을 제거
   - 구현 위치: `GlobalCacheFilter::generateCacheKey`

3. **캐시 조회**
   - `cache_backend_->lookup(key, callback)` 호출
   - 로컬 캐시는 즉시 callback, Redis는 이벤트 루프에서 나중에 callback

4. **캐시 결과 처리**
   - `HIT`: `serveCachedResponse()`로 즉시 응답
   - `MISS`: single-flight map 확인

5. **single-flight 처리**
   - 이미 동일 키의 요청이 진행 중이면 대기자로 등록
   - 그렇지 않으면 “첫 번째 요청”으로 upstream 진행

Go 개발자 관점에서는 아래처럼 볼 수 있습니다.

- `lookup()`은 **동기/비동기 둘 다 가능**한 인터페이스
- 그래서 필터는 `LookupContext` 구조체로 **동기 호출인지 여부**를 감지합니다.

```text
- LocalCache: callback이 lookup() 안에서 즉시 실행됨
- RedisCache: callback이 이벤트 루프에서 나중에 실행됨
```

Envoy에서 이런 경우를 처리할 때는:

- callback이 **동기**로 실행되면, `decodeHeaders()`가 **즉시 return**
- callback이 **비동기**면, `StopAllIterationAndWatermark`로 요청을 멈추고
  callback에서 `continueDecoding()`으로 재개

## 2.2 응답 처리: `encodeHeaders` / `encodeData`

upstream 응답을 받으면 다음과 같이 캐시합니다.

1. `encodeHeaders`: 헤더를 복사해 보관
2. `encodeData`: body를 버퍼에 모음
3. end_stream 시점에 **완전한 응답을 캐시 저장**

이때 응답 헤더에 `x-cache: MISS`를 추가합니다.

캐시된 응답을 제공할 때는 다음과 같이 표시됩니다.

- `x-cache: HIT`: 캐시 직접 히트
- `x-cache: HIT-COALESCED`: single-flight 대기 후 재사용

## 2.3 캐시 응답 구성: `serveCachedResponse`

캐시에 저장된 헤더/바디를 **복사**해서 그대로 응답합니다.
Go로 치면 `http.Response` 복사 후 `Write()`하는 흐름과 비슷합니다.

C++ 특유 포인트:

- 헤더는 `HeaderMap`을 직접 순회해서 새 맵으로 복사
- body는 `Buffer::OwnedImpl`에서 `add()`로 복사

# 01. 목적과 큰 그림

## 1.1 왜 이 책을 쓰는가?

Envoy Proxy는 현대적인 클라우드 네이티브 환경의 핵심 데이터 플레인입니다. 하지만 C++로 작성되어 있어, Go, Java, Python 등 고수준 언어에 익숙한 애플리케이션 개발자들에게는 "블랙박스"처럼 느껴질 때가 많습니다.

이 책은 `global_cache`라는 **실제 HTTP 필터 구현**을 해부하며 Envoy의 내부 동작 원리를 설명합니다. 단순히 코드를 읽는 것을 넘어, **"Envoy는 왜 이렇게 설계되었는가?"**를 이해하는 것이 목표입니다.

## 1.2 Global Cache 필터의 정체

이 필터(`envoy.filters.http.global_cache`)는 이름 그대로 **전역 캐싱**을 담당합니다.

1.  **Request Coalescing (Single-flight)**: 동일한 URL로 동시에 100개의 요청이 오면, 1개만 업스트림(백엔드)으로 보내고 나머지 99개는 대기시킵니다.
2.  **Caching**: 응답을 저장했다가, 다음 요청에 재사용합니다. Redis 등을 백엔드로 사용하여 여러 Envoy 인스턴스 간에 캐시를 공유할 수 있습니다.

### Go 개발자를 위한 비유

여러분이 Go로 웹 서버를 짠다면 `golang.org/x/sync/singleflight`와 Redis 클라이언트를 미들웨어에 넣어서 구현할 것입니다.

```go
// Go 예시
func Middleware(next http.Handler) http.Handler {
    return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
        key := generateKey(r)
        
        // 1. 캐시 조회
        if cached, ok := redis.Get(key); ok {
            w.Write(cached)
            return
        }

        // 2. Single-flight (중복 요청 방지)
        result, _ := group.Do(key, func() (interface{}, error) {
            // 실제 백엔드 호출
            return next.ServeHTTP(...)
        })
        
        // 3. 응답
        w.Write(result)
    })
}
```

Envoy의 `GlobalCacheFilter`도 논리적으로는 이와 **완전히 동일**합니다. 다만, **비동기 C++**로 구현되어 있을 뿐입니다.

## 1.3 Envoy의 실행 모델 (Event Loop)

Envoy 코드를 이해하기 위해 가장 먼저 깨야 할 고정관념은 **"요청 하나당 스레드 하나(Thread per Request)"**입니다.

-   **Java/Go**: 요청이 오면 스레드(또는 고루틴)가 하나 할당됩니다. I/O를 기다릴 때 스레드는 블로킹됩니다.
-   **Envoy/Node.js**: **스레드 하나가 모든 요청을 처리**합니다(Worker Thread). I/O가 발생하면 **콜백**을 등록하고 다음 요청을 처리합니다.

`GlobalCache` 필터가 Redis에 캐시를 조회하러 갈 때, C++ 함수는 즉시 리턴됩니다. Redis 응답이 오면 그때 다시 깨어나서(Callback) 나머지 로직을 수행합니다.

> **핵심:** 따라서 Envoy 필터 코드에는 `sleep()`이나 동기 `socket.read()` 같은 코드가 절대 있어서는 안 됩니다. 이는 워커 스레드 전체를 멈추게 합니다.

## 1.4 필터 체인 아키텍처

Envoy의 HTTP 처리는 `Filter Chain`을 통과하는 파이프라인 구조입니다.

`decodeHeaders` (요청 헤더 처리) -> `decodeData` (요청 바디 처리) -> ... -> **Upstream** -> ... -> `encodeHeaders` (응답 헤더 처리) -> `encodeData` (응답 바디 처리)

`GlobalCache` 필터는 이 체인의 중간에 위치하여:
1.  **decodeHeaders**: 요청을 가로채서 캐시가 있는지 확인합니다. 캐시가 없으면 Single-flight로 묶습니다.
2.  **encodeHeaders/encodeData**: 업스트림에서 온 응답을 가로채서 캐시에 저장합니다.

다음 장부터는 실제 코드를 보며 이 흐름을 따라가 보겠습니다.

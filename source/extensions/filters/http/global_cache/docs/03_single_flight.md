# 03. Single-flight 동작 상세

single-flight는 **동일 캐시 키의 중복 upstream 요청을 막기 위한 구조**입니다.

## 3.1 핵심 구조체

```cpp
struct InFlightRequest {
  bool completed;
  std::shared_ptr<CacheEntry> result;
  std::vector<std::weak_ptr<GlobalCacheFilter>> waiters;
};
```

- `in_flight_requests_`: `std::unordered_map` (전역 static)
- `in_flight_mutex_`: 전역 mutex
- waiters는 **weak_ptr**로 저장
  - 필터가 먼저 소멸되어도 dangling pointer 방지

Go로 보면:

- `map[string]*InFlightRequest`
- `waiters`는 `[]*Filter` 대신 `[]weakref` 같은 개념

## 3.2 동작 흐름

1. 캐시 miss 시, mutex로 map 확인
2. 이미 in-flight가 있으면 waiters에 등록
3. 없으면 본인이 최초 요청자로 등록
4. 최초 요청이 완료되면 `notifyInFlightWaiters()` 호출
5. 대기 중이던 필터는 `onInFlightComplete()`에서
   - entry가 있으면 캐시 응답
   - 없으면 upstream으로 진행

## 3.3 타임아웃 처리

대기 요청은 `single_flight_timeout`을 넘으면 기다리지 않고 upstream 진행합니다.
이는 `Event::Dispatcher::createTimer()`로 타이머를 등록해 구현합니다.
Go로 치면 `time.AfterFunc()` 같은 느낌입니다.

## 3.4 라이프사이클 정리

필터가 중간에 파괴될 수 있으므로 `onDestroy()`에서:

- 타이머 해제
- 자신이 최초 요청자였다면 waiters에 실패 통지

이 덕분에 **대기자가 영원히 멈추는 상황**을 막습니다.


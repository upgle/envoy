# 10. 구현의 트레이드오프와 주의점

## 10.1 단순 TTL 정책

- 현재 구현은 **응답 헤더(Cache-Control/Expires)**를 해석하지 않습니다.
- 무조건 `default_ttl`만 적용합니다.

## 10.2 캐시 키 단순화

- `METHOD:HOST:PATH`만 사용
- 쿼리 스트링은 path에 포함되므로 반영됨
- 헤더 기반 Vary 처리는 없음

## 10.3 단일 전역 in-flight map

- `static` map은 워커 스레드 간 공유
- mutex로 보호하지만, 스케일에서 contention 가능

## 10.4 LocalCache TTL

- TTL은 `CacheEntry.expiration_time`로 저장
- lookup 때만 만료 검사
- insert 시 전달받는 ttl 파라미터는 사용하지 않음

## 10.5 Redis 비동기 콜백

- callback은 dispatcher에 post되어 실행됨
- 즉, Redis 응답 thread와 필터 thread가 분리될 수 있음


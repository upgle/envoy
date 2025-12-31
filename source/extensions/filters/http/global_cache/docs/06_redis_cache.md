# 06. Redis 캐시 구현

Redis 캐시는 Envoy의 **Redis connection pool**을 사용합니다.
코드 위치: `redis_cache.cc` / `redis_cache.h`

## 6.1 키 구성

- 실제 Redis 키 = `key_prefix + cache_key`
- 기본 prefix는 `envoy:gc:`

## 6.2 요청 흐름

- lookup: Redis `GET`
- insert: Redis `SETEX` (TTL 포함)
- remove: Redis `DEL`

## 6.3 비동기 처리와 생명주기

Redis 요청은 비동기입니다. 요청 객체는 응답이 올 때까지 살아있어야 합니다.

이를 위해 `PendingList`라는 thread-local 리스트에
`shared_ptr<RequestBase>`를 보관합니다.

Go로 비유하면:

- goroutine이 끝나기 전까지 `context`/`callback`을 보관하는 패턴

## 6.4 응답 처리

`LookupRequest::onResponse()`에서 Redis 응답 타입을 확인합니다.

- BulkString → 직렬화된 CacheEntry
- Null/Error → MISS

성공이면 `CacheSerializer::deserialize()`로 CacheEntry 복원합니다.


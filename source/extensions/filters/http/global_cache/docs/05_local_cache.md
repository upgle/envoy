# 05. Local LRU 캐시 구현

로컬 캐시는 **LRU(Least Recently Used)** 기반의 메모리 캐시입니다.

## 5.1 자료구조 구성

- 해시맵: key → LruNode
- 이중 연결 리스트: MRU ↔ LRU

이 구조는 Go에서도 흔히 쓰는 LRU 패턴입니다.

## 5.2 동작 방식

- **lookup**
  - 해시맵에서 찾고, 있으면 만료 여부 확인
  - 히트 시 리스트에서 앞으로 이동(MRU 갱신)

- **insert**
  - entry 크기 계산
  - 용량 초과 시 tail부터 제거

- **remove**
  - 해시맵/리스트에서 동시에 제거

## 5.3 TTL 처리

`CacheEntry`에는 `expiration_time`이 들어 있습니다.
`lookup` 시점에 만료 여부를 확인하고, 만료된 항목은 즉시 제거합니다.
`expiration_time`은 전역 `default_ttl` 또는 라우트별 override TTL로 계산됩니다.

## 5.4 동기 호출

LocalCache는 lookup/insert 시 **콜백을 바로 호출**합니다.
따라서 `decodeHeaders()`에서 callback이 **즉시 실행**됩니다.

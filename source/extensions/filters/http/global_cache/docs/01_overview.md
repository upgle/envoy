# 01. 목적과 큰 그림

**이 필터의 목표**는 다음 두 가지입니다.

- 요청별 캐시 키를 기준으로 **응답을 저장/재사용**한다.
- 동일 키의 동시 요청이 몰릴 때 **single-flight**로 1번만 upstream을 호출한다.

Envoy는 Go의 goroutine 기반 서버와 다르게 **이벤트 루프 기반**으로 동작합니다.
따라서 `lookup`/`insert`는 **콜백 기반 비동기 인터페이스**로 정의되어 있고,
로컬 캐시는 동기처럼 보이지만 Redis는 실제 I/O가 비동기입니다.

핵심 클래스/파일은 다음입니다.

- 필터 본체: `source/extensions/filters/http/global_cache/global_cache_filter.cc`
- 라우트별 설정: `source/extensions/filters/http/global_cache/global_cache_filter.h`
- 캐시 공통 인터페이스: `source/extensions/filters/http/global_cache/cache_backend.h`
- 로컬 LRU: `source/extensions/filters/http/global_cache/local_cache.cc`
- Redis: `source/extensions/filters/http/global_cache/redis_cache.cc`
- L1+L2: `source/extensions/filters/http/global_cache/tiered_cache.cc`
- 직렬화: `source/extensions/filters/http/global_cache/cache_serialization.cc`
- 필터 팩토리/설정: `source/extensions/filters/http/global_cache/config.cc`

추가로, 라우트별로 아래를 override 할 수 있습니다.

- 캐시 활성화/비활성화
- `default_ttl` (캐시 저장 TTL)
- 캐시 키 구성(`cache_key`) 및 쿼리 포함 여부(legacy `include_query_params`)

`cache_key`의 세부 옵션(`include_scheme`, `include_query_params` 등)은
`docs/09_configuration_and_factory.md`의 CacheKeyConfig 섹션을 참고합니다.

# 09. 설정과 초기화 경로

## 9.1 필터 생성

`GlobalCacheFilterFactory`가 설정을 받아
적절한 backend를 생성합니다.

- `cache_backend` 미지정 → 기본 LocalCache
- `local` → LocalCache
- `redis` → RedisCache
- `tiered` → TieredCache

라우트별 설정은 `GlobalCachePerRoute`로 처리합니다.
팩토리는 per-route config를 생성해 저장하고,
필터는 `resolveMostSpecificPerFilterConfig()`로 가장 구체적인 설정을 가져옵니다.

## 9.2 config 파라미터

- `single_flight_timeout`: 대기 최대 시간
- `default_ttl`: 응답 저장 기본 TTL
- `cache_key`: 캐시 키 구성(스킴/호스트/경로/쿼리/헤더)
- 로컬 캐시: `max_entries`, `max_bytes`
- Redis: `cluster_name`, `op_timeout`, `key_prefix`, `enable_cluster_mode`
- Tiered: `write_strategy`, `populate_l1_on_l2_hit`

### 9.3 per-route config 파라미터

- `disabled`: 해당 라우트에서 캐시 비활성화/활성화
- `overrides.default_ttl`: 라우트별 TTL
- `overrides.include_query_params`: 캐시 키에 쿼리 스트링 포함 여부(legacy)
- `overrides.cache_key`: 라우트별 캐시 키 구성(설정 시 전역 cache_key를 대체)

프로토는 `api/envoy/extensions/filters/http/global_cache/v3/global_cache.proto`에 정의되어 있습니다.

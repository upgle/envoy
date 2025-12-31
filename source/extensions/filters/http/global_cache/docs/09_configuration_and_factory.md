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

### 9.2.1 CacheKeyConfig 세부 스펙

- `include_scheme`: URL scheme 포함 여부 (기본 false)
- `include_host`: host/authority 포함 여부 (기본 true)
- `include_path`: path 포함 여부 (기본 true)
- `include_query_params`: 쿼리 파라미터 포함 여부 (기본 true)
- `query_params_included`: 포함할 쿼리 이름 allowlist (기본 전체 포함)
- `query_params_excluded`: 제외할 쿼리 이름 blocklist (기본 비어 있음)
- `headers_included`: 캐시 키에 포함할 요청 헤더 이름 목록

쿼리 파라미터 규칙:
- `include_query_params`가 false이면 쿼리를 제거하며 `query_params_included`/`query_params_excluded`는 무시됩니다.
- `query_params_included`가 비어 있지 않으면 allowlist로 동작합니다.
- `query_params_excluded`에 있는 이름은 항상 제외됩니다.

요청 헤더 규칙:
- 동일 헤더의 다중 값은 `,`로 연결되어 키에 포함됩니다.

### 9.3 per-route config 파라미터

- `disabled`: 해당 라우트에서 캐시 비활성화/활성화
- `overrides.default_ttl`: 라우트별 TTL
- `overrides.include_query_params`: 캐시 키에 쿼리 스트링 포함 여부(legacy)
- `overrides.cache_key`: 라우트별 캐시 키 구성(설정 시 전역 cache_key를 대체)

legacy 동작:
- `overrides.include_query_params`는 `overrides.cache_key`가 없을 때만 적용됩니다.

### 9.4 CacheKeyConfig 예시

```yaml
cache_key:
  include_scheme: { value: true }
  include_query_params: { value: true }
  query_params_included: ["user_id", "region"]
  query_params_excluded: ["utm_source"]
  headers_included: ["x-user-tier", "x-region"]
```

```yaml
typed_per_filter_config:
  envoy.filters.http.global_cache:
    "@type": type.googleapis.com/envoy.extensions.filters.http.global_cache.v3.GlobalCachePerRoute
    overrides:
      cache_key:
        include_query_params: { value: false }
        headers_included: ["x-device"]
```

### 9.5 allowlist/blocklist 시나리오

```yaml
# allowlist: user_id, region만 포함
cache_key:
  include_query_params: { value: true }
  query_params_included: ["user_id", "region"]
```

```yaml
# blocklist: utm_source, debug는 제외
cache_key:
  include_query_params: { value: true }
  query_params_excluded: ["utm_source", "debug"]
```

### 9.6 per-route legacy vs cache_key

```yaml
# legacy include_query_params: cache_key가 없을 때만 적용
typed_per_filter_config:
  envoy.filters.http.global_cache:
    "@type": type.googleapis.com/envoy.extensions.filters.http.global_cache.v3.GlobalCachePerRoute
    overrides:
      include_query_params: { value: false }
```

```yaml
# cache_key가 설정되면 전역 cache_key를 완전히 대체
typed_per_filter_config:
  envoy.filters.http.global_cache:
    "@type": type.googleapis.com/envoy.extensions.filters.http.global_cache.v3.GlobalCachePerRoute
    overrides:
      cache_key:
        include_query_params: { value: true }
        query_params_included: ["user_id"]
```

프로토는 `api/envoy/extensions/filters/http/global_cache/v3/global_cache.proto`에 정의되어 있습니다.

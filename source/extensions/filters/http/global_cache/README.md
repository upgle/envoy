# Global Cache HTTP Filter

이 디렉토리는 `envoy.filters.http.global_cache` 필터의 구현을 포함합니다. 이 필터는
요청별 캐시 키를 기준으로 응답을 캐시하고, 단일 요청 중복을 줄이기 위한
single-flight 패턴을 제공합니다.

## 주요 기능

- 요청 메서드/호스트/경로 기반의 캐시 키 생성 (`<METHOD>:<HOST>:<PATH>`), 기본으로 쿼리 스트링 포함
- 설정으로 캐시 키 구성 요소(스킴/호스트/경로/쿼리/헤더)를 선택적으로 포함 가능
- 캐시된 응답 헤더/바디를 그대로 복원해 응답 제공
- `x-cache` 헤더로 캐시 상태 노출 (`HIT`, `HIT-COALESCED`, `MISS`)
- single-flight 패턴으로 동일 키의 동시 요청을 합류(coalesce)
- 로컬 LRU 캐시, Redis 캐시, L1+L2(로컬+Redis) 계층형 캐시 지원
- 라우트별 캐시 비활성화/TTL/쿼리 포함 여부 override 지원

## 동작 개요

1. 라우트별 override를 적용한 뒤 요청 헤더로 캐시 키를 생성합니다.
2. 캐시 조회 결과가 `HIT`이면 즉시 캐시된 응답을 반환합니다.
3. `MISS`이면 single-flight 추적을 확인합니다.
   - 이미 동일 키의 요청이 진행 중이면 완료를 기다렸다가 결과를 재사용합니다.
   - `single_flight_timeout`이 만료되면 대기 요청은 upstream으로 진행합니다.
4. upstream 응답은 `default_ttl` 동안 캐시 저장됩니다.
   - 라우트별 override가 있으면 해당 TTL이 적용됩니다.

참고:
- single-flight 대기는 이벤트 타이머로 관리되어 워커 스레드를 블로킹하지 않습니다.
- 현재 구현은 응답 헤더의 캐시 TTL을 해석하지 않고 `default_ttl`을 사용합니다.
  - 라우트별 override로 `default_ttl`을 다르게 설정할 수 있습니다.

## 설정 요약

필터 설정은 `envoy.extensions.filters.http.global_cache.v3.GlobalCache`를 사용합니다.

### GlobalCache

- `single_flight_timeout`
  - 설명: 동일 키의 in-flight 요청을 기다리는 최대 시간
  - 기본값: 5초
  - 제한: 0s ~ 300s
- `default_ttl`
  - 설명: 캐시 항목 기본 TTL
  - 기본값: 5분
  - 제한: 1s ~ 24h
- `cache_backend`
  - 설명: 캐시 백엔드 선택 (local/redis/tiered)
  - 미설정 시 로컬 LRU 캐시 기본 설정 사용
- `cache_key`
  - 설명: 캐시 키 구성(스킴/호스트/경로/쿼리/헤더)
  - 기본값: method + host + path (+ query)

### CacheKeyConfig

- `include_scheme`
  - 설명: 캐시 키에 URL scheme 포함 여부 (`http`/`https`)
  - 기본값: false
- `include_host`
  - 설명: 캐시 키에 호스트/authority 포함 여부
  - 기본값: true
- `include_path`
  - 설명: 캐시 키에 경로 포함 여부
  - 기본값: true
- `include_query_params`
  - 설명: 캐시 키에 쿼리 파라미터 포함 여부
  - 기본값: true
  - 참고: false이면 경로에서 쿼리를 제거하며, `query_params_included`/`query_params_excluded`는 무시됩니다.
- `query_params_included`
  - 설명: 지정된 이름의 쿼리만 포함(allowlist)
  - 기본값: 비어 있음(전체 포함)
- `query_params_excluded`
  - 설명: 지정된 이름의 쿼리를 제외(blocklist)
  - 기본값: 비어 있음
  - 참고: `query_params_included`가 설정되어 있어도 `query_params_excluded`가 우선 적용됩니다.
- `headers_included`
  - 설명: 캐시 키에 포함할 요청 헤더 이름 목록
  - 기본값: 비어 있음
  - 참고: 동일 헤더의 다중 값은 `,`로 연결됩니다.

쿼리 파라미터 처리 규칙:
- `include_query_params`가 true이고 `query_params_included`/`query_params_excluded`가 비어 있으면 원본 쿼리를 그대로 사용합니다.
- `query_params_included`가 비어 있지 않으면 allowlist로 동작합니다.
- `query_params_excluded`에 있는 이름은 항상 제외됩니다.

### CacheBackendConfig.local (LocalCacheConfig)

- `max_entries`
  - 설명: 캐시 최대 엔트리 수
  - 기본값: 1000
  - 제한: 1 ~ 1,000,000
- `max_bytes`
  - 설명: 캐시 총 용량(바이트)
  - 기본값: 104857600 (100MB)
  - 제한: 최소 1MB

### CacheBackendConfig.redis (RedisCacheConfig)

- `cluster_name`
  - 설명: Redis upstream 클러스터 이름 (Envoy 클러스터에 등록 필요)
  - 필수
- `op_timeout`
  - 설명: Redis 연산 타임아웃(GET/SET/DEL)
  - 기본값: 100ms
  - 제한: 1ms ~ 10s
- `key_prefix`
  - 설명: Redis 키 접두사 (네임스페이스 용도)
  - 기본값: `envoy:gc:`
- `enable_cluster_mode`
  - 설명: Redis Cluster 리다이렉션(MOVED/ASK) 지원 여부
  - 기본값: false (proto 기본값)
- 참고: Redis 저장 시 응답 헤더/바디를 직렬화하고 remaining TTL을 함께 저장합니다.

### CacheBackendConfig.tiered (TieredCacheConfig)

- `l1_local`
  - 설명: L1 로컬 캐시 설정
  - 필수
- `l2_redis`
  - 설명: L2 Redis 캐시 설정
  - 필수
- `write_strategy`
  - 설명: L1/L2 쓰기 전략
  - 기본값: `WRITE_THROUGH`
  - `WRITE_THROUGH`: L1/L2 동시 쓰기 성공 시 완료
  - `WRITE_BACK`: L1 먼저 완료, L2는 비동기 갱신
- `populate_l1_on_l2_hit`
  - 설명: L2 히트 시 L1에 채우기 여부
  - 기본값: true

## 설정 예시

### 기본 로컬 캐시

```yaml
- name: envoy.filters.http.global_cache
  typed_config:
    "@type": type.googleapis.com/envoy.extensions.filters.http.global_cache.v3.GlobalCache
```

### 로컬 LRU 캐시 상세 설정

```yaml
- name: envoy.filters.http.global_cache
  typed_config:
    "@type": type.googleapis.com/envoy.extensions.filters.http.global_cache.v3.GlobalCache
    single_flight_timeout: { seconds: 5 }
    cache_backend:
      local:
        max_entries: 1000
        max_bytes: 104857600
```

### Redis 캐시

```yaml
- name: envoy.filters.http.global_cache
  typed_config:
    "@type": type.googleapis.com/envoy.extensions.filters.http.global_cache.v3.GlobalCache
    cache_backend:
      redis:
        cluster_name: redis_cluster
        op_timeout: { seconds: 1 }
        key_prefix: "envoy:test:"
        enable_cluster_mode: false
```

### 계층형 캐시 (L1 로컬 + L2 Redis)

```yaml
- name: envoy.filters.http.global_cache
  typed_config:
    "@type": type.googleapis.com/envoy.extensions.filters.http.global_cache.v3.GlobalCache
    cache_backend:
      tiered:
        l1_local:
          max_entries: 5000
          max_bytes: 209715200
        l2_redis:
          cluster_name: redis_cluster
          op_timeout: { milliseconds: 200 }
          key_prefix: "envoy:gc:"
          enable_cluster_mode: true
        write_strategy: WRITE_THROUGH
        populate_l1_on_l2_hit: true
```

### 라우트별 override

```yaml
route_config:
  virtual_hosts:
  - name: local_service
    domains: ["*"]
    routes:
    - match: { prefix: "/no-cache" }
      route: { cluster: local_service }
      typed_per_filter_config:
        envoy.filters.http.global_cache:
          "@type": type.googleapis.com/envoy.extensions.filters.http.global_cache.v3.GlobalCachePerRoute
          disabled: true
    - match: { prefix: "/api" }
      route: { cluster: local_service }
      typed_per_filter_config:
        envoy.filters.http.global_cache:
          "@type": type.googleapis.com/envoy.extensions.filters.http.global_cache.v3.GlobalCachePerRoute
          overrides:
            default_ttl: { seconds: 30 }
            include_query_params: { value: false }
```

참고:
- `overrides.include_query_params`는 레거시 옵션으로, `overrides.cache_key`가 없을 때만 적용됩니다.
- `overrides.cache_key`가 설정되면 해당 라우트에서는 필터 레벨 `cache_key`를 완전히 대체합니다.

### 캐시 키 커스터마이즈

```yaml
- name: envoy.filters.http.global_cache
  typed_config:
    "@type": type.googleapis.com/envoy.extensions.filters.http.global_cache.v3.GlobalCache
    cache_key:
      include_scheme: { value: true }
      include_query_params: { value: true }
      query_params_included: ["user_id"]
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

### 쿼리 필터링 예시 (allowlist/blocklist)

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

### 라우트별 예시 (legacy vs cache_key)

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

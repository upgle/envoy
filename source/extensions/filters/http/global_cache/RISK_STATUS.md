# Global Cache 리스크 현황

## 해결됨

- High: single-flight 콜백이 워커 경계를 넘어 호출될 수 있는 위험(전역 맵 기반). global_cache_filter.cc
- High: 응답 바디 무제한 버퍼링으로 메모리 급증 위험. global_cache_filter.cc
- Medium: 캐시 가능성 정책 부재로 과도한 캐시 저장 위험. global_cache_filter.cc
- Medium: TieredCache WRITE_THROUGH 콜백 중복 호출 위험. tiered_cache.cc
- Medium: 구분자 기반 캐시 키 조합으로 충돌 가능성. global_cache_filter.cc

## 남아 있음

- High: Vary/Accept-Encoding 미반영으로 압축 변형 혼선. global_cache_filter.cc
- High: 요청 Cache-Control 미반영으로 클라이언트 의도 위반 가능. global_cache_filter.cc
- High: 응답 TTL 해석 미지원으로 실제 TTL과 불일치. global_cache_filter.cc
- Medium: TieredCache WRITE_THROUGH 부분 실패 시 일관성 정책 불명확. tiered_cache.cc
- Medium: Redis TTL 드리프트로 TTL 연장 가능. cache_serialization.cc
- Medium: hop-by-hop 헤더 캐시로 잘못된 헤더 전달 가능. global_cache_filter.cc
- Medium: trailers 미보존으로 gRPC 응답 손실 가능. global_cache_filter.cc
- Medium: host/authority 정규화 부족으로 캐시 분산. global_cache_filter.cc
- Medium: 쿼리 파라미터 순서에 따른 캐시 키 분리. global_cache_filter.cc
- Medium: 민감 정보가 캐시 키에 포함될 수 있음. global_cache_filter.cc
- Medium: Redis 캐시 오염/변조 방지 미흡. cache_serialization.cc
- Medium: x-cache 헤더 충돌/중복 가능. global_cache_filter.cc
- Medium: 만료 엔트리 잔존으로 메모리 점유 지속. local_cache.cc
- Medium: Content-Length 불일치 미검출 위험. global_cache_filter.cc
- High: Redis 키 prefix가 고정/공유되는 경우 테넌트 간 충돌 가능(멀티테넌트 환경). redis_cache.cc
- Medium: 캐시된 응답이 upstream의 Date/Age를 그대로 유지해 stale 판단/지표 해석에 혼선.
  global_cache_filter.cc
- Medium: HEAD 요청을 캐시할 때 바디가 없는 응답이 저장되므로, 이후 GET이 같은 키로 HIT되면 바디가 없는
  응답이 반환될 가능성(키 구성 정책에 따라). global_cache_filter.cc
- Low: Redis 연결 실패 시 경고만 로그로 남고, 시스템 상태 지표 부재로 장애 탐지 지연. redis_cache.cc

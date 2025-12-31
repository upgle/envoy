# Global Cache 구현 해설서 (Go 개발자용)

이 문서는 `envoy.filters.http.global_cache`의 **현재 구현을 책처럼 한 단계씩** 설명합니다.
C/C++에 익숙하지 않은 Go 개발자를 대상으로, Envoy의 이벤트 기반 구조와
C++ 코드에서 자주 보이는 문법/패턴을 함께 풀어 설명합니다.
또한 라우트별 캐시 override(비활성화/TTL/쿼리 포함 여부)도 포함합니다.

## 목차

- 01. 목적과 큰 그림: `source/extensions/filters/http/global_cache/docs/01_overview.md`
- 02. 필터 동작 흐름: `source/extensions/filters/http/global_cache/docs/02_request_response_flow.md`
- 03. Single-flight 상세: `source/extensions/filters/http/global_cache/docs/03_single_flight.md`
- 04. 캐시 백엔드 인터페이스: `source/extensions/filters/http/global_cache/docs/04_cache_backend_interface.md`
- 05. Local LRU 캐시: `source/extensions/filters/http/global_cache/docs/05_local_cache.md`
- 06. Redis 캐시: `source/extensions/filters/http/global_cache/docs/06_redis_cache.md`
- 07. Tiered 캐시: `source/extensions/filters/http/global_cache/docs/07_tiered_cache.md`
- 08. 직렬화 포맷: `source/extensions/filters/http/global_cache/docs/08_serialization_format.md`
- 09. 설정과 초기화 경로: `source/extensions/filters/http/global_cache/docs/09_configuration_and_factory.md`
- 10. 트레이드오프와 주의점: `source/extensions/filters/http/global_cache/docs/10_tradeoffs.md`
- 부록. C++/Go 매핑 치트시트: `source/extensions/filters/http/global_cache/docs/appendix_cpp_go_cheatsheet.md`

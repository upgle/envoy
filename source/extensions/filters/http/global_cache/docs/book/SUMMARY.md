# Summary

[서문](README.md)

---

# Part I: 개념과 아키텍처

- [Chapter 1. Envoy와 Global Cache: 큰 그림](chapter_01_overview.md)
- [Chapter 2. Modern C++ 생존 키트](chapter_02_cpp_survival_kit.md)
- [Chapter 3. Envoy 실행 모델: 워커, 디스패처, 필터 계약](chapter_03_envoy_execution_model.md)

# Part II: 핵심 구현

- [Chapter 4. 캐시 키 설계: 정확성의 출발점](chapter_04_cache_key_design.md)
- [Chapter 5. decodeHeaders: 요청 처리의 시작점](chapter_05_decode_headers.md)
- [Chapter 6. Single-Flight: 요청 합류의 모든 것](chapter_06_single_flight.md)
- [Chapter 7. encodeHeaders/encodeData: 응답 캐싱](chapter_07_encode_headers.md)

# Part III: 캐시 백엔드

- [Chapter 8. CacheBackend 인터페이스: 추상화의 힘](chapter_08_cache_backend_interface.md)
- [Chapter 9. Local LRU 캐시: 메모리 자료구조의 정석](chapter_09_local_cache.md)
- [Chapter 10. Redis 캐시: 비동기 네트워크의 세계](chapter_10_redis_cache.md)
- [Chapter 11. Tiered 캐시: 로컬과 리모트의 조화](chapter_11_tiered_cache.md)
- [Chapter 12. 직렬화: 바이너리의 세계로](chapter_12_serialization.md)

# Part IV: 설정, 운영, 테스트

- [Chapter 13. 설정과 팩토리: YAML을 코드로 바꾸는 마법](chapter_13_configuration.md)
- [Chapter 14. 관측가능성과 운영: 시스템의 내부 들여다보기](chapter_14_observability.md)
- [Chapter 15. 테스트와 품질 보증: 안정적인 필터 만들기](chapter_15_testing.md)
- [Chapter 16. 설계 트레이드오프와 운영 고려사항](chapter_16_tradeoffs.md)

---

[부록](appendix.md)

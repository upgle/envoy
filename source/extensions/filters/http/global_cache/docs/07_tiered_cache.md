# 07. Tiered 캐시 구현 (L1+L2)

TieredCache는 **L1=로컬, L2=Redis** 조합입니다.

## 7.1 조회 흐름

1. L1 lookup
2. L1 miss 시 L2 lookup
3. L2 hit 시 L1에 채움(optional)

## 7.2 쓰기 전략

- WRITE_THROUGH
  - L1/L2 모두 성공해야 완료
  - latency 높지만 일관성 높음

- WRITE_BACK
  - L1만 성공하면 완료
  - L2는 비동기 업데이트

## 7.3 남은 TTL 계산

L2 히트 시 L1에 복제할 때 TTL을 다시 계산합니다.

- `expiration_time - now` → 남은 TTL
- 0 이하이면 복제하지 않음


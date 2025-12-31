# 08. 직렬화 포맷 (Redis 저장 포맷)

Redis에는 응답 헤더와 바디가 **바이너리 문자열**로 저장됩니다.
포맷은 아래 순서를 따릅니다.

```
[4 bytes: "GC01" magic]
[8 bytes: remaining_ttl_ms]
[4 bytes: num_headers]
[headers...]
[8 bytes: body_length]
[body bytes...]
```

## 포인트

- `remaining_ttl_ms`는 “현재 시점 기준 남은 TTL”입니다.
- deserialize 시 `now + remaining_ttl`로 expiration_time 재구성
- 네트워크 바이트 오더(big endian)를 사용 (htonl/ntohl)
- `remaining_ttl_ms`는 전역 `default_ttl` 또는 라우트별 override TTL에서 계산됩니다.

Go로 구현한다면 `binary.BigEndian`으로 uint32/uint64를 읽는 형태입니다.

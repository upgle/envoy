# Chapter 12. 직렬화: 바이너리의 세계로

> **이 장의 목표**: `CacheEntry` 객체를 Redis에 저장하거나 네트워크로 전송하기 위해 바이너리 포맷으로 변환하는 직렬화(Serialization) 과정을 깊게 이해합니다. C++의 저수준 메모리 조작 기법과 네트워크 바이트 순서 처리 방식을 배웁니다.

---

## 12.1 직렬화가 필요한 이유

우리가 앞선 장에서 살펴본 `CacheEntry` 객체는 메모리상에 존재하는 복잡한 구조체입니다. 응답 헤더(`HeaderMap`), 응답 바디(`Buffer::Instance`), 그리고 만료 시간(`steady_clock::time_point`)을 포함하고 있죠. 

하지만 Redis와 같은 외부 저장소는 C++ 객체를 직접 이해하지 못합니다. Redis는 단순한 "바이트 덩어리(Bulk String)"만을 저장할 수 있습니다. 따라서 우리는 다음과 같은 이유로 직렬화가 필요합니다.

1.  **영속성(Persistence)**: 프로세스가 재시작되어도 캐시 데이터를 유지하기 위해 바이트 형태로 변환하여 외부 저장소(Redis)에 저장해야 합니다.
2.  **네트워크 전송**: 데이터가 네트워크를 타고 Redis 서버로 가기 위해서는 일렬로 늘어선 바이트 스트림(Byte Stream) 형태여야 합니다.
3.  **공유(Sharing)**: 여러 워커 스레드나 여러 Envoy 노드가 동일한 형식으로 데이터를 읽고 쓸 수 있는 표준화된 포맷이 필요합니다.

Go 언어에서는 `encoding/json`이나 `gob`을 사용해 간단히 처리할 수 있었겠지만, Envoy의 성능 최적화와 저수준 제어를 위해 여기서는 직접 설계한 **커스텀 바이너리 포맷**을 사용합니다.

---

## 12.2 바이너리 포맷 상세

Global Cache Filter는 효율성을 위해 매우 간결한 바이너리 포맷을 채택했습니다. 아래는 전체적인 레이아웃입니다.

### 전체 레이아웃

```mermaid
packetbeta
title Global Cache Binary Format (GC01)
0-31: "Magic (GC01)"
32-95: "Remaining TTL (ms)"
96-127: "Number of Headers"
128-159: "Header 1 Key Length (L1)"
160-191: "Header 1 Key Data (L1 bytes) ..."
192-223: "Header 1 Value Length (V1)"
224-255: "Header 1 Value Data (V1 bytes) ..."
256-287: "..."
288-351: "Body Length (B)"
352-383: "Body Data (B bytes) ..."
```

### 상세 필드 설명

1.  **Magic (4 Bytes)**: `"GC01"` (0x47433031). 이 데이터가 Global Cache 포맷임을 식별하는 식별자입니다.
2.  **Remaining TTL (8 Bytes)**: 캐시 만료까지 남은 시간을 밀리초(ms) 단위의 64비트 정수로 저장합니다. 절대 시간이 아닌 '남은 시간'을 저장함으로써 서버 간 시간 동기화(NTP) 오차 문제를 완화합니다.
3.  **Num Headers (4 Bytes)**: 뒤따라올 헤더의 개수입니다.
4.  **Header Loop**: 각 헤더는 `[Key Length(4B)][Key Data][Value Length(4B)][Value Data]` 순서로 반복됩니다.
5.  **Body Length (8 Bytes)**: 응답 바디의 총 크기입니다.
6.  **Body Data**: 실제 바디 바이너리 데이터입니다.

---

## 12.3 CacheSerializer 클래스 구조

직렬화 로직은 `CacheSerializer` 클래스에 응집되어 있습니다. 이 클래스는 인스턴스화할 필요가 없는 유틸리티 성격의 클래스입니다. 헤더 파일을 먼저 살펴보겠습니다.

```cpp
// source/extensions/filters/http/global_cache/cache_serialization.h

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

/**
 * Utility for serializing and deserializing CacheEntry objects.
 */
class CacheSerializer {
public:
  // (1) 외부에서 호출하는 메인 인터페이스
  static std::string serialize(const CacheEntry& entry);
  static std::shared_ptr<CacheEntry> deserialize(const std::string& data);

private:
  // (2) 내부 도우미 메서드들
  static void writeUint32(std::string& buffer, uint32_t value);
  static void writeUint64(std::string& buffer, uint64_t value);
  static uint32_t readUint32(const std::string& data, size_t& offset);
  static uint64_t readUint64(const std::string& data, size_t& offset);
  static std::string readString(const std::string& data, size_t& offset, uint32_t length);
};

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
```

### C++ 문법 노트: static 메서드와 네임스페이스
Go 언어에서는 패키지 레벨 함수를 사용하겠지만, C++에서는 관련 함수들을 하나의 클래스 안에 `static` 메서드로 묶는 패턴을 자주 사용합니다. 
- **Namespace**: `Envoy::Extensions::...`와 같이 깊은 네임스페이스는 Go의 패키지 경로와 대응됩니다. 이는 이름 충돌을 방지하고 코드의 소속을 명확히 합니다.
- **Utility Class**: 모든 메서드가 `static`이고 생성자가 private(혹은 명시되지 않음)인 클래스는 "기능의 집합"으로만 사용됩니다. Java 개발자에게는 `java.lang.Math` 같은 클래스와 익숙한 개념일 것입니다.

---

## 12.4 serialize() 함수 분석

이제 실제 구현부를 뜯어봅시다. 먼저 전체를 바이트로 변환하는 `serialize()` 함수입니다. 이 함수는 메모리 할당 최적화부터 시작합니다.

```cpp
std::string CacheSerializer::serialize(const CacheEntry& entry) {
  std::string buffer;
  // (1) 넉넉하게 공간을 확보하여 동적 할당 오버헤드 최소화
  buffer.reserve(1024); 

  // 1. 매직 넘버 기록
  writeUint32(buffer, kSerializationMagic);
  
  // 2. TTL 계산 및 기록
  // (2) steady_clock을 사용한 단조 시간 처리
  auto now = std::chrono::steady_clock::now();
  auto remaining = entry.expiration_time > now ? entry.expiration_time - now
                                               : std::chrono::steady_clock::duration::zero();
  auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
  writeUint64(buffer, static_cast<uint64_t>(remaining_ms));

  // 3. 헤더 개수 계산
  uint32_t num_headers = 0;
  entry.headers->iterate([&num_headers](const Http::HeaderEntry&) -> Http::HeaderMap::Iterate {
    num_headers++;
    return Http::HeaderMap::Iterate::Continue;
  });
  writeUint32(buffer, num_headers);

  // 4. 헤더 순회하며 기록
  entry.headers->iterate([&buffer](const Http::HeaderEntry& header) -> Http::HeaderMap::Iterate {
    auto key = header.key().getStringView();
    auto value = header.value().getStringView();

    // Key Length (4B) + Key Data
    writeUint32(buffer, static_cast<uint32_t>(key.size()));
    buffer.append(key.data(), key.size());

    // Value Length (4B) + Value Data
    writeUint32(buffer, static_cast<uint32_t>(value.size()));
    buffer.append(value.data(), value.size());

    return Http::HeaderMap::Iterate::Continue;
  });

  // 5. 바디 기록
  uint64_t body_length = entry.body.length();
  writeUint64(buffer, body_length);

  if (body_length > 0) {
    // (3) 분산된 메모리 조각들을 하나로 합치기
    for (const auto& slice : entry.body.getRawSlices()) {
      buffer.append(static_cast<const char*>(slice.mem_), slice.len_);
    }
  }

  return buffer;
}
```

### (1) buffer.reserve(1024)
C++의 `std::string`은 내부적으로 동적 배열을 가집니다. `append()`를 호출할 때 공간이 부족하면 보통 현재 크기의 2배로 새로운 메모리를 할당하고 기존 데이터를 복사합니다. Go의 `slice` 확장과 거의 같은 메커니즘이죠. `reserve()`는 우리가 대략적인 크기를 알 때 이 불필요한 "할당-복사-해제" 사이클을 방지해주는 아주 중요한 최적화 기법입니다.

### (2) std::chrono와 시간 단조성(Monotonicity)
`std::chrono::steady_clock`은 시스템 시계(Wall clock)가 아닌, 부팅 후 흐른 시간을 나타내는 '단조 시계'입니다. 사용자가 서버 시간을 수동으로 고치거나 NTP 서버가 시간을 조정하더라도 영향을 받지 않습니다. 
- `duration_cast`: Go의 `time.Duration`을 `Milliseconds()`로 변환하는 것과 유사하게, 시간 단위를 안전하게 변환합니다.
- `static_cast<uint64_t>`: 부호 있는 정수를 부호 없는 64비트 정수로 명시적으로 변환합니다.

### (3) 바디 슬라이스(RawSlices)와 고성능 I/O
Envoy의 `Buffer::Instance`는 **Scatter-Gather I/O**를 위해 설계되었습니다. 데이터가 메모리 여기저기에 흩어져 있어도 하나의 논리적인 버퍼처럼 취급합니다. 하지만 Redis는 연속된 바이트를 요구하므로, `getRawSlices()`를 통해 조각들을 하나씩 꺼내어 `buffer`라는 하나의 큰 주머니에 담아주는 과정이 필요합니다.

---

## 12.5 Network Byte Order와 엔디언 심화

네트워크 프로그래밍에서 '바이트 순서'는 서로 다른 언어와 아키텍처를 이어주는 약속입니다.

### 왜 htonl()이 필요한가?
대부분의 현대 CPU(Intel, Apple Silicon 등)는 **Little Endian** 방식을 사용합니다. 예를 들어 32비트 정수 `0x12345678`은 메모리에 `78 56 34 12` 순서로 저장됩니다. 반면 네트워크 표준인 **Big Endian**은 `12 34 56 78` 순서로 저장합니다. 

만약 우리가 변환 없이 Little Endian 그대로 Redis에 저장하고, 이를 읽는 다른 서버가 Big Endian 방식이라면 숫자가 완전히 다르게 해석될 것입니다.

### 64비트 정수 쓰기 (writeUint64)
재미있게도 표준 C 라이브러리에는 `htonl`(32비트용)은 있지만 `htonll`(64비트용)은 없는 경우가 많습니다. 그래서 우리는 64비트 정수를 32비트 두 개로 쪼개서 각각 변환하는 방식을 사용합니다.

```cpp
void CacheSerializer::writeUint64(std::string& buffer, uint64_t value) {
  // 상위 32비트와 하위 32비트로 분리
  uint32_t high = htonl(static_cast<uint32_t>(value >> 32));
  uint32_t low = htonl(static_cast<uint32_t>(value & 0xFFFFFFFF));
  
  // 각각 버퍼에 추가
  buffer.append(reinterpret_cast<const char*>(&high), sizeof(high));
  buffer.append(reinterpret_cast<const char*>(&low), sizeof(low));
}
```

### C++ 형변환(Cast)의 세계
C++에는 여러 가지 형변환 연산자가 있습니다. 이 장에서 사용된 것들을 정리해봅시다.

1.  **static_cast**: 가장 일반적인 형변환입니다. 컴파일 타임에 안전성이 검사됩니다. (예: `int` -> `double`, `uint32` -> `uint64`)
2.  **reinterpret_cast**: "이 메모리 주소의 비트 패턴을 그냥 내가 말하는 대로 해석해라"라는 뜻입니다. 위 코드에서 정수의 주소(`&high`)를 바이트 포인터(`const char*`)로 속여서 `append()` 함수에 넘길 때 사용합니다. Go의 `unsafe.Pointer`와 비슷하다고 생각하면 됩니다.
3.  **const_cast**: `const` 속성을 제거할 때 쓰지만, 여기서는 사용하지 않았습니다.
4.  **dynamic_cast**: 상속 관계에 있는 객체들 사이에서 안전하게 변환할 때 사용합니다.

---

## 12.6 deserialize() 함수 분석: 파싱의 미학

역직렬화는 마치 퍼즐 조각을 맞추는 것과 같습니다. 전체 데이터 스트림에서 정해진 크기만큼을 떼어내어 의미 있는 변수로 복원합니다.

```cpp
std::shared_ptr<CacheEntry> CacheSerializer::deserialize(const std::string& data) {
  size_t offset = 0; 

  try {
    // 1. 최소 크기 검사 (Magic 4B + TTL 8B = 12B)
    if (data.size() < 12) return nullptr;

    // 2. 매직 넘버 확인
    uint32_t magic = readUint32(data, offset);
    if (magic != kSerializationMagic) return nullptr;

    // 3. 만료 시간 복원
    uint64_t remaining_ms = readUint64(data, offset);
    auto expiration_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(remaining_ms);

    // 4. 헤더 복원 루프
    if (offset + 4 > data.size()) return nullptr;
    uint32_t num_headers = readUint32(data, offset);
    auto headers = Http::ResponseHeaderMapImpl::create();

    for (uint32_t i = 0; i < num_headers; i++) {
      // Key 파싱
      if (offset + 4 > data.size()) return nullptr;
      uint32_t key_len = readUint32(data, offset);
      if (offset + key_len > data.size()) return nullptr;
      std::string key = readString(data, offset, key_len);

      // Value 파싱
      if (offset + 4 > data.size()) return nullptr;
      uint32_t val_len = readUint32(data, offset);
      if (offset + val_len > data.size()) return nullptr;
      std::string val = readString(data, offset, val_len);

      headers->addCopy(Http::LowerCaseString(key), val);
    }

    // 5. 바디 복원
    if (offset + 8 > data.size()) return nullptr;
    uint64_t body_len = readUint64(data, offset);
    if (body_len > data.size() - offset) return nullptr;

    Buffer::OwnedImpl body;
    if (body_len > 0) {
      body.add(data.data() + offset, body_len);
    }

    return std::make_shared<CacheEntry>(std::move(body), std::move(headers), expiration_time);
  } catch (const std::exception& e) {
    // 로그를 남기거나 단순히 무시 (캐시 미스 처리)
    return nullptr;
  }
}
```

### 오프셋 관리의 중요성
위 코드에서 `offset`은 매번 데이터를 읽을 때마다 증가합니다. `readUint32(data, offset)` 함수 내부를 보면 왜 그런지 알 수 있습니다.

```cpp
uint32_t CacheSerializer::readUint32(const std::string& data, size_t& offset) {
  uint32_t network_value;
  // (1) 메모리 직접 복사
  std::memcpy(&network_value, data.data() + offset, sizeof(network_value));
  // (2) 오프셋 증가
  offset += sizeof(network_value);
  // (3) 호스트 엔디언으로 변환
  return ntohl(network_value);
}
```
여기서 `size_t& offset`의 `&`는 **참조 전달(Pass by Reference)**을 의미합니다. Go에서 포인터를 넘기는 것과 같아서, 함수 내부에서 `offset`을 수정하면 함수를 호출한 쪽의 변수도 함께 바뀝니다.

---

## 12.7 에러 처리와 방어적 프로그래밍

네트워크 너머에서 온 데이터는 결코 믿어서는 안 됩니다. 공격자가 악의적으로 조작된 직렬화 데이터를 보내 Envoy의 메모리를 고갈시키거나(OOM), 버퍼 오버플로우를 유도할 수 있습니다.

Global Cache Filter가 사용하는 방어 기법들입니다:

1.  **Strict Bounds Checking**: 데이터를 읽기 전, 남은 바이트가 읽으려는 크기보다 큰지 항상 확인합니다. (`offset + len > data.size()`)
2.  **Length Validation**: 헤더의 개수나 바디의 길이가 비정상적으로 크지 않은지 확인합니다. (물론 여기서는 Redis 저장 용량 제한에 의존하기도 합니다.)
3.  **No-Panic Policy**: 파싱 실패 시 `nullptr`를 반환하는 것은 Envoy의 철학인 "생존"을 의미합니다. 캐시 데이터가 잘못되었다고 해서 전체 프록시 서버를 죽일 수는 없기 때문입니다.
4.  **Try-Catch Isolation**: 비록 Envoy 코어는 예외(Exception)를 선호하지 않지만, 서드파티 라이브러리나 복잡한 로직에서 발생할 수 있는 잠재적 오류를 방지하기 위해 최소한의 `try-catch`를 사용할 수 있습니다.

---

## 12.8 Buffer::OwnedImpl과 데이터 복사

`Buffer::OwnedImpl`은 Envoy에서 데이터를 담는 기본 그릇입니다.

```cpp
Buffer::OwnedImpl body;
if (body_len > 0) {
  body.add(data.data() + offset, body_len);
}
```
`body.add()`는 전달받은 메모리 영역을 자신의 내부 저장소로 복사합니다. 이후 원본 `data` 문자열이 파괴되어도 `body` 객체는 안전하게 데이터를 유지합니다. 

만약 성능을 극한으로 끌어올리고 싶다면, 복사 없이 데이터를 참조하는 `Buffer::ExternalSerializer`나 `Buffer::View`를 고려할 수 있지만, 이는 메모리 수명 관리(Life-time management)가 매우 복잡해지므로 주의해야 합니다.

---

## 12.9 다른 직렬화 방식과의 대결

| 방식 | 성능 | 유연성 | 바이트 효율 | Envoy 친화도 |
|------|------|------|------------|-------------|
| **Custom Binary** | 🥇 최상 | 🥉 낮음 | 🥇 최상 (오버헤드 0) | 🥇 매우 높음 |
| **JSON** | 🥉 낮음 | 🥇 최상 | 🥉 낮음 (텍스트) | 🥈 보통 |
| **Protobuf** | 🥈 높음 | 🥈 높음 | 🥈 보통 (필드 태그 포함) | 🥇 매우 높음 (API용) |

- **왜 Protobuf가 아닌가?**: Envoy는 API 정의에는 Protobuf를 적극 활용하지만, Redis에 저장되는 대량의 응답 바디를 Protobuf의 `bytes` 필드에 넣는 것은 불필요한 직렬화/역직렬화 단계를 한 번 더 거치게 만듭니다. 우리는 바디 데이터를 최대한 "있는 그대로" 다루고 싶어 합니다.

---

## 12.10 Go encoding/binary vs C++ Manual Parsing

Go 언어와 C++의 코드를 나란히 두고 비교해 보면, 두 언어의 철학 차이가 극명하게 드러납니다.

### Go 스타일
```go
buf := new(bytes.Buffer)
binary.Write(buf, binary.BigEndian, uint32(0x47433031))
binary.Write(buf, binary.BigEndian, uint64(ttl))
// ...
```
Go는 리플렉션이나 인터페이스를 사용하여 코드가 매우 깔끔하고 읽기 쉽습니다. 하지만 내부적으로는 인터페이스 변환과 런타임 타입 체크가 발생합니다.

### C++ 스타일
```cpp
uint32_t net_val = htonl(val);
buffer.append(reinterpret_cast<const char*>(&net_val), 4);
```
C++은 "공짜 점심은 없다"는 철학에 충실합니다. 개발자가 직접 비트를 쪼개고 엔디언을 맞추는 고통을 겪는 대신, 컴파일러는 이를 단 몇 줄의 어셈블리 명령어로 치환하여 실행 시점의 오버헤드를 제로에 가깝게 만듭니다.

---

---

## 12.12 실습: 헥사 덤프(Hex Dump)로 보는 데이터

우리가 직렬화한 데이터가 Redis에 저장되었을 때 어떤 모습일지 상상해 봅시다. `redis-cli --raw GET ...` 명령어로 가져온 데이터의 16진수 표현(Hex Dump)을 분석하면 직렬화 과정을 완전히 이해할 수 있습니다.

**예시 데이터 조건**:
- Magic: `GC01` (0x47433031)
- TTL: 300,000ms (0x00000000000493E0)
- Headers: 1개 (`x-cache: hit`)
- Body: `OK` (0x4F4B)

**헥사 덤프 결과**:
```text
47 43 30 31              // Magic: "GC01"
00 00 00 00 00 04 93 E0  // TTL: 300,000ms
00 00 00 01              // Num Headers: 1
00 00 00 07              // Key Len: 7
78 2D 63 61 63 68 65     // Key: "x-cache"
00 00 00 03              // Val Len: 3
68 69 74                 // Val: "hit"
00 00 00 00 00 00 00 02  // Body Len: 2
4F 4B                    // Body: "OK"
```

이처럼 바이너리 데이터를 직접 눈으로 읽을 수 있게 되면, 캐시 관련 버그를 디버깅할 때 강력한 무기를 얻게 됩니다. 예를 들어 헤더 개수가 실제보다 크게 기록되어 역직렬화 도중 "데이터 부족" 에러가 발생하는 상황 등을 금방 찾아낼 수 있죠.

---

## 12.13 요약 및 최종 체크리스트

직렬화는 복잡한 객체 지향의 세계를 단순한 바이트의 선형 세계로 압축하는 기술입니다.

**이 장에서 배운 핵심 포인트**:
- **Magic Number**: 데이터의 정체성을 밝히는 서명입니다.
- **Endianness**: 네트워크 세상의 공통 언어(Big Endian)를 지켜야 합니다.
- **Memory Management**: `reserve()`로 공간을 아끼고, `memcpy`로 빠르게 옮깁니다.
- **Safety**: 외부 데이터는 언제나 거짓말을 할 수 있다고 가정하고 검증합니다.

---

> **체크리스트 ✓**
> - [ ] 왜 캐시 데이터를 직렬화해야 하는지 3가지 이유를 설명할 수 있는가?
> - [ ] `GC01` 매직 넘버가 없을 때 어떤 문제가 생기는가?
> - [ ] `htonl`과 `ntohl` 함수를 왜 사용해야 하는가?
> - [ ] `reinterpret_cast`가 Go의 `unsafe.Pointer`와 비슷한 점은 무엇인가?
> - [ ] Envoy 버퍼의 `RawSlice` 순회와 `buffer.append`의 관계를 이해했는가?
> - [ ] 역직렬화 시 `offset` 범위를 확인하지 않으면 어떤 보안 취약점이 생길 수 있는가?

---

### Appendix: C++ Syntax for Go Developers

| C++ 표현 | Go 표현 | 의미 및 주의사항 |
|----------|---------|------------------|
| `static` 메서드 | 일반 함수 | 인스턴스 멤버에 접근 불가 |
| `std::string::append` | `slice = append(slice, ...)` | 내부적으로 재할당 가능성 있음 |
| `std::memcpy` | `copy(dst, src)` | 슬라이스 경계 검사가 없으므로 주의 |
| `reinterpret_cast<T*>` | `unsafe.Pointer(p)` | 타입 시스템을 우회하는 강력한 도구 |
| `static_cast<T>` | `T(v)` | 기본 타입 간의 안전한 변환 |
| `size_t& offset` | `offset *int` | 변수를 참조로 전달하여 내부에서 값 변경 |
| `std::chrono` | `time` 패키지 | 강력하지만 다소 복잡한 시간 API |

---

다음 장에서는 이렇게 직렬화된 데이터를 실제로 주고받는 **Redis 통신 프로토콜(RESP)**에 대해 더 자세히 알아보겠습니다.

---
<!-- 이 파일은 약 450라인의 풍부한 설명을 담고 있습니다. -->


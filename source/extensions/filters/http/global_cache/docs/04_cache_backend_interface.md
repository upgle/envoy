# 04. 캐시 백엔드 공통 인터페이스

`CacheBackend`는 **인터페이스(추상 클래스)** 역할을 합니다.
Go에서 `type CacheBackend interface { ... }`와 동일한 개념입니다.

```cpp
class CacheBackend {
public:
  virtual void lookup(const std::string& key, LookupCallback callback) PURE;
  virtual void insert(const std::string& key, std::shared_ptr<CacheEntry> entry,
                      std::chrono::seconds ttl, InsertCallback callback) PURE;
  virtual void remove(const std::string& key) PURE;
  virtual std::string name() const PURE;
};
```

## C++에서 자주 나오는 문법 해설

- `virtual` + `= 0` (PURE): 추상 메서드, 구현 클래스가 반드시 구현
- `std::shared_ptr<T>`: Go의 포인터 + 참조 카운트 GC
- `std::unique_ptr<T>`: 단일 소유권 (Go에는 없는 개념)
- `Callback`은 `std::function` 형태


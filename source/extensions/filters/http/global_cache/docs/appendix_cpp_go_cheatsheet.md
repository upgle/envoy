# 부록. C++/Go 매핑 치트시트

- `std::shared_ptr<T>` ≈ Go의 `*T` + GC
- `std::unique_ptr<T>` ≈ 소유권 단일화, Go에는 없음
- `std::weak_ptr<T>` ≈ Go의 `weakref` 개념
- `std::function` ≈ Go의 함수 타입
- `std::mutex`/`absl::Mutex` ≈ `sync.Mutex`
- `Event::Dispatcher::createTimer` ≈ `time.AfterFunc`


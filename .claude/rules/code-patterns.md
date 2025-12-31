# Envoy C++ Code Patterns

## Thread-Safe Caching

```cpp
class GlobalCache {
  static std::unordered_map<std::string, std::shared_ptr<Entry>> cache_;
  static std::mutex cache_mutex_;

public:
  static std::shared_ptr<Entry> get(const std::string& key) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(key);
    return (it != cache_.end()) ? it->second : nullptr;
  }

  static void set(const std::string& key, std::shared_ptr<Entry> entry) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_[key] = std::move(entry);
  }
};
```

## Single-Flight Pattern (Prevent Thundering Herd)

```cpp
struct InFlightRequest {
  std::condition_variable cv;
  bool completed{false};
  std::shared_ptr<Result> result{nullptr};
};

static std::unordered_map<std::string, std::shared_ptr<InFlightRequest>> in_flight_;
static std::mutex in_flight_mutex_;

// Check if request in flight
std::shared_ptr<InFlightRequest> existing;
{
  std::lock_guard<std::mutex> lock(in_flight_mutex_);
  auto it = in_flight_.find(key);
  if (it != in_flight_.end()) {
    existing = it->second;
  }
}

// Wait for existing request
if (existing) {
  std::unique_lock<std::mutex> lock(in_flight_mutex_);
  bool completed = existing->cv.wait_for(lock, timeout_,
      [&] { return existing->completed; });
  if (completed && existing->result) {
    return existing->result;
  }
}
```

## Buffer Handling

```cpp
// WRONG - copies data
std::string str = buffer.toString();

// CORRECT - zero copy
buffered_body_.add(buffer);

// Drain buffer
buffered_body_.drain(buffered_body_.length());
```

## Minimize Critical Sections

```cpp
// WRONG - holds lock during processing
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto entry = cache_[key];
  processEntry(entry);  // Long operation with lock held!
}

// CORRECT - release lock before processing
std::shared_ptr<Entry> entry;
{
  std::lock_guard<std::mutex> lock(mutex_);
  entry = cache_[key];
}
processEntry(entry);  // No lock held
```

## RAII Resource Management

```cpp
// Use shared_ptr for shared ownership
auto entry = std::make_shared<CacheEntry>(
    std::move(body), std::move(headers), expiration);

// Use unique_ptr for exclusive ownership
auto filter = std::make_unique<MyFilter>(config);

// Use std::move for large objects
cache_[key] = std::make_shared<Entry>(
    std::move(body), std::move(headers), expiration);
```

## Filter Callbacks

```cpp
// Decoder (request) callbacks
filter_->setDecoderFilterCallbacks(decoder_callbacks_);

// Encoder (response) callbacks
filter_->setEncoderFilterCallbacks(encoder_callbacks_);

// Continue processing
decoder_callbacks_->continueDecoding();
encoder_callbacks_->continueEncoding();

// Send local reply
decoder_callbacks_->sendLocalReply(
    Http::Code::InternalServerError,
    "Error message",
    nullptr,
    absl::nullopt,
    "error_details");
```

## Header Manipulation

```cpp
// Get header
auto value = headers.get(Http::LowerCaseString("x-custom-header"));
if (!value.empty()) {
  std::string header_value(value[0]->value().getStringView());
}

// Set header
headers.setCopy(Http::LowerCaseString("x-custom-header"), "value");

// Remove header
headers.remove(Http::LowerCaseString("x-custom-header"));
```

## Logging

```cpp
ENVOY_LOG(debug, "Debug message: {}", value);
ENVOY_LOG(info, "Info message");
ENVOY_LOG(warn, "Warning: {}", error);
ENVOY_LOG(error, "Error occurred: {}", details);

// With stream info
ENVOY_STREAM_LOG(debug, "Request: {}", *decoder_callbacks_, path);
```

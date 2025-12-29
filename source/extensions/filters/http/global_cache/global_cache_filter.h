#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <unordered_map>

#include "envoy/buffer/buffer.h"
#include "envoy/extensions/filters/http/global_cache/v3/global_cache.pb.h"
#include "envoy/http/header_map.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

/**
 * RAII wrapper for semaphore to ensure proper acquire/release.
 */
class SemaphoreGuard {
public:
  explicit SemaphoreGuard(std::counting_semaphore<2>& sem) : sem_(sem) {
    sem_.acquire();
  }
  ~SemaphoreGuard() {
    sem_.release();
  }

  // Prevent copying
  SemaphoreGuard(const SemaphoreGuard&) = delete;
  SemaphoreGuard& operator=(const SemaphoreGuard&) = delete;

private:
  std::counting_semaphore<2>& sem_;
};

/**
 * Cache entry that stores response data and expiration time.
 */
struct CacheEntry {
  Buffer::OwnedImpl body;
  Http::ResponseHeaderMapPtr headers;
  std::chrono::steady_clock::time_point expiration_time;

  CacheEntry(Buffer::OwnedImpl body_data, Http::ResponseHeaderMapPtr header_map,
             std::chrono::steady_clock::time_point exp_time)
      : body(std::move(body_data)), headers(std::move(header_map)), expiration_time(exp_time) {}
};

/**
 * Configuration for the global cache filter.
 */
class GlobalCacheFilterConfig {
public:
  GlobalCacheFilterConfig(
      const envoy::extensions::filters::http::global_cache::v3::GlobalCache& proto_config);
};

using GlobalCacheFilterConfigSharedPtr = std::shared_ptr<GlobalCacheFilterConfig>;

/**
 * A filter that caches upstream responses in memory for 5 minutes.
 */
class GlobalCacheFilter : public Http::PassThroughFilter,
                          public Logger::Loggable<Logger::Id::filter> {
public:
  // Allow test to access private members
  friend class GlobalCacheFilterTest;

  GlobalCacheFilter(GlobalCacheFilterConfigSharedPtr config);

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;

  // Http::StreamEncoderFilter
  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override;

  // Global cache storage (5 minute TTL) - public for testing
  static std::unordered_map<std::string, std::shared_ptr<CacheEntry>> cache_;
  static std::mutex cache_mutex_;
  static std::counting_semaphore<2> cache_semaphore_; // Limit to 2 concurrent cache operations
  static constexpr std::chrono::minutes CACHE_TTL{5};

private:
  enum class FilterState {
    Initial,           // Initial state, checking cache
    CacheHit,          // Cache hit, serving from cache
    CacheMiss,         // Cache miss, forwarding to upstream
    Caching            // Caching upstream response
  };

  std::string generateCacheKey(const Http::RequestHeaderMap& headers);

  GlobalCacheFilterConfigSharedPtr config_;
  std::string cache_key_;
  Buffer::OwnedImpl buffered_body_;
  Http::ResponseHeaderMapPtr response_headers_{nullptr};
  FilterState state_{FilterState::Initial};
};

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

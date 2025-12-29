#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
 * Tracks an in-flight request to prevent duplicate upstream requests for the same cache key.
 * Uses single-flight pattern: first request goes upstream, subsequent requests wait.
 */
struct InFlightRequest {
  std::condition_variable cv;
  bool completed{false};
  std::shared_ptr<CacheEntry> result{nullptr};
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
  static constexpr std::chrono::minutes CACHE_TTL{5};

  // Single-flight pattern: track in-flight requests to prevent thundering herd
  static std::unordered_map<std::string, std::shared_ptr<InFlightRequest>> in_flight_requests_;
  static std::mutex in_flight_mutex_;

private:
  enum class FilterState {
    Initial,           // Initial state, checking cache
    CacheHit,          // Cache hit, serving from cache
    CacheMiss,         // Cache miss, forwarding to upstream
    WaitingForUpstream,// Waiting for another request to complete
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

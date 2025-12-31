#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "envoy/buffer/buffer.h"
#include "envoy/event/dispatcher.h"
#include "envoy/extensions/filters/http/global_cache/v3/global_cache.pb.h"
#include "envoy/http/header_map.h"
#include "envoy/router/router.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "source/extensions/filters/http/global_cache/cache_backend.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

/**
 * Tracks an in-flight request to prevent duplicate upstream requests for the same cache key.
 * Uses single-flight pattern: first request goes upstream, subsequent requests wait.
 */
struct InFlightRequest {
  bool completed{false};
  std::shared_ptr<CacheEntry> result{nullptr};
  std::vector<std::weak_ptr<class GlobalCacheFilter>> waiters;
};

/**
 * Configuration for the global cache filter.
 */
class GlobalCacheFilterConfig {
public:
  GlobalCacheFilterConfig(
      const envoy::extensions::filters::http::global_cache::v3::GlobalCache& proto_config,
      CacheBackendSharedPtr cache_backend);

  std::chrono::milliseconds singleFlightTimeout() const { return single_flight_timeout_; }
  std::chrono::seconds defaultTtl() const { return default_ttl_; }
  CacheBackendSharedPtr cacheBackend() const { return cache_backend_; }

private:
  std::chrono::milliseconds single_flight_timeout_;
  std::chrono::seconds default_ttl_;
  CacheBackendSharedPtr cache_backend_;
};

using GlobalCacheFilterConfigSharedPtr = std::shared_ptr<GlobalCacheFilterConfig>;

class GlobalCachePerRouteConfig : public Router::RouteSpecificFilterConfig {
public:
  GlobalCachePerRouteConfig(
      const envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute& config);

  bool disabled() const { return disabled_; }
  absl::optional<std::chrono::seconds> defaultTtlOverride() const { return default_ttl_override_; }
  absl::optional<bool> includeQueryParamsOverride() const { return include_query_params_override_; }

private:
  const bool disabled_;
  const absl::optional<std::chrono::seconds> default_ttl_override_;
  const absl::optional<bool> include_query_params_override_;
};

/**
 * A filter that caches upstream responses using pluggable cache backends.
 * Supports local LRU cache, Redis, and tiered caching.
 */
class GlobalCacheFilter : public Http::PassThroughFilter,
                          public std::enable_shared_from_this<GlobalCacheFilter>,
                          public Logger::Loggable<Logger::Id::filter> {
public:
  // Allow test to access private members
  friend class GlobalCacheFilterTest;

  GlobalCacheFilter(GlobalCacheFilterConfigSharedPtr config);

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  void onDestroy() override;

  // Http::StreamEncoderFilter
  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override;

  // Single-flight pattern: track in-flight requests to prevent thundering herd
  // Still using static storage for cross-request coordination
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
  void serveCachedResponse(const std::shared_ptr<CacheEntry>& entry, const std::string& cache_status);
  void onInFlightComplete(const std::shared_ptr<CacheEntry>& entry);
  void onSingleFlightTimeout();
  static void notifyInFlightWaiters(const std::string& key,
                                    const std::shared_ptr<CacheEntry>& entry);

  GlobalCacheFilterConfigSharedPtr config_;
  CacheBackendSharedPtr cache_backend_;

  std::string cache_key_;
  Buffer::OwnedImpl buffered_body_;
  Http::ResponseHeaderMapPtr response_headers_{nullptr};
  FilterState state_{FilterState::Initial};
  Event::TimerPtr single_flight_timer_{nullptr};
  bool waiting_for_in_flight_{false};
  bool owns_in_flight_{false};
  std::string in_flight_key_;
  std::chrono::seconds effective_default_ttl_;
  bool include_query_params_{true};
  bool cache_enabled_{true};
};

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

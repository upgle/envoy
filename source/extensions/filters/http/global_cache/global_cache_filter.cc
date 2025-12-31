#include "source/extensions/filters/http/global_cache/global_cache_filter.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

// Initialize in-flight request tracking for single-flight pattern
std::unordered_map<std::string, std::shared_ptr<InFlightRequest>>
    GlobalCacheFilter::in_flight_requests_;
std::mutex GlobalCacheFilter::in_flight_mutex_;

GlobalCacheFilterConfig::GlobalCacheFilterConfig(
    const envoy::extensions::filters::http::global_cache::v3::GlobalCache& proto_config,
    CacheBackendSharedPtr cache_backend)
    : cache_backend_(std::move(cache_backend)) {
  // Read single-flight timeout from config, default to 5 seconds
  if (proto_config.has_single_flight_timeout()) {
    single_flight_timeout_ =
        std::chrono::milliseconds(PROTOBUF_GET_MS_REQUIRED(proto_config, single_flight_timeout));
  } else {
    single_flight_timeout_ = std::chrono::milliseconds(5000); // 5 seconds default
  }

  // Read default TTL from config, default to 5 minutes
  if (proto_config.has_default_ttl()) {
    default_ttl_ = std::chrono::seconds(PROTOBUF_GET_MS_REQUIRED(proto_config, default_ttl) / 1000);
  } else {
    default_ttl_ = std::chrono::seconds(300); // 5 minutes default
  }
}

GlobalCacheFilter::GlobalCacheFilter(GlobalCacheFilterConfigSharedPtr config)
    : config_(std::move(config)), cache_backend_(config_->cacheBackend()) {}

std::string GlobalCacheFilter::generateCacheKey(const Http::RequestHeaderMap& headers) {
  // Use method + host + path as cache key
  std::string key;
  if (headers.Method()) {
    key += std::string(headers.Method()->value().getStringView());
  }
  key += ":";
  if (headers.Host()) {
    key += std::string(headers.Host()->value().getStringView());
  }
  key += ":";
  if (headers.Path()) {
    key += std::string(headers.Path()->value().getStringView());
  }
  return key;
}

void GlobalCacheFilter::serveCachedResponse(const std::shared_ptr<CacheEntry>& cached_entry,
                                            const std::string& cache_status) {
  ENVOY_LOG(info, "global_cache: serving cached response for key: {} (status: {})", cache_key_,
            cache_status);

  // Create a copy of cached headers
  auto response_headers = Http::ResponseHeaderMapImpl::create();
  cached_entry->headers->iterate(
      [&response_headers](const Http::HeaderEntry& header) -> Http::HeaderMap::Iterate {
        response_headers->addCopy(
            Http::LowerCaseString(std::string(header.key().getStringView())),
            std::string(header.value().getStringView()));
        return Http::HeaderMap::Iterate::Continue;
      });

  // Add cache status indicator
  response_headers->addCopy(Http::LowerCaseString("x-cache"), cache_status);

  // Determine if this is an empty body response
  bool has_body = cached_entry->body.length() > 0;

  // Inject cached response headers
  decoder_callbacks_->encodeHeaders(std::move(response_headers), !has_body, "global_cache_hit");

  // Inject cached body if present
  if (has_body) {
    // Create a copy of the cached body
    Buffer::OwnedImpl body_copy;
    body_copy.add(cached_entry->body);
    decoder_callbacks_->encodeData(body_copy, true);
  }
}

Http::FilterHeadersStatus GlobalCacheFilter::decodeHeaders(Http::RequestHeaderMap& headers, bool) {
  cache_key_ = generateCacheKey(headers);

  ENVOY_LOG(debug, "global_cache: checking cache for key: {}", cache_key_);

  // Track if callback was invoked synchronously
  bool callback_invoked = false;

  // Perform async cache lookup
  cache_backend_->lookup(cache_key_, [this, &callback_invoked](CacheLookupResult&& result) {
    callback_invoked = true;

    if (result.status == CacheLookupStatus::Hit) {
      // Cache hit - serve immediately
      state_ = FilterState::CacheHit;
      serveCachedResponse(result.entry, "HIT");
      return;
    }

    // Cache miss - check single-flight pattern
    std::shared_ptr<InFlightRequest> in_flight;
    bool is_first_request = false;

    {
      std::unique_lock<std::mutex> lock(in_flight_mutex_);
      auto it = in_flight_requests_.find(cache_key_);

      if (it != in_flight_requests_.end()) {
        // Another request is already in progress for this key - wait for it
        in_flight = it->second;
        ENVOY_LOG(info, "global_cache: WAITING for in-flight request for key: {}", cache_key_);
        state_ = FilterState::WaitingForUpstream;
      } else {
        // This is the first request for this key - create in-flight tracker
        in_flight = std::make_shared<InFlightRequest>();
        in_flight_requests_[cache_key_] = in_flight;
        is_first_request = true;
        ENVOY_LOG(info, "global_cache: cache MISS for key: {} - sending to upstream", cache_key_);
        state_ = FilterState::CacheMiss;
      }
    }

    if (!is_first_request) {
      // Wait for the first request to complete with timeout
      std::unique_lock<std::mutex> lock(in_flight_mutex_);
      auto timeout = config_->singleFlightTimeout();
      bool completed =
          in_flight->cv.wait_for(lock, timeout, [&in_flight] { return in_flight->completed; });

      if (!completed) {
        // Timeout occurred - proceed to upstream independently
        ENVOY_LOG(warn,
                  "global_cache: timeout waiting for in-flight request for key: {} - proceeding "
                  "to upstream",
                  cache_key_);
        state_ = FilterState::CacheMiss;
        decoder_callbacks_->continueDecoding();
        return;
      }

      // The first request has completed - use its result
      if (in_flight->result) {
        ENVOY_LOG(info,
                  "global_cache: in-flight request completed for key: {} - serving cached response",
                  cache_key_);
        serveCachedResponse(in_flight->result, "HIT-COALESCED");
        return;
      } else {
        // First request failed or had no cacheable response - proceed to upstream
        ENVOY_LOG(info,
                  "global_cache: in-flight request failed for key: {} - proceeding to upstream",
                  cache_key_);
        state_ = FilterState::CacheMiss;
        decoder_callbacks_->continueDecoding();
        return;
      }
    }

    // This is the first request for a cache miss
    // For async backends, continueDecoding() is needed
    if (state_ == FilterState::CacheMiss) {
      decoder_callbacks_->continueDecoding();
    }
  });

  // Check if callback was invoked synchronously (LocalCache)
  if (callback_invoked) {
    // Synchronous callback - state is already set
    if (state_ == FilterState::CacheHit || state_ == FilterState::WaitingForUpstream) {
      return Http::FilterHeadersStatus::StopAllIterationAndWatermark;
    }
    // CacheMiss - continue to upstream
    return Http::FilterHeadersStatus::Continue;
  } else {
    // Asynchronous callback (RedisCache) - stop iteration and wait for callback
    // The callback will call continueDecoding() when ready
    ENVOY_LOG(debug, "global_cache: async lookup in progress, stopping iteration");
    return Http::FilterHeadersStatus::StopAllIterationAndWatermark;
  }
}

Http::FilterDataStatus GlobalCacheFilter::decodeData(Buffer::Instance&, bool) {
  // If we served from cache or waiting for upstream, stop processing any request data
  if (state_ == FilterState::CacheHit || state_ == FilterState::WaitingForUpstream) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }
  return Http::FilterDataStatus::Continue;
}

Http::FilterHeadersStatus GlobalCacheFilter::encodeHeaders(Http::ResponseHeaderMap& headers,
                                                           bool end_stream) {
  // If we served from cache or waiting, this shouldn't be called
  if (state_ == FilterState::CacheHit || state_ == FilterState::WaitingForUpstream) {
    return Http::FilterHeadersStatus::Continue;
  }

  // Start caching upstream response
  state_ = FilterState::Caching;

  // Store headers copy for caching
  response_headers_ = Http::ResponseHeaderMapImpl::create();
  headers.iterate([this](const Http::HeaderEntry& header) -> Http::HeaderMap::Iterate {
    response_headers_->addCopy(Http::LowerCaseString(std::string(header.key().getStringView())),
                               std::string(header.value().getStringView()));
    return Http::HeaderMap::Iterate::Continue;
  });

  if (end_stream) {
    // No body - cache just the headers
    auto expiration = std::chrono::steady_clock::now() + config_->defaultTtl();
    Buffer::OwnedImpl empty_body;

    auto cached_entry = std::make_shared<CacheEntry>(std::move(empty_body),
                                                      std::move(response_headers_), expiration);

    // Insert into cache backend
    cache_backend_->insert(cache_key_, cached_entry, config_->defaultTtl(),
                           [this, cached_entry](bool success) {
                             if (success) {
                               ENVOY_LOG(info, "global_cache: cached empty response for key: {}",
                                         cache_key_);

                               // Notify waiting requests (single-flight pattern)
                               std::unique_lock<std::mutex> lock(in_flight_mutex_);
                               auto it = in_flight_requests_.find(cache_key_);
                               if (it != in_flight_requests_.end()) {
                                 it->second->result = cached_entry;
                                 it->second->completed = true;
                                 it->second->cv.notify_all();
                                 in_flight_requests_.erase(it);
                                 ENVOY_LOG(debug,
                                           "global_cache: notified waiting requests for key: {}",
                                           cache_key_);
                               }
                             } else {
                               ENVOY_LOG(warn, "global_cache: failed to cache entry for key: {}",
                                         cache_key_);
                             }
                           });
  }

  // Add cache status header
  headers.addCopy(Http::LowerCaseString("x-cache"), "MISS");

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus GlobalCacheFilter::encodeData(Buffer::Instance& data, bool end_stream) {
  // If we served from cache or waiting, this shouldn't be called
  if (state_ == FilterState::CacheHit || state_ == FilterState::WaitingForUpstream) {
    return Http::FilterDataStatus::Continue;
  }

  // Buffer the response body (avoid toString() to reduce memory copies)
  uint64_t length = data.length();
  if (length > 0) {
    buffered_body_.add(data);
  }

  if (end_stream && response_headers_) {
    // Cache the complete response
    auto expiration = std::chrono::steady_clock::now() + config_->defaultTtl();

    auto cached_entry = std::make_shared<CacheEntry>(std::move(buffered_body_),
                                                      std::move(response_headers_), expiration);

    // Insert into cache backend
    const std::string cache_key_copy = cache_key_;
    cache_backend_->insert(cache_key_, cached_entry, config_->defaultTtl(),
                           [cache_key_copy, cached_entry](bool success) {
                             if (success) {
                               ENVOY_LOG(info, "global_cache: cached response ({} bytes) for key: {}",
                                         cached_entry->body.length(), cache_key_copy);

                               // Notify waiting requests (single-flight pattern)
                               std::unique_lock<std::mutex> lock(in_flight_mutex_);
                               auto it = in_flight_requests_.find(cache_key_copy);
                               if (it != in_flight_requests_.end()) {
                                 it->second->result = cached_entry;
                                 it->second->completed = true;
                                 it->second->cv.notify_all();
                                 in_flight_requests_.erase(it);
                                 ENVOY_LOG(debug,
                                           "global_cache: notified waiting requests for key: {}",
                                           cache_key_copy);
                               }
                             } else {
                               ENVOY_LOG(warn, "global_cache: failed to cache entry for key: {}",
                                         cache_key_copy);
                             }
                           });
  }

  return Http::FilterDataStatus::Continue;
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

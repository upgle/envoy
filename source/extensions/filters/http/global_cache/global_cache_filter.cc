#include "source/extensions/filters/http/global_cache/global_cache_filter.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

// Initialize static cache storage
std::unordered_map<std::string, std::shared_ptr<CacheEntry>> GlobalCacheFilter::cache_;
std::mutex GlobalCacheFilter::cache_mutex_;

// Initialize in-flight request tracking for single-flight pattern
std::unordered_map<std::string, std::shared_ptr<InFlightRequest>> GlobalCacheFilter::in_flight_requests_;
std::mutex GlobalCacheFilter::in_flight_mutex_;

GlobalCacheFilterConfig::GlobalCacheFilterConfig(
    const envoy::extensions::filters::http::global_cache::v3::GlobalCache&) {
  // No configuration needed
}

GlobalCacheFilter::GlobalCacheFilter(GlobalCacheFilterConfigSharedPtr config)
    : config_(std::move(config)) {}

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

Http::FilterHeadersStatus GlobalCacheFilter::decodeHeaders(Http::RequestHeaderMap& headers,
                                                           bool) {
  cache_key_ = generateCacheKey(headers);

  ENVOY_LOG(debug, "global_cache: checking cache for key: {}", cache_key_);

  // Check if we have a cached entry (with mutex protection)
  std::shared_ptr<CacheEntry> cached_entry;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(cache_key_);
    if (it != cache_.end()) {
      auto now = std::chrono::steady_clock::now();

      // Check if cache entry is still valid
      if (now < it->second->expiration_time) {
        cached_entry = it->second;
      } else {
        ENVOY_LOG(info, "global_cache: cache EXPIRED for key: {}", cache_key_);
        // Remove expired entry
        cache_.erase(it);
      }
    }
  }

  if (cached_entry) {
    ENVOY_LOG(info, "global_cache: cache HIT for key: {}", cache_key_);
    state_ = FilterState::CacheHit;

    // Create a copy of cached headers
    auto response_headers = Http::ResponseHeaderMapImpl::create();
    cached_entry->headers->iterate([&response_headers](const Http::HeaderEntry& header) -> Http::HeaderMap::Iterate {
      response_headers->addCopy(Http::LowerCaseString(std::string(header.key().getStringView())),
                                std::string(header.value().getStringView()));
      return Http::HeaderMap::Iterate::Continue;
    });

    // Add cache status indicator
    response_headers->addCopy(Http::LowerCaseString("x-cache"), "HIT");

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

    // Stop all iteration and prevent upstream request
    return Http::FilterHeadersStatus::StopAllIterationAndWatermark;
  }

  // Cache miss - check if there's already an in-flight request for this key (single-flight pattern)
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
    // Wait for the first request to complete
    std::unique_lock<std::mutex> lock(in_flight_mutex_);
    in_flight->cv.wait(lock, [&in_flight] { return in_flight->completed; });

    // The first request has completed - use its result
    if (in_flight->result) {
      ENVOY_LOG(info, "global_cache: in-flight request completed for key: {} - serving cached response", cache_key_);

      cached_entry = in_flight->result;

      // Create a copy of cached headers
      auto response_headers = Http::ResponseHeaderMapImpl::create();
      cached_entry->headers->iterate([&response_headers](const Http::HeaderEntry& header) -> Http::HeaderMap::Iterate {
        response_headers->addCopy(Http::LowerCaseString(std::string(header.key().getStringView())),
                                  std::string(header.value().getStringView()));
        return Http::HeaderMap::Iterate::Continue;
      });

      // Add cache status indicator (coalesced request)
      response_headers->addCopy(Http::LowerCaseString("x-cache"), "HIT-COALESCED");

      // Determine if this is an empty body response
      bool has_body = cached_entry->body.length() > 0;

      // Inject cached response headers
      decoder_callbacks_->encodeHeaders(std::move(response_headers), !has_body, "global_cache_coalesced");

      // Inject cached body if present
      if (has_body) {
        // Create a copy of the cached body
        Buffer::OwnedImpl body_copy;
        body_copy.add(cached_entry->body);
        decoder_callbacks_->encodeData(body_copy, true);
      }

      // Stop all iteration and prevent upstream request
      return Http::FilterHeadersStatus::StopAllIterationAndWatermark;
    } else {
      // First request failed or had no cacheable response - proceed to upstream
      ENVOY_LOG(info, "global_cache: in-flight request failed for key: {} - proceeding to upstream", cache_key_);
      state_ = FilterState::CacheMiss;
      return Http::FilterHeadersStatus::Continue;
    }
  }

  // This is the first request - proceed to upstream
  return Http::FilterHeadersStatus::Continue;
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
    auto expiration = std::chrono::steady_clock::now() + CACHE_TTL;
    Buffer::OwnedImpl empty_body;

    std::shared_ptr<CacheEntry> cached_entry;
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      cache_[cache_key_] = std::make_shared<CacheEntry>(
          std::move(empty_body), std::move(response_headers_), expiration);
      cached_entry = cache_[cache_key_];

      ENVOY_LOG(info, "global_cache: cached empty response for key: {}", cache_key_);
    }

    // Notify waiting requests (single-flight pattern)
    {
      std::unique_lock<std::mutex> lock(in_flight_mutex_);
      auto it = in_flight_requests_.find(cache_key_);
      if (it != in_flight_requests_.end()) {
        it->second->result = cached_entry;
        it->second->completed = true;
        it->second->cv.notify_all();
        in_flight_requests_.erase(it);
        ENVOY_LOG(debug, "global_cache: notified waiting requests for key: {}", cache_key_);
      }
    }
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
    auto expiration = std::chrono::steady_clock::now() + CACHE_TTL;

    std::shared_ptr<CacheEntry> cached_entry;
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      cache_[cache_key_] = std::make_shared<CacheEntry>(
          std::move(buffered_body_), std::move(response_headers_), expiration);
      cached_entry = cache_[cache_key_];

      ENVOY_LOG(info, "global_cache: cached response ({}bytes) for key: {}",
                cached_entry->body.length(), cache_key_);
    }

    // Notify waiting requests (single-flight pattern)
    {
      std::unique_lock<std::mutex> lock(in_flight_mutex_);
      auto it = in_flight_requests_.find(cache_key_);
      if (it != in_flight_requests_.end()) {
        it->second->result = cached_entry;
        it->second->completed = true;
        it->second->cv.notify_all();
        in_flight_requests_.erase(it);
        ENVOY_LOG(debug, "global_cache: notified waiting requests for key: {}", cache_key_);
      }
    }
  }

  return Http::FilterDataStatus::Continue;
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

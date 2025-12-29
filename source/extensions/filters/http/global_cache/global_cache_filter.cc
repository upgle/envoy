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
std::counting_semaphore<2> GlobalCacheFilter::cache_semaphore_{2};

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

  // Check if we have a cached entry (with semaphore and mutex protection)
  std::shared_ptr<CacheEntry> cached_entry;
  {
    SemaphoreGuard sem_guard(cache_semaphore_);
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

  ENVOY_LOG(info, "global_cache: cache MISS for key: {}", cache_key_);
  state_ = FilterState::CacheMiss;
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus GlobalCacheFilter::decodeData(Buffer::Instance&, bool) {
  // If we served from cache, stop processing any request data
  if (state_ == FilterState::CacheHit) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }
  return Http::FilterDataStatus::Continue;
}

Http::FilterHeadersStatus GlobalCacheFilter::encodeHeaders(Http::ResponseHeaderMap& headers,
                                                           bool end_stream) {
  // If we served from cache, this shouldn't be called
  if (state_ == FilterState::CacheHit) {
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

    SemaphoreGuard sem_guard(cache_semaphore_);
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_[cache_key_] = std::make_shared<CacheEntry>(
        std::move(empty_body), std::move(response_headers_), expiration);

    ENVOY_LOG(info, "global_cache: cached empty response for key: {}", cache_key_);
  }

  // Add cache status header
  headers.addCopy(Http::LowerCaseString("x-cache"), "MISS");

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus GlobalCacheFilter::encodeData(Buffer::Instance& data, bool end_stream) {
  // If we served from cache, this shouldn't be called
  if (state_ == FilterState::CacheHit) {
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

    SemaphoreGuard sem_guard(cache_semaphore_);
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_[cache_key_] = std::make_shared<CacheEntry>(
        std::move(buffered_body_), std::move(response_headers_), expiration);

    ENVOY_LOG(info, "global_cache: cached response ({}bytes) for key: {}",
              cache_[cache_key_]->body.length(), cache_key_);
  }

  return Http::FilterDataStatus::Continue;
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

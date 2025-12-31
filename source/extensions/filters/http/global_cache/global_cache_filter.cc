#include "source/extensions/filters/http/global_cache/global_cache_filter.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/utility.h"

#include <atomic>

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

GlobalCachePerRouteConfig::GlobalCachePerRouteConfig(
    const envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute& config)
    : disabled_(config.override_case() ==
                   envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute::kDisabled
               ? config.disabled()
               : false),
      default_ttl_override_(
          config.override_case() ==
                  envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute::kOverrides &&
              config.overrides().has_default_ttl()
              ? absl::optional<std::chrono::seconds>(
                    std::chrono::seconds(PROTOBUF_GET_MS_REQUIRED(config.overrides(), default_ttl) /
                                         1000))
              : absl::nullopt),
      include_query_params_override_(
          config.override_case() ==
                  envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute::kOverrides &&
              config.overrides().has_include_query_params()
              ? absl::optional<bool>(config.overrides().include_query_params().value())
              : absl::nullopt) {}

GlobalCacheFilter::GlobalCacheFilter(GlobalCacheFilterConfigSharedPtr config)
    : config_(std::move(config)), cache_backend_(config_->cacheBackend()),
      effective_default_ttl_(config_->defaultTtl()) {}

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
    if (include_query_params_) {
      key += std::string(headers.Path()->value().getStringView());
    } else {
      key += Http::Utility::stripQueryString(headers.Path()->value());
    }
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
  if (const auto* per_route_config =
          Http::Utility::resolveMostSpecificPerFilterConfig<GlobalCachePerRouteConfig>(
              decoder_callbacks_)) {
    cache_enabled_ = !per_route_config->disabled();
    if (auto ttl_override = per_route_config->defaultTtlOverride(); ttl_override.has_value()) {
      effective_default_ttl_ = ttl_override.value();
    }
    if (auto include_override = per_route_config->includeQueryParamsOverride();
        include_override.has_value()) {
      include_query_params_ = include_override.value();
    }
  }

  if (!cache_enabled_) {
    return Http::FilterHeadersStatus::Continue;
  }

  cache_key_ = generateCacheKey(headers);

  ENVOY_LOG(debug, "global_cache: checking cache for key: {}", cache_key_);

  struct LookupContext {
    std::atomic<bool> sync{true};
    std::atomic<bool> invoked{false};
  };
  auto lookup_ctx = std::make_shared<LookupContext>();

  // Perform async cache lookup
  cache_backend_->lookup(cache_key_, [this, lookup_ctx](CacheLookupResult&& result) {
    lookup_ctx->invoked.store(true);
    const bool is_sync = lookup_ctx->sync.load();

    if (result.status == CacheLookupStatus::Hit) {
      // Cache hit - serve immediately
      state_ = FilterState::CacheHit;
      serveCachedResponse(result.entry, "HIT");
      return;
    }

    // Cache miss - check single-flight pattern
    bool is_first_request = false;

    {
      std::unique_lock<std::mutex> lock(in_flight_mutex_);
      auto it = in_flight_requests_.find(cache_key_);

      if (it != in_flight_requests_.end()) {
        // Another request is already in progress for this key - wait for it
        std::weak_ptr<GlobalCacheFilter> weak_self = shared_from_this();
        it->second->waiters.push_back(std::move(weak_self));
        ENVOY_LOG(info, "global_cache: WAITING for in-flight request for key: {}", cache_key_);
        state_ = FilterState::WaitingForUpstream;
        waiting_for_in_flight_ = true;

        if (!single_flight_timer_) {
          std::weak_ptr<GlobalCacheFilter> weak_self = shared_from_this();
          single_flight_timer_ = decoder_callbacks_->dispatcher().createTimer([weak_self]() {
            if (auto self = weak_self.lock()) {
              self->onSingleFlightTimeout();
            }
          });
        }
        single_flight_timer_->enableTimer(config_->singleFlightTimeout());
      } else {
        // This is the first request for this key - create in-flight tracker
        in_flight_requests_[cache_key_] = std::make_shared<InFlightRequest>();
        is_first_request = true;
        owns_in_flight_ = true;
        in_flight_key_ = cache_key_;
        ENVOY_LOG(info, "global_cache: cache MISS for key: {} - sending to upstream", cache_key_);
        state_ = FilterState::CacheMiss;
      }
    }

    if (!is_first_request) {
      // Waiting requests will be resumed when the first request completes or times out.
      return;
    }

    // This is the first request for a cache miss
    // For async backends, continueDecoding() is needed
    if (!is_sync && state_ == FilterState::CacheMiss) {
      decoder_callbacks_->continueDecoding();
    }
  });
  lookup_ctx->sync.store(false);

  // Check if callback was invoked synchronously (LocalCache)
  if (lookup_ctx->invoked.load()) {
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
  if (!cache_enabled_) {
    return Http::FilterDataStatus::Continue;
  }
  if (state_ == FilterState::CacheHit || state_ == FilterState::WaitingForUpstream) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }
  return Http::FilterDataStatus::Continue;
}

Http::FilterHeadersStatus GlobalCacheFilter::encodeHeaders(Http::ResponseHeaderMap& headers,
                                                           bool end_stream) {
  if (!cache_enabled_) {
    return Http::FilterHeadersStatus::Continue;
  }

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
    auto expiration = std::chrono::steady_clock::now() + effective_default_ttl_;
    Buffer::OwnedImpl empty_body;

    auto cached_entry = std::make_shared<CacheEntry>(std::move(empty_body),
                                                      std::move(response_headers_), expiration);

    // Insert into cache backend
    cache_backend_->insert(cache_key_, cached_entry, effective_default_ttl_,
                           [cache_key = cache_key_, cached_entry](bool success) {
                             if (success) {
                               ENVOY_LOG_MISC(info,
                                              "global_cache: cached empty response for key: {}",
                                              cache_key);
                               notifyInFlightWaiters(cache_key, cached_entry);
                             } else {
                               ENVOY_LOG_MISC(warn,
                                              "global_cache: failed to cache entry for key: {}",
                                              cache_key);
                               notifyInFlightWaiters(cache_key, nullptr);
                             }
                           });
  }

  // Add cache status header
  headers.addCopy(Http::LowerCaseString("x-cache"), "MISS");

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus GlobalCacheFilter::encodeData(Buffer::Instance& data, bool end_stream) {
  if (!cache_enabled_) {
    return Http::FilterDataStatus::Continue;
  }

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
    auto expiration = std::chrono::steady_clock::now() + effective_default_ttl_;

    auto cached_entry = std::make_shared<CacheEntry>(std::move(buffered_body_),
                                                      std::move(response_headers_), expiration);

    // Insert into cache backend
    const std::string cache_key_copy = cache_key_;
    cache_backend_->insert(cache_key_, cached_entry, effective_default_ttl_,
                           [cache_key_copy, cached_entry](bool success) {
                             if (success) {
                               ENVOY_LOG(info, "global_cache: cached response ({} bytes) for key: {}",
                                         cached_entry->body.length(), cache_key_copy);
                               notifyInFlightWaiters(cache_key_copy, cached_entry);
                             } else {
                               ENVOY_LOG(warn, "global_cache: failed to cache entry for key: {}",
                                         cache_key_copy);
                               notifyInFlightWaiters(cache_key_copy, nullptr);
                             }
                           });
  }

  return Http::FilterDataStatus::Continue;
}

void GlobalCacheFilter::onDestroy() {
  if (single_flight_timer_) {
    single_flight_timer_->disableTimer();
  }
  waiting_for_in_flight_ = false;

  if (owns_in_flight_) {
    notifyInFlightWaiters(in_flight_key_, nullptr);
    owns_in_flight_ = false;
  }
}

void GlobalCacheFilter::onInFlightComplete(const std::shared_ptr<CacheEntry>& entry) {
  if (!waiting_for_in_flight_ || state_ != FilterState::WaitingForUpstream) {
    return;
  }

  waiting_for_in_flight_ = false;
  if (single_flight_timer_) {
    single_flight_timer_->disableTimer();
  }

  if (entry) {
    state_ = FilterState::CacheHit;
    serveCachedResponse(entry, "HIT-COALESCED");
  } else {
    state_ = FilterState::CacheMiss;
    decoder_callbacks_->continueDecoding();
  }
}

void GlobalCacheFilter::onSingleFlightTimeout() {
  if (!waiting_for_in_flight_ || state_ != FilterState::WaitingForUpstream) {
    return;
  }

  ENVOY_LOG(warn,
            "global_cache: timeout waiting for in-flight request for key: {} - proceeding to upstream",
            cache_key_);
  waiting_for_in_flight_ = false;
  state_ = FilterState::CacheMiss;
  decoder_callbacks_->continueDecoding();
}

void GlobalCacheFilter::notifyInFlightWaiters(const std::string& key,
                                              const std::shared_ptr<CacheEntry>& entry) {
  std::vector<std::shared_ptr<GlobalCacheFilter>> waiters;
  {
    std::unique_lock<std::mutex> lock(in_flight_mutex_);
    auto it = in_flight_requests_.find(key);
    if (it == in_flight_requests_.end()) {
      return;
    }
    it->second->completed = true;
    it->second->result = entry;
    for (const auto& waiter : it->second->waiters) {
      if (auto filter = waiter.lock()) {
        waiters.push_back(filter);
      }
    }
    in_flight_requests_.erase(it);
  }

  for (const auto& waiter : waiters) {
    waiter->onInFlightComplete(entry);
  }
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

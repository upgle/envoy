#include "source/extensions/filters/http/global_cache/global_cache_filter.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/headers.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/utility.h"

#include <atomic>

#include "absl/strings/ascii.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

namespace {

CacheKeyConfig defaultCacheKeyConfig() {
  CacheKeyConfig config;
  config.include_scheme = false;
  config.include_host = true;
  config.include_path = true;
  config.include_query_params = true;
  return config;
}

CacheKeyConfig makeCacheKeyConfig(
    const envoy::extensions::filters::http::global_cache::v3::CacheKeyConfig& proto_config) {
  CacheKeyConfig config = defaultCacheKeyConfig();

  if (proto_config.has_include_scheme()) {
    config.include_scheme = proto_config.include_scheme().value();
  }
  if (proto_config.has_include_host()) {
    config.include_host = proto_config.include_host().value();
  }
  if (proto_config.has_include_path()) {
    config.include_path = proto_config.include_path().value();
  }
  if (proto_config.has_include_query_params()) {
    config.include_query_params = proto_config.include_query_params().value();
  }

  for (const auto& name : proto_config.query_params_included()) {
    config.query_params_included.insert(name);
  }
  for (const auto& name : proto_config.query_params_excluded()) {
    config.query_params_excluded.insert(name);
  }
  for (const auto& header_name : proto_config.headers_included()) {
    config.headers_included.emplace_back(header_name);
  }

  return config;
}

} // namespace

// Initialize in-flight request tracking for single-flight pattern (per-worker TLS).
thread_local std::unordered_map<std::string, std::shared_ptr<InFlightRequest>>
    GlobalCacheFilter::in_flight_requests_;

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

  cache_key_config_ = defaultCacheKeyConfig();
  if (proto_config.has_cache_key()) {
    cache_key_config_ = makeCacheKeyConfig(proto_config.cache_key());
  }

  if (proto_config.has_skip_if_response_has_cache_control()) {
    skip_if_response_has_cache_control_ = proto_config.skip_if_response_has_cache_control().value();
  }
  if (proto_config.has_skip_if_response_has_set_cookie()) {
    skip_if_response_has_set_cookie_ = proto_config.skip_if_response_has_set_cookie().value();
  }

  if (proto_config.allowed_methods().empty()) {
    allowed_methods_.insert("GET");
    allowed_methods_.insert("HEAD");
  } else {
    for (const auto& method : proto_config.allowed_methods()) {
      std::string normalized_method = method;
      absl::AsciiStrToUpper(&normalized_method);
      if (!normalized_method.empty()) {
        allowed_methods_.insert(std::move(normalized_method));
      }
    }
  }

  if (proto_config.allowed_status_codes().empty()) {
    for (uint32_t status = 200; status < 300; ++status) {
      allowed_status_codes_.insert(status);
    }
  } else {
    for (const auto status : proto_config.allowed_status_codes()) {
      allowed_status_codes_.insert(status);
    }
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
      cache_key_override_(
          config.override_case() ==
                  envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute::kOverrides &&
              config.overrides().has_cache_key()
              ? absl::optional<CacheKeyConfig>(makeCacheKeyConfig(config.overrides().cache_key()))
              : absl::nullopt) {}

GlobalCacheFilter::GlobalCacheFilter(GlobalCacheFilterConfigSharedPtr config)
    : config_(std::move(config)), cache_backend_(config_->cacheBackend()),
      effective_default_ttl_(config_->defaultTtl()) {}

bool GlobalCacheFilter::isCacheableRequest(const Http::RequestHeaderMap& headers) const {
  if (!headers.Method()) {
    return false;
  }
  const auto method = headers.Method()->value().getStringView();
  std::string normalized_method(method);
  absl::AsciiStrToUpper(&normalized_method);
  return config_->allowedMethods().contains(normalized_method);
}

bool GlobalCacheFilter::isCacheableResponse(const Http::ResponseHeaderMap& headers) const {
  const auto status = Http::Utility::getResponseStatus(headers);
  if (!config_->allowedStatusCodes().contains(status)) {
    return false;
  }

  if (config_->skipIfResponseHasSetCookie() &&
      !headers.get(Http::Headers::get().SetCookie).empty()) {
    return false;
  }

  if (config_->skipIfResponseHasCacheControl() &&
      !headers.get(Http::CustomHeaders::get().CacheControl).empty()) {
    return false;
  }

  return true;
}

std::string GlobalCacheFilter::generateCacheKey(const Http::RequestHeaderMap& headers) {
  std::string key;
  if (headers.Method()) {
    key += std::string(headers.Method()->value().getStringView());
  }
  if (effective_cache_key_config_.include_scheme) {
    key += ":";
    if (headers.Scheme()) {
      key += std::string(headers.Scheme()->value().getStringView());
    }
  }
  if (effective_cache_key_config_.include_host) {
    key += ":";
    if (headers.Host()) {
      key += std::string(headers.Host()->value().getStringView());
    }
  }
  if (effective_cache_key_config_.include_path) {
    key += ":";
    key += buildPathForCacheKey(headers);
  }
  if (!effective_cache_key_config_.headers_included.empty()) {
    key += buildHeaderKeyFragment(headers);
  }
  return key;
}

std::string GlobalCacheFilter::buildPathForCacheKey(const Http::RequestHeaderMap& headers) const {
  if (!headers.Path()) {
    return "";
  }

  const auto& path = headers.Path()->value();
  if (!effective_cache_key_config_.include_query_params) {
    return Http::Utility::stripQueryString(path);
  }

  if (effective_cache_key_config_.query_params_included.empty() &&
      effective_cache_key_config_.query_params_excluded.empty()) {
    return std::string(path.getStringView());
  }

  const auto query_params = Http::Utility::QueryParamsMulti::parseQueryString(path.getStringView());
  Http::Utility::QueryParamsMulti filtered_params;
  const bool include_by_default = effective_cache_key_config_.query_params_included.empty();

  for (const auto& entry : query_params.data()) {
    const auto& name = entry.first;
    const auto& values = entry.second;
    for (const auto& value : values) {
      bool include = include_by_default ||
                     effective_cache_key_config_.query_params_included.contains(name);
      if (include &&
          effective_cache_key_config_.query_params_excluded.contains(name)) {
        include = false;
      }
      if (include) {
        filtered_params.add(name, value);
      }
    }
  }

  return filtered_params.replaceQueryString(path);
}

std::string GlobalCacheFilter::buildHeaderKeyFragment(const Http::RequestHeaderMap& headers) const {
  std::string fragment;

  for (const auto& header_name : effective_cache_key_config_.headers_included) {
    fragment += ":h:";
    fragment += header_name.get();
    fragment += "=";

    const auto values = headers.get(header_name);
    bool first = true;
    for (size_t i = 0; i < values.size(); ++i) {
      const auto* entry = values[i];
      if (!first) {
        fragment += ",";
      }
      fragment += std::string(entry->value().getStringView());
      first = false;
    }
  }

  return fragment;
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
    if (auto cache_key_override = per_route_config->cacheKeyOverride();
        cache_key_override.has_value()) {
      effective_cache_key_config_ = cache_key_override.value();
    } else {
      effective_cache_key_config_ = config_->cacheKeyConfig();
    }
  } else {
    effective_cache_key_config_ = config_->cacheKeyConfig();
  }

  if (!cache_enabled_) {
    return Http::FilterHeadersStatus::Continue;
  }

  cacheable_request_ = isCacheableRequest(headers);
  if (!cacheable_request_) {
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

  if (!cacheable_request_) {
    return Http::FilterHeadersStatus::Continue;
  }

  // If we served from cache or waiting, this shouldn't be called
  if (state_ == FilterState::CacheHit || state_ == FilterState::WaitingForUpstream) {
    return Http::FilterHeadersStatus::Continue;
  }

  cacheable_response_ = isCacheableResponse(headers);
  if (!cacheable_response_) {
    if (owns_in_flight_) {
      notifyInFlightWaiters(in_flight_key_, nullptr);
      owns_in_flight_ = false;
    }
    state_ = FilterState::CacheMiss;
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

  if (!cacheable_request_ || !cacheable_response_) {
    return Http::FilterDataStatus::Continue;
  }

  // If we served from cache or waiting, this shouldn't be called
  if (state_ == FilterState::CacheHit || state_ == FilterState::WaitingForUpstream) {
    return Http::FilterDataStatus::Continue;
  }

  // Buffer the response body (avoid toString() to reduce memory copies)
  uint64_t length = data.length();
  if (length > 0) {
    if (buffered_body_.length() + length > kMaxCachedResponseBytes) {
      cacheable_response_ = false;
      buffered_body_.drain(buffered_body_.length());
      response_headers_.reset();
      if (owns_in_flight_) {
        notifyInFlightWaiters(in_flight_key_, nullptr);
        owns_in_flight_ = false;
      }
      return Http::FilterDataStatus::Continue;
    }
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

  for (const auto& waiter : waiters) {
    waiter->onInFlightComplete(entry);
  }
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

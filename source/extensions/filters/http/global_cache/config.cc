#include "source/extensions/filters/http/global_cache/config.h"

#include "envoy/registry/registry.h"

#include "source/extensions/filters/http/global_cache/global_cache_filter.h"
#include "source/extensions/filters/http/global_cache/local_cache.h"
#include "source/extensions/filters/http/global_cache/redis_cache.h"
#include "source/extensions/filters/http/global_cache/tiered_cache.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

namespace {

/**
 * Create a cache backend based on the proto configuration.
 * Returns LocalCache if no backend is specified (default).
 */
CacheBackendSharedPtr createCacheBackend(
    const envoy::extensions::filters::http::global_cache::v3::GlobalCache& proto_config,
    Server::Configuration::FactoryContext& context) {

  // Check if cache_backend is configured
  if (!proto_config.has_cache_backend()) {
    // Default to local cache with default settings
    return std::make_shared<LocalCache>();
  }

  const auto& backend_config = proto_config.cache_backend();

  switch (backend_config.backend_type_case()) {
  case envoy::extensions::filters::http::global_cache::v3::CacheBackendConfig::kLocal:
    // Local in-memory LRU cache
    return std::make_shared<LocalCache>(backend_config.local());

  case envoy::extensions::filters::http::global_cache::v3::CacheBackendConfig::kRedis:
    // Redis cache backend
    return std::make_shared<RedisCache>(backend_config.redis(),
                                        context.serverFactoryContext().clusterManager(),
                                        context.serverFactoryContext().threadLocal(),
                                        context.serverFactoryContext(), context.scope());

  case envoy::extensions::filters::http::global_cache::v3::CacheBackendConfig::kTiered: {
    // Tiered cache (L1 local + L2)
    const auto& tiered_config = backend_config.tiered();

    // Create L1 (local) cache
    auto l1 = std::make_shared<LocalCache>(tiered_config.l1_local());

    // Create L2 cache (RedisCache or LocalCache for testing)
    CacheBackendSharedPtr l2;
    if (tiered_config.has_l2_redis()) {
      // Production: Use Redis as L2
      l2 = std::make_shared<RedisCache>(tiered_config.l2_redis(),
                                        context.serverFactoryContext().clusterManager(),
                                        context.serverFactoryContext().threadLocal(),
                                        context.serverFactoryContext(), context.scope());
    } else {
      // For testing: create another local cache as L2
      // In production config, this branch won't be used
      envoy::extensions::filters::http::global_cache::v3::LocalCacheConfig l2_config;
      l2_config.set_max_entries(10000);
      l2_config.set_max_bytes(1073741824); // 1GB
      l2 = std::make_shared<LocalCache>(l2_config);
    }

    bool populate_l1 = tiered_config.populate_l1_on_l2_hit();
    return std::make_shared<TieredCache>(l1, l2, tiered_config.write_strategy(), populate_l1);
  }

  default:
    // Default to local cache
    return std::make_shared<LocalCache>();
  }
}

} // namespace

absl::StatusOr<Http::FilterFactoryCb> GlobalCacheFilterFactory::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::global_cache::v3::GlobalCache& proto_config,
    const std::string& /*stats_prefix*/, Server::Configuration::FactoryContext& context) {

  // Create cache backend based on configuration
  CacheBackendSharedPtr cache_backend = createCacheBackend(proto_config, context);

  GlobalCacheFilterConfigSharedPtr filter_config =
      std::make_shared<GlobalCacheFilterConfig>(proto_config, cache_backend);

  return [filter_config](Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<GlobalCacheFilter>(filter_config));
  };
}

REGISTER_FACTORY(GlobalCacheFilterFactory, Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

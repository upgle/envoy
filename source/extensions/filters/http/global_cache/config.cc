#include "source/extensions/filters/http/global_cache/config.h"

#include "envoy/registry/registry.h"

#include "source/extensions/filters/http/global_cache/global_cache_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

absl::StatusOr<Http::FilterFactoryCb> GlobalCacheFilterFactory::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::global_cache::v3::GlobalCache& proto_config,
    const std::string& /*stats_prefix*/, Server::Configuration::FactoryContext& /*context*/) {

  GlobalCacheFilterConfigSharedPtr filter_config =
      std::make_shared<GlobalCacheFilterConfig>(proto_config);

  return [filter_config](Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<GlobalCacheFilter>(filter_config));
  };
}

REGISTER_FACTORY(GlobalCacheFilterFactory, Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

#pragma once

#include "envoy/extensions/filters/http/global_cache/v3/global_cache.pb.h"
#include "envoy/extensions/filters/http/global_cache/v3/global_cache.pb.validate.h"

#include "source/extensions/filters/http/common/factory_base.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

/**
 * Config registration for the global_cache filter.
 */
class GlobalCacheFilterFactory
    : public Common::DualFactoryBase<
          envoy::extensions::filters::http::global_cache::v3::GlobalCache,
          envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute> {
public:
  GlobalCacheFilterFactory() : DualFactoryBase("envoy.filters.http.global_cache") {}

private:
  absl::StatusOr<Http::FilterFactoryCb> createFilterFactoryFromProtoTyped(
      const envoy::extensions::filters::http::global_cache::v3::GlobalCache& proto_config,
      const std::string& stats_prefix, DualInfo,
      Server::Configuration::ServerFactoryContext& context) override;

  absl::StatusOr<Router::RouteSpecificFilterConfigConstSharedPtr>
  createRouteSpecificFilterConfigTyped(
      const envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute& proto_config,
      Server::Configuration::ServerFactoryContext& context,
      ProtobufMessage::ValidationVisitor& validator) override;
};

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

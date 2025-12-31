#include "source/extensions/filters/http/global_cache/redis_cache.h"

#include "source/common/common/assert.h"
#include "source/common/protobuf/utility.h"
#include "envoy/server/factory_context.h"
#include "source/extensions/common/redis/cluster_refresh_manager_impl.h"
#include "source/extensions/filters/network/common/redis/codec_impl.h"
#include "source/extensions/filters/network/common/redis/redis_command_stats.h"
#include "source/extensions/filters/network/common/redis/utility.h"
#include "source/extensions/filters/network/redis_proxy/conn_pool_impl.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

namespace {
// Default timeout if not specified in config
constexpr std::chrono::milliseconds kDefaultOpTimeout{100};
// Default key prefix
const std::string kDefaultKeyPrefix = "envoy:gc:";
} // namespace

RedisCache::RedisCache(
    const envoy::extensions::filters::http::global_cache::v3::RedisCacheConfig& config,
    Upstream::ClusterManager& cluster_manager, ThreadLocal::Instance& tls,
    Server::Configuration::ServerFactoryContext& server_context, Stats::Scope& stats_scope)
    : cluster_manager_(cluster_manager), tls_(tls), pending_requests_(tls),
      stats_scope_(stats_scope),
      cluster_name_(config.cluster_name()),
      key_prefix_(config.key_prefix().empty() ? kDefaultKeyPrefix : config.key_prefix()),
      op_timeout_(config.has_op_timeout()
                      ? std::chrono::milliseconds(PROTOBUF_GET_MS_REQUIRED(config, op_timeout))
                      : kDefaultOpTimeout),
      enable_cluster_mode_(config.enable_cluster_mode()) {

  // Create command stats
  redis_command_stats_ =
      NetworkFilters::Common::Redis::RedisCommandStats::createRedisCommandStats(
          stats_scope_.symbolTable());

  // Allocate TLS slot for pending request lifetime management.
  pending_requests_.set([](Event::Dispatcher&) { return std::make_shared<PendingList>(); });

  envoy::extensions::filters::network::redis_proxy::v3::RedisProxy::ConnPoolSettings settings;
  *settings.mutable_op_timeout() =
      Protobuf::util::TimeUtil::MillisecondsToDuration(op_timeout_.count());
  settings.set_enable_redirection(enable_cluster_mode_);
  settings.set_enable_hashtagging(false);
  settings.set_read_policy(
      envoy::extensions::filters::network::redis_proxy::v3::RedisProxy::ConnPoolSettings::MASTER);

  auto redis_stats_scope =
      stats_scope_.createScope(absl::StrCat("redis_cache.", cluster_name_, "."));
  auto refresh_manager = Extensions::Common::Redis::getClusterRefreshManager(
      server_context.singletonManager(), server_context.mainThreadDispatcher(), cluster_manager,
      server_context.timeSource());
  auto conn_pool = std::make_shared<NetworkFilters::RedisProxy::ConnPool::InstanceImpl>(
      cluster_name_, cluster_manager_,
      NetworkFilters::Common::Redis::Client::ClientFactoryImpl::instance_, tls, settings,
      server_context.api(), std::move(redis_stats_scope), redis_command_stats_, refresh_manager,
      Extensions::Common::DynamicForwardProxy::DnsCacheSharedPtr(), absl::nullopt,
      absl::nullopt);
  conn_pool->init();
  conn_pool_ = std::move(conn_pool);

  ENVOY_LOG(info, "RedisCache initialized: cluster={}, prefix={}, timeout={}ms, cluster_mode={}",
            cluster_name_, key_prefix_, op_timeout_.count(), enable_cluster_mode_);
}

void RedisCache::lookup(const std::string& key, LookupCallback callback) {
  const std::string redis_key = buildRedisKey(key);

  auto* pending_list = getPendingList();
  if (!pending_list) {
    ENVOY_LOG(warn, "RedisCache: No TLS pending list available for lookup, returning miss");
    callback(CacheLookupResult{CacheLookupStatus::Miss});
    return;
  }

  // Create GET request
  auto request = makeGetRequest(redis_key);

  // Create callback handler with shared_ptr for proper lifecycle management
  auto request_handler =
      std::make_shared<LookupRequest>(std::move(callback), tls_.dispatcher());
  auto request_base = std::static_pointer_cast<RequestBase>(request_handler);
  pending_list->pending_requests_.push_back(request_base);
  request_handler->setPending(pending_list, std::prev(pending_list->pending_requests_.end()));

  // Make async request
  NetworkFilters::Common::Redis::Client::NoOpTransaction transaction;
  auto* pool_request = conn_pool_->makeRequest(redis_key, std::move(request), *request_handler,
                                               transaction);
  if (!pool_request) {
    ENVOY_LOG(warn, "RedisCache: Failed to make GET request for key: {}", redis_key);
    // Trigger failure callback (will be posted to dispatcher inside onFailure)
    request_handler->onFailure();
    return;
  }

  // Note: request_handler is kept alive by the lambda capture in the callback
}

void RedisCache::insert(const std::string& key, std::shared_ptr<CacheEntry> entry,
                        std::chrono::seconds ttl, InsertCallback callback) {
  const std::string redis_key = buildRedisKey(key);

  auto* pending_list = getPendingList();
  if (!pending_list) {
    ENVOY_LOG(warn, "RedisCache: No TLS pending list available for insert, failing");
    callback(false);
    return;
  }

  // Serialize entry
  std::string serialized_value;
  try {
    serialized_value = CacheSerializer::serialize(*entry);
  } catch (const std::exception& e) {
    ENVOY_LOG(error, "RedisCache: Failed to serialize entry for key {}: {}", redis_key, e.what());
    tls_.dispatcher().post([callback = std::move(callback)]() mutable {
      callback(false);
    });
    return;
  }

  // Create SETEX request
  auto request = makeSetexRequest(redis_key, serialized_value, ttl);

  // Create callback handler with shared_ptr for proper lifecycle management
  auto request_handler =
      std::make_shared<InsertRequest>(std::move(callback), tls_.dispatcher());
  auto request_base = std::static_pointer_cast<RequestBase>(request_handler);
  pending_list->pending_requests_.push_back(request_base);
  request_handler->setPending(pending_list, std::prev(pending_list->pending_requests_.end()));

  // Make async request
  NetworkFilters::Common::Redis::Client::NoOpTransaction transaction;
  auto* pool_request = conn_pool_->makeRequest(redis_key, std::move(request), *request_handler,
                                               transaction);
  if (!pool_request) {
    ENVOY_LOG(warn, "RedisCache: Failed to make SETEX request for key: {}", redis_key);
    // Trigger failure callback (will be posted to dispatcher inside onFailure)
    request_handler->onFailure();
    return;
  }

  // Note: request_handler is kept alive by the lambda capture in the callback
}

void RedisCache::remove(const std::string& key) {
  const std::string redis_key = buildRedisKey(key);

  auto* pending_list = getPendingList();
  if (!pending_list) {
    ENVOY_LOG(warn, "RedisCache: No TLS pending list available for remove");
    return;
  }

  auto request = makeDelRequest(redis_key);
  static NetworkFilters::RedisProxy::ConnPool::DoNothingPoolCallbacks do_nothing_callbacks;
  NetworkFilters::Common::Redis::Client::NoOpTransaction transaction;
  auto* pool_request = conn_pool_->makeRequest(redis_key, std::move(request), do_nothing_callbacks,
                                               transaction);
  if (!pool_request) {
    ENVOY_LOG(warn, "RedisCache: Failed to make DEL request for key: {}", redis_key);
  }
}

NetworkFilters::Common::Redis::RespValue
RedisCache::makeGetRequest(const std::string& redis_key) const {
  using NetworkFilters::Common::Redis::RespType;
  using NetworkFilters::Common::Redis::RespValue;

  std::vector<RespValue> values(2);
  values[0].type(RespType::BulkString);
  values[0].asString() = "GET";
  values[1].type(RespType::BulkString);
  values[1].asString() = redis_key;

  RespValue request;
  request.type(RespType::Array);
  request.asArray().swap(values);
  return request;
}

NetworkFilters::Common::Redis::RespValue
RedisCache::makeSetexRequest(const std::string& redis_key, const std::string& value,
                              std::chrono::seconds ttl) const {
  using NetworkFilters::Common::Redis::RespType;
  using NetworkFilters::Common::Redis::RespValue;

  std::vector<RespValue> values(4);
  values[0].type(RespType::BulkString);
  values[0].asString() = "SETEX";
  values[1].type(RespType::BulkString);
  values[1].asString() = redis_key;
  values[2].type(RespType::BulkString);
  values[2].asString() = std::to_string(ttl.count());
  values[3].type(RespType::BulkString);
  values[3].asString() = value;

  RespValue request;
  request.type(RespType::Array);
  request.asArray().swap(values);
  return request;
}

NetworkFilters::Common::Redis::RespValue
RedisCache::makeDelRequest(const std::string& redis_key) const {
  using NetworkFilters::Common::Redis::RespType;
  using NetworkFilters::Common::Redis::RespValue;

  std::vector<RespValue> values(2);
  values[0].type(RespType::BulkString);
  values[0].asString() = "DEL";
  values[1].type(RespType::BulkString);
  values[1].asString() = redis_key;

  RespValue request;
  request.type(RespType::Array);
  request.asArray().swap(values);
  return request;
}

RedisCache::PendingList* RedisCache::getPendingList() {
  if (!pending_requests_.currentThreadRegistered()) {
    return nullptr;
  }
  auto pending_list_opt = pending_requests_.get();
  if (!pending_list_opt.has_value()) {
    return nullptr;
  }
  return &(*pending_list_opt);
}

// LookupRequest implementation
void RedisCache::LookupRequest::onResponse(
    NetworkFilters::Common::Redis::RespValuePtr&& value) {
  using NetworkFilters::Common::Redis::RespType;

  // Capture shared_ptr to keep this object alive during async post
  auto self = shared_from_this();
  clearPending();

  if (!value) {
    ENVOY_LOG_MISC(warn, "RedisCache: Received null response for GET");
    dispatcher_.post([self]() mutable {
      self->callback_(CacheLookupResult{CacheLookupStatus::Miss});
    });
    return;
  }

  // Check response type
  if (value->type() == RespType::BulkString) {
    // Got data - deserialize
    const std::string& serialized = value->asString();
    if (serialized.empty()) {
      // Empty string means key not found (Redis returns null bulk string as empty)
      dispatcher_.post([self]() mutable {
        self->callback_(CacheLookupResult{CacheLookupStatus::Miss});
      });
    } else {
      try {
        auto entry = CacheSerializer::deserialize(serialized);
        dispatcher_.post([self, entry]() mutable {
          self->callback_(CacheLookupResult{CacheLookupStatus::Hit, entry});
        });
      } catch (const std::exception& e) {
        ENVOY_LOG_MISC(error, "RedisCache: Failed to deserialize entry: {}", e.what());
        dispatcher_.post([self]() mutable {
          self->callback_(CacheLookupResult{CacheLookupStatus::Miss});
        });
      }
    }
  } else if (value->type() == RespType::Null) {
    // Key not found
    dispatcher_.post([self]() mutable {
      self->callback_(CacheLookupResult{CacheLookupStatus::Miss});
    });
  } else if (value->type() == RespType::Error) {
    ENVOY_LOG_MISC(warn, "RedisCache: GET returned error: {}", value->asString());
    dispatcher_.post([self]() mutable {
      self->callback_(CacheLookupResult{CacheLookupStatus::Miss});
    });
  } else {
    ENVOY_LOG_MISC(warn, "RedisCache: Unexpected response type for GET");
    dispatcher_.post([self]() mutable {
      self->callback_(CacheLookupResult{CacheLookupStatus::Miss});
    });
  }
}

void RedisCache::LookupRequest::onFailure() {
  ENVOY_LOG_MISC(warn, "RedisCache: GET request failed");

  // Capture shared_ptr to keep this object alive during async post
  auto self = shared_from_this();
  clearPending();
  dispatcher_.post([self]() mutable {
    self->callback_(CacheLookupResult{CacheLookupStatus::Miss});
  });
}

// InsertRequest implementation
void RedisCache::InsertRequest::onResponse(
    NetworkFilters::Common::Redis::RespValuePtr&& value) {
  using NetworkFilters::Common::Redis::RespType;

  // Capture shared_ptr to keep this object alive during async post
  auto self = shared_from_this();
  clearPending();

  bool success = false;
  if (value && value->type() == RespType::SimpleString && value->asString() == "OK") {
    success = true;
  } else if (value && value->type() == RespType::Error) {
    ENVOY_LOG_MISC(warn, "RedisCache: SETEX returned error: {}", value->asString());
  }

  dispatcher_.post([self, success]() mutable {
    self->callback_(success);
  });
}

void RedisCache::InsertRequest::onFailure() {
  ENVOY_LOG_MISC(warn, "RedisCache: SETEX request failed");

  // Capture shared_ptr to keep this object alive during async post
  auto self = shared_from_this();
  clearPending();
  dispatcher_.post([self]() mutable {
    self->callback_(false);
  });
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

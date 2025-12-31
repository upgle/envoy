#include "source/extensions/filters/http/global_cache/redis_cache.h"

#include "source/common/common/assert.h"
#include "source/extensions/filters/network/common/redis/client_impl.h"
#include "source/extensions/filters/network/common/redis/codec_impl.h"
#include "source/extensions/filters/network/common/redis/redis_command_stats.h"
#include "source/extensions/filters/network/common/redis/utility.h"

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
    Upstream::ClusterManager& cluster_manager, ThreadLocal::Instance& tls, Stats::Scope& stats_scope)
    : cluster_manager_(cluster_manager), tls_(tls), tls_slot_(tls), stats_scope_(stats_scope),
      cluster_name_(config.cluster_name()),
      key_prefix_(config.key_prefix().empty() ? kDefaultKeyPrefix : config.key_prefix()),
      op_timeout_(config.has_op_timeout()
                      ? std::chrono::milliseconds(PROTOBUF_GET_MS_REQUIRED(config, op_timeout))
                      : kDefaultOpTimeout),
      enable_cluster_mode_(config.enable_cluster_mode()) {

  // Create Redis client config
  redis_config_ = createRedisConfig();

  // Create command stats
  redis_command_stats_ =
      NetworkFilters::Common::Redis::RedisCommandStats::createRedisCommandStats(
          stats_scope_.symbolTable());

  // Allocate TLS slot and initialize with factory
  tls_slot_.set([](Event::Dispatcher&) { return std::make_shared<ThreadLocalClient>(nullptr); });

  ENVOY_LOG(info, "RedisCache initialized: cluster={}, prefix={}, timeout={}ms, cluster_mode={}",
            cluster_name_, key_prefix_, op_timeout_.count(), enable_cluster_mode_);
}

void RedisCache::lookup(const std::string& key, LookupCallback callback) {
  const std::string redis_key = buildRedisKey(key);

  // Get ThreadLocal client
  auto* tls_client = getThreadLocalClient();
  if (!tls_client || !tls_client->client_) {
    ENVOY_LOG(warn, "RedisCache: No Redis client available for lookup, returning miss");
    // We need a dispatcher to post the callback. Try to get it from TLS if available,
    // otherwise we are in trouble (shouldn't happen if getThreadLocalClient returns valid ptr)
    if (tls_client && tls_slot_.currentThreadRegistered()) {
      tls_.dispatcher().post([callback = std::move(callback)]() mutable {
        callback(CacheLookupResult{CacheLookupStatus::Miss});
      });
    } else {
      // Fallback: execute inline (risky but better than crash) or drop?
      // Since we can't get TLS, we might be on a thread without it initialized.
      // But we just called getThreadLocalClient.
      // Assuming it's safe to run inline if we have no dispatcher context.
      callback(CacheLookupResult{CacheLookupStatus::Miss});
    }
    return;
  }

  // Create GET request
  auto request = makeGetRequest(redis_key);

  // Create callback handler with shared_ptr for proper lifecycle management
  auto request_handler =
      std::make_shared<LookupRequest>(std::move(callback), tls_.dispatcher());
  auto request_base = std::static_pointer_cast<RequestBase>(request_handler);
  tls_client->pending_requests_.push_back(request_base);
  request_handler->setPending(tls_client, std::prev(tls_client->pending_requests_.end()));

  // Make async request
  auto* pool_request = tls_client->client_->makeRequest(request, *request_handler);
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

  // Get ThreadLocal client
  auto* tls_client = getThreadLocalClient();
  if (!tls_client || !tls_client->client_) {
    ENVOY_LOG(warn, "RedisCache: No Redis client available for insert, failing");
    if (tls_client && tls_slot_.currentThreadRegistered()) {
      tls_.dispatcher().post([callback = std::move(callback)]() mutable {
        callback(false);
      });
    } else {
      callback(false);
    }
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
  tls_client->pending_requests_.push_back(request_base);
  request_handler->setPending(tls_client, std::prev(tls_client->pending_requests_.end()));

  // Make async request
  auto* pool_request = tls_client->client_->makeRequest(request, *request_handler);
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

  // Get ThreadLocal client
  auto* tls_client = getThreadLocalClient();
  if (!tls_client || !tls_client->client_) {
    ENVOY_LOG(warn, "RedisCache: No Redis client available for remove");
    return;
  }

  // Create DEL request
  auto request = makeDelRequest(redis_key);

  // Use DoNothingPoolCallbacks for fire-and-forget DEL
  static NetworkFilters::Common::Redis::Client::DoNothingPoolCallbacks do_nothing_callbacks;

  // Make async request (fire-and-forget)
  auto* pool_request = tls_client->client_->makeRequest(request, do_nothing_callbacks);
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

RedisCache::ThreadLocalClient* RedisCache::getThreadLocalClient() {
  if (!tls_slot_.currentThreadRegistered()) {
    return nullptr;
  }
  auto tls_client_opt = tls_slot_.get();
  if (!tls_client_opt.has_value()) {
    return nullptr;
  }

  auto& tls_client = *tls_client_opt;

  if (tls_client.client_) {
    if (!tls_client.connection_open_) {
      if (tls_client.client_->active()) {
        ENVOY_LOG_MISC(warn, "RedisCache: Redis client closed with pending requests");
        return nullptr;
      }
      tls_client.client_.reset();
      tls_client.needs_reset_ = false;
    }
  }

  // If we already have a valid client, check host health/changes?
  // Redis client implementation usually handles connection management.
  // But if the LB picks a different host, we might want to switch?
  // Standard Redis client is tied to a host.

  // Note: For simplicity, we only create the client if it doesn't exist.
  // Dynamic host switching for the SAME client object is not supported by this simple factory
  // usage. If the host becomes unhealthy, the client might fail. Real implementation might need
  // to periodically check or use a cluster client that handles redirection/topology.

  if (tls_client.client_ && tls_client.client_->active()) {
    return &tls_client;
  }

  // Get thread-local cluster
  auto thread_local_cluster = cluster_manager_.getThreadLocalCluster(cluster_name_);
  if (!thread_local_cluster) {
    // Rate limit log to avoid spamming
    ENVOY_LOG_MISC(warn, "RedisCache: Cluster {} not found", cluster_name_);
    return &tls_client; // Return with null client
  }

  // Choose a host using load balancer
  auto& lb = thread_local_cluster->loadBalancer();
  auto result = lb.chooseHost(nullptr);
  if (!result.host) {
    ENVOY_LOG_MISC(warn, "RedisCache: No healthy host available in cluster {}", cluster_name_);
    return &tls_client;
  }

  auto host = result.host;

  // If we have a client but the host changed, close old client
  if (tls_client.client_ && tls_client.host_ != host) {
    ENVOY_LOG(debug, "RedisCache: Host changed, closing old client");
    tls_client.client_->close();
    tls_client.client_.reset();
  }

  // Create new client if needed
  if (!tls_client.client_) {
    ENVOY_LOG(debug, "RedisCache: Creating new Redis client for host {}",
              host->address()->asStringView());

    // Use singleton ClientFactory to create client with the current thread dispatcher.
    tls_client.client_ = NetworkFilters::Common::Redis::Client::ClientFactoryImpl::instance_.create(
        host, tls_.dispatcher(), redis_config_, redis_command_stats_, stats_scope_,
        "", // auth_username (empty for now)
        "", // auth_password (empty for now)
        false, // is_transaction_client
        absl::nullopt, // aws_iam_config
        absl::nullopt  // aws_iam_authenticator
    );

    tls_client.host_ = host;
    tls_client.connection_open_ = true;
    tls_client.needs_reset_ = false;

    if (!tls_client.client_) {
      ENVOY_LOG(error, "RedisCache: Failed to create Redis client");
    } else {
      tls_client.client_->addConnectionCallbacks(tls_client);
    }
  }

  return &tls_client;
}

NetworkFilters::Common::Redis::Client::ConfigSharedPtr RedisCache::createRedisConfig() {
  // Create a simple config for Redis client
  // We'll use a custom config class that implements the Config interface
  class SimpleRedisConfig : public NetworkFilters::Common::Redis::Client::Config {
  public:
    explicit SimpleRedisConfig(std::chrono::milliseconds op_timeout)
        : op_timeout_(op_timeout) {}

    bool disableOutlierEvents() const override { return false; }
    std::chrono::milliseconds opTimeout() const override { return op_timeout_; }
    bool enableHashtagging() const override { return false; }
    bool enableRedirection() const override { return true; }
    uint32_t maxBufferSizeBeforeFlush() const override { return 0; }
    std::chrono::milliseconds bufferFlushTimeoutInMs() const override {
      return std::chrono::milliseconds(1);
    }
    uint32_t maxUpstreamUnknownConnections() const override { return 0; }
    bool enableCommandStats() const override { return false; }
    NetworkFilters::Common::Redis::Client::ReadPolicy readPolicy() const override {
      return NetworkFilters::Common::Redis::Client::ReadPolicy::Primary;
    }
    bool connectionRateLimitEnabled() const override { return false; }
    uint32_t connectionRateLimitPerSec() const override { return 0; }

  private:
    const std::chrono::milliseconds op_timeout_;
  };

  return std::make_shared<SimpleRedisConfig>(op_timeout_);
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
  markClientForReset();
  clearPending();
  dispatcher_.post([self]() mutable {
    self->callback_(CacheLookupResult{CacheLookupStatus::Miss});
  });
}

void RedisCache::LookupRequest::onRedirection(
    NetworkFilters::Common::Redis::RespValuePtr&& /*value*/, const std::string& host_address,
    bool ask_redirection) {
  // TODO: Handle Redis cluster redirections
  // For now, treat as failure
  ENVOY_LOG_MISC(warn, "RedisCache: GET redirection to {} (ask={}), not implemented",
                 host_address, ask_redirection);

  // Capture shared_ptr to keep this object alive during async post
  auto self = shared_from_this();
  markClientForReset();
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
  markClientForReset();
  clearPending();
  dispatcher_.post([self]() mutable {
    self->callback_(false);
  });
}

void RedisCache::InsertRequest::onRedirection(
    NetworkFilters::Common::Redis::RespValuePtr&& /*value*/, const std::string& host_address,
    bool ask_redirection) {
  // TODO: Handle Redis cluster redirections
  ENVOY_LOG_MISC(warn, "RedisCache: SETEX redirection to {} (ask={}), not implemented",
                 host_address, ask_redirection);

  // Capture shared_ptr to keep this object alive during async post
  auto self = shared_from_this();
  markClientForReset();
  clearPending();
  dispatcher_.post([self]() mutable {
    self->callback_(false);
  });
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

#pragma once

#include <string>
#include <list>

#include "envoy/event/dispatcher.h"
#include "envoy/extensions/filters/http/global_cache/v3/global_cache.pb.h"
#include "envoy/server/factory_context.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/upstream/cluster_manager.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/global_cache/cache_backend.h"
#include "source/extensions/filters/http/global_cache/cache_serialization.h"
#include "source/extensions/filters/network/common/redis/codec.h"
#include "source/extensions/filters/network/redis_proxy/conn_pool.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {
/**
 * Redis-backed cache implementation using Envoy's Redis connection pool.
 *
 * Supports both standalone and cluster modes with async operations.
 * Uses CacheSerializer for entry serialization.
 * Uses ThreadLocal::Slot to manage per-thread Redis clients.
 */
class RedisCache : public CacheBackend, public Logger::Loggable<Logger::Id::filter> {
public:
  RedisCache(const envoy::extensions::filters::http::global_cache::v3::RedisCacheConfig& config,
             Upstream::ClusterManager& cluster_manager, ThreadLocal::Instance& tls,
             Server::Configuration::ServerFactoryContext& server_context,
             Stats::Scope& stats_scope);

  ~RedisCache() override = default;

  // CacheBackend interface
  void lookup(const std::string& key, LookupCallback callback) override;
  void insert(const std::string& key, std::shared_ptr<CacheEntry> entry,
              std::chrono::seconds ttl, InsertCallback callback) override;
  void remove(const std::string& key) override;
  std::string name() const override { return "redis_cache"; }

private:
  class RequestBase;

  struct PendingList : public ThreadLocal::ThreadLocalObject {
    std::list<std::shared_ptr<RequestBase>> pending_requests_;
  };

  class RequestBase {
  public:
    virtual ~RequestBase() = default;

    void setPending(PendingList* owner, std::list<std::shared_ptr<RequestBase>>::iterator it) {
      owner_ = owner;
      it_ = it;
      pending_ = true;
    }

  protected:
    void clearPending() {
      if (!pending_ || owner_ == nullptr) {
        return;
      }
      owner_->pending_requests_.erase(it_);
      pending_ = false;
      owner_ = nullptr;
    }

  private:
    PendingList* owner_{nullptr};
    std::list<std::shared_ptr<RequestBase>>::iterator it_{};
    bool pending_{false};
  };

  /**
   * Callback handler for Redis GET operations.
   */
  class LookupRequest : public NetworkFilters::RedisProxy::ConnPool::PoolCallbacks,
                        public RequestBase,
                        public std::enable_shared_from_this<LookupRequest> {
  public:
    LookupRequest(LookupCallback callback, Event::Dispatcher& dispatcher)
        : callback_(std::move(callback)), dispatcher_(dispatcher) {}

    // PoolCallbacks interface
    void onResponse(NetworkFilters::Common::Redis::RespValuePtr&& value) override;
    void onFailure() override;

  private:
    LookupCallback callback_;
    Event::Dispatcher& dispatcher_;
  };

  /**
   * Callback handler for Redis SET/SETEX operations.
   */
  class InsertRequest : public NetworkFilters::RedisProxy::ConnPool::PoolCallbacks,
                        public RequestBase,
                        public std::enable_shared_from_this<InsertRequest> {
  public:
    InsertRequest(InsertCallback callback, Event::Dispatcher& dispatcher)
        : callback_(std::move(callback)), dispatcher_(dispatcher) {}

    // PoolCallbacks interface
    void onResponse(NetworkFilters::Common::Redis::RespValuePtr&& value) override;
    void onFailure() override;

  private:
    InsertCallback callback_;
    Event::Dispatcher& dispatcher_;
  };

  /**
   * Helper to build full Redis key with prefix.
   */
  std::string buildRedisKey(const std::string& cache_key) const {
    return key_prefix_ + cache_key;
  }

  /**
   * Create a Redis GET request.
   */
  NetworkFilters::Common::Redis::RespValue
  makeGetRequest(const std::string& redis_key) const;

  /**
   * Create a Redis SETEX request (SET with expiration).
   */
  NetworkFilters::Common::Redis::RespValue
  makeSetexRequest(const std::string& redis_key, const std::string& value,
                   std::chrono::seconds ttl) const;

  /**
   * Create a Redis DEL request.
   */
  NetworkFilters::Common::Redis::RespValue makeDelRequest(const std::string& redis_key) const;

  /**
   * Get or create Redis client for the current thread.
   */
  PendingList* getPendingList();

  Upstream::ClusterManager& cluster_manager_;
  ThreadLocal::Instance& tls_;
  ThreadLocal::TypedSlot<PendingList> pending_requests_;
  Stats::Scope& stats_scope_;
  const std::string cluster_name_;
  const std::string key_prefix_;
  const std::chrono::milliseconds op_timeout_;
  const bool enable_cluster_mode_;

  NetworkFilters::RedisProxy::ConnPool::InstanceSharedPtr conn_pool_;
  NetworkFilters::Common::Redis::RedisCommandStatsSharedPtr redis_command_stats_;
};

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

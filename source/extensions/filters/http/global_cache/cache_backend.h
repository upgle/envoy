#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "envoy/buffer/buffer.h"
#include "envoy/http/header_map.h"

#include "source/common/buffer/buffer_impl.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

/**
 * Cache entry that stores response data and expiration time.
 * Shared across all cache backend implementations.
 */
struct CacheEntry {
  Buffer::OwnedImpl body;
  Http::ResponseHeaderMapPtr headers;
  std::chrono::steady_clock::time_point expiration_time;

  CacheEntry(Buffer::OwnedImpl body_data, Http::ResponseHeaderMapPtr header_map,
             std::chrono::steady_clock::time_point exp_time)
      : body(std::move(body_data)), headers(std::move(header_map)), expiration_time(exp_time) {}
};

/**
 * Result status for cache lookup operations.
 */
enum class CacheLookupStatus {
  Hit,   // Cache hit, entry returned
  Miss,  // Cache miss, entry is null
  Error  // Backend error occurred
};

/**
 * Result of a cache lookup operation.
 */
struct CacheLookupResult {
  CacheLookupStatus status;
  std::shared_ptr<CacheEntry> entry;  // null if miss or error

  CacheLookupResult(CacheLookupStatus s, std::shared_ptr<CacheEntry> e = nullptr)
      : status(s), entry(std::move(e)) {}
};

/**
 * Abstract interface for cache backends.
 * Implementations: LocalCache (LRU), RedisCache, TieredCache.
 *
 * All operations are async via callbacks:
 * - LocalCache invokes callbacks synchronously (fast path)
 * - RedisCache posts callbacks to dispatcher after async I/O
 * - TieredCache chains L1 -> L2 lookups
 */
class CacheBackend {
public:
  virtual ~CacheBackend() = default;

  /**
   * Callback invoked when lookup completes.
   * @param result lookup result with status and optional entry
   */
  using LookupCallback = std::function<void(CacheLookupResult&& result)>;

  /**
   * Callback invoked when insert completes.
   * @param success true if insert succeeded, false on error
   */
  using InsertCallback = std::function<void(bool success)>;

  /**
   * Asynchronously lookup a cache entry by key.
   *
   * For LocalCache: Callback is invoked synchronously before returning.
   * For RedisCache: Callback is posted to dispatcher after async response.
   * For TieredCache: Callback is invoked after L1 hit or L2 response.
   *
   * @param key the cache key
   * @param callback invoked with lookup result when operation completes
   */
  virtual void lookup(const std::string& key, LookupCallback callback) PURE;

  /**
   * Asynchronously insert a cache entry.
   *
   * @param key the cache key
   * @param entry the cache entry to store
   * @param ttl time-to-live for this entry
   * @param callback invoked with success status when operation completes
   */
  virtual void insert(const std::string& key, std::shared_ptr<CacheEntry> entry,
                      std::chrono::seconds ttl, InsertCallback callback) PURE;

  /**
   * Remove an entry from the cache.
   * Fire-and-forget operation (no callback).
   *
   * @param key the cache key to remove
   */
  virtual void remove(const std::string& key) PURE;

  /**
   * Get the backend type name for logging and debugging.
   * @return backend name (e.g., "local_lru", "redis_cluster", "tiered")
   */
  virtual std::string name() const PURE;
};

using CacheBackendPtr = std::unique_ptr<CacheBackend>;
using CacheBackendSharedPtr = std::shared_ptr<CacheBackend>;

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

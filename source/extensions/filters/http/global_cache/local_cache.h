#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "envoy/extensions/filters/http/global_cache/v3/global_cache.pb.h"

#include "source/extensions/filters/http/global_cache/cache_backend.h"

#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

/**
 * LRU cache node for doubly-linked list implementation.
 */
struct LruNode {
  std::string key;
  std::shared_ptr<CacheEntry> entry;
  std::chrono::seconds ttl;

  LruNode* prev{nullptr};
  LruNode* next{nullptr};
  size_t size_bytes{0};  // Estimated memory footprint

  LruNode(std::string k, std::shared_ptr<CacheEntry> e, std::chrono::seconds t, size_t size)
      : key(std::move(k)), entry(std::move(e)), ttl(t), size_bytes(size) {}
};

/**
 * Thread-safe local in-memory cache with LRU eviction.
 *
 * Uses doubly-linked list + hash map for O(1) operations:
 * - lookup: O(1) hash lookup + move to front
 * - insert: O(1) add to front + evict if needed
 * - remove: O(1) hash lookup + unlink
 *
 * Thread safety: Protected by a single mutex (absl::Mutex).
 * Eviction policy: LRU (Least Recently Used).
 * Size limits: Both max_entries and max_bytes enforced.
 */
class LocalCache : public CacheBackend {
public:
  /**
   * Create a local cache with the given configuration.
   * @param config proto configuration
   */
  explicit LocalCache(
      const envoy::extensions::filters::http::global_cache::v3::LocalCacheConfig& config);

  /**
   * Create a local cache with default settings.
   * Default: max_entries=1000, max_bytes=100MB
   */
  LocalCache();

  ~LocalCache() override;

  // CacheBackend interface (synchronous implementation)
  void lookup(const std::string& key, LookupCallback callback) override;
  void insert(const std::string& key, std::shared_ptr<CacheEntry> entry,
              std::chrono::seconds ttl, InsertCallback callback) override;
  void remove(const std::string& key) override;
  std::string name() const override { return "local_lru"; }

  // Stats accessors (for testing and debugging)
  size_t size() const;
  size_t bytes() const;

private:
  /**
   * Check if an entry is expired.
   * Must be called with mutex_ held.
   */
  bool isExpired(const CacheEntry& entry) const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  /**
   * Evict entries until size and byte limits are satisfied.
   * Evicts from tail (LRU) to head (MRU).
   * Must be called with mutex_ held.
   */
  void evictIfNeeded() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  /**
   * Move a node to the front (MRU position).
   * Must be called with mutex_ held.
   */
  void moveToFront(LruNode* node) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  /**
   * Add a new node to the front.
   * Must be called with mutex_ held.
   */
  void addToFront(LruNode* node) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  /**
   * Remove a node from the linked list.
   * Must be called with mutex_ held.
   */
  void removeNode(LruNode* node) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  /**
   * Calculate the memory footprint of a cache entry.
   * Approximates: headers size + body size + overhead.
   */
  static size_t calculateEntrySize(const CacheEntry& entry);

  mutable absl::Mutex mutex_;
  absl::flat_hash_map<std::string, std::unique_ptr<LruNode>> cache_ ABSL_GUARDED_BY(mutex_);

  LruNode* head_ ABSL_GUARDED_BY(mutex_){nullptr};  // Most recently used
  LruNode* tail_ ABSL_GUARDED_BY(mutex_){nullptr};  // Least recently used

  const size_t max_entries_;
  const size_t max_bytes_;
  size_t current_entries_ ABSL_GUARDED_BY(mutex_){0};
  size_t current_bytes_ ABSL_GUARDED_BY(mutex_){0};
};

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

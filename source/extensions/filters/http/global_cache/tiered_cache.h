#pragma once

#include "envoy/extensions/filters/http/global_cache/v3/global_cache.pb.h"

#include "source/extensions/filters/http/global_cache/cache_backend.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

/**
 * Two-tier cache implementation: L1 (local) + L2 (remote).
 *
 * Lookup flow:
 * 1. Check L1 (fast local cache)
 * 2. If L1 miss, check L2 (remote cache like Redis)
 * 3. If L2 hit, populate L1 and return entry
 * 4. If L2 miss, return miss
 *
 * Insert flow:
 * - WRITE_THROUGH: Write to both L1 and L2, complete when both succeed
 * - WRITE_BACK: Write to L1 immediately, async write to L2
 */
class TieredCache : public CacheBackend {
public:
  using WriteStrategy =
      envoy::extensions::filters::http::global_cache::v3::TieredCacheConfig::WriteStrategy;

  /**
   * Create a tiered cache with L1 and L2 backends.
   *
   * @param l1_cache the L1 (local) cache backend
   * @param l2_cache the L2 (remote) cache backend
   * @param write_strategy the write strategy (WRITE_THROUGH or WRITE_BACK)
   * @param populate_l1_on_l2_hit whether to populate L1 when L2 hits
   */
  TieredCache(CacheBackendSharedPtr l1_cache, CacheBackendSharedPtr l2_cache,
              WriteStrategy write_strategy, bool populate_l1_on_l2_hit = true);

  // CacheBackend interface
  void lookup(const std::string& key, LookupCallback callback) override;
  void insert(const std::string& key, std::shared_ptr<CacheEntry> entry,
              std::chrono::seconds ttl, InsertCallback callback) override;
  void remove(const std::string& key) override;
  std::string name() const override { return "tiered"; }

private:
  /**
   * Called when L1 lookup misses - check L2.
   */
  void lookupL2AndPopulateL1(const std::string& key, std::chrono::seconds remaining_ttl,
                             LookupCallback callback);

  /**
   * Calculate remaining TTL for an entry based on its expiration time.
   */
  std::chrono::seconds calculateRemainingTtl(const CacheEntry& entry) const;

  CacheBackendSharedPtr l1_cache_;
  CacheBackendSharedPtr l2_cache_;
  WriteStrategy write_strategy_;
  bool populate_l1_on_l2_hit_;
};

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

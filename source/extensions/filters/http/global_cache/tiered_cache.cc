#include "source/extensions/filters/http/global_cache/tiered_cache.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

TieredCache::TieredCache(CacheBackendSharedPtr l1_cache, CacheBackendSharedPtr l2_cache,
                         WriteStrategy write_strategy, bool populate_l1_on_l2_hit)
    : l1_cache_(std::move(l1_cache)), l2_cache_(std::move(l2_cache)),
      write_strategy_(write_strategy), populate_l1_on_l2_hit_(populate_l1_on_l2_hit) {}

void TieredCache::lookup(const std::string& key, LookupCallback callback) {
  // Step 1: Check L1 cache (local)
  l1_cache_->lookup(key, [this, key, callback](CacheLookupResult&& l1_result) {
    if (l1_result.status == CacheLookupStatus::Hit) {
      // L1 hit - return immediately
      callback(std::move(l1_result));
      return;
    }

    // Step 2: L1 miss - check L2 cache (remote)
    if (!l2_cache_) {
      // No L2 cache configured - return miss
      callback(CacheLookupResult{CacheLookupStatus::Miss});
      return;
    }

    l2_cache_->lookup(key, [this, key, callback](CacheLookupResult&& l2_result) {
      if (l2_result.status == CacheLookupStatus::Hit && l2_result.entry) {
        // L2 hit - optionally populate L1
        if (populate_l1_on_l2_hit_) {
          auto remaining_ttl = calculateRemainingTtl(*l2_result.entry);
          if (remaining_ttl.count() > 0) {
            // Populate L1 asynchronously (fire-and-forget)
            l1_cache_->insert(key, l2_result.entry, remaining_ttl, [](bool) {});
          }
        }
        // Return L2 result
        callback(std::move(l2_result));
      } else {
        // L2 miss or error - return miss
        callback(CacheLookupResult{CacheLookupStatus::Miss});
      }
    });
  });
}

void TieredCache::insert(const std::string& key, std::shared_ptr<CacheEntry> entry,
                         std::chrono::seconds ttl, InsertCallback callback) {
  if (write_strategy_ ==
      envoy::extensions::filters::http::global_cache::v3::TieredCacheConfig::WRITE_THROUGH) {
    // WRITE_THROUGH: Write to both L1 and L2, complete when both succeed
    // We need to track both completions
    struct CompletionState {
      bool l1_done = false;
      bool l2_done = false;
      bool l1_success = false;
      bool l2_success = false;
      bool callback_called = false;
      std::mutex mutex;
    };
    auto completion_state = std::make_shared<CompletionState>();

    // Insert into L1
    l1_cache_->insert(key, entry, ttl, [completion_state, callback](bool l1_success) {
      std::lock_guard<std::mutex> lock(completion_state->mutex);
      completion_state->l1_done = true;
      completion_state->l1_success = l1_success;

      // If both are done, invoke callback
      if (completion_state->l2_done && !completion_state->callback_called) {
        completion_state->callback_called = true;
        callback(completion_state->l1_success && completion_state->l2_success);
      }
    });

    // Insert into L2 (if available)
    if (l2_cache_) {
      l2_cache_->insert(key, entry, ttl, [completion_state, callback](bool l2_success) {
        std::lock_guard<std::mutex> lock(completion_state->mutex);
        completion_state->l2_done = true;
        completion_state->l2_success = l2_success;

        // If both are done, invoke callback
        if (completion_state->l1_done && !completion_state->callback_called) {
          completion_state->callback_called = true;
          callback(completion_state->l1_success && completion_state->l2_success);
        }
      });
    } else {
      // No L2 cache - just L1 result
      std::lock_guard<std::mutex> lock(completion_state->mutex);
      completion_state->l2_done = true;
      completion_state->l2_success = true;
      if (completion_state->l1_done && !completion_state->callback_called) {
        completion_state->callback_called = true;
        callback(completion_state->l1_success);
      }
    }

  } else {
    // WRITE_BACK: Write to L1 immediately, async write to L2
    l1_cache_->insert(key, entry, ttl, callback);

    // Fire-and-forget write to L2
    if (l2_cache_) {
      l2_cache_->insert(key, entry, ttl, [](bool) {});
    }
  }
}

void TieredCache::remove(const std::string& key) {
  // Remove from both caches
  l1_cache_->remove(key);
  if (l2_cache_) {
    l2_cache_->remove(key);
  }
}

std::chrono::seconds TieredCache::calculateRemainingTtl(const CacheEntry& entry) const {
  auto now = std::chrono::steady_clock::now();
  if (now >= entry.expiration_time) {
    return std::chrono::seconds(0);
  }

  auto remaining = std::chrono::duration_cast<std::chrono::seconds>(entry.expiration_time - now);
  return remaining;
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

#include "source/extensions/filters/http/global_cache/tiered_cache.h"
#include "source/extensions/filters/http/global_cache/local_cache.h"

#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {
namespace {

class TieredCacheTest : public testing::Test {
public:
  TieredCacheTest() {
    // Create L1 (local) cache
    envoy::extensions::filters::http::global_cache::v3::LocalCacheConfig l1_config;
    l1_config.set_max_entries(10);
    l1_config.set_max_bytes(100000);
    l1_cache_ = std::make_shared<LocalCache>(l1_config);

    // Create L2 (another local cache simulating remote)
    envoy::extensions::filters::http::global_cache::v3::LocalCacheConfig l2_config;
    l2_config.set_max_entries(100);
    l2_config.set_max_bytes(1000000);
    l2_cache_ = std::make_shared<LocalCache>(l2_config);
  }

protected:
  std::shared_ptr<CacheEntry> createEntry(const std::string& body_content) {
    Buffer::OwnedImpl body(body_content);
    auto headers = Http::ResponseHeaderMapImpl::create();
    headers->setStatus("200");
    auto expiration = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    return std::make_shared<CacheEntry>(std::move(body), std::move(headers), expiration);
  }

  CacheBackendSharedPtr l1_cache_;
  CacheBackendSharedPtr l2_cache_;
};

// Test write-through strategy
TEST_F(TieredCacheTest, WriteThroughStrategy) {
  auto tiered = std::make_shared<TieredCache>(
      l1_cache_, l2_cache_,
      envoy::extensions::filters::http::global_cache::v3::TieredCacheConfig::WRITE_THROUGH);

  auto entry = createEntry("test data");

  // Insert with write-through
  bool insert_done = false;
  bool insert_success = false;
  tiered->insert("key1", entry, std::chrono::seconds(300), [&](bool success) {
    insert_done = true;
    insert_success = success;
  });

  // Should complete and succeed
  EXPECT_TRUE(insert_done);
  EXPECT_TRUE(insert_success);

  // Verify both L1 and L2 have the entry
  auto l1_local = std::static_pointer_cast<LocalCache>(l1_cache_);
  auto l2_local = std::static_pointer_cast<LocalCache>(l2_cache_);
  EXPECT_EQ(1, l1_local->size());
  EXPECT_EQ(1, l2_local->size());
}

// Test write-back strategy
TEST_F(TieredCacheTest, WriteBackStrategy) {
  auto tiered = std::make_shared<TieredCache>(
      l1_cache_, l2_cache_,
      envoy::extensions::filters::http::global_cache::v3::TieredCacheConfig::WRITE_BACK);

  auto entry = createEntry("test data");

  // Insert with write-back
  bool insert_done = false;
  tiered->insert("key1", entry, std::chrono::seconds(300), [&](bool success) {
    insert_done = true;
    EXPECT_TRUE(success);
  });

  // Should complete immediately (L1 only)
  EXPECT_TRUE(insert_done);

  // Both L1 and L2 should have the entry (L2 written asynchronously but happens immediately in local)
  auto l1_local = std::static_pointer_cast<LocalCache>(l1_cache_);
  auto l2_local = std::static_pointer_cast<LocalCache>(l2_cache_);
  EXPECT_EQ(1, l1_local->size());
  EXPECT_EQ(1, l2_local->size());
}

// Test L1 hit
TEST_F(TieredCacheTest, L1Hit) {
  auto tiered = std::make_shared<TieredCache>(
      l1_cache_, l2_cache_,
      envoy::extensions::filters::http::global_cache::v3::TieredCacheConfig::WRITE_THROUGH);

  // Insert entry
  auto entry = createEntry("l1 data");
  tiered->insert("key1", entry, std::chrono::seconds(300), [](bool) {});

  // Lookup should hit L1
  CacheLookupResult result{CacheLookupStatus::Miss};
  tiered->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });

  EXPECT_EQ(CacheLookupStatus::Hit, result.status);
  EXPECT_NE(nullptr, result.entry);
  EXPECT_EQ("l1 data", result.entry->body.toString());
}

// Test L1 miss, L2 hit with population
TEST_F(TieredCacheTest, L1MissL2HitWithPopulation) {
  auto tiered = std::make_shared<TieredCache>(
      l1_cache_, l2_cache_,
      envoy::extensions::filters::http::global_cache::v3::TieredCacheConfig::WRITE_THROUGH, true);

  // Insert directly into L2 only
  auto entry = createEntry("l2 data");
  l2_cache_->insert("key1", entry, std::chrono::seconds(300), [](bool) {});

  // Verify L1 is empty, L2 has entry
  auto l1_local = std::static_pointer_cast<LocalCache>(l1_cache_);
  EXPECT_EQ(0, l1_local->size());

  // Lookup should miss L1, hit L2, and populate L1
  CacheLookupResult result{CacheLookupStatus::Miss};
  tiered->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });

  EXPECT_EQ(CacheLookupStatus::Hit, result.status);
  EXPECT_NE(nullptr, result.entry);
  EXPECT_EQ("l2 data", result.entry->body.toString());

  // L1 should now have the entry (populated from L2)
  EXPECT_EQ(1, l1_local->size());
}

// Test L1 miss, L2 hit without population
TEST_F(TieredCacheTest, L1MissL2HitWithoutPopulation) {
  auto tiered = std::make_shared<TieredCache>(
      l1_cache_, l2_cache_,
      envoy::extensions::filters::http::global_cache::v3::TieredCacheConfig::WRITE_THROUGH, false);

  // Insert directly into L2 only
  auto entry = createEntry("l2 data");
  l2_cache_->insert("key1", entry, std::chrono::seconds(300), [](bool) {});

  // Lookup should miss L1, hit L2, but NOT populate L1
  CacheLookupResult result{CacheLookupStatus::Miss};
  tiered->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });

  EXPECT_EQ(CacheLookupStatus::Hit, result.status);
  EXPECT_NE(nullptr, result.entry);

  // L1 should still be empty (no population)
  auto l1_local = std::static_pointer_cast<LocalCache>(l1_cache_);
  EXPECT_EQ(0, l1_local->size());
}

// Test L1 miss, L2 miss
TEST_F(TieredCacheTest, L1MissL2Miss) {
  auto tiered = std::make_shared<TieredCache>(
      l1_cache_, l2_cache_,
      envoy::extensions::filters::http::global_cache::v3::TieredCacheConfig::WRITE_THROUGH);

  // Lookup non-existent key
  CacheLookupResult result{CacheLookupStatus::Hit};
  tiered->lookup("nonexistent", [&result](CacheLookupResult&& r) { result = std::move(r); });

  EXPECT_EQ(CacheLookupStatus::Miss, result.status);
  EXPECT_EQ(nullptr, result.entry);
}

// Test remove from both tiers
TEST_F(TieredCacheTest, RemoveFromBothTiers) {
  auto tiered = std::make_shared<TieredCache>(
      l1_cache_, l2_cache_,
      envoy::extensions::filters::http::global_cache::v3::TieredCacheConfig::WRITE_THROUGH);

  // Insert entry
  auto entry = createEntry("data");
  tiered->insert("key1", entry, std::chrono::seconds(300), [](bool) {});

  // Verify both have the entry
  auto l1_local = std::static_pointer_cast<LocalCache>(l1_cache_);
  auto l2_local = std::static_pointer_cast<LocalCache>(l2_cache_);
  EXPECT_EQ(1, l1_local->size());
  EXPECT_EQ(1, l2_local->size());

  // Remove
  tiered->remove("key1");

  // Both should be empty
  EXPECT_EQ(0, l1_local->size());
  EXPECT_EQ(0, l2_local->size());
}

// Test with null L2 cache
TEST_F(TieredCacheTest, NullL2Cache) {
  auto tiered = std::make_shared<TieredCache>(
      l1_cache_, nullptr,
      envoy::extensions::filters::http::global_cache::v3::TieredCacheConfig::WRITE_THROUGH);

  // Insert should work (L1 only)
  auto entry = createEntry("l1 only");
  bool insert_done = false;
  tiered->insert("key1", entry, std::chrono::seconds(300), [&](bool success) {
    insert_done = true;
    EXPECT_TRUE(success);
  });
  EXPECT_TRUE(insert_done);

  // Lookup should work (L1 only)
  CacheLookupResult result{CacheLookupStatus::Miss};
  tiered->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(CacheLookupStatus::Hit, result.status);

  // Lookup miss should return miss (no L2 to check)
  tiered->lookup("nonexistent", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(CacheLookupStatus::Miss, result.status);
}

} // namespace
} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

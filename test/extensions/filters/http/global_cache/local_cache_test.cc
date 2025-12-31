#include "source/extensions/filters/http/global_cache/local_cache.h"

#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {
namespace {

class LocalCacheTest : public testing::Test {
public:
  LocalCacheTest() {
    envoy::extensions::filters::http::global_cache::v3::LocalCacheConfig config;
    config.set_max_entries(3);
    config.set_max_bytes(10000);
    cache_ = std::make_unique<LocalCache>(config);
  }

protected:
  std::shared_ptr<CacheEntry> createEntry(const std::string& body_content) {
    Buffer::OwnedImpl body(body_content);
    auto headers = Http::ResponseHeaderMapImpl::create();
    headers->setStatus("200");
    auto expiration = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    return std::make_shared<CacheEntry>(std::move(body), std::move(headers), expiration);
  }

  std::shared_ptr<CacheEntry> createExpiredEntry(const std::string& body_content) {
    Buffer::OwnedImpl body(body_content);
    auto headers = Http::ResponseHeaderMapImpl::create();
    headers->setStatus("200");
    auto expiration = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    return std::make_shared<CacheEntry>(std::move(body), std::move(headers), expiration);
  }

  std::unique_ptr<LocalCache> cache_;
};

// Test basic insert and lookup
TEST_F(LocalCacheTest, BasicInsertAndLookup) {
  auto entry = createEntry("response body");

  // Insert
  bool insert_success = false;
  cache_->insert("key1", entry, std::chrono::seconds(300),
                 [&insert_success](bool success) { insert_success = success; });
  EXPECT_TRUE(insert_success);
  EXPECT_EQ(cache_->size(), 1);

  // Lookup hit
  CacheLookupResult result{CacheLookupStatus::Miss};
  cache_->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Hit);
  EXPECT_NE(result.entry, nullptr);
  EXPECT_EQ(result.entry->body.toString(), "response body");
}

// Test cache miss
TEST_F(LocalCacheTest, CacheMiss) {
  CacheLookupResult result{CacheLookupStatus::Hit};
  cache_->lookup("nonexistent", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Miss);
  EXPECT_EQ(result.entry, nullptr);
}

// Test LRU eviction by entry count
TEST_F(LocalCacheTest, LruEvictionByCount) {
  auto entry1 = createEntry("body1");
  auto entry2 = createEntry("body2");
  auto entry3 = createEntry("body3");
  auto entry4 = createEntry("body4");

  // Insert 3 entries (max capacity)
  cache_->insert("key1", entry1, std::chrono::seconds(300), [](bool) {});
  cache_->insert("key2", entry2, std::chrono::seconds(300), [](bool) {});
  cache_->insert("key3", entry3, std::chrono::seconds(300), [](bool) {});
  EXPECT_EQ(cache_->size(), 3);

  // Insert 4th entry should evict key1 (LRU)
  cache_->insert("key4", entry4, std::chrono::seconds(300), [](bool) {});
  EXPECT_EQ(cache_->size(), 3);

  // key1 should be evicted
  CacheLookupResult result{CacheLookupStatus::Hit};
  cache_->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Miss);

  // key2, key3, key4 should still be present
  cache_->lookup("key2", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Hit);

  cache_->lookup("key3", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Hit);

  cache_->lookup("key4", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Hit);
}

// Test LRU update on access
TEST_F(LocalCacheTest, LruUpdateOnAccess) {
  auto entry1 = createEntry("body1");
  auto entry2 = createEntry("body2");
  auto entry3 = createEntry("body3");
  auto entry4 = createEntry("body4");

  // Insert 3 entries
  cache_->insert("key1", entry1, std::chrono::seconds(300), [](bool) {});
  cache_->insert("key2", entry2, std::chrono::seconds(300), [](bool) {});
  cache_->insert("key3", entry3, std::chrono::seconds(300), [](bool) {});

  // Access key1 to make it MRU
  CacheLookupResult result{CacheLookupStatus::Miss};
  cache_->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Hit);

  // Insert key4, should evict key2 (now LRU)
  cache_->insert("key4", entry4, std::chrono::seconds(300), [](bool) {});

  // key2 should be evicted
  cache_->lookup("key2", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Miss);

  // key1 should still be present
  cache_->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Hit);
}

// Test expired entry is removed on lookup
TEST_F(LocalCacheTest, ExpiredEntryRemoved) {
  auto expired_entry = createExpiredEntry("expired body");

  cache_->insert("expired", expired_entry, std::chrono::seconds(300), [](bool) {});
  EXPECT_EQ(cache_->size(), 1);

  // Lookup should return miss and remove expired entry
  CacheLookupResult result{CacheLookupStatus::Hit};
  cache_->lookup("expired", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Miss);
  EXPECT_EQ(cache_->size(), 0);
}

// Test update existing entry
TEST_F(LocalCacheTest, UpdateExistingEntry) {
  auto entry1 = createEntry("body1");
  auto entry2 = createEntry("updated body");

  // Insert
  cache_->insert("key1", entry1, std::chrono::seconds(300), [](bool) {});
  EXPECT_EQ(cache_->size(), 1);

  // Update
  cache_->insert("key1", entry2, std::chrono::seconds(300), [](bool) {});
  EXPECT_EQ(cache_->size(), 1);  // Size should not change

  // Verify updated content
  CacheLookupResult result{CacheLookupStatus::Miss};
  cache_->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Hit);
  EXPECT_EQ(result.entry->body.toString(), "updated body");
}

// Test remove
TEST_F(LocalCacheTest, Remove) {
  auto entry = createEntry("body");

  cache_->insert("key1", entry, std::chrono::seconds(300), [](bool) {});
  EXPECT_EQ(cache_->size(), 1);

  cache_->remove("key1");
  EXPECT_EQ(cache_->size(), 0);

  CacheLookupResult result{CacheLookupStatus::Hit};
  cache_->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Miss);
}

// Test remove nonexistent key (should not crash)
TEST_F(LocalCacheTest, RemoveNonexistent) {
  cache_->remove("nonexistent");
  EXPECT_EQ(cache_->size(), 0);
}

// Test default constructor
TEST_F(LocalCacheTest, DefaultConstructor) {
  auto default_cache = std::make_unique<LocalCache>();
  auto entry = createEntry("body");

  bool insert_success = false;
  default_cache->insert("key1", entry, std::chrono::seconds(300),
                        [&insert_success](bool success) { insert_success = success; });
  EXPECT_TRUE(insert_success);

  CacheLookupResult result{CacheLookupStatus::Miss};
  default_cache->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });
  EXPECT_EQ(result.status, CacheLookupStatus::Hit);
  EXPECT_EQ(default_cache->name(), "local_lru");
}

// Test entry too large for cache
TEST_F(LocalCacheTest, EntryTooLarge) {
  envoy::extensions::filters::http::global_cache::v3::LocalCacheConfig config;
  config.set_max_entries(10);
  config.set_max_bytes(100);  // Very small cache
  auto small_cache = std::make_unique<LocalCache>(config);

  // Create a large entry
  std::string large_body(1000, 'x');
  auto large_entry = createEntry(large_body);

  bool insert_success = true;
  small_cache->insert("large", large_entry, std::chrono::seconds(300),
                      [&insert_success](bool success) { insert_success = success; });
  EXPECT_FALSE(insert_success);  // Should reject entry too large for cache
  EXPECT_EQ(small_cache->size(), 0);
}

} // namespace
} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

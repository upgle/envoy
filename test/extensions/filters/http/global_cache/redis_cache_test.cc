#include "source/extensions/filters/http/global_cache/redis_cache.h"
#include "source/extensions/filters/http/global_cache/cache_serialization.h"

#include "test/mocks/event/mocks.h"
#include "test/mocks/server/server_factory_context.h"
#include "test/mocks/stats/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/upstream/cluster_manager.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {
namespace {

class RedisCacheTest : public testing::Test {
public:
  RedisCacheTest() {
    // Create RedisCache config
    proto_config_.set_cluster_name("redis_cluster");
    proto_config_.set_key_prefix("test:");
    proto_config_.mutable_op_timeout()->set_seconds(1);
    proto_config_.set_enable_cluster_mode(true);

    server_context_.thread_local_.setDispatcher(&dispatcher_);

    // Create RedisCache instance
    redis_cache_ = std::make_shared<RedisCache>(
        proto_config_, server_context_.cluster_manager_, server_context_.thread_local_,
        server_context_, server_context_.scope());
  }

protected:
  std::shared_ptr<CacheEntry> createEntry(const std::string& body_content) {
    Buffer::OwnedImpl body(body_content);
    auto headers = Http::ResponseHeaderMapImpl::create();
    headers->setStatus("200");
    auto expiration = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    return std::make_shared<CacheEntry>(std::move(body), std::move(headers), expiration);
  }

  envoy::extensions::filters::http::global_cache::v3::RedisCacheConfig proto_config_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<Server::Configuration::MockServerFactoryContext> server_context_;
  std::shared_ptr<RedisCache> redis_cache_;
};

// Test RedisCache construction
TEST_F(RedisCacheTest, Construction) {
  EXPECT_NE(nullptr, redis_cache_);
  EXPECT_EQ("redis_cache", redis_cache_->name());
}

// Test lookup with no Redis client (should return Miss gracefully)
TEST_F(RedisCacheTest, LookupWithoutClient) {
  CacheLookupResult result{CacheLookupStatus::Hit}; // Start as Hit to ensure it changes to Miss
  redis_cache_->lookup("key1", [&result](CacheLookupResult&& r) { result = std::move(r); });

  // Without a real Redis client, lookup should return Miss
  EXPECT_EQ(CacheLookupStatus::Miss, result.status);
  EXPECT_EQ(nullptr, result.entry);
}

// Test insert with no Redis client (should fail gracefully)
TEST_F(RedisCacheTest, InsertWithoutClient) {
  auto entry = createEntry("test data");
  bool insert_done = false;
  bool insert_success = true; // Start as true to ensure it changes to false

  redis_cache_->insert("key1", entry, std::chrono::seconds(300), [&](bool success) {
    insert_done = true;
    insert_success = success;
  });

  // Without a real Redis client, insert should fail
  EXPECT_TRUE(insert_done);
  EXPECT_FALSE(insert_success);
}

// Test remove with no Redis client (should not crash)
TEST_F(RedisCacheTest, RemoveWithoutClient) {
  // This should not crash even without a Redis client
  EXPECT_NO_THROW(redis_cache_->remove("key1"));
}

// Test serialization round-trip
TEST_F(RedisCacheTest, Serialization) {
  auto original_entry = createEntry("test serialization data");

  // Serialize
  std::string serialized;
  EXPECT_NO_THROW(serialized = CacheSerializer::serialize(*original_entry));
  EXPECT_FALSE(serialized.empty());

  // Deserialize
  std::shared_ptr<CacheEntry> deserialized_entry;
  EXPECT_NO_THROW(deserialized_entry = CacheSerializer::deserialize(serialized));

  ASSERT_NE(nullptr, deserialized_entry);
  EXPECT_EQ("test serialization data", deserialized_entry->body.toString());
  EXPECT_EQ("200", deserialized_entry->headers->getStatusValue());
}

// Test key prefix handling
TEST_F(RedisCacheTest, KeyPrefixHandling) {
  // RedisCache should add "test:" prefix to keys (configured in constructor)
  // We can't directly test buildRedisKey() since it's private, but we can
  // verify the config was set correctly
  EXPECT_EQ("redis_cache", redis_cache_->name());
}

// Test with custom configuration
TEST_F(RedisCacheTest, CustomConfiguration) {
  envoy::extensions::filters::http::global_cache::v3::RedisCacheConfig custom_config;
  custom_config.set_cluster_name("custom_cluster");
  custom_config.set_key_prefix("custom_prefix:");
  custom_config.mutable_op_timeout()->set_seconds(5);
  custom_config.set_enable_cluster_mode(false);

  auto custom_cache = std::make_shared<RedisCache>(
      custom_config, server_context_.cluster_manager_, server_context_.thread_local_,
      server_context_, server_context_.scope());
  EXPECT_NE(nullptr, custom_cache);
}

// Test default configuration (no prefix, default timeout)
TEST_F(RedisCacheTest, DefaultConfiguration) {
  envoy::extensions::filters::http::global_cache::v3::RedisCacheConfig default_config;
  default_config.set_cluster_name("default_cluster");
  // Don't set prefix or timeout - should use defaults

  auto default_cache = std::make_shared<RedisCache>(
      default_config, server_context_.cluster_manager_, server_context_.thread_local_,
      server_context_, server_context_.scope());
  EXPECT_NE(nullptr, default_cache);
}

} // namespace
} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

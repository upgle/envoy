#include "source/extensions/filters/http/global_cache/global_cache_filter.h"
#include "source/extensions/filters/http/global_cache/local_cache.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/server/factory_context.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::NiceMock;
using testing::Return;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

class GlobalCacheFilterTest : public testing::Test {
public:
  GlobalCacheFilterTest() {
    // Create a local cache backend for testing
    cache_backend_ = std::make_shared<LocalCache>();

    envoy::extensions::filters::http::global_cache::v3::GlobalCache proto_config;
    config_ = std::make_shared<GlobalCacheFilterConfig>(proto_config, cache_backend_);
  }

  void setupFilter() {
    filter_ = std::make_shared<GlobalCacheFilter>(config_);
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
    filter_->setEncoderFilterCallbacks(encoder_callbacks_);
  }

protected:
  CacheBackendSharedPtr cache_backend_;
  GlobalCacheFilterConfigSharedPtr config_;
  std::shared_ptr<GlobalCacheFilter> filter_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;
};

// Test cache miss - first request goes to upstream
TEST_F(GlobalCacheFilterTest, CacheMiss) {
  setupFilter();

  // First request - should be a cache miss
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/api/data"}, {":authority", "example.com"}};

  // decodeHeaders should return Continue (cache miss, forward to upstream)
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  // Simulate upstream response
  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  // Response body
  Buffer::OwnedImpl response_body("test response");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_body, true));

  // Verify response was cached (check cache backend size)
  auto local_cache = std::static_pointer_cast<LocalCache>(cache_backend_);
  EXPECT_EQ(1, local_cache->size());
}

// Test cache hit - second request served from cache
TEST_F(GlobalCacheFilterTest, CacheHit) {
  // First request - populate cache
  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/api/data"}, {":authority", "example.com"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Buffer::OwnedImpl response_body("cached data");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_body, true));

  // Second request with same key - should hit cache
  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers2{
      {":method", "GET"}, {":path", "/api/data"}, {":authority", "example.com"}};

  // decodeHeaders should return StopAllIterationAndWatermark (cache hit)
  // The filter will call decoder_callbacks_.encodeHeaders() and encodeData() internally
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers2, true));
}

// Test different cache keys
TEST_F(GlobalCacheFilterTest, DifferentCacheKeys) {
  // First request
  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers1{
      {":method", "GET"}, {":path", "/api/data1"}, {":authority", "example.com"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers1, true));

  Http::TestResponseHeaderMapImpl response_headers1{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers1, false));

  Buffer::OwnedImpl response_body1("data1");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_body1, true));

  // Second request with different path
  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers2{
      {":method", "GET"}, {":path", "/api/data2"}, {":authority", "example.com"}};

  // Should be cache miss (different key)
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers2, true));

  // Should have 1 entry in cache (first request)
  auto local_cache = std::static_pointer_cast<LocalCache>(cache_backend_);
  EXPECT_EQ(1, local_cache->size());
}

// Test empty body response
TEST_F(GlobalCacheFilterTest, EmptyBodyResponse) {
  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/api/empty"}, {":authority", "example.com"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  // Response with no body (end_stream = true in headers)
  Http::TestResponseHeaderMapImpl response_headers{{":status", "204"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, true));

  // Should be cached even with no body
  auto local_cache = std::static_pointer_cast<LocalCache>(cache_backend_);
  EXPECT_EQ(1, local_cache->size());

  // Second request should hit cache
  setupFilter();
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers, true));
}

// Test cache with multiple data chunks
TEST_F(GlobalCacheFilterTest, MultipleDataChunks) {
  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/api/chunked"}, {":authority", "example.com"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  // Multiple data chunks
  Buffer::OwnedImpl chunk1("chunk1");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(chunk1, false));

  Buffer::OwnedImpl chunk2("chunk2");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(chunk2, false));

  Buffer::OwnedImpl chunk3("chunk3");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(chunk3, true));

  // Should be cached
  auto local_cache = std::static_pointer_cast<LocalCache>(cache_backend_);
  EXPECT_EQ(1, local_cache->size());

  // Verify cached body contains all chunks by looking up the entry
  std::shared_ptr<CacheEntry> cached_entry;
  local_cache->lookup("GET:example.com:/api/chunked", [&cached_entry](CacheLookupResult&& result) {
    cached_entry = result.entry;
  });
  ASSERT_NE(cached_entry, nullptr);
  EXPECT_EQ("chunk1chunk2chunk3", cached_entry->body.toString());
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

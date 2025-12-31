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

  void setFilterConfig(
      const envoy::extensions::filters::http::global_cache::v3::GlobalCache& proto_config) {
    config_ = std::make_shared<GlobalCacheFilterConfig>(proto_config, cache_backend_);
  }

  void setPerRouteConfig(
      const envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute& proto_config) {
    per_route_config_ = std::make_shared<GlobalCachePerRouteConfig>(proto_config);
    ON_CALL(decoder_callbacks_, mostSpecificPerFilterConfig())
        .WillByDefault(Return(per_route_config_.get()));
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
  std::shared_ptr<GlobalCachePerRouteConfig> per_route_config_;
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

TEST_F(GlobalCacheFilterTest, PerRouteDisableSkipsCache) {
  envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute per_route;
  per_route.set_disabled(true);
  setPerRouteConfig(per_route);
  setupFilter();

  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/api/disabled"}, {":authority", "example.com"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Buffer::OwnedImpl response_body("disabled response");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_body, true));

  auto local_cache = std::static_pointer_cast<LocalCache>(cache_backend_);
  EXPECT_EQ(0, local_cache->size());
  EXPECT_TRUE(response_headers.get(Http::LowerCaseString("x-cache")).empty());
}

TEST_F(GlobalCacheFilterTest, PerRouteExcludeQueryParams) {
  envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute per_route;
  auto* overrides = per_route.mutable_overrides();
  overrides->mutable_include_query_params()->set_value(false);
  setPerRouteConfig(per_route);

  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers1{
      {":method", "GET"}, {":path", "/api/data?user=1"}, {":authority", "example.com"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers1, true));

  Http::TestResponseHeaderMapImpl response_headers1{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers1, false));

  Buffer::OwnedImpl response_body1("data");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_body1, true));

  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers2{
      {":method", "GET"}, {":path", "/api/data?user=2"}, {":authority", "example.com"}};

  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers2, true));
}

TEST_F(GlobalCacheFilterTest, PerRouteDefaultTtlOverride) {
  envoy::extensions::filters::http::global_cache::v3::GlobalCachePerRoute per_route;
  auto* overrides = per_route.mutable_overrides();
  overrides->mutable_default_ttl()->set_seconds(1);
  setPerRouteConfig(per_route);
  setupFilter();

  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/api/ttl"}, {":authority", "example.com"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, true));

  std::shared_ptr<CacheEntry> cached_entry;
  auto local_cache = std::static_pointer_cast<LocalCache>(cache_backend_);
  local_cache->lookup("GET:example.com:/api/ttl", [&cached_entry](CacheLookupResult&& result) {
    cached_entry = result.entry;
  });
  ASSERT_NE(cached_entry, nullptr);
  const auto ttl = cached_entry->expiration_time - start;
  EXPECT_GE(ttl, std::chrono::milliseconds(500));
  EXPECT_LE(ttl, std::chrono::seconds(2));
}

TEST_F(GlobalCacheFilterTest, CacheKeyIncludesHeaders) {
  envoy::extensions::filters::http::global_cache::v3::GlobalCache proto_config;
  proto_config.mutable_cache_key()->add_headers_included("x-user");
  setFilterConfig(proto_config);

  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers1{
      {":method", "GET"},
      {":path", "/api/data"},
      {":authority", "example.com"},
      {"x-user", "u1"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers1, true));
  Http::TestResponseHeaderMapImpl response_headers1{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers1, false));
  Buffer::OwnedImpl response_body1("data1");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_body1, true));

  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers2{
      {":method", "GET"},
      {":path", "/api/data"},
      {":authority", "example.com"},
      {"x-user", "u2"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers2, true));
  Http::TestResponseHeaderMapImpl response_headers2{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers2, false));
  Buffer::OwnedImpl response_body2("data2");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_body2, true));

  auto local_cache = std::static_pointer_cast<LocalCache>(cache_backend_);
  EXPECT_EQ(2, local_cache->size());
}

TEST_F(GlobalCacheFilterTest, CacheKeyQueryParamAllowlist) {
  envoy::extensions::filters::http::global_cache::v3::GlobalCache proto_config;
  auto* cache_key = proto_config.mutable_cache_key();
  cache_key->mutable_include_query_params()->set_value(true);
  cache_key->add_query_params_included("user");
  setFilterConfig(proto_config);

  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers1{
      {":method", "GET"},
      {":path", "/api/data?user=1&role=admin"},
      {":authority", "example.com"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers1, true));
  Http::TestResponseHeaderMapImpl response_headers1{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers1, false));
  Buffer::OwnedImpl response_body1("data");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_body1, true));

  setupFilter();
  Http::TestRequestHeaderMapImpl request_headers2{
      {":method", "GET"},
      {":path", "/api/data?user=1&role=guest"},
      {":authority", "example.com"}};
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers2, true));
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

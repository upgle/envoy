#include "source/extensions/filters/http/global_cache/global_cache_filter.h"

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
namespace {

class GlobalCacheFilterTest : public testing::Test {
public:
  GlobalCacheFilterTest() {
    envoy::extensions::filters::http::global_cache::v3::GlobalCache proto_config;
    config_ = std::make_shared<GlobalCacheFilterConfig>(proto_config);
    filter_ = std::make_shared<GlobalCacheFilter>(config_);
    filter_->setEncoderFilterCallbacks(encoder_callbacks_);
  }

protected:
  GlobalCacheFilterConfigSharedPtr config_;
  std::shared_ptr<GlobalCacheFilter> filter_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;
};

// Test that the filter replaces the response body with "cached"
TEST_F(GlobalCacheFilterTest, ReplacesResponseBody) {
  // Setup headers
  Http::TestResponseHeaderMapImpl headers{{":status", "200"}};

  // Call encodeHeaders - should remove Content-Length
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, false));

  // Verify Content-Length was removed
  EXPECT_FALSE(headers.ContentLength());

  // Setup data with original body
  Buffer::OwnedImpl data("original response body");

  // Call encodeData with end_stream = true
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data, true));

  // Verify the body was replaced with "cached"
  EXPECT_EQ("cached", data.toString());
}

// Test with multiple data chunks
TEST_F(GlobalCacheFilterTest, HandlesMultipleDataChunks) {
  Http::TestResponseHeaderMapImpl headers{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, false));

  // First chunk - should be drained
  Buffer::OwnedImpl data1("chunk1");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data1, false));
  EXPECT_EQ(0, data1.length());

  // Second chunk - should be drained
  Buffer::OwnedImpl data2("chunk2");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data2, false));
  EXPECT_EQ(0, data2.length());

  // Final chunk - should add "cached"
  Buffer::OwnedImpl data3("chunk3");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data3, true));
  EXPECT_EQ("cached", data3.toString());
}

// Test with no body (end_stream = true in headers)
TEST_F(GlobalCacheFilterTest, HandlesNoBody) {
  Http::TestResponseHeaderMapImpl headers{{":status", "204"}};

  // When end_stream is true in headers, should just continue
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, true));

  // Content-Length should still be present if it was there
  headers.setContentLength(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, true));
}

// Test empty response body
TEST_F(GlobalCacheFilterTest, HandlesEmptyBody) {
  Http::TestResponseHeaderMapImpl headers{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, false));

  // Empty data with end_stream
  Buffer::OwnedImpl data("");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data, true));

  // Should still add "cached"
  EXPECT_EQ("cached", data.toString());
}

// Test large response body
TEST_F(GlobalCacheFilterTest, HandlesLargeBody) {
  Http::TestResponseHeaderMapImpl headers{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(headers, false));

  // Large data
  std::string large_body(10000, 'x');
  Buffer::OwnedImpl data(large_body);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data, true));

  // Should be replaced with "cached"
  EXPECT_EQ("cached", data.toString());
  EXPECT_EQ(6, data.length()); // "cached" is 6 bytes
}

} // namespace
} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

# Envoy Testing

## Test Commands

### Run filter tests
```bash
ENVOY_DOCKER_BUILD_DIR=/home/seonghyun/envoy-build \
  ./ci/run_envoy_docker.sh \
  'bazel test -c fastbuild --config=clang //test/extensions/filters/http/FILTER/...'
```

### Run specific test
```bash
bazel test -c fastbuild --config=clang //test/extensions/filters/http/FILTER:specific_test
```

### With output
```bash
bazel test --test_output=all //test/extensions/filters/http/FILTER/...
```

### With verbose logging
```bash
bazel test --test_output=streamed //test/... --test_arg="--" --test_arg="-l trace"
```

### Skip cache
```bash
bazel test //test/... --cache_test_results=no
```

## Unit Test Template

```cpp
#include "source/extensions/filters/http/YOUR_FILTER/filter.h"
#include "test/mocks/http/mocks.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace YourFilter {
namespace {

class YourFilterTest : public testing::Test {
public:
  YourFilterTest() {
    config_ = std::make_shared<FilterConfig>(proto_config_);
    filter_ = std::make_shared<Filter>(config_);
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
  }

protected:
  FilterConfigProto proto_config_;
  FilterConfigSharedPtr config_;
  std::shared_ptr<Filter> filter_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
};

TEST_F(YourFilterTest, BasicTest) {
  Http::TestRequestHeaderMapImpl headers{{":method", "GET"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue,
            filter_->decodeHeaders(headers, true));
}

} // namespace
} // namespace YourFilter
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
```

## Test BUILD File

```python
envoy_extension_cc_test(
    name = "filter_test",
    srcs = ["filter_test.cc"],
    extension_names = ["envoy.filters.http.YOUR_FILTER"],
    deps = [
        "//source/extensions/filters/http/YOUR_FILTER:filter_lib",
        "//test/mocks/http:http_mocks",
        "//test/test_common:utility_lib",
    ],
)
```

## Sanitizer Tests

```bash
# ASAN + UBSAN
bazel test -c dbg --config=asan //test/...

# TSAN (Docker sandbox)
bazel test -c dbg --config=docker-tsan //test/...

# MSAN (Docker sandbox)
bazel test -c dbg --config=docker-msan //test/...
```

## Coverage

```bash
# Full coverage
test/run_envoy_bazel_coverage.sh

# Specific test
VALIDATE_COVERAGE=false test/run_envoy_bazel_coverage.sh //test/extensions/filters/http/FILTER:test
```

Result: `generated/coverage/coverage.html`

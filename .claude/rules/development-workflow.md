# Envoy Development Workflow

## HTTP Filter Structure

```
source/extensions/filters/http/FILTER/
├── BUILD
├── config.h
├── config.cc
├── filter.h
└── filter.cc

api/envoy/extensions/filters/http/FILTER/v3/
├── BUILD
└── filter.proto

test/extensions/filters/http/FILTER/
├── BUILD
└── filter_test.cc
```

## Adding New Filter

1. **Create filter files** (structure above)

2. **Register in extensions_build_config.bzl**:
   ```python
   # source/extensions/extensions_build_config.bzl
   EXTENSIONS = {
       "envoy.filters.http.YOUR_FILTER": "//source/extensions/filters/http/YOUR_FILTER:config",
   }
   ```
   > First build after this change takes 10-20 min (normal). Subsequent builds fast.

3. **Development cycle**:
   ```bash
   # Edit code
   vim source/extensions/filters/http/FILTER/filter.cc

   # Build (16 sec)
   ENVOY_DOCKER_BUILD_DIR=/home/seonghyun/envoy-build \
     ./ci/run_envoy_docker.sh \
     'bazel build -c fastbuild --config=clang //source/extensions/filters/http/FILTER:all'

   # Test (30 sec)
   ENVOY_DOCKER_BUILD_DIR=/home/seonghyun/envoy-build \
     ./ci/run_envoy_docker.sh \
     'bazel test -c fastbuild --config=clang //test/extensions/filters/http/FILTER/...'
   ```

## Proto File Template

```protobuf
syntax = "proto3";
package envoy.extensions.filters.http.YOUR_FILTER.v3;

import "google/protobuf/duration.proto";
import "validate/validate.proto";
import "udpa/annotations/status.proto";

option (udpa.annotations.file_status).package_version_status = ACTIVE;

message FilterConfig {
  google.protobuf.Duration timeout = 1 [(validate.rules).duration = {
    gte {seconds: 0}
    lte {seconds: 300}
  }];
}
```

## Reading Proto Config in C++

```cpp
// Duration field with default
if (proto_config.has_timeout()) {
  timeout_ = std::chrono::milliseconds(
      PROTOBUF_GET_MS_REQUIRED(proto_config, timeout));
} else {
  timeout_ = std::chrono::milliseconds(5000);
}
```

## Commit Message Format

```
component: brief description in imperative mood

Longer description explaining what and why.

Changes:
- Specific change 1
- Specific change 2
```

Example:
```
http: implement single-flight pattern for global_cache filter

Replace semaphore-based concurrency control with single-flight pattern.

Changes:
- Add InFlightRequest struct with condition_variable
- Track in-flight requests per cache key
- Add configurable timeout
```

# Envoy Architecture

## Directory Structure

```
source/
├── common/           # Shared utilities (buffer, http, network, config)
├── extensions/       # Pluggable extensions
│   └── filters/
│       ├── http/     # HTTP L7 filters (most dev happens here)
│       └── network/  # L4 network filters
├── server/           # Standalone server implementation
└── exe/              # Main binary (envoy-static)

api/                  # Protocol buffer definitions
├── envoy/extensions/filters/http/*/v3/*.proto

test/                 # Tests (mirrors source/ structure)
├── mocks/            # Mock implementations
└── test_common/      # Test utilities

contrib/              # Community-contributed extensions
mobile/               # Envoy Mobile
```

## Key Files

- `source/extensions/extensions_build_config.bzl` - Extension registry
- `.bazelrc` - Build config (C++20, clang)
- `bazel/repository_locations.bzl` - Dependency versions
- `STYLE.md` - Full coding standards
- `CONTRIBUTING.md` - PR guidelines

## Filter Lifecycle

```cpp
// Request phase (decoder)
Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap&, bool end_stream);
Http::FilterDataStatus decodeData(Buffer::Instance&, bool end_stream);
Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap&);

// Response phase (encoder)
Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap&, bool end_stream);
Http::FilterDataStatus encodeData(Buffer::Instance&, bool end_stream);
Http::FilterTrailersStatus encodeTrailers(Http::ResponseTrailerMap&);
```

## Testing Notes

- Default: `StrictMock` (fails on unexpected calls)
- Use `NiceMock` when mock behavior isn't the test focus
- Use `Event::SimulatedTimeSystem` for time-dependent tests (fast, deterministic)

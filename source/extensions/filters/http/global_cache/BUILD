load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

# Cache backend interface (header-only)
envoy_cc_library(
    name = "cache_backend_interface",
    hdrs = ["cache_backend.h"],
    deps = [
        "//envoy/buffer:buffer_interface",
        "//envoy/http:header_map_interface",
        "//source/common/buffer:buffer_lib",
    ],
)

# Cache serialization utilities
envoy_cc_library(
    name = "cache_serialization_lib",
    srcs = ["cache_serialization.cc"],
    hdrs = ["cache_serialization.h"],
    deps = [
        ":cache_backend_interface",
        "//source/common/http:header_map_lib",
    ],
)

# Local LRU cache implementation
envoy_cc_library(
    name = "local_cache_lib",
    srcs = ["local_cache.cc"],
    hdrs = ["local_cache.h"],
    deps = [
        ":cache_backend_interface",
        "//source/common/common:assert_lib",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/synchronization",
        "@envoy_api//envoy/extensions/filters/http/global_cache/v3:pkg_cc_proto",
    ],
)

# Tiered cache implementation
envoy_cc_library(
    name = "tiered_cache_lib",
    srcs = ["tiered_cache.cc"],
    hdrs = ["tiered_cache.h"],
    deps = [
        ":cache_backend_interface",
        "@envoy_api//envoy/extensions/filters/http/global_cache/v3:pkg_cc_proto",
    ],
)

# Redis cache implementation
envoy_cc_library(
    name = "redis_cache_lib",
    srcs = ["redis_cache.cc"],
    hdrs = ["redis_cache.h"],
    deps = [
        ":cache_backend_interface",
        ":cache_serialization_lib",
        "//envoy/event:dispatcher_interface",
        "//envoy/stats:stats_interface",
        "//envoy/upstream:cluster_manager_interface",
        "//source/common/common:assert_lib",
        "//source/common/common:logger_lib",
        "//source/common/protobuf:utility_lib",
        "//source/extensions/common/redis:cluster_refresh_manager_lib",
        "//source/extensions/filters/network/common/redis:client_lib",
        "//source/extensions/filters/network/common/redis:codec_lib",
        "//source/extensions/filters/network/common/redis:redis_command_stats_lib",
        "//source/extensions/filters/network/common/redis:utility_lib",
        "//source/extensions/filters/network/redis_proxy:conn_pool_lib",
        "@envoy_api//envoy/extensions/filters/http/global_cache/v3:pkg_cc_proto",
        "@com_google_absl//absl/strings",
    ],
)

envoy_cc_library(
    name = "global_cache_filter_lib",
    srcs = ["global_cache_filter.cc"],
    hdrs = ["global_cache_filter.h"],
    deps = [
        ":cache_backend_interface",
        "//envoy/http:filter_interface",
        "//source/common/buffer:buffer_lib",
        "//source/common/common:logger_lib",
        "//source/common/http:header_map_lib",
        "//source/common/http:headers_lib",
        "//source/common/http:utility_lib",
        "//source/extensions/filters/http/common:pass_through_filter_lib",
        "@envoy_api//envoy/extensions/filters/http/global_cache/v3:pkg_cc_proto",
    ],
)

envoy_cc_extension(
    name = "config",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    deps = [
        ":global_cache_filter_lib",
        ":local_cache_lib",
        ":redis_cache_lib",
        ":tiered_cache_lib",
        "//envoy/registry",
        "//source/extensions/filters/http/common:factory_base_lib",
        "@envoy_api//envoy/extensions/filters/http/global_cache/v3:pkg_cc_proto",
    ],
)

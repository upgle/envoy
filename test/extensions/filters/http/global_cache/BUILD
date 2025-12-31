load(
    "//bazel:envoy_build_system.bzl",
    "envoy_package",
)
load(
    "//test/extensions:extensions_build_system.bzl",
    "envoy_extension_cc_test",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_extension_cc_test(
    name = "local_cache_test",
    srcs = ["local_cache_test.cc"],
    extension_names = ["envoy.filters.http.global_cache"],
    deps = [
        "//source/extensions/filters/http/global_cache:local_cache_lib",
        "//test/test_common:utility_lib",
        "@envoy_api//envoy/extensions/filters/http/global_cache/v3:pkg_cc_proto",
    ],
)

envoy_extension_cc_test(
    name = "tiered_cache_test",
    srcs = ["tiered_cache_test.cc"],
    extension_names = ["envoy.filters.http.global_cache"],
    deps = [
        "//source/extensions/filters/http/global_cache:local_cache_lib",
        "//source/extensions/filters/http/global_cache:tiered_cache_lib",
        "//test/test_common:utility_lib",
        "@envoy_api//envoy/extensions/filters/http/global_cache/v3:pkg_cc_proto",
    ],
)

envoy_extension_cc_test(
    name = "redis_cache_test",
    srcs = ["redis_cache_test.cc"],
    extension_names = ["envoy.filters.http.global_cache"],
    deps = [
        "//source/extensions/filters/http/global_cache:cache_serialization_lib",
        "//source/extensions/filters/http/global_cache:redis_cache_lib",
        "//test/mocks/event:event_mocks",
        "//test/mocks/stats:stats_mocks",
        "//test/mocks/upstream:cluster_manager_mocks",
        "//test/test_common:utility_lib",
        "@envoy_api//envoy/extensions/filters/http/global_cache/v3:pkg_cc_proto",
    ],
)

envoy_extension_cc_test(
    name = "global_cache_filter_test",
    srcs = ["global_cache_filter_test.cc"],
    extension_names = ["envoy.filters.http.global_cache"],
    deps = [
        "//source/extensions/filters/http/global_cache:config",
        "//source/extensions/filters/http/global_cache:global_cache_filter_lib",
        "//source/extensions/filters/http/global_cache:local_cache_lib",
        "//test/mocks/http:http_mocks",
        "//test/mocks/server:factory_context_mocks",
        "@envoy_api//envoy/extensions/filters/http/global_cache/v3:pkg_cc_proto",
    ],
)

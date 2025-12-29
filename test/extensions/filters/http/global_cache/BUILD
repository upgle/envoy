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
    name = "global_cache_filter_test",
    srcs = ["global_cache_filter_test.cc"],
    extension_names = ["envoy.filters.http.global_cache"],
    deps = [
        "//source/extensions/filters/http/global_cache:global_cache_filter_lib",
        "//test/mocks/http:http_mocks",
        "//test/mocks/server:factory_context_mocks",
        "@envoy_api//envoy/extensions/filters/http/global_cache/v3:pkg_cc_proto",
    ],
)

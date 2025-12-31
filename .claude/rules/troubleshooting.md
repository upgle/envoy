# Envoy Troubleshooting

## Build is Slow

### Check 1: Using CI scripts?
```bash
# WRONG (30+ min)
./ci/run_envoy_docker.sh './ci/do_ci.sh dev'

# CORRECT (16-60 sec)
./ci/run_envoy_docker.sh 'bazel build -c fastbuild --config=clang TARGET'
```

### Check 2: Build directory set?
```bash
echo $ENVOY_DOCKER_BUILD_DIR
# Should output: /home/seonghyun/envoy-build

# If empty, set it:
export ENVOY_DOCKER_BUILD_DIR=/home/seonghyun/envoy-build
```

### Check 3: Building too much?
```bash
# WRONG
bazel build //...

# CORRECT
bazel build //source/extensions/filters/http/FILTER:all
```

## Cache Not Working

### Inconsistent flags
Always use same flags: `-c fastbuild --config=clang`

### Restart Bazel server
```bash
bazel shutdown
```

### Clean cache (last resort)
```bash
bazel clean              # Next build slow
bazel clean --expunge    # Full clean, very slow rebuild
```

## Debug Build Issues

### See rebuild reasons
```bash
bazel build --explain=explain.txt --verbose_explanations \
  -c fastbuild --config=clang TARGET
cat explain.txt
```

### See executed commands
```bash
bazel build -s -c fastbuild --config=clang TARGET
```

## Find Correct Binary

### Problem: Using stale binary
```bash
# Find latest by timestamp
find $ENVOY_DOCKER_BUILD_DIR -name "envoy-static" -type f 2>/dev/null | xargs ls -t | head -1
```

### Docker build output location
```
$ENVOY_DOCKER_BUILD_DIR/.cache/bazel/_bazel_envoybuild/<hash>/execroot/envoy/bazel-out/<config>/bin/source/exe/envoy-static
```

### Verify binary has your changes
```bash
BINARY=$(find $ENVOY_DOCKER_BUILD_DIR -name "envoy-static" -type f 2>/dev/null | xargs ls -t | head -1)
ls -lh $BINARY  # Check timestamp
strings $BINARY | grep "your_filter_name"  # Check symbol exists
```

## Memory Issues

### OOM during build
```bash
# Limit parallel jobs
bazel build --jobs=2 envoy

# Or add to user.bazelrc
echo "build --jobs=2" >> user.bazelrc
```

## Library Not Found

`libtinfo.so.5` or similar errors = build environment issue.
Use Docker-based build which handles dependencies.

## Test Failures

### Run with verbose output
```bash
bazel test --test_output=all //test/...
```

### Run with trace logging
```bash
bazel test --test_output=streamed //test/... --test_arg="--" --test_arg="-l trace"
```

### Skip test cache
```bash
bazel test --cache_test_results=no //test/...
```

## Format Check Failed

```bash
# Check format
bazel run //tools/code_format:check_format -- check

# Auto-fix
bazel run //tools/code_format:check_format -- fix

# Spelling check
bazel run //tools/spelling:check_spelling_pedantic -- check --target_root=$(pwd)

# Spelling fix
bazel run //tools/spelling:check_spelling_pedantic -- fix --target_root=$(pwd)
```

## Quick Diagnostic

```bash
# 1. Check environment
echo "BUILD_DIR: $ENVOY_DOCKER_BUILD_DIR"

# 2. Check cache size
du -sh $ENVOY_DOCKER_BUILD_DIR 2>/dev/null || echo "Not set"

# 3. Find latest binary
find $ENVOY_DOCKER_BUILD_DIR -name "envoy-static" -type f 2>/dev/null | xargs ls -lth | head -3
```

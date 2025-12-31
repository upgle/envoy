# Envoy Build Commands

## Critical Rules

1. **NEVER use CI scripts for development**
   ```bash
   # WRONG (30+ min)
   ./ci/run_envoy_docker.sh './ci/do_ci.sh dev'

   # CORRECT (16-60 sec incremental)
   ./ci/run_envoy_docker.sh 'bazel build -c fastbuild --config=clang TARGET'
   ```

2. **ALWAYS set ENVOY_DOCKER_BUILD_DIR**
   ```bash
   export ENVOY_DOCKER_BUILD_DIR=/home/seonghyun/envoy-build
   ```

3. **Build specific targets only**
   ```bash
   # WRONG
   bazel build //...

   # CORRECT
   bazel build //source/extensions/filters/http/FILTER:all
   ```

4. **Use consistent flags**: `-c fastbuild --config=clang`

## Build Commands

### Filter only (16 sec incremental)
```bash
ENVOY_DOCKER_BUILD_DIR=/home/seonghyun/envoy-build \
  ./ci/run_envoy_docker.sh \
  'bazel build -c fastbuild --config=clang //source/extensions/filters/http/FILTER:all'
```

### Full binary (19 sec incremental, 30 min first)
```bash
ENVOY_DOCKER_BUILD_DIR=/home/seonghyun/envoy-build \
  ./ci/run_envoy_docker.sh \
  'bazel build -c fastbuild --config=clang //source/exe:envoy-static'
```

### Local build (no Docker)
```bash
echo "build --config=clang" >> user.bazelrc
bazel build -c fastbuild //source/extensions/filters/http/FILTER:all
```

## Build Modes

| Mode | Flag | Use Case |
|------|------|----------|
| fastbuild | `-c fastbuild` | Development (default) |
| opt | `-c opt` | Release, performance testing |
| dbg | `-c dbg` | GDB debugging |

## Build Time Reference

| Target | First | Incremental |
|--------|-------|-------------|
| Filter only | ~1 min | **16 sec** |
| Tests | ~9 min | **30 sec** |
| envoy-static | ~30 min | **19 sec** |

## Find Latest Binary

```bash
find $ENVOY_DOCKER_BUILD_DIR -name "envoy-static" -type f 2>/dev/null | xargs ls -t | head -1
```

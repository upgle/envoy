#!/bin/bash
set -e

# Cleanup function
cleanup() {
    echo "Cleaning up..."
    if [[ -n "${ENVOY_PID:-}" ]]; then
        curl -s http://localhost:9901/quitquitquit >/dev/null || true
        sleep 1
    fi
    kill $(jobs -p) 2>/dev/null || true
    rm -f envoy.log backend.log backend.pid envoy.pid
}
trap cleanup EXIT

echo "Starting Backend Server..."
python3 test_redis_backend.py > backend.log 2>&1 &
BACKEND_PID=$!
echo "Backend started with PID $BACKEND_PID"

echo "Waiting for backend..."
sleep 2

echo "Starting Envoy..."
if [[ -z "${ENVOY_DOCKER_BUILD_DIR:-}" ]]; then
    echo "ERROR: ENVOY_DOCKER_BUILD_DIR is not set. Please export it."
    exit 1
fi
ENVOY_BIN=$(find "$ENVOY_DOCKER_BUILD_DIR" -path '*/execroot/envoy/bazel-out/*/bin/source/exe/envoy-static' -type f 2>/dev/null | xargs ls -t | head -1)
if [[ -z "$ENVOY_BIN" ]]; then
    echo "ERROR: Could not find envoy-static under $ENVOY_DOCKER_BUILD_DIR"
    exit 1
fi

echo "Clearing Redis keys for test prefix..."
REDIS_CONTAINER=${REDIS_CONTAINER:-redis-standalone}
REDIS_KEY_PREFIX=${REDIS_KEY_PREFIX:-envoy:test:}
REDIS_PORT=${REDIS_PORT:-6379}
REDIS_CLUSTER_MODE=${REDIS_CLUSTER_MODE:-false}
REDIS_CLI_ARGS=()
if [[ "${REDIS_CLUSTER_MODE}" == "true" ]]; then
    REDIS_CLI_ARGS+=("-c")
fi
REDIS_CLI_ARGS+=("-p" "${REDIS_PORT}")
if command -v docker >/dev/null 2>&1; then
    if docker ps --format '{{.Names}}' | grep -q "^${REDIS_CONTAINER}\$"; then
        docker exec "$REDIS_CONTAINER" sh -c \
            "redis-cli ${REDIS_CLI_ARGS[*]} --scan --pattern '${REDIS_KEY_PREFIX}*' | xargs -r redis-cli ${REDIS_CLI_ARGS[*]} del" >/dev/null
    else
        echo "Redis container '${REDIS_CONTAINER}' not running; skipping key cleanup."
    fi
else
    echo "docker not available; skipping key cleanup."
fi

"$ENVOY_BIN" -c test_redis_integration.yaml --log-level info > envoy.log 2>&1 &
ENVOY_PID=$!
echo "Envoy started with PID $ENVOY_PID"

echo "Waiting for Envoy to initialize..."
sleep 5

echo "----------------------------------------------------------------"
echo "Sending Request #1 (Expect Cache MISS / Backend HIT)"
echo "----------------------------------------------------------------"
curl -v http://localhost:10000/test
echo ""

echo "----------------------------------------------------------------"
echo "Sending Request #2 (Expect Cache HIT / Backend MISS)"
echo "----------------------------------------------------------------"
curl -v http://localhost:10000/test
echo ""

echo "----------------------------------------------------------------"
echo "Checking Backend Log for Hit Count"
echo "----------------------------------------------------------------"
if [[ -s backend.log ]]; then
    cat backend.log
else
    echo "Backend log is empty."
fi
if command -v rg >/dev/null 2>&1; then
    HIT_COUNT=$(rg -c "Request #" backend.log || true)
else
    HIT_COUNT=$(grep -c "Request #" backend.log || true)
fi
echo "Backend request count: ${HIT_COUNT}"

echo "----------------------------------------------------------------"
echo "Checking Envoy Log for Redis Initialization"
echo "----------------------------------------------------------------"
grep "RedisCache initialized" envoy.log

echo "----------------------------------------------------------------"
echo "Checking Redis Keys"
echo "----------------------------------------------------------------"
REDIS_CONTAINER=${REDIS_CONTAINER:-redis-standalone}
if command -v docker >/dev/null 2>&1; then
    if docker ps --format '{{.Names}}' | grep -q "^${REDIS_CONTAINER}\$"; then
        docker exec "$REDIS_CONTAINER" redis-cli ${REDIS_CLI_ARGS[*]} --scan --pattern "envoy:test:*" | head -20
    else
        echo "Redis container '${REDIS_CONTAINER}' not running; skipping key check."
    fi
else
    echo "docker not available; skipping key check."
fi

echo "Done."

#!/bin/bash
set -e

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

json_field() {
    python3 -c 'import json,sys; payload=json.load(sys.stdin); print(payload.get("request_number",""))'
}

fetch_response() {
    local url=$1
    local attempt=0
    local header_file=""
    local body_file=""
    header_file=$(mktemp)
    body_file=$(mktemp)
    while [[ $attempt -lt 10 ]]; do
        if curl -sf -D "$header_file" -o "$body_file" "$url"; then
            local request_number=""
            local x_cache=""
            request_number=$(json_field < "$body_file")
            x_cache=$(awk 'BEGIN{IGNORECASE=1} /^x-cache:/ {gsub(/\r/,""); print $2}' "$header_file" | tail -n 1)
            rm -f "$header_file" "$body_file"
            echo "$request_number $x_cache"
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 1
    done
    rm -f "$header_file" "$body_file"
    echo " "
    return 1
}

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

"$ENVOY_BIN" -c test_redis_integration_per_route.yaml --log-level info > envoy.log 2>&1 &
ENVOY_PID=$!
echo "Envoy started with PID $ENVOY_PID"

echo "Waiting for Envoy to initialize..."
sleep 5

echo "----------------------------------------------------------------"
echo "Per-route disabled: /nocache should bypass cache"
echo "----------------------------------------------------------------"
read -r nocache_1 nocache_1_cache <<<"$(fetch_response http://localhost:10000/nocache)"
read -r nocache_2 nocache_2_cache <<<"$(fetch_response http://localhost:10000/nocache)"
echo "nocache request numbers: $nocache_1 -> $nocache_2"
echo "nocache x-cache: ${nocache_1_cache:-<none>} -> ${nocache_2_cache:-<none>}"

echo "----------------------------------------------------------------"
echo "Query params excluded: /query?user=1 then /query?user=2 should reuse cache"
echo "----------------------------------------------------------------"
read -r query_1 query_1_cache <<<"$(fetch_response "http://localhost:10000/query?user=1")"
read -r query_2 query_2_cache <<<"$(fetch_response "http://localhost:10000/query?user=2")"
echo "query request numbers: $query_1 -> $query_2"
echo "query x-cache: $query_1_cache -> $query_2_cache"

echo "----------------------------------------------------------------"
echo "TTL override: /ttl should expire after 1s"
echo "----------------------------------------------------------------"
read -r ttl_1 ttl_1_cache <<<"$(fetch_response http://localhost:10000/ttl)"
sleep 2
read -r ttl_2 ttl_2_cache <<<"$(fetch_response http://localhost:10000/ttl)"
echo "ttl request numbers: $ttl_1 -> $ttl_2"
echo "ttl x-cache: $ttl_1_cache -> $ttl_2_cache"

echo "----------------------------------------------------------------"
echo "Backend log tail"
echo "----------------------------------------------------------------"
if [[ -s backend.log ]]; then
    tail -n 20 backend.log
else
    echo "Backend log is empty."
fi

echo "Done."

#!/bin/bash
set -e

echo "================================================================================================"
echo "Single-Flight Pattern Test Suite for global_cache filter"
echo "================================================================================================"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if Envoy binary exists
ENVOY_BINARY="bazel-bin/source/exe/envoy-static"
if [ ! -f "$ENVOY_BINARY" ]; then
    echo -e "${YELLOW}Warning: Envoy binary not found at $ENVOY_BINARY${NC}"
    echo "Building Envoy..."
    ENVOY_DOCKER_BUILD_DIR=/home/seonghyun/envoy-build \
      ./ci/run_envoy_docker.sh \
      'bazel build -c fastbuild --config=clang //source/exe:envoy-static'
fi

# Make scripts executable
chmod +x slow_backend.py test_single_flight.py

echo ""
echo "Step 1: Starting slow backend server (2s delay per request)..."
echo "------------------------------------------------------------------------------------------------"
python3 slow_backend.py &
BACKEND_PID=$!
echo -e "${GREEN}✓ Backend server started (PID: $BACKEND_PID)${NC}"
sleep 1

echo ""
echo "Step 2: Starting Envoy with debug logging..."
echo "------------------------------------------------------------------------------------------------"
echo "Config: test_global_cache.yaml"
echo "Log level: debug (to see single-flight messages)"
echo ""

# Start Envoy in background with debug logs
$ENVOY_BINARY -c test_global_cache.yaml -l debug 2>&1 | grep -E "(global_cache|envoy)" &
ENVOY_PID=$!
echo -e "${GREEN}✓ Envoy started (PID: $ENVOY_PID)${NC}"

# Wait for Envoy to be ready
echo ""
echo "Waiting for Envoy to be ready..."
for i in {1..10}; do
    if curl -s http://localhost:9901/ready > /dev/null 2>&1; then
        echo -e "${GREEN}✓ Envoy is ready${NC}"
        break
    fi
    if [ $i -eq 10 ]; then
        echo -e "${RED}✗ Envoy failed to start${NC}"
        kill $BACKEND_PID $ENVOY_PID 2>/dev/null
        exit 1
    fi
    sleep 1
done

echo ""
echo "Step 3: Running single-flight pattern test..."
echo "================================================================================================"
echo ""
echo "Watch the logs below for single-flight behavior:"
echo "  - First request: 'cache MISS for key: ... - sending to upstream'"
echo "  - Subsequent: 'WAITING for in-flight request for key: ...'"
echo "  - Completion: 'notified waiting requests for key: ...'"
echo ""
echo "================================================================================================"
echo ""

# Run the test
python3 test_single_flight.py

# Cleanup
echo ""
echo ""
echo "Cleaning up..."
kill $BACKEND_PID $ENVOY_PID 2>/dev/null
echo -e "${GREEN}✓ Test complete${NC}"
echo ""

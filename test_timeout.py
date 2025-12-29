#!/usr/bin/env python3
"""
Test script to verify single-flight timeout behavior.
When the first request takes longer than the timeout, subsequent requests should
timeout and proceed to upstream independently.
"""

import asyncio
import aiohttp
import time
import sys

async def send_request(session, url, request_id):
    """Send a single HTTP request and measure timing."""
    start = time.time()
    try:
        async with session.get(url) as response:
            duration = time.time() - start
            x_cache = response.headers.get('x-cache', 'N/A')
            body = await response.text()
            print(f"Request {request_id}: status={response.status}, "
                  f"x-cache={x_cache}, duration={duration:.3f}s")
            return {
                'id': request_id,
                'status': response.status,
                'x_cache': x_cache,
                'duration': duration,
                'body_length': len(body)
            }
    except Exception as e:
        duration = time.time() - start
        print(f"Request {request_id}: ERROR - {e}, duration={duration:.3f}s")
        return {
            'id': request_id,
            'error': str(e),
            'duration': duration
        }

async def test_timeout(url, num_requests=5):
    """Test single-flight timeout behavior."""
    print(f"\n{'='*70}")
    print(f"Testing single-flight timeout with {num_requests} concurrent requests")
    print(f"URL: {url}")
    print(f"Backend delay: 3 seconds")
    print(f"Single-flight timeout: 1 second")
    print(f"{'='*70}\n")

    async with aiohttp.ClientSession() as session:
        # Send all requests concurrently
        tasks = [send_request(session, url, i+1) for i in range(num_requests)]
        start_time = time.time()
        results = await asyncio.gather(*tasks)
        total_duration = time.time() - start_time

        print(f"\n{'='*70}")
        print(f"Test completed in {total_duration:.3f}s")
        print(f"{'='*70}\n")

        # Analyze results
        cache_misses = sum(1 for r in results if r.get('x_cache') == 'MISS')
        cache_coalesced = sum(1 for r in results if r.get('x_cache') == 'HIT-COALESCED')
        errors = sum(1 for r in results if 'error' in r)

        # Count how many completed around 3s (waited for backend)
        # vs around 1s (timed out)
        fast_requests = sum(1 for r in results if r.get('duration', 0) < 2)
        slow_requests = sum(1 for r in results if r.get('duration', 0) >= 2)

        print("Results Summary:")
        print(f"  Cache MISS: {cache_misses}")
        print(f"  Cache HIT-COALESCED: {cache_coalesced}")
        print(f"  Errors: {errors}")
        print(f"\n  Fast completion (<2s, timeout): {fast_requests}")
        print(f"  Slow completion (>=2s, waited): {slow_requests}")

        print("\nExpected behavior with timeout:")
        print("  - 1st request: MISS, takes ~3s (waits for backend)")
        print("  - 2nd-5th requests: MISS, timeout after ~1s and go upstream")
        print("  - Backend should receive 5 requests (timeout causes independent upstream calls)")

        if cache_misses >= 4 and fast_requests >= 4:
            print("\n✅ SUCCESS: Timeout behavior is working correctly!")
            print("   Most requests timed out and went to upstream independently.")
        elif cache_coalesced >= 3:
            print("\n❌ FAILURE: Requests waited too long (no timeout occurred)")
            print("   Expected most requests to timeout after 1s")
        else:
            print("\n⚠️  MIXED: Some timeouts occurred, results may vary")

        return results

async def main():
    envoy_url = "http://localhost:10000/test"

    if len(sys.argv) > 1:
        envoy_url = sys.argv[1]

    print("Single-Flight Timeout Test")
    print("=" * 70)
    print("\nSetup required:")
    print("  1. Very slow backend (3s delay): python3 very_slow_backend.py")
    print("  2. Envoy with 1s timeout:")
    print("     bazel-bin/source/exe/envoy-static -c test_global_cache_with_timeout.yaml -l debug")
    print("\nWatch Envoy logs for:")
    print("  - 'WAITING for in-flight request' (requests waiting)")
    print("  - 'timeout waiting for in-flight request' (timeout occurred)")
    print("\nWatch backend logs for:")
    print("  - Multiple backend requests (5 requests, not 1)")
    print("=" * 70)

    try:
        await test_timeout(envoy_url, num_requests=5)

    except aiohttp.ClientConnectorError as e:
        print(f"\n❌ ERROR: Cannot connect to Envoy at {envoy_url}")
        print("Make sure Envoy is running!")
        sys.exit(1)

if __name__ == "__main__":
    asyncio.run(main())

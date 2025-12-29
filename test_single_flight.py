#!/usr/bin/env python3
"""
Test script to verify single-flight pattern in global_cache filter.
Sends concurrent requests to the same endpoint and checks if only one upstream request is made.
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

async def test_concurrent_requests(url, num_requests=5):
    """Send multiple concurrent requests to the same URL."""
    print(f"\n{'='*70}")
    print(f"Testing single-flight pattern with {num_requests} concurrent requests")
    print(f"URL: {url}")
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
        cache_hits = sum(1 for r in results if r.get('x_cache') == 'HIT')
        cache_misses = sum(1 for r in results if r.get('x_cache') == 'MISS')
        cache_coalesced = sum(1 for r in results if r.get('x_cache') == 'HIT-COALESCED')
        errors = sum(1 for r in results if 'error' in r)

        print("Results Summary:")
        print(f"  Cache MISS (first upstream request): {cache_misses}")
        print(f"  Cache HIT-COALESCED (waited for first): {cache_coalesced}")
        print(f"  Cache HIT (from existing cache): {cache_hits}")
        print(f"  Errors: {errors}")

        print("\nExpected behavior for single-flight pattern:")
        print("  - 1 request should be MISS (first request goes upstream)")
        print(f"  - {num_requests-1} requests should be HIT-COALESCED (waited for first)")
        print("  - All requests should complete around the same time")

        if cache_misses == 1 and cache_coalesced == num_requests - 1:
            print("\n✅ SUCCESS: Single-flight pattern is working correctly!")
        else:
            print("\n❌ FAILURE: Single-flight pattern may not be working as expected")

        return results

async def test_subsequent_request(url, delay=1):
    """Test that subsequent request after cache TTL uses existing cache."""
    print(f"\n{'='*70}")
    print(f"Testing subsequent request after {delay}s delay")
    print(f"{'='*70}\n")

    await asyncio.sleep(delay)

    async with aiohttp.ClientSession() as session:
        result = await send_request(session, url, "subsequent")

        print("\nExpected behavior:")
        print("  - Should be HIT (from cache populated by previous test)")

        if result.get('x_cache') == 'HIT':
            print("\n✅ SUCCESS: Cache is working correctly!")
        else:
            print(f"\n⚠️  Got x-cache={result.get('x_cache')}, expected HIT")

async def main():
    envoy_url = "http://localhost:10000/test"

    if len(sys.argv) > 1:
        envoy_url = sys.argv[1]

    print("Single-Flight Pattern Test for global_cache filter")
    print("=" * 70)
    print("\nMake sure Envoy is running with:")
    print("  1. Backend server: python3 simple_backend.py")
    print("  2. Envoy with debug logs:")
    print("     bazel-bin/source/exe/envoy-static -c test_global_cache.yaml -l debug")
    print("\nWatch Envoy logs for:")
    print("  - 'WAITING for in-flight request' (subsequent requests)")
    print("  - 'notified waiting requests' (when first completes)")
    print("=" * 70)

    try:
        # Test 1: Concurrent requests (should trigger single-flight)
        await test_concurrent_requests(envoy_url, num_requests=5)

        # Test 2: Subsequent request (should use cache)
        await test_subsequent_request(envoy_url, delay=1)

    except aiohttp.ClientConnectorError as e:
        print(f"\n❌ ERROR: Cannot connect to Envoy at {envoy_url}")
        print("Make sure Envoy is running!")
        sys.exit(1)

if __name__ == "__main__":
    asyncio.run(main())

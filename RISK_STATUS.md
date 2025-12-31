# Global Cache Risk Status

## Resolved

- Cross-thread single-flight callback risk
  - Fix: in-flight tracking moved to worker TLS.
- Unbounded response buffering
  - Fix: cap cached response body to 1MB; skip caching when exceeded.
- Cacheability policy too permissive
  - Fix: allowlisted methods/status codes; optional header-based skip flags.
- TieredCache WRITE_THROUGH callback duplication
  - Fix: ensure callback fires once.
- Cache key collisions from delimiter-based concatenation
  - Fix: length-prefixed, tagged key part encoding.

## Remaining

- Vary/Accept-Encoding not reflected in cache key
  - Risk: serving compressed variants to incompatible clients.
- Request Cache-Control not honored
  - Risk: cache entries created even when client says no-store/no-cache.
- Response TTL parsing not implemented
  - Risk: ignores Cache-Control max-age/Expires and uses default_ttl.
- TieredCache WRITE_THROUGH partial failure semantics
  - Risk: L1 may accept while L2 fails; policy not clearly documented.

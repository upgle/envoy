#pragma once

#include <memory>
#include <string>

#include "source/extensions/filters/http/global_cache/cache_backend.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

/**
 * Utility for serializing and deserializing CacheEntry objects.
 *
 * Binary format:
 * [8 bytes: expiration_time_ms (uint64_t)]
 * [4 bytes: num_headers (uint32_t)]
 * [for each header:]
 *   [4 bytes: key_length (uint32_t)]
 *   [key_length bytes: key data]
 *   [4 bytes: value_length (uint32_t)]
 *   [value_length bytes: value data]
 * [4 bytes: body_length (uint32_t)]
 * [body_length bytes: body data]
 */
class CacheSerializer {
public:
  /**
   * Serialize a CacheEntry to a binary string for storage in Redis.
   *
   * @param entry the cache entry to serialize
   * @return serialized binary string
   */
  static std::string serialize(const CacheEntry& entry);

  /**
   * Deserialize a binary string back to a CacheEntry.
   *
   * @param data the serialized binary string
   * @return CacheEntry pointer, or nullptr if deserialization fails
   */
  static std::shared_ptr<CacheEntry> deserialize(const std::string& data);

private:
  /**
   * Write a uint32_t to the buffer in network byte order.
   */
  static void writeUint32(std::string& buffer, uint32_t value);

  /**
   * Write a uint64_t to the buffer in network byte order.
   */
  static void writeUint64(std::string& buffer, uint64_t value);

  /**
   * Read a uint32_t from the buffer at the given offset.
   * @return the value and advances offset
   */
  static uint32_t readUint32(const std::string& data, size_t& offset);

  /**
   * Read a uint64_t from the buffer at the given offset.
   * @return the value and advances offset
   */
  static uint64_t readUint64(const std::string& data, size_t& offset);

  /**
   * Read a string from the buffer at the given offset.
   * @param length the number of bytes to read
   * @return the string and advances offset
   */
  static std::string readString(const std::string& data, size_t& offset, uint32_t length);
};

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

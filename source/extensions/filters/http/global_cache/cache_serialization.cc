#include "source/extensions/filters/http/global_cache/cache_serialization.h"

#include "source/common/http/header_map_impl.h"

#include <arpa/inet.h>

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

namespace {
constexpr uint32_t kSerializationMagic = 0x47433031; // "GC01"
} // namespace

std::string CacheSerializer::serialize(const CacheEntry& entry) {
  std::string buffer;
  buffer.reserve(1024); // Reserve some space to avoid multiple allocations

  // 1. Write format magic and remaining TTL
  writeUint32(buffer, kSerializationMagic);
  auto now = std::chrono::steady_clock::now();
  auto remaining = entry.expiration_time > now ? entry.expiration_time - now
                                               : std::chrono::steady_clock::duration::zero();
  auto remaining_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
  writeUint64(buffer, static_cast<uint64_t>(remaining_ms));

  // 2. Count headers
  uint32_t num_headers = 0;
  entry.headers->iterate([&num_headers](const Http::HeaderEntry&) -> Http::HeaderMap::Iterate {
    num_headers++;
    return Http::HeaderMap::Iterate::Continue;
  });

  writeUint32(buffer, num_headers);

  // 3. Write headers
  entry.headers->iterate([&buffer](const Http::HeaderEntry& header) -> Http::HeaderMap::Iterate {
    auto key = header.key().getStringView();
    auto value = header.value().getStringView();

    // Write key
    writeUint32(buffer, static_cast<uint32_t>(key.size()));
    buffer.append(key.data(), key.size());

    // Write value
    writeUint32(buffer, static_cast<uint32_t>(value.size()));
    buffer.append(value.data(), value.size());

    return Http::HeaderMap::Iterate::Continue;
  });

  // 4. Write body
  uint64_t body_length = entry.body.length();
  writeUint64(buffer, body_length);

  if (body_length > 0) {
    // Copy body data
    for (const auto& slice : entry.body.getRawSlices()) {
      buffer.append(static_cast<const char*>(slice.mem_), slice.len_);
    }
  }

  return buffer;
}

std::shared_ptr<CacheEntry> CacheSerializer::deserialize(const std::string& data) {
  size_t offset = 0;

  try {
    // 1. Read format magic and remaining TTL
    if (data.size() < 12) {
      return nullptr;
    }
    uint32_t magic = readUint32(data, offset);
    if (magic != kSerializationMagic) {
      return nullptr;
    }
    uint64_t remaining_ms = readUint64(data, offset);
    auto expiration_time =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(remaining_ms);

    // 2. Read number of headers
    if (offset + 4 > data.size()) {
      return nullptr;
    }
    uint32_t num_headers = readUint32(data, offset);

    // Create header map
    auto headers = Http::ResponseHeaderMapImpl::create();

    // 3. Read headers
    for (uint32_t i = 0; i < num_headers; i++) {
      // Read key
      if (offset + 4 > data.size()) {
        return nullptr;
      }
      uint32_t key_length = readUint32(data, offset);
      if (offset + key_length > data.size()) {
        return nullptr;
      }
      std::string key = readString(data, offset, key_length);

      // Read value
      if (offset + 4 > data.size()) {
        return nullptr;
      }
      uint32_t value_length = readUint32(data, offset);
      if (offset + value_length > data.size()) {
        return nullptr;
      }
      std::string value = readString(data, offset, value_length);

      // Add header
      headers->addCopy(Http::LowerCaseString(key), value);
    }

    // 4. Read body
    if (offset + 8 > data.size()) {
      return nullptr;
    }
    uint64_t body_length = readUint64(data, offset);
    if (body_length > data.size() - offset) {
      return nullptr;
    }

    Buffer::OwnedImpl body;
    if (body_length > 0) {
      body.add(data.data() + offset, body_length);
      offset += body_length;
    }

    // Create and return cache entry
    return std::make_shared<CacheEntry>(std::move(body), std::move(headers), expiration_time);

  } catch (const std::exception&) {
    // Deserialization failed
    return nullptr;
  }
}

void CacheSerializer::writeUint32(std::string& buffer, uint32_t value) {
  uint32_t network_value = htonl(value);
  buffer.append(reinterpret_cast<const char*>(&network_value), sizeof(network_value));
}

void CacheSerializer::writeUint64(std::string& buffer, uint64_t value) {
  // Convert to network byte order (big endian)
  uint32_t high = htonl(static_cast<uint32_t>(value >> 32));
  uint32_t low = htonl(static_cast<uint32_t>(value & 0xFFFFFFFF));
  buffer.append(reinterpret_cast<const char*>(&high), sizeof(high));
  buffer.append(reinterpret_cast<const char*>(&low), sizeof(low));
}

uint32_t CacheSerializer::readUint32(const std::string& data, size_t& offset) {
  uint32_t network_value;
  std::memcpy(&network_value, data.data() + offset, sizeof(network_value));
  offset += sizeof(network_value);
  return ntohl(network_value);
}

uint64_t CacheSerializer::readUint64(const std::string& data, size_t& offset) {
  uint32_t high, low;
  std::memcpy(&high, data.data() + offset, sizeof(high));
  offset += sizeof(high);
  std::memcpy(&low, data.data() + offset, sizeof(low));
  offset += sizeof(low);

  uint64_t value = (static_cast<uint64_t>(ntohl(high)) << 32) | ntohl(low);
  return value;
}

std::string CacheSerializer::readString(const std::string& data, size_t& offset, uint32_t length) {
  std::string result(data.data() + offset, length);
  offset += length;
  return result;
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

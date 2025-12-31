#include "source/extensions/filters/http/global_cache/local_cache.h"

#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GlobalCache {

namespace {
constexpr size_t kDefaultMaxEntries = 1000;
constexpr size_t kDefaultMaxBytes = 104857600;  // 100 MB
constexpr size_t kEstimatedHeaderOverhead = 1024;  // Estimated overhead per header
} // namespace

LocalCache::LocalCache(
    const envoy::extensions::filters::http::global_cache::v3::LocalCacheConfig& config)
    : max_entries_(config.max_entries() > 0 ? config.max_entries() : kDefaultMaxEntries),
      max_bytes_(config.max_bytes() > 0 ? config.max_bytes() : kDefaultMaxBytes) {}

LocalCache::LocalCache() : max_entries_(kDefaultMaxEntries), max_bytes_(kDefaultMaxBytes) {}

LocalCache::~LocalCache() {
  absl::MutexLock lock(&mutex_);
  cache_.clear();
  head_ = nullptr;
  tail_ = nullptr;
}

void LocalCache::lookup(const std::string& key, LookupCallback callback) {
  std::shared_ptr<CacheEntry> entry;
  CacheLookupStatus status = CacheLookupStatus::Miss;

  {
    absl::MutexLock lock(&mutex_);

    auto it = cache_.find(key);
    if (it == cache_.end()) {
      // Cache miss - status already set to Miss
    } else {
      LruNode* node = it->second.get();

      // Check if expired
      if (isExpired(*node->entry)) {
        // Remove expired entry
        removeNode(node);
        cache_.erase(it);
        current_entries_--;
        current_bytes_ -= node->size_bytes;
        // status already set to Miss
      } else {
        // Cache hit - move to front (MRU)
        moveToFront(node);
        status = CacheLookupStatus::Hit;
        entry = node->entry;
      }
    }
  } // Release mutex before calling callback

  // Invoke callback after releasing mutex to avoid deadlocks
  callback(CacheLookupResult{status, entry});
}

void LocalCache::insert(const std::string& key, std::shared_ptr<CacheEntry> entry,
                        std::chrono::seconds ttl, InsertCallback callback) {
  bool success = true;

  {
    absl::MutexLock lock(&mutex_);

    // Calculate entry size
    size_t entry_size = calculateEntrySize(*entry);

    // Check if entry is too large for cache
    if (entry_size > max_bytes_) {
      // Entry is larger than entire cache capacity, reject
      success = false;
    } else {
      // Check if key already exists
      auto it = cache_.find(key);
      if (it != cache_.end()) {
        // Update existing entry
        LruNode* existing_node = it->second.get();
        current_bytes_ -= existing_node->size_bytes;

        existing_node->entry = entry;
        existing_node->ttl = ttl;
        existing_node->size_bytes = entry_size;

        current_bytes_ += entry_size;
        moveToFront(existing_node);
      } else {
        // Create new node
        auto new_node = std::make_unique<LruNode>(key, entry, ttl, entry_size);
        LruNode* node_ptr = new_node.get();

        // Insert into hash map
        cache_.emplace(key, std::move(new_node));
        current_entries_++;
        current_bytes_ += entry_size;

        // Add to front of list
        addToFront(node_ptr);

        // Evict if necessary
        evictIfNeeded();
      }
    }
  } // Release mutex before calling callback

  // Invoke callback after releasing mutex to avoid deadlocks
  callback(success);
}

void LocalCache::remove(const std::string& key) {
  absl::MutexLock lock(&mutex_);

  auto it = cache_.find(key);
  if (it == cache_.end()) {
    return;
  }

  LruNode* node = it->second.get();
  removeNode(node);
  current_entries_--;
  current_bytes_ -= node->size_bytes;
  cache_.erase(it);
}

size_t LocalCache::size() const {
  absl::MutexLock lock(&mutex_);
  return current_entries_;
}

size_t LocalCache::bytes() const {
  absl::MutexLock lock(&mutex_);
  return current_bytes_;
}

bool LocalCache::isExpired(const CacheEntry& entry) const {
  return std::chrono::steady_clock::now() >= entry.expiration_time;
}

void LocalCache::evictIfNeeded() {
  // Evict from tail (LRU) until within limits
  while ((current_entries_ > max_entries_ || current_bytes_ > max_bytes_) && tail_ != nullptr) {
    LruNode* victim = tail_;

    // Remove from hash map
    cache_.erase(victim->key);
    current_entries_--;
    current_bytes_ -= victim->size_bytes;

    // Remove from linked list
    removeNode(victim);
  }
}

void LocalCache::moveToFront(LruNode* node) {
  if (node == head_) {
    // Already at front
    return;
  }

  // Remove from current position
  if (node->prev != nullptr) {
    node->prev->next = node->next;
  }
  if (node->next != nullptr) {
    node->next->prev = node->prev;
  }
  if (node == tail_) {
    tail_ = node->prev;
  }

  // Add to front
  node->prev = nullptr;
  node->next = head_;
  if (head_ != nullptr) {
    head_->prev = node;
  }
  head_ = node;

  if (tail_ == nullptr) {
    tail_ = node;
  }
}

void LocalCache::addToFront(LruNode* node) {
  node->prev = nullptr;
  node->next = head_;

  if (head_ != nullptr) {
    head_->prev = node;
  }
  head_ = node;

  if (tail_ == nullptr) {
    tail_ = node;
  }
}

void LocalCache::removeNode(LruNode* node) {
  if (node->prev != nullptr) {
    node->prev->next = node->next;
  }
  if (node->next != nullptr) {
    node->next->prev = node->prev;
  }

  if (node == head_) {
    head_ = node->next;
  }
  if (node == tail_) {
    tail_ = node->prev;
  }

  node->prev = nullptr;
  node->next = nullptr;
}

size_t LocalCache::calculateEntrySize(const CacheEntry& entry) {
  size_t size = 0;

  // Body size
  size += entry.body.length();

  // Headers size (approximate)
  // Iterate through all headers and sum key + value lengths
  entry.headers->iterate([&size](const Http::HeaderEntry& header) -> Http::HeaderMap::Iterate {
    size += header.key().getStringView().size();
    size += header.value().getStringView().size();
    return Http::HeaderMap::Iterate::Continue;
  });

  // Add estimated overhead
  size += kEstimatedHeaderOverhead;

  return size;
}

} // namespace GlobalCache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

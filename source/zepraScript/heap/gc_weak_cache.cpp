// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_weak_cache.cpp — Weak-value caches (JIT, shape, transition table)

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>

namespace Zepra::Heap {

enum class CacheKind : uint8_t {
    JITCode,
    ShapeTable,
    TransitionTable,
    InlineCache,
    RegExpCache,
    Custom,
};

struct WeakCacheEntry {
    uint64_t key;
    void* value;       // Weakly held — not preventing GC
    CacheKind kind;

    WeakCacheEntry() : key(0), value(nullptr), kind(CacheKind::Custom) {}
    WeakCacheEntry(uint64_t k, void* v, CacheKind knd)
        : key(k), value(v), kind(knd) {}
};

class WeakCache {
public:
    explicit WeakCache(CacheKind kind, size_t capacity = 4096)
        : kind_(kind), capacity_(capacity), hitCount_(0), missCount_(0) {}

    void put(uint64_t key, void* value) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (entries_.size() >= capacity_) {
            evictOldest();
        }

        entries_[key] = WeakCacheEntry(key, value, kind_);
    }

    void* get(uint64_t key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it != entries_.end() && it->second.value) {
            hitCount_++;
            return it->second.value;
        }
        missCount_++;
        return nullptr;
    }

    void remove(uint64_t key) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(key);
    }

    // GC sweep: remove entries with dead values.
    size_t sweep(std::function<bool(void* cell)> isMarked) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t removed = 0;

        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (!it->second.value || !isMarked(it->second.value)) {
                it = entries_.erase(it);
                removed++;
            } else {
                ++it;
            }
        }

        stats_.totalSwept += removed;
        return removed;
    }

    // Invalidate entire cache (e.g., after deoptimization).
    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        stats_.flushCount++;
    }

    CacheKind kind() const { return kind_; }
    size_t count() const { return entries_.size(); }
    size_t capacity() const { return capacity_; }

    double hitRate() const {
        uint64_t total = hitCount_ + missCount_;
        return total > 0 ? static_cast<double>(hitCount_) / total : 0;
    }

    struct Stats {
        uint64_t totalSwept = 0;
        uint64_t flushCount = 0;
    };

    const Stats& stats() const { return stats_; }

private:
    void evictOldest() {
        if (entries_.empty()) return;
        entries_.erase(entries_.begin());
    }

    CacheKind kind_;
    size_t capacity_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, WeakCacheEntry> entries_;
    uint64_t hitCount_;
    uint64_t missCount_;
    Stats stats_;
};

// Multi-cache manager: one WeakCache per kind.
class WeakCacheManager {
public:
    WeakCache* getOrCreate(CacheKind kind, size_t capacity = 4096) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = caches_.find(kind);
        if (it != caches_.end()) return it->second.get();

        auto cache = std::make_unique<WeakCache>(kind, capacity);
        WeakCache* ptr = cache.get();
        caches_[kind] = std::move(cache);
        return ptr;
    }

    // Sweep all caches during GC.
    size_t sweepAll(std::function<bool(void* cell)> isMarked) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (auto& [kind, cache] : caches_) {
            total += cache->sweep(isMarked);
        }
        return total;
    }

    void flushAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [kind, cache] : caches_) {
            cache->flush();
        }
    }

    size_t totalEntries() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (auto& [kind, cache] : caches_) total += cache->count();
        return total;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<CacheKind, std::unique_ptr<WeakCache>> caches_;
};

} // namespace Zepra::Heap

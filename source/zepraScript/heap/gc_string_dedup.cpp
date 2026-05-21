// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_string_dedup.cpp — String deduplication during GC

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <functional>

namespace Zepra::Heap {

struct StringRef {
    const char* data;
    uint32_t length;
    uint32_t hash;
    void* owner;      // Cell that owns this string data

    bool equals(const StringRef& other) const {
        if (hash != other.hash || length != other.length) return false;
        return memcmp(data, other.data, length) == 0;
    }
};

class StringDeduplicator {
public:
    struct Config {
        size_t minStringLength;     // Only dedup strings >= this length
        size_t maxTableSize;        // Max dedup table entries
        bool enableDuringMinorGC;

        Config() : minStringLength(8), maxTableSize(65536), enableDuringMinorGC(false) {}
    };

    struct Callbacks {
        std::function<void(void* cell, const char* newData)> replaceStringData;
        std::function<void(const char* data, uint32_t length)> freeStringData;
    };

    explicit StringDeduplicator(const Config& config = Config{}) : config_(config) {}

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // Process a string during GC marking. Returns true if deduplicated.
    bool processString(void* cell, const char* data, uint32_t length) {
        if (length < config_.minStringLength) return false;

        uint32_t hash = hashString(data, length);
        StringRef ref{data, length, hash, cell};

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = dedupTable_.find(hash);
        if (it != dedupTable_.end()) {
            // Check for exact match.
            for (auto& existing : it->second) {
                if (ref.equals(existing)) {
                    // Dedup: point this cell to the existing data.
                    if (data != existing.data && cb_.replaceStringData) {
                        cb_.replaceStringData(cell, existing.data);
                        stats_.deduplicatedCount++;
                        stats_.savedBytes += length;
                    }
                    return true;
                }
            }
            // Hash collision — not a duplicate.
            if (dedupTable_.size() < config_.maxTableSize) {
                it->second.push_back(ref);
            }
        } else {
            if (dedupTable_.size() < config_.maxTableSize) {
                dedupTable_[hash].push_back(ref);
            }
        }

        stats_.processedCount++;
        return false;
    }

    // Clear the dedup table after GC cycle.
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        dedupTable_.clear();
    }

    // Shrink table if it's consuming too much memory.
    void trimTable(size_t maxEntries) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dedupTable_.size() <= maxEntries) return;

        // Simple strategy: clear oldest entries (by removing from front).
        while (dedupTable_.size() > maxEntries) {
            dedupTable_.erase(dedupTable_.begin());
        }
    }

    struct Stats {
        uint64_t processedCount = 0;
        uint64_t deduplicatedCount = 0;
        uint64_t savedBytes = 0;
        uint64_t tableEvictions = 0;
    };

    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = {}; }

    size_t tableSize() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dedupTable_.size();
    }

    double deduplicationRate() const {
        return stats_.processedCount > 0
            ? static_cast<double>(stats_.deduplicatedCount) / stats_.processedCount
            : 0;
    }

private:
    static uint32_t hashString(const char* data, uint32_t length) {
        // FNV-1a hash.
        uint32_t hash = 2166136261u;
        for (uint32_t i = 0; i < length; i++) {
            hash ^= static_cast<uint8_t>(data[i]);
            hash *= 16777619u;
        }
        return hash;
    }

    Config config_;
    Callbacks cb_;
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, std::vector<StringRef>> dedupTable_;
    Stats stats_;
};

} // namespace Zepra::Heap

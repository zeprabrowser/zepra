// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_complete_subspace.cpp — Full subspace with destructor dispatch table

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <mutex>
#include <vector>
#include <functional>
#include <unordered_map>

namespace Zepra::Heap {

enum class CellType : uint8_t;  // From gc_cell.cpp

// Destructor entry: maps CellType → destructor function.
struct DestructorEntry {
    CellType type;
    std::function<void(void* cell)> destructor;
    bool requiresOrdering;  // Must run before dependent destructors.
    uint16_t priority;       // Lower = runs first.
};

class CompleteSubspace {
public:
    struct Config {
        uint16_t subspaceId;
        const char* name;
        uint16_t cellSize;
        bool allowParallelSweep;
        bool allowCompaction;

        Config() : subspaceId(0), name(""), cellSize(0)
            , allowParallelSweep(true), allowCompaction(true) {}
    };

    explicit CompleteSubspace(const Config& config) : config_(config)
        , liveBytes_(0), sweepPending_(false) {}

    uint16_t id() const { return config_.subspaceId; }
    const char* name() const { return config_.name; }

    // Register a destructor for a specific cell type in this subspace.
    void registerDestructor(CellType type, std::function<void(void* cell)> fn,
                            bool ordered = false, uint16_t priority = 100) {
        std::lock_guard<std::mutex> lock(mutex_);
        DestructorEntry entry;
        entry.type = type;
        entry.destructor = std::move(fn);
        entry.requiresOrdering = ordered;
        entry.priority = priority;
        destructors_[static_cast<size_t>(type)] = entry;
    }

    bool hasDestructor(CellType type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = destructors_.find(static_cast<size_t>(type));
        return it != destructors_.end() && it->second.destructor;
    }

    // Call destructor for a specific cell.
    void destruct(void* cell, CellType type) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = destructors_.find(static_cast<size_t>(type));
        if (it != destructors_.end() && it->second.destructor) {
            it->second.destructor(cell);
            destructionCount_++;
        }
    }

    // Batch destruction: sort by priority, then call in order.
    void destructBatch(std::vector<std::pair<void*, CellType>>& cells) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Separate ordered and unordered.
        std::vector<std::pair<void*, CellType>> ordered;
        std::vector<std::pair<void*, CellType>> unordered;

        for (auto& [cell, type] : cells) {
            auto it = destructors_.find(static_cast<size_t>(type));
            if (it == destructors_.end() || !it->second.destructor) continue;

            if (it->second.requiresOrdering) {
                ordered.push_back({cell, type});
            } else {
                unordered.push_back({cell, type});
            }
        }

        // Sort ordered set by priority.
        std::sort(ordered.begin(), ordered.end(),
            [this](const auto& a, const auto& b) {
                return destructors_[static_cast<size_t>(a.second)].priority
                     < destructors_[static_cast<size_t>(b.second)].priority;
            });

        // Run ordered first, then unordered.
        for (auto& [cell, type] : ordered) {
            destructors_[static_cast<size_t>(type)].destructor(cell);
            destructionCount_++;
        }
        for (auto& [cell, type] : unordered) {
            destructors_[static_cast<size_t>(type)].destructor(cell);
            destructionCount_++;
        }
    }

    // Pre-sweep: collect cells needing destruction before freeing memory.
    void preSweep(std::function<void(std::function<void(void*, CellType, bool)>)> scanner) {
        std::lock_guard<std::mutex> lock(mutex_);
        sweepPending_ = true;

        pendingDestructors_.clear();

        scanner([this](void* cell, CellType type, bool isMarked) {
            if (!isMarked && hasDestructorLocked(type)) {
                pendingDestructors_.push_back({cell, type});
            }
        });
    }

    // Execute pending destructors from pre-sweep.
    size_t executePendingDestructors() {
        std::lock_guard<std::mutex> lock(mutex_);
        destructBatchLocked(pendingDestructors_);
        size_t count = pendingDestructors_.size();
        pendingDestructors_.clear();
        sweepPending_ = false;
        return count;
    }

    void recordAllocation(size_t bytes) { liveBytes_ += bytes; }
    void recordFree(size_t bytes) { if (liveBytes_ >= bytes) liveBytes_ -= bytes; }
    size_t liveBytes() const { return liveBytes_; }
    uint64_t destructionCount() const { return destructionCount_; }
    bool hasPendingDestructors() const { return !pendingDestructors_.empty(); }

    bool allowParallelSweep() const { return config_.allowParallelSweep; }
    bool allowCompaction() const { return config_.allowCompaction; }

private:
    bool hasDestructorLocked(CellType type) const {
        auto it = destructors_.find(static_cast<size_t>(type));
        return it != destructors_.end() && it->second.destructor;
    }

    void destructBatchLocked(std::vector<std::pair<void*, CellType>>& cells) {
        for (auto& [cell, type] : cells) {
            auto it = destructors_.find(static_cast<size_t>(type));
            if (it != destructors_.end() && it->second.destructor) {
                it->second.destructor(cell);
                destructionCount_++;
            }
        }
    }

    Config config_;
    mutable std::mutex mutex_;
    std::unordered_map<size_t, DestructorEntry> destructors_;
    std::vector<std::pair<void*, CellType>> pendingDestructors_;
    size_t liveBytes_;
    uint64_t destructionCount_ = 0;
    bool sweepPending_;
};

} // namespace Zepra::Heap

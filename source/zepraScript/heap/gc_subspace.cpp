// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_subspace.cpp — Type-partitioned allocation subspace

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <mutex>
#include <vector>
#include <functional>
#include <unordered_map>
#include <memory>

namespace Zepra::Heap {

enum class CellType : uint8_t;  // From gc_cell.cpp
struct ArenaHeader;              // From gc_arena.cpp

class Subspace {
public:
    using SubspaceId = uint16_t;

    struct Config {
        SubspaceId id;
        const char* name;
        uint16_t cellSize;
        bool requiresDestruction;
        bool requiresFinalization;
        bool canParallelSweep;

        Config() : id(0), name(""), cellSize(0), requiresDestruction(false)
            , requiresFinalization(false), canParallelSweep(true) {}
    };

    explicit Subspace(const Config& config) : config_(config), totalAllocated_(0)
        , totalFreed_(0), liveBytes_(0) {}

    SubspaceId id() const { return config_.id; }
    const char* name() const { return config_.name; }
    uint16_t cellSize() const { return config_.cellSize; }
    bool requiresDestruction() const { return config_.requiresDestruction; }
    bool requiresFinalization() const { return config_.requiresFinalization; }
    bool canParallelSweep() const { return config_.canParallelSweep; }

    // Arena management within this subspace.
    void addArena(ArenaHeader* arena) {
        std::lock_guard<std::mutex> lock(mutex_);
        arenas_.push_back(arena);
    }

    void removeArena(ArenaHeader* arena) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = arenas_.begin(); it != arenas_.end(); ++it) {
            if (*it == arena) {
                arenas_.erase(it);
                return;
            }
        }
    }

    ArenaHeader* findArenaWithSpace() const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* a : arenas_) {
            if (!isFull(a)) return a;
        }
        return nullptr;
    }

    template<typename Fn>
    void forEachArena(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* a : arenas_) fn(a);
    }

    size_t arenaCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return arenas_.size();
    }

    // Allocation tracking.
    void recordAllocation(size_t bytes) {
        totalAllocated_ += bytes;
        liveBytes_ += bytes;
    }

    void recordFree(size_t bytes) {
        totalFreed_ += bytes;
        if (liveBytes_ >= bytes) liveBytes_ -= bytes;
    }

    size_t totalAllocated() const { return totalAllocated_; }
    size_t totalFreed() const { return totalFreed_; }
    size_t liveBytes() const { return liveBytes_; }

    void updateLiveBytes(size_t bytes) { liveBytes_ = bytes; }

    // Sweep callback: called per arena, returns freed bytes.
    size_t sweep(std::function<size_t(ArenaHeader*)> sweepFn,
                 std::function<void(void*, uint16_t)> destructorFn) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t totalFreed = 0;
        for (auto* a : arenas_) {
            if (requiresDestruction() && destructorFn) {
                callDestructorsBeforeSweep(a, destructorFn);
            }
            totalFreed += sweepFn(a);
        }
        totalFreed_ += totalFreed;
        if (liveBytes_ >= totalFreed) liveBytes_ -= totalFreed;
        return totalFreed;
    }

    // Remove empty arenas after sweep.
    size_t removeEmptyArenas(std::function<bool(const ArenaHeader*)> isEmpty,
                             std::function<void(ArenaHeader*)> onRemoved) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t removed = 0;
        auto it = arenas_.begin();
        while (it != arenas_.end()) {
            if (isEmpty(*it)) {
                ArenaHeader* arena = *it;
                it = arenas_.erase(it);
                if (onRemoved) onRemoved(arena);
                removed++;
            } else {
                ++it;
            }
        }
        return removed;
    }

private:
    static bool isFull(const ArenaHeader* arena);

    void callDestructorsBeforeSweep(ArenaHeader* arena,
                                    std::function<void(void*, uint16_t)> destructorFn) {
        // Iterate unmarked cells and call destructors before freeing.
        // Implementation depends on ArenaHeader's mark bitmap.
        (void)arena;
        (void)destructorFn;
    }

    Config config_;
    mutable std::mutex mutex_;
    std::vector<ArenaHeader*> arenas_;
    size_t totalAllocated_;
    size_t totalFreed_;
    size_t liveBytes_;
};

// Subspace registry: maps CellType → Subspace.
class SubspaceRegistry {
public:
    Subspace* registerSubspace(const Subspace::Config& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto sub = std::make_unique<Subspace>(config);
        Subspace* ptr = sub.get();
        subspaces_.push_back(std::move(sub));
        idMap_[config.id] = ptr;
        return ptr;
    }

    Subspace* findById(Subspace::SubspaceId id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = idMap_.find(id);
        return it != idMap_.end() ? it->second : nullptr;
    }

    template<typename Fn>
    void forEach(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& s : subspaces_) fn(s.get());
    }

    // Sweep all subspaces.
    size_t sweepAll(std::function<size_t(ArenaHeader*)> sweepFn,
                    std::function<void(void*, uint16_t)> destructorFn) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (auto& s : subspaces_) {
            total += s->sweep(sweepFn, destructorFn);
        }
        return total;
    }

    size_t subspaceCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return subspaces_.size();
    }

    size_t totalLiveBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (auto& s : subspaces_) total += s->liveBytes();
        return total;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Subspace>> subspaces_;
    std::unordered_map<Subspace::SubspaceId, Subspace*> idMap_;
};

} // namespace Zepra::Heap

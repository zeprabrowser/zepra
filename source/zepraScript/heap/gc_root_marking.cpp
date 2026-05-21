// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_root_marking.cpp — Root enumeration: stack, global, handle, VM roots

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

namespace Zepra::Heap {

enum class RootKind : uint8_t {
    Stack,
    Global,
    Handle,
    VM,
    JIT,
    FinalizationRegistry,
    WeakRef,
    Debugger,
};

struct RootEntry {
    void** slot;
    RootKind kind;
    const char* description;
};

class RootSet {
public:
    void addRoot(void** slot, RootKind kind, const char* desc = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        roots_.push_back({slot, kind, desc});
    }

    void removeRoot(void** slot) {
        std::lock_guard<std::mutex> lock(mutex_);
        roots_.erase(
            std::remove_if(roots_.begin(), roots_.end(),
                [slot](const RootEntry& e) { return e.slot == slot; }),
            roots_.end());
    }

    template<typename Fn>
    void forEach(Fn&& fn) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : roots_) fn(entry);
    }

    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return roots_.size();
    }

    size_t countOfKind(RootKind kind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t c = 0;
        for (auto& e : roots_) {
            if (e.kind == kind) c++;
        }
        return c;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        roots_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<RootEntry> roots_;
};

class RootMarker {
public:
    struct Callbacks {
        std::function<void(void* cell)> markCell;
        std::function<bool(void* ptr)> isInHeap;
        std::function<size_t()> scanStack;     // From ConservativeScanner
        std::function<void(std::function<void(void**)> visitor)> iterateHandles;
        std::function<void(std::function<void(void**)> visitor)> iterateGlobals;
        std::function<void(std::function<void(void**)> visitor)> iterateVMRoots;
        std::function<void(std::function<void(void**)> visitor)> iterateJITRoots;
    };

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // Mark all roots, populating the mark worklist.
    size_t markAllRoots() {
        size_t total = 0;

        // 1. Stack roots (conservative).
        if (cb_.scanStack) {
            size_t stackRoots = cb_.scanStack();
            total += stackRoots;
            stats_.stackRoots = stackRoots;
        }

        // 2. Handle roots (exact).
        if (cb_.iterateHandles) {
            size_t handleRoots = markFromIterator(cb_.iterateHandles);
            total += handleRoots;
            stats_.handleRoots = handleRoots;
        }

        // 3. Global roots.
        if (cb_.iterateGlobals) {
            size_t globalRoots = markFromIterator(cb_.iterateGlobals);
            total += globalRoots;
            stats_.globalRoots = globalRoots;
        }

        // 4. VM-internal roots.
        if (cb_.iterateVMRoots) {
            size_t vmRoots = markFromIterator(cb_.iterateVMRoots);
            total += vmRoots;
            stats_.vmRoots = vmRoots;
        }

        // 5. JIT roots (code references, inline caches).
        if (cb_.iterateJITRoots) {
            size_t jitRoots = markFromIterator(cb_.iterateJITRoots);
            total += jitRoots;
            stats_.jitRoots = jitRoots;
        }

        // 6. Explicit root set.
        rootSet_.forEach([&](const RootEntry& entry) {
            if (entry.slot && *entry.slot) {
                if (cb_.isInHeap && cb_.isInHeap(*entry.slot)) {
                    if (cb_.markCell) cb_.markCell(*entry.slot);
                    total++;
                }
            }
        });

        stats_.totalRoots = total;
        stats_.rootMarkings++;
        return total;
    }

    RootSet& rootSet() { return rootSet_; }
    const RootSet& rootSet() const { return rootSet_; }

    struct Stats {
        size_t totalRoots = 0;
        size_t stackRoots = 0;
        size_t handleRoots = 0;
        size_t globalRoots = 0;
        size_t vmRoots = 0;
        size_t jitRoots = 0;
        uint64_t rootMarkings = 0;
    };

    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = {}; }

private:
    size_t markFromIterator(
            std::function<void(std::function<void(void**)>)>& iterator) {
        size_t marked = 0;
        iterator([&](void** slot) {
            if (slot && *slot) {
                if (cb_.isInHeap && cb_.isInHeap(*slot)) {
                    if (cb_.markCell) cb_.markCell(*slot);
                    marked++;
                }
            }
        });
        return marked;
    }

    Callbacks cb_;
    RootSet rootSet_;
    Stats stats_;
};

} // namespace Zepra::Heap

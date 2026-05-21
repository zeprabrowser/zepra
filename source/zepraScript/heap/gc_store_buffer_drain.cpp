// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_store_buffer_drain.cpp — Drain store buffer into nursery root set

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <vector>
#include <functional>
#include <unordered_set>

namespace Zepra::Heap {

class StoreBufferDrainer {
public:
    struct NurseryRange {
        uintptr_t base;
        size_t size;

        bool contains(uintptr_t addr) const {
            return addr >= base && addr < base + size;
        }
    };

    struct Callbacks {
        std::function<bool(void* ptr)> isInNursery;
        std::function<void(void* nurseryObj)> addToRootSet;
        std::function<void(void** slot, void* target)> updateForwardedPointer;
        std::function<bool(void* obj)> isForwarded;
        std::function<void*(void* obj)> forwardingAddress;
    };

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }
    void setNurseryRange(uintptr_t base, size_t size) { nursery_ = {base, size}; }

    // Process edge records: find old→nursery edges, add nursery targets to root set.
    void drainEdges(void** slot, void* target) {
        if (!cb_.isInNursery) return;

        // If target is in nursery, it's a root for minor GC.
        if (cb_.isInNursery(target)) {
            if (cb_.addToRootSet) cb_.addToRootSet(target);
            nurseryRoots_.insert(target);
            stats_.nurseryEdges++;
        }

        // If target was forwarded (already promoted), update the slot.
        if (cb_.isForwarded && cb_.isForwarded(target)) {
            void* newAddr = cb_.forwardingAddress(target);
            if (newAddr && cb_.updateForwardedPointer) {
                cb_.updateForwardedPointer(slot, newAddr);
                stats_.forwardedEdges++;
            }
        }

        stats_.totalEdges++;
    }

    // Process cell records: rescan mutated cells for nursery references.
    void drainCells(void* cell, uint16_t offset) {
        if (!cell) return;

        // The cell at `offset` was mutated. Check if it now points into nursery.
        void** slot = reinterpret_cast<void**>(static_cast<uint8_t*>(cell) + offset);
        void* target = *slot;

        if (target && cb_.isInNursery && cb_.isInNursery(target)) {
            if (cb_.addToRootSet) cb_.addToRootSet(target);
            nurseryRoots_.insert(target);
            stats_.cellDrainedSlots++;
        }

        stats_.totalCells++;
    }

    // Process whole-object records: scan all pointer fields in the object.
    void drainWholeObject(void* object, uint32_t size,
                          std::function<void(void** slot)> fieldIterator) {
        if (!object || !fieldIterator) return;

        // Scan every pointer-sized field in the object.
        size_t ptrCount = size / sizeof(void*);
        void** fields = static_cast<void**>(object);

        for (size_t i = 0; i < ptrCount; i++) {
            void* target = fields[i];
            if (target && cb_.isInNursery && cb_.isInNursery(target)) {
                if (cb_.addToRootSet) cb_.addToRootSet(target);
                nurseryRoots_.insert(target);
            }
        }

        stats_.wholeObjects++;
    }

    // Deduplicate nursery roots (remove duplicates before scavenger).
    size_t deduplicateRoots() {
        size_t before = nurseryRoots_.size();
        // Already a set, so dedup is implicit.
        return before;
    }

    void clear() {
        nurseryRoots_.clear();
    }

    size_t nurseryRootCount() const { return nurseryRoots_.size(); }

    struct Stats {
        uint64_t totalEdges = 0;
        uint64_t nurseryEdges = 0;
        uint64_t forwardedEdges = 0;
        uint64_t totalCells = 0;
        uint64_t cellDrainedSlots = 0;
        uint64_t wholeObjects = 0;
    };

    const Stats& stats() const { return stats_; }

    void resetStats() {
        stats_ = {};
    }

    template<typename Fn>
    void forEachNurseryRoot(Fn&& fn) const {
        for (void* root : nurseryRoots_) fn(root);
    }

private:
    NurseryRange nursery_{0, 0};
    Callbacks cb_;
    std::unordered_set<void*> nurseryRoots_;
    Stats stats_;
};

} // namespace Zepra::Heap

// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_barrier_set.cpp — Unified barrier API: card + store-buffer + SATB + conditional

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <atomic>
#include <functional>

namespace Zepra::Heap {

enum class BarrierMode : uint8_t {
    None          = 0,
    CardMarking   = 1,
    StoreBuffer   = 2,
    SATB          = 3,      // Snapshot-at-the-beginning
    Combined      = 4,      // Card + SATB
};

class BarrierSet {
public:
    struct Callbacks {
        std::function<void(void* card)> dirtyCard;
        std::function<void(void** slot, void* target)> recordEdge;
        std::function<void(void* oldValue)> satbLog;
        std::function<bool(void* ptr)> isInNursery;
        std::function<bool(void* ptr)> isInHeap;
    };

    BarrierSet() : mode_(BarrierMode::Combined), enabled_(true) {}

    void setMode(BarrierMode mode) { mode_ = mode; }
    BarrierMode mode() const { return mode_; }

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }

    // Write barrier: called on every pointer store `*slot = target`.
    void writeBarrier(void** slot, void* oldValue, void* newValue) {
        if (!enabled_) return;

        stats_.totalBarriers++;

        switch (mode_) {
            case BarrierMode::None:
                break;

            case BarrierMode::CardMarking:
                cardBarrier(slot);
                break;

            case BarrierMode::StoreBuffer:
                storeBufferBarrier(slot, newValue);
                break;

            case BarrierMode::SATB:
                satbBarrier(oldValue);
                break;

            case BarrierMode::Combined:
                cardBarrier(slot);
                if (needsGenerationalBarrier(newValue)) {
                    storeBufferBarrier(slot, newValue);
                }
                if (isIncrementalMarking_) {
                    satbBarrier(oldValue);
                }
                break;
        }
    }

    // Pre-write barrier (SATB): log the old value before overwriting.
    void preWriteBarrier(void* oldValue) {
        if (!enabled_ || !isIncrementalMarking_) return;
        satbBarrier(oldValue);
    }

    // Post-write barrier (generational): record new cross-generation edges.
    void postWriteBarrier(void** slot, void* newValue) {
        if (!enabled_) return;

        if (needsGenerationalBarrier(newValue)) {
            storeBufferBarrier(slot, newValue);
            stats_.generationalBarriers++;
        }
    }

    // Deletion barrier (used when removing references).
    void deleteBarrier(void* oldValue) {
        if (!enabled_ || !isIncrementalMarking_) return;
        satbBarrier(oldValue);
        stats_.deletionBarriers++;
    }

    void setIncrementalMarking(bool active) { isIncrementalMarking_ = active; }
    bool isIncrementalMarking() const { return isIncrementalMarking_; }

    struct Stats {
        uint64_t totalBarriers = 0;
        uint64_t cardBarriers = 0;
        uint64_t storeBufferRecords = 0;
        uint64_t satbLogs = 0;
        uint64_t generationalBarriers = 0;
        uint64_t deletionBarriers = 0;
        uint64_t skippedBarriers = 0;
    };

    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = {}; }

private:
    void cardBarrier(void** slot) {
        if (cb_.dirtyCard) {
            // Card = slot address >> card shift (typically 9 bits = 512 byte cards).
            void* card = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) >> 9);
            cb_.dirtyCard(card);
            stats_.cardBarriers++;
        }
    }

    void storeBufferBarrier(void** slot, void* target) {
        if (cb_.recordEdge) {
            cb_.recordEdge(slot, target);
            stats_.storeBufferRecords++;
        }
    }

    void satbBarrier(void* oldValue) {
        if (!oldValue) return;
        if (cb_.satbLog && cb_.isInHeap && cb_.isInHeap(oldValue)) {
            cb_.satbLog(oldValue);
            stats_.satbLogs++;
        }
    }

    bool needsGenerationalBarrier(void* target) const {
        if (!target || !cb_.isInNursery) return false;
        return cb_.isInNursery(target);
    }

    BarrierMode mode_;
    Callbacks cb_;
    Stats stats_;
    bool enabled_ = true;
    bool isIncrementalMarking_ = false;
};

} // namespace Zepra::Heap

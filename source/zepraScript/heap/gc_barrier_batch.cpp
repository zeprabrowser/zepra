// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_barrier_batch.cpp — Bulk barrier for memcpy/memmove of reference arrays

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <functional>
#include <atomic>

namespace Zepra::Heap {

class BarrierBatch {
public:
    struct Callbacks {
        std::function<void(void** slot, void* target)> recordEdge;
        std::function<void(void* card)> dirtyCard;
        std::function<void(void* oldValue)> satbLog;
        std::function<bool(void* ptr)> isInNursery;
        std::function<bool(void* ptr)> isInHeap;
    };

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }
    void setIncrementalMarking(bool active) { isIncrementalMarking_ = active; }

    // Bulk write barrier: fires barriers for every pointer in the copied range.
    // Used when copying an array of references (e.g., Array.prototype.splice).
    void arrayCopy(void** dst, const void* const* src, size_t count) {
        if (count == 0) return;

        // SATB: log old values if incremental marking.
        if (isIncrementalMarking_ && cb_.satbLog && cb_.isInHeap) {
            for (size_t i = 0; i < count; i++) {
                void* oldVal = dst[i];
                if (oldVal && cb_.isInHeap(oldVal)) {
                    cb_.satbLog(oldVal);
                    stats_.satbLogs++;
                }
            }
        }

        // Copy.
        memcpy(dst, src, count * sizeof(void*));

        // Post barrier: record generational edges and dirty cards.
        postBarrierRange(dst, count);

        stats_.batchOps++;
        stats_.totalElements += count;
    }

    // Bulk move barrier: for memmove of reference arrays (overlapping regions).
    void arrayMove(void** dst, void** src, size_t count) {
        if (count == 0) return;

        // SATB: log destination old values.
        if (isIncrementalMarking_ && cb_.satbLog && cb_.isInHeap) {
            for (size_t i = 0; i < count; i++) {
                void* oldVal = dst[i];
                if (oldVal && cb_.isInHeap(oldVal)) {
                    cb_.satbLog(oldVal);
                    stats_.satbLogs++;
                }
            }
        }

        memmove(dst, src, count * sizeof(void*));
        postBarrierRange(dst, count);

        stats_.batchOps++;
        stats_.totalElements += count;
    }

    // Bulk fill barrier: for Array.fill with a reference value.
    void arrayFill(void** dst, void* value, size_t count) {
        if (count == 0) return;

        // SATB: log old values.
        if (isIncrementalMarking_ && cb_.satbLog && cb_.isInHeap) {
            for (size_t i = 0; i < count; i++) {
                void* oldVal = dst[i];
                if (oldVal && cb_.isInHeap(oldVal)) {
                    cb_.satbLog(oldVal);
                    stats_.satbLogs++;
                }
            }
        }

        // Fill.
        for (size_t i = 0; i < count; i++) dst[i] = value;

        // Post barrier: only need to check the fill value once.
        if (value && cb_.isInNursery && cb_.isInNursery(value)) {
            for (size_t i = 0; i < count; i++) {
                if (cb_.recordEdge) cb_.recordEdge(&dst[i], value);
                stats_.generationalRecords++;
            }
        }

        dirtyCardRange(dst, count);

        stats_.batchOps++;
        stats_.totalElements += count;
    }

    // Bulk zero barrier: for clearing reference arrays.
    void arrayClear(void** dst, size_t count) {
        if (count == 0) return;

        // SATB: log old values before clearing.
        if (isIncrementalMarking_ && cb_.satbLog && cb_.isInHeap) {
            for (size_t i = 0; i < count; i++) {
                void* oldVal = dst[i];
                if (oldVal && cb_.isInHeap(oldVal)) {
                    cb_.satbLog(oldVal);
                    stats_.satbLogs++;
                }
            }
        }

        memset(dst, 0, count * sizeof(void*));
        // No post barrier needed for null stores.

        stats_.batchOps++;
        stats_.totalElements += count;
    }

    struct Stats {
        uint64_t batchOps = 0;
        uint64_t totalElements = 0;
        uint64_t generationalRecords = 0;
        uint64_t cardsDirtied = 0;
        uint64_t satbLogs = 0;
    };

    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = {}; }

private:
    void postBarrierRange(void** dst, size_t count) {
        bool anyNursery = false;

        for (size_t i = 0; i < count; i++) {
            void* target = dst[i];
            if (!target) continue;

            if (cb_.isInNursery && cb_.isInNursery(target)) {
                if (cb_.recordEdge) cb_.recordEdge(&dst[i], target);
                anyNursery = true;
                stats_.generationalRecords++;
            }
        }

        dirtyCardRange(dst, count);
    }

    void dirtyCardRange(void** dst, size_t count) {
        if (!cb_.dirtyCard || count == 0) return;

        static constexpr size_t kCardShift = 9;  // 512-byte cards
        uintptr_t firstCard = reinterpret_cast<uintptr_t>(dst) >> kCardShift;
        uintptr_t lastCard = reinterpret_cast<uintptr_t>(dst + count - 1) >> kCardShift;

        for (uintptr_t card = firstCard; card <= lastCard; card++) {
            cb_.dirtyCard(reinterpret_cast<void*>(card));
            stats_.cardsDirtied++;
        }
    }

    Callbacks cb_;
    Stats stats_;
    bool isIncrementalMarking_ = false;
};

} // namespace Zepra::Heap

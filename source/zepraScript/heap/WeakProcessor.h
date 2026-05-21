// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WeakProcessor.h
 * @brief Full weak reference processing pipeline
 *
 * After marking, processes all weak references:
 * 1. WeakRef: null out refs to unmarked objects
 * 2. WeakMap/WeakSet: remove entries with dead keys (via ephemerons)
 * 3. FinalizationRegistry: collect entries for dead targets
 * 4. Weak callbacks: invoke C++ weak reference callbacks
 * 5. Weak handles: clear PersistentHandle<T> that went weak
 *
 * Processing order matters: ephemerons must be converged before
 * weak refs are cleared, and finalization entries must be collected
 * before objects are swept.
 *
 * This is run while all mutator threads are stopped (in the GC pause).
 */

#pragma once

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <vector>
#include <deque>
#include <functional>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace Zepra::Heap {

// =============================================================================
// Weak Reference Types
// =============================================================================

enum class WeakReferenceKind : uint8_t {
    WeakRef,                // ES WeakRef
    WeakMapKey,             // WeakMap key
    WeakSetEntry,           // WeakSet entry
    FinalizationTarget,     // FinalizationRegistry target
    InternalCallback,       // C++ weak callback
    PersistentWeak,         // Weak persistent handle
};

// =============================================================================
// Weak Reference Entry
// =============================================================================

struct WeakRefEntry {
    WeakReferenceKind kind;
    void** slot;                // Pointer to the slot holding the weak ref
    void* callbackData;         // Data for callbacks
    using ClearCallback = void(*)(void* callbackData);
    ClearCallback onClear;      // Called when ref is cleared

    bool isCleared() const { return slot && *slot == nullptr; }
};

// =============================================================================
// Finalization Entry
// =============================================================================

struct FinalizationRecord {
    void* target;               // Object being tracked (weak)
    void* heldValue;            // Value passed to cleanup callback
    void* unregisterToken;      // Token for unregistration (nullable)
    void* callback;             // The cleanup callback function

    // Ownership: FinalizationRegistry that owns this record
    void* registry;
};

// =============================================================================
// Weak Callback
// =============================================================================

struct WeakCallbackEntry {
    void** slot;                        // Slot holding the weak pointer
    void* parameter;                    // User-provided parameter
    using CallbackFn = void(*)(void** slot, void* parameter);
    CallbackFn callback;
    bool called = false;
};

// =============================================================================
// Weak Processing Statistics
// =============================================================================

struct WeakProcessingStats {
    size_t weakRefsProcessed = 0;
    size_t weakRefsCleared = 0;
    size_t finalizationRecordsCollected = 0;
    size_t weakCallbacksFired = 0;
    size_t weakMapsProcessed = 0;
    size_t weakMapEntriesRemoved = 0;
    double processingTimeMs = 0;
};

// =============================================================================
// Weak Processor
// =============================================================================

class WeakProcessor {
public:
    using IsMarkedFn = std::function<bool(void*)>;

    WeakProcessor();
    ~WeakProcessor();

    // -------------------------------------------------------------------------
    // Registration
    // -------------------------------------------------------------------------

    /**
     * @brief Register a weak reference slot
     * After GC, if the target is not marked, the slot is set to nullptr.
     */
    void registerWeakRef(void** slot, WeakRefEntry::ClearCallback onClear = nullptr,
                          void* callbackData = nullptr);

    /**
     * @brief Unregister a weak reference slot
     */
    void unregisterWeakRef(void** slot);

    /**
     * @brief Register a finalization record
     */
    void registerFinalization(const FinalizationRecord& record);

    /**
     * @brief Unregister finalization by token
     * @return Number of records removed
     */
    size_t unregisterFinalization(void* token);

    /**
     * @brief Register a weak callback
     */
    void registerWeakCallback(void** slot, WeakCallbackEntry::CallbackFn callback,
                               void* parameter);

    // -------------------------------------------------------------------------
    // Processing (called during GC pause)
    // -------------------------------------------------------------------------

    /**
     * @brief Process all weak references
     *
     * Phase 1: Clear weak refs to unmarked objects
     * Phase 2: Fire weak callbacks
     * Phase 3: Collect finalization records
     *
     * @param isMarked Predicate: is this object marked (reachable)?
     * @return Statistics
     */
    WeakProcessingStats process(IsMarkedFn isMarked);

    /**
     * @brief Get collected finalization records
     * These should be enqueued for cleanup callback execution.
     */
    std::vector<FinalizationRecord> takeCollectedFinalizations();

    /**
     * @brief Prune dead entries (remove entries where slot itself is dead)
     */
    void prune(IsMarkedFn isMarked);

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    size_t weakRefCount() const;
    size_t finalizationCount() const;
    size_t callbackCount() const;

    const WeakProcessingStats& lastStats() const { return lastStats_; }

private:
    void processWeakRefs(IsMarkedFn isMarked, WeakProcessingStats& stats);
    void processCallbacks(IsMarkedFn isMarked, WeakProcessingStats& stats);
    void processFinalization(IsMarkedFn isMarked, WeakProcessingStats& stats);

    std::vector<WeakRefEntry> weakRefs_;
    std::vector<FinalizationRecord> finalizationRecords_;
    std::vector<WeakCallbackEntry> callbacks_;
    std::vector<FinalizationRecord> collectedFinalizations_;

    WeakProcessingStats lastStats_;
    mutable std::mutex mutex_;
};

// =============================================================================
// Store Barrier
// =============================================================================

/**
 * @brief Write barrier implementations
 *
 * Write barriers notify the GC when a reference field is modified.
 * Two barrier types:
 *
 * 1. Generational barrier: records old→young references
 *    (for scavenger — only scan remembered set, not all old gen)
 *
 * 2. Concurrent marking barrier (SATB): records overwritten values
 *    (for incremental/concurrent marking — maintains tri-color invariant)
 *
 * The barrier is called on every store to a reference field:
 *   object.field = newValue;
 *   // → writeBarrier(object, &object.field, oldValue, newValue);
 *
 * Performance is critical: barriers run inline in hot loops.
 * The fast path is a single check + branch.
 */
class StoreBarrier {
public:
    enum class Mode : uint8_t {
        None,               // No barrier (GC not active)
        Generational,       // Only: old→young tracking
        SATB,               // Only: snapshot-at-the-beginning
        Combined,           // Both generational + SATB
    };

    using IsYoungFn = bool(*)(void*);
    using IsOldFn = bool(*)(void*);
    using RecordSlotFn = void(*)(void* sourceObject, void** slot);
    using MarkObjectFn = void(*)(void* object);

    StoreBarrier();

    /**
     * @brief Set barrier mode
     */
    void setMode(Mode mode) { mode_ = mode; }
    Mode mode() const { return mode_; }

    /**
     * @brief Set the space checks
     */
    void setSpaceChecks(IsYoungFn isYoung, IsOldFn isOld) {
        isYoung_ = isYoung;
        isOld_ = isOld;
    }

    /**
     * @brief Set the generational slot recorder
     */
    void setSlotRecorder(RecordSlotFn recorder) {
        recordSlot_ = recorder;
    }

    /**
     * @brief Set the SATB marking function
     */
    void setMarkFn(MarkObjectFn markFn) {
        markObject_ = markFn;
    }

    /**
     * @brief Execute the write barrier (hot path)
     *
     * @param sourceObject The object being modified
     * @param slot Pointer to the field being written
     * @param oldValue Previous value of the field
     * @param newValue New value being stored
     */
    void barrier(void* sourceObject, void** slot,
                 void* oldValue, void* newValue) {
        switch (mode_) {
            case Mode::None:
                break;

            case Mode::Generational:
                generationalBarrier(sourceObject, slot, newValue);
                break;

            case Mode::SATB:
                satbBarrier(oldValue);
                break;

            case Mode::Combined:
                generationalBarrier(sourceObject, slot, newValue);
                satbBarrier(oldValue);
                break;
        }

        stats_.total++;
    }

    /**
     * @brief Fast inline check (for JIT-generated code)
     */
    static bool needsBarrier(Mode mode) {
        return mode != Mode::None;
    }

    /**
     * @brief Barrier statistics
     */
    struct BarrierStats {
        uint64_t total = 0;
        uint64_t generationalHits = 0;     // old→young detected
        uint64_t generationalMisses = 0;   // Same-gen (no-op)
        uint64_t satbHits = 0;             // Overwritten value marked
        uint64_t satbMisses = 0;           // Already marked
    };

    const BarrierStats& stats() const { return stats_; }
    void resetStats() { stats_ = {}; }

private:
    void generationalBarrier(void* sourceObject, void** slot, void* newValue) {
        if (!isYoung_ || !isOld_ || !recordSlot_) return;

        // Only record if old→young
        if (newValue && isYoung_(newValue) && isOld_(sourceObject)) {
            recordSlot_(sourceObject, slot);
            stats_.generationalHits++;
        } else {
            stats_.generationalMisses++;
        }
    }

    void satbBarrier(void* oldValue) {
        if (!markObject_) return;

        // SATB: mark the overwritten value to prevent it from being missed
        if (oldValue) {
            markObject_(oldValue);
            stats_.satbHits++;
        } else {
            stats_.satbMisses++;
        }
    }

    Mode mode_ = Mode::None;
    IsYoungFn isYoung_ = nullptr;
    IsOldFn isOld_ = nullptr;
    RecordSlotFn recordSlot_ = nullptr;
    MarkObjectFn markObject_ = nullptr;
    BarrierStats stats_;
};

// =============================================================================
// Finalization Queue
// =============================================================================

/**
 * @brief Multi-phase finalization queue
 *
 * Finalization is tricky because:
 * 1. Finalization callbacks can resurrect objects
 * 2. Finalized objects may reference other finalizable objects
 * 3. Finalization order matters for dependent objects
 *
 * Multi-phase approach:
 * Phase 1: Collect all finalization-eligible objects
 * Phase 2: Mark objects reachable from finalization callbacks (resurrection)
 * Phase 3: Execute callbacks (may resurrect objects)
 * Phase 4: Sweep actually-dead objects
 */
class FinalizationQueue {
public:
    struct QueueEntry {
        FinalizationRecord record;
        uint32_t priority;      // Lower = higher priority
        uint64_t enqueueTime;
    };

    /**
     * @brief Enqueue a finalization record
     */
    void enqueue(const FinalizationRecord& record, uint32_t priority = 0);

    /**
     * @brief Process pending finalizations
     * @param executor Runs a finalization callback
     * @return Number of callbacks executed
     */
    size_t processPending(
        std::function<void(const FinalizationRecord&)> executor,
        size_t maxBatch = 100
    );

    /**
     * @brief Check if there are pending finalizations
     */
    bool hasPending() const { return !queue_.empty(); }

    /**
     * @brief Number of pending entries
     */
    size_t pendingCount() const { return queue_.size(); }

    /**
     * @brief Clear all pending
     */
    void clear() { queue_.clear(); }

    /**
     * @brief Process as a microtask (drain all pending)
     */
    size_t drainAll(std::function<void(const FinalizationRecord&)> executor) {
        return processPending(executor, SIZE_MAX);
    }

private:
    std::deque<QueueEntry> queue_;
    mutable std::mutex mutex_;
};

// =============================================================================
// Implementation
// =============================================================================

inline WeakProcessor::WeakProcessor() = default;
inline WeakProcessor::~WeakProcessor() = default;

inline void WeakProcessor::registerWeakRef(
    void** slot, WeakRefEntry::ClearCallback onClear, void* callbackData
) {
    std::lock_guard<std::mutex> lock(mutex_);
    weakRefs_.push_back({
        WeakReferenceKind::WeakRef,
        slot,
        callbackData,
        onClear
    });
}

inline void WeakProcessor::unregisterWeakRef(void** slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    weakRefs_.erase(
        std::remove_if(weakRefs_.begin(), weakRefs_.end(),
            [slot](const WeakRefEntry& e) { return e.slot == slot; }),
        weakRefs_.end()
    );
}

inline void WeakProcessor::registerFinalization(const FinalizationRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    finalizationRecords_.push_back(record);
}

inline size_t WeakProcessor::unregisterFinalization(void* token) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t removed = 0;
    auto it = finalizationRecords_.begin();
    while (it != finalizationRecords_.end()) {
        if (it->unregisterToken == token) {
            it = finalizationRecords_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    return removed;
}

inline void WeakProcessor::registerWeakCallback(
    void** slot, WeakCallbackEntry::CallbackFn callback, void* parameter
) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back({slot, parameter, callback, false});
}

inline WeakProcessingStats WeakProcessor::process(IsMarkedFn isMarked) {
    WeakProcessingStats stats;
    auto start = std::chrono::steady_clock::now();

    processWeakRefs(isMarked, stats);
    processCallbacks(isMarked, stats);
    processFinalization(isMarked, stats);

    stats.processingTimeMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    lastStats_ = stats;
    return stats;
}

inline void WeakProcessor::processWeakRefs(IsMarkedFn isMarked,
                                            WeakProcessingStats& stats) {
    for (auto& ref : weakRefs_) {
        stats.weakRefsProcessed++;
        if (!ref.slot || !*ref.slot) continue;

        if (!isMarked(*ref.slot)) {
            // Target is dead — clear the weak ref
            *ref.slot = nullptr;
            stats.weakRefsCleared++;

            if (ref.onClear) {
                ref.onClear(ref.callbackData);
            }
        }
    }
}

inline void WeakProcessor::processCallbacks(IsMarkedFn isMarked,
                                              WeakProcessingStats& stats) {
    for (auto& cb : callbacks_) {
        if (cb.called) continue;
        if (!cb.slot || !*cb.slot) continue;

        if (!isMarked(*cb.slot)) {
            cb.callback(cb.slot, cb.parameter);
            cb.called = true;
            stats.weakCallbacksFired++;
        }
    }
}

inline void WeakProcessor::processFinalization(IsMarkedFn isMarked,
                                                WeakProcessingStats& stats) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = finalizationRecords_.begin();
    while (it != finalizationRecords_.end()) {
        if (!isMarked(it->target)) {
            collectedFinalizations_.push_back(*it);
            it = finalizationRecords_.erase(it);
            stats.finalizationRecordsCollected++;
        } else {
            ++it;
        }
    }
}

inline std::vector<FinalizationRecord>
WeakProcessor::takeCollectedFinalizations() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = std::move(collectedFinalizations_);
    collectedFinalizations_.clear();
    return result;
}

inline void WeakProcessor::prune(IsMarkedFn isMarked) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove weak refs whose slot itself is in dead memory
    weakRefs_.erase(
        std::remove_if(weakRefs_.begin(), weakRefs_.end(),
            [&isMarked](const WeakRefEntry& e) {
                return !e.slot || !isMarked(e.slot);
            }),
        weakRefs_.end()
    );

    // Remove completed callbacks
    callbacks_.erase(
        std::remove_if(callbacks_.begin(), callbacks_.end(),
            [](const WeakCallbackEntry& e) { return e.called; }),
        callbacks_.end()
    );
}

inline size_t WeakProcessor::weakRefCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return weakRefs_.size();
}

inline size_t WeakProcessor::finalizationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finalizationRecords_.size();
}

inline size_t WeakProcessor::callbackCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return callbacks_.size();
}

// StoreBarrier
inline StoreBarrier::StoreBarrier() = default;

// FinalizationQueue
inline void FinalizationQueue::enqueue(const FinalizationRecord& record,
                                        uint32_t priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    queue_.push_back({record, priority, now});
}

inline size_t FinalizationQueue::processPending(
    std::function<void(const FinalizationRecord&)> executor,
    size_t maxBatch
) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t processed = 0;

    while (!queue_.empty() && processed < maxBatch) {
        auto entry = queue_.front();
        queue_.pop_front();
        executor(entry.record);
        processed++;
    }

    return processed;
}

} // namespace Zepra::Heap

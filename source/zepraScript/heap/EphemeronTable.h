// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file EphemeronTable.h
 * @brief Ephemeron table for WeakMap/WeakRef GC semantics
 *
 * Ephemerons are key-value pairs where the value is only considered
 * reachable if the key is reachable through other means. This is
 * the correct GC semantics for WeakMap.
 *
 * The challenge: during marking, we can't process an ephemeron entry
 * until we know if its key is marked. But marking the value might
 * make new keys reachable. Requires iterative convergence.
 *
 * Algorithm:
 * 1. During mark phase, ephemeron entries are deferred
 * 2. After initial marking, iterate deferred entries:
 *    - If key is marked → mark value, re-scan
 *    - If key is unmarked → skip (entry will be cleared on sweep)
 * 3. Repeat until no new marking occurs (convergence)
 *
 * This matches the ES spec for WeakMap/WeakSet/WeakRef/FinalizationRegistry.
 */

#pragma once

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

namespace Zepra::Heap {

// =============================================================================
// Ephemeron Entry
// =============================================================================

struct EphemeronEntry {
    void* key;              // Weak reference to key
    void* value;            // Strong reference to value (if key is alive)
    uint32_t hash;          // Cached hash of key
    bool processed;         // Already processed in this GC cycle
};

// =============================================================================
// Ephemeron Table
// =============================================================================

class EphemeronTable {
public:
    static constexpr size_t INITIAL_CAPACITY = 64;
    static constexpr double LOAD_FACTOR = 0.75;

    EphemeronTable();
    ~EphemeronTable();

    // -------------------------------------------------------------------------
    // Map-like interface
    // -------------------------------------------------------------------------

    void set(void* key, void* value);
    void* get(void* key) const;
    bool has(void* key) const;
    bool remove(void* key);
    void clear();
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    // -------------------------------------------------------------------------
    // GC Integration
    // -------------------------------------------------------------------------

    /**
     * @brief Phase 1: Register deferred entries during mark phase
     * Called by the marker when encountering an ephemeron table.
     */
    void registerForProcessing();

    /**
     * @brief Phase 2: Process deferred entries
     * Iterates entries. For each entry where key is marked,
     * marks the value and returns true (new marking happened).
     *
     * @param isMarked Check if an object is marked
     * @param markObject Mark an object and its transitive closure
     * @return true if any new objects were marked
     */
    bool processEphemerons(
        std::function<bool(void*)> isMarked,
        std::function<void(void*)> markObject
    );

    /**
     * @brief Phase 3: Sweep dead entries (key was not marked)
     * @param isMarked Check if an object is marked
     * @return Number of entries removed
     */
    size_t sweepDeadEntries(std::function<bool(void*)> isMarked);

    /**
     * @brief Update pointers after compaction
     */
    void updatePointers(std::function<void*(void*)> forwarding);

    // -------------------------------------------------------------------------
    // Iteration
    // -------------------------------------------------------------------------

    using EntryVisitor = std::function<void(void* key, void* value)>;
    void forEach(EntryVisitor visitor) const;

    // -------------------------------------------------------------------------
    // WeakRef support
    // -------------------------------------------------------------------------

    /**
     * @brief Register a weak reference
     * After GC, weak refs to collected objects are nullified.
     */
    void registerWeakRef(void** slot);

    /**
     * @brief Process weak references after marking
     */
    void processWeakRefs(std::function<bool(void*)> isMarked);

    // -------------------------------------------------------------------------
    // FinalizationRegistry support
    // -------------------------------------------------------------------------

    struct FinalizationEntry {
        void* target;           // Object being tracked
        void* heldValue;        // Value passed to cleanup callback
        void* unregisterToken;  // Token for unregistration
    };

    void registerFinalization(void* target, void* heldValue,
                               void* unregisterToken = nullptr);

    /**
     * @brief Collect entries for finalized objects
     * @return Entries whose targets are no longer reachable
     */
    std::vector<FinalizationEntry> collectFinalizedEntries(
        std::function<bool(void*)> isMarked);

    bool unregisterByToken(void* token);

private:
    // Hash table
    size_t findSlot(void* key, uint32_t hash) const;
    void rehash();
    static uint32_t hashPointer(void* ptr);

    std::vector<EphemeronEntry> entries_;
    size_t size_ = 0;
    size_t capacity_ = 0;

    // Weak references
    std::vector<void**> weakRefs_;

    // Finalization registry
    std::vector<FinalizationEntry> finalizationEntries_;

    mutable std::mutex mutex_;
};

// =============================================================================
// Global Ephemeron Processing
// =============================================================================

/**
 * @brief Manages all ephemeron tables during GC
 *
 * Called by the GC to process all ephemeron tables in the correct order
 * (iterative convergence).
 */
class EphemeronProcessor {
public:
    /**
     * @brief Register a table for processing during this GC cycle
     */
    void registerTable(EphemeronTable* table);

    /**
     * @brief Run iterative convergence on all registered tables
     * @return Total number of iterations needed
     */
    size_t processAll(
        std::function<bool(void*)> isMarked,
        std::function<void(void*)> markObject
    );

    /**
     * @brief Sweep all registered tables
     */
    void sweepAll(std::function<bool(void*)> isMarked);

    /**
     * @brief Collect all finalization entries
     */
    std::vector<EphemeronTable::FinalizationEntry> collectAllFinalized(
        std::function<bool(void*)> isMarked);

    /**
     * @brief Clear for next GC cycle
     */
    void reset();

private:
    std::vector<EphemeronTable*> tables_;
    static constexpr size_t MAX_ITERATIONS = 100;  // Safety limit
};

// =============================================================================
// Implementation
// =============================================================================

inline EphemeronTable::EphemeronTable() {
    capacity_ = INITIAL_CAPACITY;
    entries_.resize(capacity_);
    for (auto& e : entries_) {
        e.key = nullptr;
        e.value = nullptr;
        e.hash = 0;
        e.processed = false;
    }
}

inline EphemeronTable::~EphemeronTable() = default;

inline uint32_t EphemeronTable::hashPointer(void* ptr) {
    uintptr_t h = reinterpret_cast<uintptr_t>(ptr);
    h = (h ^ (h >> 16)) * 0x45d9f3b;
    h = (h ^ (h >> 16)) * 0x45d9f3b;
    h = h ^ (h >> 16);
    return static_cast<uint32_t>(h);
}

inline size_t EphemeronTable::findSlot(void* key, uint32_t hash) const {
    size_t mask = capacity_ - 1;
    size_t idx = hash & mask;
    size_t probes = 0;

    while (probes < capacity_) {
        if (entries_[idx].key == nullptr || entries_[idx].key == key) {
            return idx;
        }
        idx = (idx + 1) & mask;
        probes++;
    }
    return capacity_;  // Table full (shouldn't happen with load factor)
}

inline void EphemeronTable::set(void* key, void* value) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (static_cast<double>(size_ + 1) / static_cast<double>(capacity_) > LOAD_FACTOR) {
        rehash();
    }

    uint32_t hash = hashPointer(key);
    size_t idx = findSlot(key, hash);
    if (idx >= capacity_) return;

    bool isNew = entries_[idx].key == nullptr;
    entries_[idx].key = key;
    entries_[idx].value = value;
    entries_[idx].hash = hash;
    entries_[idx].processed = false;

    if (isNew) size_++;
}

inline void* EphemeronTable::get(void* key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t hash = hashPointer(key);
    size_t idx = findSlot(key, hash);
    if (idx < capacity_ && entries_[idx].key == key) {
        return entries_[idx].value;
    }
    return nullptr;
}

inline bool EphemeronTable::has(void* key) const {
    return get(key) != nullptr;
}

inline bool EphemeronTable::remove(void* key) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t hash = hashPointer(key);
    size_t idx = findSlot(key, hash);
    if (idx < capacity_ && entries_[idx].key == key) {
        entries_[idx].key = nullptr;
        entries_[idx].value = nullptr;
        size_--;
        return true;
    }
    return false;
}

inline void EphemeronTable::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : entries_) {
        e.key = nullptr;
        e.value = nullptr;
    }
    size_ = 0;
}

inline void EphemeronTable::rehash() {
    size_t newCap = capacity_ * 2;
    std::vector<EphemeronEntry> oldEntries = std::move(entries_);
    entries_.resize(newCap);
    for (auto& e : entries_) { e.key = nullptr; e.value = nullptr; }
    capacity_ = newCap;
    size_ = 0;

    for (auto& old : oldEntries) {
        if (old.key) {
            set(old.key, old.value);  // Re-insert (recursive lock safe with lock already held)
        }
    }
}

inline void EphemeronTable::registerForProcessing() {
    for (auto& e : entries_) {
        e.processed = false;
    }
}

inline bool EphemeronTable::processEphemerons(
    std::function<bool(void*)> isMarked,
    std::function<void(void*)> markObject
) {
    bool madeProgress = false;

    for (auto& e : entries_) {
        if (!e.key || e.processed) continue;

        if (isMarked(e.key)) {
            // Key is reachable → value must also be reachable
            if (e.value && !isMarked(e.value)) {
                markObject(e.value);
                madeProgress = true;
            }
            e.processed = true;
        }
    }

    return madeProgress;
}

inline size_t EphemeronTable::sweepDeadEntries(std::function<bool(void*)> isMarked) {
    size_t removed = 0;
    for (auto& e : entries_) {
        if (e.key && !isMarked(e.key)) {
            e.key = nullptr;
            e.value = nullptr;
            size_--;
            removed++;
        }
    }
    return removed;
}

inline void EphemeronTable::updatePointers(std::function<void*(void*)> forwarding) {
    for (auto& e : entries_) {
        if (e.key) {
            void* newKey = forwarding(e.key);
            void* newValue = e.value ? forwarding(e.value) : nullptr;
            e.key = newKey;
            e.value = newValue;
            e.hash = hashPointer(newKey);
        }
    }
}

inline void EphemeronTable::forEach(EntryVisitor visitor) const {
    for (const auto& e : entries_) {
        if (e.key) visitor(e.key, e.value);
    }
}

inline void EphemeronTable::registerWeakRef(void** slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    weakRefs_.push_back(slot);
}

inline void EphemeronTable::processWeakRefs(std::function<bool(void*)> isMarked) {
    for (auto** slot : weakRefs_) {
        if (*slot && !isMarked(*slot)) {
            *slot = nullptr;  // Target collected → null out weak ref
        }
    }
}

inline void EphemeronTable::registerFinalization(
    void* target, void* heldValue, void* unregisterToken) {
    std::lock_guard<std::mutex> lock(mutex_);
    finalizationEntries_.push_back({target, heldValue, unregisterToken});
}

inline std::vector<EphemeronTable::FinalizationEntry>
EphemeronTable::collectFinalizedEntries(std::function<bool(void*)> isMarked) {
    std::vector<FinalizationEntry> collected;
    auto it = finalizationEntries_.begin();
    while (it != finalizationEntries_.end()) {
        if (!isMarked(it->target)) {
            collected.push_back(*it);
            it = finalizationEntries_.erase(it);
        } else {
            ++it;
        }
    }
    return collected;
}

inline bool EphemeronTable::unregisterByToken(void* token) {
    auto it = finalizationEntries_.begin();
    bool removed = false;
    while (it != finalizationEntries_.end()) {
        if (it->unregisterToken == token) {
            it = finalizationEntries_.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    return removed;
}

// EphemeronProcessor

inline void EphemeronProcessor::registerTable(EphemeronTable* table) {
    tables_.push_back(table);
    table->registerForProcessing();
}

inline size_t EphemeronProcessor::processAll(
    std::function<bool(void*)> isMarked,
    std::function<void(void*)> markObject
) {
    size_t iterations = 0;
    bool madeProgress = true;

    while (madeProgress && iterations < MAX_ITERATIONS) {
        madeProgress = false;
        for (auto* table : tables_) {
            if (table->processEphemerons(isMarked, markObject)) {
                madeProgress = true;
            }
        }
        iterations++;
    }

    return iterations;
}

inline void EphemeronProcessor::sweepAll(std::function<bool(void*)> isMarked) {
    for (auto* table : tables_) {
        table->sweepDeadEntries(isMarked);
    }
}

inline std::vector<EphemeronTable::FinalizationEntry>
EphemeronProcessor::collectAllFinalized(std::function<bool(void*)> isMarked) {
    std::vector<EphemeronTable::FinalizationEntry> all;
    for (auto* table : tables_) {
        auto entries = table->collectFinalizedEntries(isMarked);
        all.insert(all.end(), entries.begin(), entries.end());
    }
    return all;
}

inline void EphemeronProcessor::reset() {
    tables_.clear();
}

} // namespace Zepra::Heap

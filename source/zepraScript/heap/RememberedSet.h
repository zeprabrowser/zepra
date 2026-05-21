// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file RememberedSet.h
 * @brief Remembered set for generational GC write barriers
 *
 * Tracks old-generation → young-generation references so the scavenger
 * can avoid scanning the entire old gen during minor GC.
 *
 * Three implementations:
 * 1. CardTable — coarse-grained (one bit per 512-byte card)
 * 2. HashSet — fine-grained (exact slot recording)
 * 3. SequentialStore — ordered buffer for cache-friendly iteration
 *
 * The card table is the default for old gen. The hash set is used
 * for large arrays where card granularity wastes scanning time.
 */

#pragma once

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>
#include <unordered_set>
#include <atomic>
#include <functional>

namespace Zepra::Heap {

// =============================================================================
// Card Table
// =============================================================================

/**
 * @brief Coarse-grained remembered set via card marking
 *
 * Divides the heap into cards (512 bytes each). When an old-gen object
 * stores a pointer to a young-gen object, the card containing the
 * source object is dirtied.
 *
 * During scavenge, only dirty cards are scanned for young-gen refs.
 */
class CardTable {
public:
    static constexpr size_t CARD_SIZE = 512;
    static constexpr uint8_t CLEAN = 0;
    static constexpr uint8_t DIRTY = 1;

    CardTable() = default;

    /**
     * @brief Initialize card table for a heap region
     */
    void initialize(void* heapBase, size_t heapSize) {
        heapBase_ = reinterpret_cast<uintptr_t>(heapBase);
        heapSize_ = heapSize;
        size_t numCards = (heapSize + CARD_SIZE - 1) / CARD_SIZE;
        cards_.resize(numCards, CLEAN);
    }

    /**
     * @brief Dirty the card containing the given address
     * Called by the write barrier.
     */
    void dirty(void* address) {
        size_t index = cardIndex(address);
        if (index < cards_.size()) {
            cards_[index] = DIRTY;
        }
    }

    /**
     * @brief Check if card is dirty
     */
    bool isDirty(void* address) const {
        size_t index = cardIndex(address);
        return index < cards_.size() && cards_[index] == DIRTY;
    }

    /**
     * @brief Clear a single card
     */
    void clean(void* address) {
        size_t index = cardIndex(address);
        if (index < cards_.size()) {
            cards_[index] = CLEAN;
        }
    }

    /**
     * @brief Clear all cards
     */
    void clearAll() {
        std::memset(cards_.data(), CLEAN, cards_.size());
    }

    /**
     * @brief Iterate dirty cards
     * Callback receives the start address and size of each dirty card.
     */
    void forEachDirtyCard(
        std::function<void(void* cardStart, size_t cardSize)> callback
    ) const {
        for (size_t i = 0; i < cards_.size(); i++) {
            if (cards_[i] == DIRTY) {
                void* cardStart = reinterpret_cast<void*>(heapBase_ + i * CARD_SIZE);
                size_t cardSize = CARD_SIZE;
                // Clamp last card
                if ((i + 1) * CARD_SIZE > heapSize_) {
                    cardSize = heapSize_ - i * CARD_SIZE;
                }
                callback(cardStart, cardSize);
            }
        }
    }

    /**
     * @brief Count dirty cards
     */
    size_t dirtyCount() const {
        size_t count = 0;
        for (uint8_t c : cards_) {
            if (c == DIRTY) count++;
        }
        return count;
    }

    /**
     * @brief Total cards
     */
    size_t totalCards() const { return cards_.size(); }

    /**
     * @brief Dirty ratio (0.0 - 1.0)
     */
    double dirtyRatio() const {
        return cards_.empty() ? 0.0
            : static_cast<double>(dirtyCount()) / static_cast<double>(cards_.size());
    }

private:
    size_t cardIndex(void* address) const {
        uintptr_t addr = reinterpret_cast<uintptr_t>(address);
        if (addr < heapBase_) return SIZE_MAX;
        return (addr - heapBase_) / CARD_SIZE;
    }

    uintptr_t heapBase_ = 0;
    size_t heapSize_ = 0;
    std::vector<uint8_t> cards_;
};

// =============================================================================
// Slot-Based Remembered Set
// =============================================================================

/**
 * @brief Fine-grained remembered set: records exact slots
 *
 * More precise than card table — no unnecessary scanning.
 * Higher memory overhead per entry, but reduces scavenge time
 * when there are few old→young references.
 */
class SlotRememberedSet {
public:
    /**
     * @brief Record an old→young reference slot
     */
    void record(void** slot) {
        slots_.insert(slot);
    }

    /**
     * @brief Remove a slot
     */
    void remove(void** slot) {
        slots_.erase(slot);
    }

    /**
     * @brief Check if slot is recorded
     */
    bool contains(void** slot) const {
        return slots_.count(slot) > 0;
    }

    /**
     * @brief Check if object (by address) has any slots recorded
     */
    bool containsObject(void* object) const {
        // Conservative: check if any recorded slot falls within
        // a reasonable range from the object address
        // (Simplified — real implementation needs object size info)
        for (auto** slot : slots_) {
            auto* slotAddr = reinterpret_cast<char*>(slot);
            auto* objAddr = static_cast<char*>(object);
            if (slotAddr >= objAddr && slotAddr < objAddr + 4096) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Iterate all recorded slots
     * Called during scavenge to update references.
     */
    void forEach(std::function<void(void** slot)> callback) const {
        for (auto** slot : slots_) {
            callback(slot);
        }
    }

    /**
     * @brief Remove slots that no longer point to young gen
     */
    void prune(std::function<bool(void*)> isYoung) {
        auto it = slots_.begin();
        while (it != slots_.end()) {
            if (!*(*it) || !isYoung(*(*it))) {
                it = slots_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /**
     * @brief Clear all recorded slots
     */
    void clear() { slots_.clear(); }

    size_t size() const { return slots_.size(); }

private:
    std::unordered_set<void**> slots_;
};

// =============================================================================
// Sequential Store Buffer (SSB)
// =============================================================================

/**
 * @brief Ordered buffer for write barrier entries
 *
 * Write barriers append to a sequential buffer instead of
 * updating a hash set. This is faster because:
 * - No hashing overhead
 * - Cache-friendly sequential writes
 * - Bulk processing during GC
 *
 * Duplicates are allowed (deduplicated during processing).
 */
class SequentialStoreBuffer {
public:
    static constexpr size_t DEFAULT_CAPACITY = 16 * 1024;  // 16K entries

    explicit SequentialStoreBuffer(size_t capacity = DEFAULT_CAPACITY)
        : capacity_(capacity) {
        buffer_.reserve(capacity);
    }

    /**
     * @brief Record a slot (fast path — no dedup)
     * @return true if buffer is now full (needs processing)
     */
    bool record(void** slot) {
        buffer_.push_back(slot);
        return buffer_.size() >= capacity_;
    }

    /**
     * @brief Process all entries
     * Deduplicates and calls callback for each unique slot.
     */
    void processAndClear(std::function<void(void** slot)> callback) {
        // Sort for dedup and cache locality
        std::sort(buffer_.begin(), buffer_.end());
        auto last = std::unique(buffer_.begin(), buffer_.end());

        for (auto it = buffer_.begin(); it != last; ++it) {
            callback(*it);
        }

        buffer_.clear();
    }

    /**
     * @brief Clear without processing
     */
    void clear() { buffer_.clear(); }

    size_t size() const { return buffer_.size(); }
    bool full() const { return buffer_.size() >= capacity_; }
    bool empty() const { return buffer_.empty(); }

    /**
     * @brief Flush to a SlotRememberedSet
     */
    void flushTo(SlotRememberedSet& rset) {
        processAndClear([&](void** slot) { rset.record(slot); });
    }

private:
    std::vector<void**> buffer_;
    size_t capacity_;
};

// =============================================================================
// Combined Remembered Set
// =============================================================================

/**
 * @brief Production remembered set combining all strategies
 *
 * Write barrier → SSB (fast) → overflow → CardTable + SlotSet
 */
class RememberedSet {
public:
    RememberedSet() = default;

    /**
     * @brief Initialize with heap region
     */
    void initialize(void* heapBase, size_t heapSize) {
        cardTable_.initialize(heapBase, heapSize);
    }

    /**
     * @brief Record a write barrier event (fast path)
     */
    void recordWrite(void* sourceObject, void** slot) {
        // Fast path: SSB
        if (ssb_.record(slot)) {
            // SSB full — flush to card table
            flush();
        }
        // Also dirty the card (for mixed-mode scanning)
        cardTable_.dirty(sourceObject);
    }

    /**
     * @brief Flush SSB to card table / slot set
     */
    void flush() {
        ssb_.flushTo(slotSet_);
    }

    /**
     * @brief Process all remembered references during scavenge
     *
     * For each slot pointing into young gen:
     * 1. Update the pointer if the young object was moved
     * 2. Remove the entry if the young object was promoted
     */
    void processForScavenge(
        std::function<bool(void*)> isYoung,
        std::function<void(void** slot)> updateCallback
    ) {
        // Flush any pending SSB entries
        flush();

        // Process exact slots first
        slotSet_.forEach([&](void** slot) {
            if (*slot && isYoung(*slot)) {
                updateCallback(slot);
            }
        });

        // Also scan dirty cards for any missed references
        cardTable_.forEachDirtyCard([&](void* cardStart, size_t cardSize) {
            // Scan the card for pointers
            auto** start = static_cast<void**>(cardStart);
            auto** end = reinterpret_cast<void**>(
                static_cast<char*>(cardStart) + cardSize);
            for (auto** slot = start; slot < end; slot++) {
                if (*slot && isYoung(*slot)) {
                    updateCallback(slot);
                }
            }
        });
    }

    /**
     * @brief Clear after full GC (no remembered set needed)
     */
    void clear() {
        ssb_.clear();
        slotSet_.clear();
        cardTable_.clearAll();
    }

    /**
     * @brief Post-scavenge cleanup
     */
    void postScavengeCleanup(std::function<bool(void*)> isYoung) {
        slotSet_.prune(isYoung);
        cardTable_.clearAll();
    }

    /**
     * @brief Check if an object is in the remembered set
     */
    bool containsObject(void* object) const {
        return cardTable_.isDirty(object) || slotSet_.containsObject(object);
    }

    // Accessors
    const CardTable& cardTable() const { return cardTable_; }
    const SlotRememberedSet& slotSet() const { return slotSet_; }
    const SequentialStoreBuffer& ssb() const { return ssb_; }

private:
    CardTable cardTable_;
    SlotRememberedSet slotSet_;
    SequentialStoreBuffer ssb_;
};

} // namespace Zepra::Heap

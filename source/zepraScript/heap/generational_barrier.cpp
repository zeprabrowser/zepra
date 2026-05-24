// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file generational_barrier.cpp
 * @brief Production write barriers for generational + concurrent GC
 *
 * Three barrier types wired together:
 *
 * 1. Generational store barrier (old → young)
 *    - On every reference store into old-gen object, mark the containing
 *      card dirty in the card table so minor GC can find roots
 *    - Fast path: single byte write to card table (no branch on common case)
 *    - Filtering: skip if source is in nursery (young → young is irrelevant)
 *
 * 2. SATB (Snapshot-At-The-Beginning) barrier for concurrent marking
 *    - On overwrite of a reference field, push the OLD value to the
 *      thread-local SATB buffer before the store executes
 *    - This ensures the concurrent marker sees a consistent snapshot
 *    - Buffer overflow: spill to global SATB queue
 *
 * 3. Incremental update barrier for compaction
 *    - After moving an object, update the forwarding table
 *    - On load of a reference, check forwarding and fixup
 *    - Read barrier variant used during compaction phase only
 *
 * Performance is critical: barriers fire on every reference store.
 * The fast path must be branchless or single-branch.
 */

#include <atomic>
#include <mutex>
#include <vector>
#include <deque>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <memory>
#include <algorithm>

#ifdef __linux__
#include <sys/mman.h>
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#endif

namespace Zepra::Heap {

// =============================================================================
// Card Table (Inlined for barrier fast path)
// =============================================================================

/**
 * @brief Minimal card table for the write barrier fast path
 *
 * The card table is a byte array where each byte maps to a 512-byte
 * region of the heap. A dirty card means the region may contain
 * old→young references that need scanning.
 *
 * Layout: one byte per 512-byte card.
 * Total memory: heap_size / 512 bytes  (e.g. 512MB heap → 1MB card table)
 *
 * Card states are kept simple for branch-free fast path:
 *   0x00 = CLEAN
 *   0x01 = DIRTY
 *   0x7F = YOUNG_GEN (skip during old-gen scans)
 */
class BarrierCardTable {
public:
    static constexpr size_t CARD_SHIFT = 9;     // 512 bytes per card
    static constexpr size_t CARD_SIZE = 1 << CARD_SHIFT;
    static constexpr uint8_t CLEAN = 0x00;
    static constexpr uint8_t DIRTY = 0x01;
    static constexpr uint8_t YOUNG_GEN = 0x7F;

    bool initialize(uintptr_t heapBase, size_t heapSize) {
        heapBase_ = heapBase;
        heapSize_ = heapSize;
        cardCount_ = (heapSize + CARD_SIZE - 1) >> CARD_SHIFT;

#ifdef __linux__
        // mmap for zero-init and lazy commit
        cards_ = static_cast<uint8_t*>(mmap(
            nullptr, cardCount_,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
            -1, 0));
        if (cards_ == MAP_FAILED) {
            cards_ = nullptr;
            return false;
        }
        mmapped_ = true;
#else
        cards_ = new(std::nothrow) uint8_t[cardCount_];
        if (!cards_) return false;
        std::memset(cards_, CLEAN, cardCount_);
        mmapped_ = false;
#endif
        return true;
    }

    void destroy() {
        if (!cards_) return;
#ifdef __linux__
        if (mmapped_) {
            munmap(cards_, cardCount_);
        } else {
            delete[] cards_;
        }
#else
        delete[] cards_;
#endif
        cards_ = nullptr;
    }

    // Fast path: single byte write, no branch
    void markDirty(uintptr_t addr) {
        size_t idx = (addr - heapBase_) >> CARD_SHIFT;
        cards_[idx] = DIRTY;
    }

    void markYoung(uintptr_t addr) {
        size_t idx = (addr - heapBase_) >> CARD_SHIFT;
        cards_[idx] = YOUNG_GEN;
    }

    void markYoungRange(uintptr_t start, uintptr_t end) {
        size_t startIdx = (start - heapBase_) >> CARD_SHIFT;
        size_t endIdx = (end - heapBase_) >> CARD_SHIFT;
        endIdx = std::min(endIdx, cardCount_);
        std::memset(cards_ + startIdx, YOUNG_GEN, endIdx - startIdx);
    }

    void clearCard(size_t idx) { cards_[idx] = CLEAN; }

    void clearRange(uintptr_t start, uintptr_t end) {
        size_t startIdx = (start - heapBase_) >> CARD_SHIFT;
        size_t endIdx = (end - heapBase_) >> CARD_SHIFT;
        endIdx = std::min(endIdx, cardCount_);
        std::memset(cards_ + startIdx, CLEAN, endIdx - startIdx);
    }

    void clearAll() {
        if (cards_) {
#ifdef __linux__
            if (mmapped_) {
                // madvise DONTNEED resets pages to zero (lazy re-commit)
                madvise(cards_, cardCount_, MADV_DONTNEED);
            } else {
                std::memset(cards_, CLEAN, cardCount_);
            }
#else
            std::memset(cards_, CLEAN, cardCount_);
#endif
        }
    }

    bool isDirty(size_t idx) const { return cards_[idx] == DIRTY; }
    bool isYoung(size_t idx) const { return cards_[idx] == YOUNG_GEN; }
    uint8_t* raw() { return cards_; }
    const uint8_t* raw() const { return cards_; }
    size_t cardCount() const { return cardCount_; }

    uintptr_t cardBaseAddress(size_t idx) const {
        return heapBase_ + (idx << CARD_SHIFT);
    }

    size_t cardIndex(uintptr_t addr) const {
        return (addr - heapBase_) >> CARD_SHIFT;
    }

    /**
     * @brief Iterate dirty cards and invoke callback
     *
     * The callback receives the card range [start, end) and card index.
     * After visiting, the card is cleared.
     */
    template <typename Callback>
    size_t forEachDirtyCard(Callback callback) {
        size_t count = 0;
        for (size_t i = 0; i < cardCount_; i++) {
            if (cards_[i] == DIRTY) {
                uintptr_t start = cardBaseAddress(i);
                uintptr_t end = start + CARD_SIZE;
                if (end > heapBase_ + heapSize_) end = heapBase_ + heapSize_;
                callback(start, end, i);
                cards_[i] = CLEAN;
                count++;
            }
        }
        return count;
    }

    /**
     * @brief Count dirty cards (for metrics)
     */
    size_t dirtyCount() const {
        size_t count = 0;
        // Process 8 bytes at a time for speed
        size_t aligned = cardCount_ & ~size_t(7);
        for (size_t i = 0; i < aligned; i += 8) {
            uint64_t word;
            std::memcpy(&word, cards_ + i, 8);
            if (word == 0) continue;  // 8 clean cards at once
            // Check individual bytes
            for (size_t j = 0; j < 8; j++) {
                if (cards_[i + j] == DIRTY) count++;
            }
        }
        for (size_t i = aligned; i < cardCount_; i++) {
            if (cards_[i] == DIRTY) count++;
        }
        return count;
    }

private:
    uintptr_t heapBase_ = 0;
    size_t heapSize_ = 0;
    size_t cardCount_ = 0;
    uint8_t* cards_ = nullptr;
    bool mmapped_ = false;
};

// =============================================================================
// SATB Buffer (Thread-Local)
// =============================================================================

/**
 * @brief Per-thread SATB buffer for concurrent marking
 *
 * Fixed-size ring buffer. When full, the oldest entries are
 * compacted into the global queue.
 */
class SATBLocalBuffer {
public:
    static constexpr size_t CAPACITY = 1024;

    SATBLocalBuffer() {
        buffer_ = new(std::nothrow) void*[CAPACITY];
        if (buffer_) std::memset(buffer_, 0, CAPACITY * sizeof(void*));
    }

    ~SATBLocalBuffer() { delete[] buffer_; }

    SATBLocalBuffer(const SATBLocalBuffer&) = delete;
    SATBLocalBuffer& operator=(const SATBLocalBuffer&) = delete;

    /**
     * @brief Push the old value before an overwrite
     * @return false if buffer is full (caller must flush)
     */
    bool push(void* oldValue) {
        if (!buffer_ || count_ >= CAPACITY) return false;
        buffer_[count_++] = oldValue;
        return true;
    }

    void* const* data() const { return buffer_; }
    size_t count() const { return count_; }
    bool full() const { return count_ >= CAPACITY; }
    bool empty() const { return count_ == 0; }

    void clear() { count_ = 0; }

    /**
     * @brief Transfer contents to callback, then clear
     */
    void drainTo(std::function<void(void*, size_t)> sink) {
        if (count_ > 0 && sink) {
            sink(buffer_, count_);
            count_ = 0;
        }
    }

private:
    void** buffer_ = nullptr;
    size_t count_ = 0;
};

// =============================================================================
// Global SATB Queue
// =============================================================================

/**
 * @brief Thread-safe aggregation point for SATB buffers
 *
 * When a thread's local SATB buffer fills, it spills here.
 * The concurrent marker drains this queue periodically.
 */
class GlobalSATBQueue {
public:
    struct Chunk {
        static constexpr size_t SIZE = 1024;
        void* entries[SIZE];
        size_t count;
    };

    void pushChunk(void* const* entries, size_t count) {
        auto chunk = std::make_unique<Chunk>();
        size_t toCopy = std::min(count, Chunk::SIZE);
        std::memcpy(chunk->entries, entries, toCopy * sizeof(void*));
        chunk->count = toCopy;

        std::lock_guard<std::mutex> lock(mutex_);
        chunks_.push_back(std::move(chunk));
        totalEntries_.fetch_add(toCopy, std::memory_order_relaxed);
    }

    /**
     * @brief Drain one chunk into the marker's work queue
     * @return true if a chunk was drained
     */
    bool drainOne(std::function<void(void*)> visitor) {
        std::unique_ptr<Chunk> chunk;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (chunks_.empty()) return false;
            chunk = std::move(chunks_.front());
            chunks_.pop_front();
        }

        for (size_t i = 0; i < chunk->count; i++) {
            if (chunk->entries[i]) {
                visitor(chunk->entries[i]);
            }
        }

        totalEntries_.fetch_sub(chunk->count, std::memory_order_relaxed);
        return true;
    }

    /**
     * @brief Drain all pending entries
     */
    size_t drainAll(std::function<void(void*)> visitor) {
        size_t drained = 0;
        while (drainOne(visitor)) {
            drained++;
        }
        return drained;
    }

    size_t pendingChunks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return chunks_.size();
    }

    size_t pendingEntries() const {
        return totalEntries_.load(std::memory_order_relaxed);
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return chunks_.empty();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        chunks_.clear();
        totalEntries_.store(0, std::memory_order_relaxed);
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::unique_ptr<Chunk>> chunks_;
    std::atomic<size_t> totalEntries_{0};
};

// =============================================================================
// Barrier Configuration
// =============================================================================

struct BarrierConfig {
    // Generational
    bool generationalBarrierEnabled;

    // Concurrent marking
    bool satbBarrierEnabled;

    // Compaction read barrier
    bool readBarrierEnabled;

    // Heap boundaries for fast filtering
    uintptr_t nurseryStart;
    uintptr_t nurseryEnd;
    uintptr_t heapStart;
    uintptr_t heapEnd;

    BarrierConfig()
        : generationalBarrierEnabled(true)
        , satbBarrierEnabled(false)
        , readBarrierEnabled(false)
        , nurseryStart(0)
        , nurseryEnd(0)
        , heapStart(0)
        , heapEnd(0) {}
};

// =============================================================================
// Write Barrier Engine
// =============================================================================

/**
 * @brief Central write barrier dispatch
 *
 * The VM calls writeBarrier() on every reference store.
 * This function must be as fast as possible.
 *
 * Typical fast path (generational only, no concurrent marking):
 *   if (source is old-gen && target is nursery) → mark card dirty
 *
 * Full path (generational + SATB):
 *   1. SATB: push old value to local buffer (before the store)
 *   2. Store the new value
 *   3. Generational: if source in old-gen, mark card dirty
 */
class WriteBarrierEngine {
public:
    struct Stats {
        uint64_t storeBarriersFired = 0;
        uint64_t cardsDirtied = 0;
        uint64_t satbPushes = 0;
        uint64_t satbFlushes = 0;
        uint64_t readBarriersFired = 0;
        uint64_t barriersFiltered = 0;
    };

    WriteBarrierEngine() = default;
    ~WriteBarrierEngine() { cardTable_.destroy(); }

    WriteBarrierEngine(const WriteBarrierEngine&) = delete;
    WriteBarrierEngine& operator=(const WriteBarrierEngine&) = delete;

    bool initialize(uintptr_t heapBase, size_t heapSize) {
        return cardTable_.initialize(heapBase, heapSize);
    }

    void setConfig(const BarrierConfig& config) {
        config_ = config;
    }

    BarrierCardTable& cardTable() { return cardTable_; }
    GlobalSATBQueue& satbQueue() { return satbQueue_; }
    const Stats& stats() const { return stats_; }

    // -------------------------------------------------------------------------
    // Pre-store barrier (SATB)
    // -------------------------------------------------------------------------

    /**
     * @brief Called BEFORE writing a new reference value to a slot
     *
     * Captures the old value for the concurrent marker's snapshot.
     * Only active when concurrent marking is in progress.
     *
     * @param slot The reference slot about to be overwritten
     * @param localBuf The current thread's SATB buffer
     */
    void preStoreBarrier(void** slot, SATBLocalBuffer& localBuf) {
        if (!config_.satbBarrierEnabled) return;
        if (!slot) return;

        void* oldValue = *slot;
        if (!oldValue) return;

        // Only push heap pointers
        auto addr = reinterpret_cast<uintptr_t>(oldValue);
        if (addr < config_.heapStart || addr >= config_.heapEnd) return;

        stats_.satbPushes++;

        if (!localBuf.push(oldValue)) {
            // Buffer full — flush to global queue
            localBuf.drainTo([this](void* buf, size_t count) {
                satbQueue_.pushChunk(static_cast<void* const*>(buf), count);
            });
            stats_.satbFlushes++;
            localBuf.push(oldValue);
        }
    }

    // -------------------------------------------------------------------------
    // Post-store barrier (generational)
    // -------------------------------------------------------------------------

    /**
     * @brief Called AFTER writing a reference into an object field
     *
     * Checks if this creates an old→young reference and marks
     * the card dirty if so.
     *
     * @param sourceAddr Address of the object containing the field
     * @param targetAddr Address of the referenced object (new value)
     */
    void postStoreBarrier(uintptr_t sourceAddr, uintptr_t targetAddr) {
        if (!config_.generationalBarrierEnabled) return;

        stats_.storeBarriersFired++;

        // Fast filter: if source is in nursery, skip
        // (young→young and young→old don't need tracking)
        if (sourceAddr >= config_.nurseryStart &&
            sourceAddr < config_.nurseryEnd) {
            stats_.barriersFiltered++;
            return;
        }

        // Fast filter: if target is NOT in nursery, skip
        // (old→old doesn't need tracking for minor GC)
        if (targetAddr < config_.nurseryStart ||
            targetAddr >= config_.nurseryEnd) {
            stats_.barriersFiltered++;
            return;
        }

        // Old → young reference detected: mark card dirty
        cardTable_.markDirty(sourceAddr);
        stats_.cardsDirtied++;
    }

    /**
     * @brief Combined barrier: pre-store SATB + post-store generational
     *
     * This is the main entry point called by the VM on every
     * reference field write.
     *
     * @param slot Pointer to the reference field
     * @param newValue The new reference being stored
     * @param localBuf Thread-local SATB buffer
     */
    void writeBarrier(void** slot, void* newValue, SATBLocalBuffer& localBuf) {
        // 1. SATB pre-barrier (capture old value before overwrite)
        preStoreBarrier(slot, localBuf);

        // 2. The actual store happens in the VM (not here)

        // 3. Generational post-barrier
        auto sourceAddr = reinterpret_cast<uintptr_t>(slot);
        auto targetAddr = reinterpret_cast<uintptr_t>(newValue);
        postStoreBarrier(sourceAddr, targetAddr);
    }

    // -------------------------------------------------------------------------
    // Read barrier (compaction)
    // -------------------------------------------------------------------------

    /**
     * @brief Read barrier: resolve forwarding pointer during compaction
     *
     * Only active during incremental compaction phase.
     * Checks if the loaded reference points to a forwarded object
     * and updates it in-place.
     *
     * @param slot The reference slot being read
     * @param forwardingLookup Function to check forwarding table
     */
    void readBarrier(void** slot,
                     std::function<void*(void*)> forwardingLookup) {
        if (!config_.readBarrierEnabled) return;
        if (!slot || !*slot) return;

        stats_.readBarriersFired++;

        void* forwarded = forwardingLookup(*slot);
        if (forwarded && forwarded != *slot) {
            *slot = forwarded;
        }
    }

    // -------------------------------------------------------------------------
    // Barrier phase transitions
    // -------------------------------------------------------------------------

    void enableSATB() { config_.satbBarrierEnabled = true; }
    void disableSATB() { config_.satbBarrierEnabled = false; }
    void enableReadBarrier() { config_.readBarrierEnabled = true; }
    void disableReadBarrier() { config_.readBarrierEnabled = false; }
    void enableGenerational() { config_.generationalBarrierEnabled = true; }
    void disableGenerational() { config_.generationalBarrierEnabled = false; }

    void updateNurseryBounds(uintptr_t start, uintptr_t end) {
        config_.nurseryStart = start;
        config_.nurseryEnd = end;
        // Mark nursery cards as YOUNG so old-gen scan skips them
        cardTable_.markYoungRange(start, end);
    }

    void updateHeapBounds(uintptr_t start, uintptr_t end) {
        config_.heapStart = start;
        config_.heapEnd = end;
    }

    // -------------------------------------------------------------------------
    // Card scanning for minor GC
    // -------------------------------------------------------------------------

    /**
     * @brief Scan all dirty cards for old→young references
     *
     * Iterates every word in each dirty card region. If a word
     * looks like a nursery pointer, calls the visitor.
     *
     * @param isNurseryPtr Check if address is in nursery
     * @param visitor Called for each old→young reference slot found
     * @return Number of dirty cards processed
     */
    size_t scanDirtyCards(
        std::function<bool(uintptr_t)> isNurseryPtr,
        std::function<void(void** slot)> visitor
    ) {
        return cardTable_.forEachDirtyCard(
            [&](uintptr_t cardStart, uintptr_t cardEnd, size_t /*cardIdx*/) {
                // Scan every pointer-aligned word in the card
                for (uintptr_t addr = cardStart; addr < cardEnd;
                     addr += sizeof(void*)) {
                    auto** slot = reinterpret_cast<void**>(addr);
                    auto value = reinterpret_cast<uintptr_t>(*slot);

                    if (isNurseryPtr(value)) {
                        visitor(slot);
                    }
                }
            });
    }

    void resetStats() { stats_ = {}; }

private:
    BarrierConfig config_;
    BarrierCardTable cardTable_;
    GlobalSATBQueue satbQueue_;
    Stats stats_;
};

} // namespace Zepra::Heap

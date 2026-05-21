// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file Scavenger.h
 * @brief Semi-space copying collector for young generation
 *
 * The scavenger implements Cheney's algorithm for the nursery:
 * - Two semi-spaces: from-space and to-space
 * - Allocation via bump pointer in to-space
 * - Collection copies live objects from from-space to to-space
 * - Survivors promoted to old generation after N copies
 *
 * This is the primary young-gen collector. It's fast because:
 * - Most objects die young (generational hypothesis)
 * - Copying is cache-friendly (sequential read, sequential write)
 * - No fragmentation (compaction is implicit)
 */

#pragma once
#include "zepra_alloc.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <functional>
#include <atomic>

namespace Zepra::Heap {

// Forward declarations
class OldGeneration;

// =============================================================================
// Scavenger Configuration
// =============================================================================

struct ScavengerConfig {
    size_t semiSpaceSize = 2 * 1024 * 1024;     // 2MB per semi-space (4MB total)
    size_t maxSemiSpaceSize = 16 * 1024 * 1024;  // 16MB max per semi-space
    uint8_t promotionAge = 2;                     // Promote after surviving N scavenges
    size_t promotionThreshold = 0;                // Promote objects > this size immediately (0=disabled)
    bool adaptiveSizing = true;                   // Grow semi-space if survival rate is high
    double survivalRateThreshold = 0.5;           // Grow if survival rate exceeds this
    double growthFactor = 1.5;                    // Growth multiplier
};

// =============================================================================
// Object Header for Semi-Space
// =============================================================================

struct alignas(8) ScavengerHeader {
    // Word 0: GC metadata
    uint32_t size;             // Object size (excluding header)
    uint8_t age;               // Number of scavenges survived
    uint8_t marked : 1;        // Mark bit for tri-color marking
    uint8_t forwarded : 1;     // Object has been forwarded (moved)
    uint8_t pinned : 1;        // Object cannot be moved
    uint8_t hasFinalizer : 1;
    uint8_t reserved : 4;
    uint16_t typeTag;          // Object type for tracing

    // Word 1: Forwarding address (when forwarded=1) or next pointer
    union {
        void* forwardingAddress;   // Points to copy in to-space
        ScavengerHeader* next;     // Free-list threading (unused in bump allocator)
    };

    void* object() {
        return reinterpret_cast<char*>(this) + sizeof(ScavengerHeader);
    }

    static ScavengerHeader* fromObject(void* obj) {
        return reinterpret_cast<ScavengerHeader*>(
            static_cast<char*>(obj) - sizeof(ScavengerHeader));
    }

    size_t totalSize() const {
        return sizeof(ScavengerHeader) + size;
    }
};

// =============================================================================
// Semi-Space
// =============================================================================

class SemiSpace {
public:
    SemiSpace() = default;

    bool initialize(size_t size);
    void destroy();

    void* allocate(size_t size);
    void reset();

    bool contains(const void* ptr) const;
    size_t used() const { return static_cast<size_t>(current_ - start_); }
    size_t available() const { return static_cast<size_t>(end_ - current_); }
    size_t capacity() const { return static_cast<size_t>(end_ - start_); }
    bool isEmpty() const { return current_ == start_; }

    char* start() const { return start_; }
    char* current() const { return current_; }
    char* end() const { return end_; }

    // Resize (must be empty)
    bool resize(size_t newSize);

private:
    char* start_ = nullptr;
    char* current_ = nullptr;
    char* end_ = nullptr;
};

// =============================================================================
// Scavenger Statistics
// =============================================================================

struct ScavengerStats {
    uint64_t scavengeCount = 0;
    uint64_t totalBytesPromoted = 0;
    uint64_t totalBytesCopied = 0;
    uint64_t totalBytesFreed = 0;
    uint64_t objectsPromoted = 0;
    uint64_t objectsCopied = 0;
    uint64_t objectsFreed = 0;

    // Per-scavenge
    size_t lastScavengeBytesPromoted = 0;
    size_t lastScavengeBytesCopied = 0;
    size_t lastScavengeBytesFreed = 0;
    double lastSurvivalRate = 0.0;
    uint64_t lastScavengeDurationUs = 0;

    // Semi-space sizes
    size_t semiSpaceSize = 0;
    size_t semiSpaceUsed = 0;
};

// =============================================================================
// Root Visitor
// =============================================================================

/**
 * @brief Callback type for root enumeration
 * The visitor receives a pointer-to-pointer. If the pointed-to object
 * is in from-space, the scavenger updates the pointer to the new location.
 */
using RootVisitor = std::function<void(void** slot)>;

/**
 * @brief Callback to enumerate all roots
 * Implementation must call visitor for every root slot.
 */
using RootEnumerator = std::function<void(RootVisitor visitor)>;

// =============================================================================
// Object Tracer
// =============================================================================

/**
 * @brief Traces references within an object
 * Called for each live object. Implementation must call visitor for
 * every reference slot within the object.
 */
using ObjectTracer = std::function<void(void* object, RootVisitor visitor)>;

// =============================================================================
// Scavenger
// =============================================================================

class Scavenger {
public:
    explicit Scavenger(const ScavengerConfig& config = ScavengerConfig{});
    ~Scavenger();

    // Non-copyable
    Scavenger(const Scavenger&) = delete;
    Scavenger& operator=(const Scavenger&) = delete;

    /**
     * @brief Initialize semi-spaces
     * @return true on success
     */
    bool initialize();

    /**
     * @brief Allocate in nursery (bump pointer)
     * @param size Object size in bytes
     * @return Pointer to usable memory, or nullptr if nursery is full
     */
    void* allocate(size_t size);

    /**
     * @brief Run a scavenge (minor GC)
     *
     * 1. Swap from-space and to-space
     * 2. Copy live objects from from-space to to-space
     * 3. Promote old survivors to old generation
     * 4. Update all references
     *
     * @param enumerateRoots Called to enumerate GC roots
     * @param traceObject Called to trace references in each object
     * @param promoteCallback Called when an object is promoted to old gen
     */
    void scavenge(
        RootEnumerator enumerateRoots,
        ObjectTracer traceObject,
        std::function<void*(void* object, size_t size)> promoteCallback
    );

    /**
     * @brief Check if pointer is in nursery
     */
    bool isInNursery(const void* ptr) const;

    /**
     * @brief Check if nursery needs collection
     */
    bool needsScavenge() const;

    /**
     * @brief Force nursery to need collection (for testing)
     */
    void fillNursery();

    /**
     * @brief Current statistics
     */
    const ScavengerStats& stats() const { return stats_; }

    /**
     * @brief Get nursery utilization (0.0 - 1.0)
     */
    double utilization() const;

private:
    /**
     * @brief Copy a single object from from-space to to-space
     * @return Pointer to new location (in to-space or old gen)
     */
    void* copyObject(
        ScavengerHeader* header,
        std::function<void*(void* object, size_t size)>& promoteCallback
    );

    /**
     * @brief Process the Cheney scan queue
     */
    void processScanQueue(
        ObjectTracer& traceObject,
        std::function<void*(void* object, size_t size)>& promoteCallback
    );

    /**
     * @brief Update a reference slot (root or field)
     */
    void updateSlot(
        void** slot,
        std::function<void*(void* object, size_t size)>& promoteCallback
    );

    /**
     * @brief Adapt semi-space size based on survival rate
     */
    void adaptSize();

    ScavengerConfig config_;
    ScavengerStats stats_;

    SemiSpace spaceA_;
    SemiSpace spaceB_;
    SemiSpace* toSpace_;      // Current allocation space
    SemiSpace* fromSpace_;    // Previous allocation space (being scavenged)

    // Cheney scan pointer (within to-space)
    char* scanPtr_ = nullptr;
};

// =============================================================================
// SemiSpace Implementation
// =============================================================================

inline bool SemiSpace::initialize(size_t size) {
    start_ = static_cast<char*>(zepra_aligned_alloc(4096, size));
    if (!start_) return false;
    current_ = start_;
    end_ = start_ + size;
    std::memset(start_, 0, size);
    return true;
}

inline void SemiSpace::destroy() {
    if (start_) {
        std::free(start_);
        start_ = current_ = end_ = nullptr;
    }
}

inline void* SemiSpace::allocate(size_t size) {
    size_t totalSize = sizeof(ScavengerHeader) + size;
    totalSize = (totalSize + 7) & ~7;  // 8-byte align

    if (current_ + totalSize > end_) return nullptr;

    auto* header = new (current_) ScavengerHeader{};
    header->size = static_cast<uint32_t>(size);
    header->age = 0;
    header->marked = 0;
    header->forwarded = 0;
    header->pinned = 0;
    header->hasFinalizer = 0;
    header->forwardingAddress = nullptr;

    current_ += totalSize;
    return header->object();
}

inline void SemiSpace::reset() {
    current_ = start_;
}

inline bool SemiSpace::contains(const void* ptr) const {
    const char* p = static_cast<const char*>(ptr);
    return p >= start_ && p < end_;
}

inline bool SemiSpace::resize(size_t newSize) {
    if (used() > 0) return false;  // Must be empty
    destroy();
    return initialize(newSize);
}

// =============================================================================
// Scavenger Implementation
// =============================================================================

inline Scavenger::Scavenger(const ScavengerConfig& config)
    : config_(config)
    , toSpace_(&spaceA_)
    , fromSpace_(&spaceB_) {}

inline Scavenger::~Scavenger() {
    spaceA_.destroy();
    spaceB_.destroy();
}

inline bool Scavenger::initialize() {
    if (!spaceA_.initialize(config_.semiSpaceSize)) return false;
    if (!spaceB_.initialize(config_.semiSpaceSize)) return false;
    stats_.semiSpaceSize = config_.semiSpaceSize;
    return true;
}

inline void* Scavenger::allocate(size_t size) {
    return toSpace_->allocate(size);
}

inline bool Scavenger::isInNursery(const void* ptr) const {
    return toSpace_->contains(ptr) || fromSpace_->contains(ptr);
}

inline bool Scavenger::needsScavenge() const {
    double usage = static_cast<double>(toSpace_->used()) /
                   static_cast<double>(toSpace_->capacity());
    return usage > 0.75;
}

inline double Scavenger::utilization() const {
    if (toSpace_->capacity() == 0) return 0.0;
    return static_cast<double>(toSpace_->used()) /
           static_cast<double>(toSpace_->capacity());
}

inline void Scavenger::scavenge(
    RootEnumerator enumerateRoots,
    ObjectTracer traceObject,
    std::function<void*(void* object, size_t size)> promoteCallback
) {
    stats_.scavengeCount++;

    size_t beforeUsed = toSpace_->used();

    // 1. Swap spaces
    std::swap(toSpace_, fromSpace_);
    toSpace_->reset();
    scanPtr_ = toSpace_->start();

    // 2. Process roots — copy referenced objects to to-space
    enumerateRoots([this, &promoteCallback](void** slot) {
        updateSlot(slot, promoteCallback);
    });

    // 3. Cheney scan — breadth-first copy
    processScanQueue(traceObject, promoteCallback);

    // 4. Calculate stats
    size_t afterUsed = toSpace_->used();
    double survivalRate = beforeUsed > 0
        ? static_cast<double>(afterUsed) / static_cast<double>(beforeUsed)
        : 0.0;

    stats_.lastSurvivalRate = survivalRate;
    stats_.lastScavengeBytesCopied = afterUsed;
    stats_.lastScavengeBytesFreed = beforeUsed > afterUsed ? beforeUsed - afterUsed : 0;
    stats_.totalBytesCopied += afterUsed;
    stats_.totalBytesFreed += stats_.lastScavengeBytesFreed;
    stats_.semiSpaceUsed = afterUsed;

    // 5. Adaptive resizing
    if (config_.adaptiveSizing) {
        adaptSize();
    }
}

inline void Scavenger::updateSlot(
    void** slot,
    std::function<void*(void* object, size_t size)>& promoteCallback
) {
    void* obj = *slot;
    if (!obj || !fromSpace_->contains(obj)) return;

    ScavengerHeader* header = ScavengerHeader::fromObject(obj);

    if (header->forwarded) {
        // Already copied — update pointer
        *slot = header->forwardingAddress;
    } else {
        // Copy to new location
        void* newLocation = copyObject(header, promoteCallback);
        *slot = newLocation;
    }
}

inline void* Scavenger::copyObject(
    ScavengerHeader* header,
    std::function<void*(void* object, size_t size)>& promoteCallback
) {
    size_t objSize = header->size;

    // Check promotion criteria
    bool shouldPromote =
        header->age >= config_.promotionAge ||
        (config_.promotionThreshold > 0 && objSize > config_.promotionThreshold);

    void* newLocation;

    if (shouldPromote && promoteCallback) {
        // Promote to old generation
        newLocation = promoteCallback(header->object(), objSize);
        if (newLocation) {
            stats_.objectsPromoted++;
            stats_.totalBytesPromoted += objSize;
            stats_.lastScavengeBytesPromoted += objSize;
        } else {
            // Promotion failed (old gen full) — copy to to-space instead
            newLocation = toSpace_->allocate(objSize);
            if (!newLocation) return nullptr;  // Out of nursery space
            std::memcpy(newLocation, header->object(), objSize);
            auto* newHeader = ScavengerHeader::fromObject(newLocation);
            newHeader->age = header->age + 1;
        }
    } else {
        // Copy to to-space
        newLocation = toSpace_->allocate(objSize);
        if (!newLocation) return nullptr;
        std::memcpy(newLocation, header->object(), objSize);
        auto* newHeader = ScavengerHeader::fromObject(newLocation);
        newHeader->age = header->age + 1;
        stats_.objectsCopied++;
    }

    // Set forwarding pointer in old location
    header->forwarded = 1;
    header->forwardingAddress = newLocation;

    return newLocation;
}

inline void Scavenger::processScanQueue(
    ObjectTracer& traceObject,
    std::function<void*(void* object, size_t size)>& promoteCallback
) {
    // Cheney's algorithm: scan pointer chases allocation pointer
    while (scanPtr_ < toSpace_->current()) {
        auto* header = reinterpret_cast<ScavengerHeader*>(scanPtr_);
        void* obj = header->object();

        // Trace this object's fields
        traceObject(obj, [this, &promoteCallback](void** slot) {
            updateSlot(slot, promoteCallback);
        });

        // Advance scan pointer
        size_t totalSize = (header->totalSize() + 7) & ~7;
        scanPtr_ += totalSize;
    }
}

inline void Scavenger::adaptSize() {
    if (stats_.lastSurvivalRate > config_.survivalRateThreshold) {
        size_t newSize = static_cast<size_t>(
            static_cast<double>(toSpace_->capacity()) * config_.growthFactor);
        if (newSize <= config_.maxSemiSpaceSize && newSize > toSpace_->capacity()) {
            // Will take effect on next scavenge (after swap)
            fromSpace_->resize(newSize);
            stats_.semiSpaceSize = newSize;
        }
    }
}

inline void Scavenger::fillNursery() {
    while (toSpace_->allocate(64)) {}  // Fill with dummy allocations
}

} // namespace Zepra::Heap

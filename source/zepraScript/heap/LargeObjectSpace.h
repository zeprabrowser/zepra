// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file LargeObjectSpace.h
 * @brief Large Object Space for ZepraScript GC
 *
 * Objects larger than the LOS threshold (8KB default) are allocated
 * directly from the OS and tracked in a separate free-list.
 * Large objects are never moved — they're pinned and swept in-place.
 *
 * Design:
 * - mmap-backed allocation (page-aligned)
 * - Intrusive doubly-linked list for iteration
 * - Never compacted — avoids copying cost for large arrays/buffers
 * - Individual object free on sweep (no bump-pointer)
 */

#pragma once
#include "zepra_alloc.h"

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>

#ifdef __linux__
#include <sys/mman.h>
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#endif

namespace Zepra::Heap {

// =============================================================================
// Configuration
// =============================================================================

struct LOSConfig {
    size_t threshold = 8 * 1024;                    // 8KB — objects >= this go to LOS
    size_t maxTotalSize = 256 * 1024 * 1024;        // 256MB max LOS
    size_t pageSize = 4096;                         // OS page size
    bool useGuardPages = true;                      // Guard page after each allocation
    bool zeroOnFree = true;                         // Zero memory on free (security)
};

// =============================================================================
// LOS Object Header
// =============================================================================

/**
 * @brief Header prepended to every large object
 *
 * Layout: [LOSHeader][padding][object data][guard page]
 */
struct alignas(16) LOSHeader {
    // GC metadata
    uint32_t marked : 1;
    uint32_t pinned : 1;
    uint32_t hasFinalizer : 1;
    uint32_t reserved : 29;

    // Size tracking
    size_t objectSize;          // Requested size
    size_t mappedSize;          // Total mmap'd size (header + padding + object + guard)

    // Intrusive list
    LOSHeader* prev;
    LOSHeader* next;

    // Type info for tracing
    uint32_t typeTag;
    uint32_t gcAge;

    // Allocation metadata
    uint64_t allocTimestamp;    // Monotonic clock at allocation

    // Get pointer to object data
    void* object() {
        return reinterpret_cast<char*>(this) + alignedHeaderSize();
    }

    const void* object() const {
        return reinterpret_cast<const char*>(this) + alignedHeaderSize();
    }

    static constexpr size_t alignedHeaderSize() {
        return (sizeof(LOSHeader) + 15) & ~15;  // 16-byte aligned
    }

    static LOSHeader* fromObject(void* obj) {
        return reinterpret_cast<LOSHeader*>(
            reinterpret_cast<char*>(obj) - alignedHeaderSize());
    }
};

// =============================================================================
// LOS Statistics
// =============================================================================

struct LOSStats {
    size_t totalAllocated = 0;
    size_t totalFreed = 0;
    size_t currentObjects = 0;
    size_t currentBytes = 0;
    size_t peakBytes = 0;
    size_t peakObjects = 0;
    size_t allocationCount = 0;
    size_t freeCount = 0;
    size_t failedAllocations = 0;
    size_t oomCount = 0;

    // Fragmentation metric (0.0 = no fragmentation, 1.0 = severe)
    double fragmentation() const {
        if (totalAllocated == 0) return 0.0;
        return 1.0 - static_cast<double>(currentBytes) /
               static_cast<double>(totalAllocated - totalFreed);
    }
};

// =============================================================================
// Large Object Space
// =============================================================================

class LargeObjectSpace {
public:
    using FinalizerFn = std::function<void(void* object)>;

    explicit LargeObjectSpace(const LOSConfig& config = LOSConfig{});
    ~LargeObjectSpace();

    // Non-copyable, non-movable
    LargeObjectSpace(const LargeObjectSpace&) = delete;
    LargeObjectSpace& operator=(const LargeObjectSpace&) = delete;

    // -------------------------------------------------------------------------
    // Allocation
    // -------------------------------------------------------------------------

    /**
     * @brief Allocate a large object
     * @param size Requested size in bytes
     * @return Pointer to usable memory, or nullptr on failure
     */
    void* allocate(size_t size);

    /**
     * @brief Allocate with zero-initialization
     */
    void* allocateZeroed(size_t size);

    /**
     * @brief Free a specific large object
     */
    void free(void* object);

    // -------------------------------------------------------------------------
    // GC Integration
    // -------------------------------------------------------------------------

    /**
     * @brief Mark an object as reachable
     */
    void mark(void* object);

    /**
     * @brief Check if object is marked
     */
    bool isMarked(void* object) const;

    /**
     * @brief Sweep unmarked objects
     * @return Number of bytes freed
     */
    size_t sweep();

    /**
     * @brief Clear all mark bits (pre-mark phase)
     */
    void clearMarks();

    /**
     * @brief Register finalizer for object
     */
    void setFinalizer(void* object, FinalizerFn finalizer);

    // -------------------------------------------------------------------------
    // Iteration
    // -------------------------------------------------------------------------

    /**
     * @brief Iterate all live objects
     */
    using IteratorFn = std::function<void(void* object, size_t size)>;
    void forEach(IteratorFn fn);

    /**
     * @brief Iterate only marked objects
     */
    void forEachMarked(IteratorFn fn);

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    /**
     * @brief Check if a pointer belongs to LOS
     */
    bool contains(const void* ptr) const;

    /**
     * @brief Check if size qualifies for LOS
     */
    bool shouldAllocateInLOS(size_t size) const {
        return size >= config_.threshold;
    }

    /**
     * @brief Current statistics
     */
    const LOSStats& stats() const { return stats_; }

    /**
     * @brief Check if LOS is at capacity
     */
    bool isAtCapacity() const {
        return stats_.currentBytes >= config_.maxTotalSize;
    }

    /**
     * @brief Available space
     */
    size_t availableBytes() const {
        return config_.maxTotalSize > stats_.currentBytes
            ? config_.maxTotalSize - stats_.currentBytes : 0;
    }

private:
    // Platform allocation
    void* platformAllocate(size_t size);
    void platformFree(void* ptr, size_t size);

    // List management
    void addToList(LOSHeader* header);
    void removeFromList(LOSHeader* header);

    // Finalization
    void runFinalizer(LOSHeader* header);

    LOSConfig config_;
    LOSStats stats_;

    // Intrusive list of all LOS objects
    LOSHeader* head_ = nullptr;
    LOSHeader* tail_ = nullptr;

    // Finalizer registry
    struct FinalizerEntry {
        void* object;
        FinalizerFn fn;
    };
    std::vector<FinalizerEntry> finalizers_;

    // Thread safety
    mutable std::mutex mutex_;
};

// =============================================================================
// Implementation
// =============================================================================

inline LargeObjectSpace::LargeObjectSpace(const LOSConfig& config)
    : config_(config) {
#ifdef __linux__
    config_.pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

inline LargeObjectSpace::~LargeObjectSpace() {
    // Free all remaining objects
    LOSHeader* current = head_;
    while (current) {
        LOSHeader* next = current->next;
        runFinalizer(current);
        platformFree(current, current->mappedSize);
        current = next;
    }
}

inline void* LargeObjectSpace::allocate(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (stats_.currentBytes + size > config_.maxTotalSize) {
        stats_.failedAllocations++;
        stats_.oomCount++;
        return nullptr;
    }

    // Calculate total mapped size
    size_t headerSize = LOSHeader::alignedHeaderSize();
    size_t totalNeeded = headerSize + size;

    // Align to page boundary
    size_t mappedSize = (totalNeeded + config_.pageSize - 1) & ~(config_.pageSize - 1);

    // Add guard page if enabled
    if (config_.useGuardPages) {
        mappedSize += config_.pageSize;
    }

    void* mem = platformAllocate(mappedSize);
    if (!mem) {
        stats_.failedAllocations++;
        return nullptr;
    }

    // Initialize header
    auto* header = new (mem) LOSHeader{};
    header->objectSize = size;
    header->mappedSize = mappedSize;
    header->marked = 0;
    header->pinned = 0;
    header->hasFinalizer = 0;
    header->gcAge = 0;
    header->typeTag = 0;

    // Set guard page (PROT_NONE)
    if (config_.useGuardPages) {
#ifdef __linux__
        void* guardPage = reinterpret_cast<char*>(mem) + mappedSize - config_.pageSize;
        mprotect(guardPage, config_.pageSize, PROT_NONE);
#endif
    }

    addToList(header);

    stats_.allocationCount++;
    stats_.totalAllocated += size;
    stats_.currentBytes += size;
    stats_.currentObjects++;
    if (stats_.currentBytes > stats_.peakBytes) stats_.peakBytes = stats_.currentBytes;
    if (stats_.currentObjects > stats_.peakObjects) stats_.peakObjects = stats_.currentObjects;

    return header->object();
}

inline void* LargeObjectSpace::allocateZeroed(size_t size) {
    void* ptr = allocate(size);
    if (ptr) {
        std::memset(ptr, 0, size);
    }
    return ptr;
}

inline void LargeObjectSpace::free(void* object) {
    if (!object) return;
    std::lock_guard<std::mutex> lock(mutex_);

    LOSHeader* header = LOSHeader::fromObject(object);

    runFinalizer(header);

    if (config_.zeroOnFree) {
        std::memset(object, 0, header->objectSize);
    }

    stats_.totalFreed += header->objectSize;
    stats_.currentBytes -= header->objectSize;
    stats_.currentObjects--;
    stats_.freeCount++;

    removeFromList(header);
    platformFree(header, header->mappedSize);
}

inline void LargeObjectSpace::mark(void* object) {
    if (!object) return;
    LOSHeader* header = LOSHeader::fromObject(object);
    header->marked = 1;
}

inline bool LargeObjectSpace::isMarked(void* object) const {
    if (!object) return false;
    const LOSHeader* header = LOSHeader::fromObject(const_cast<void*>(object));
    return header->marked != 0;
}

inline size_t LargeObjectSpace::sweep() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t freedBytes = 0;

    LOSHeader* current = head_;
    while (current) {
        LOSHeader* next = current->next;

        if (!current->marked && !current->pinned) {
            // Unreachable — free it
            void* obj = current->object();
            runFinalizer(current);

            size_t objSize = current->objectSize;
            freedBytes += objSize;

            if (config_.zeroOnFree) {
                std::memset(obj, 0, objSize);
            }

            stats_.totalFreed += objSize;
            stats_.currentBytes -= objSize;
            stats_.currentObjects--;
            stats_.freeCount++;

            removeFromList(current);
            platformFree(current, current->mappedSize);
        } else {
            current->marked = 0;  // Reset for next cycle
            current->gcAge++;
        }

        current = next;
    }

    return freedBytes;
}

inline void LargeObjectSpace::clearMarks() {
    LOSHeader* current = head_;
    while (current) {
        current->marked = 0;
        current = current->next;
    }
}

inline void LargeObjectSpace::setFinalizer(void* object, FinalizerFn finalizer) {
    if (!object) return;
    LOSHeader* header = LOSHeader::fromObject(object);
    header->hasFinalizer = 1;
    finalizers_.push_back({object, std::move(finalizer)});
}

inline void LargeObjectSpace::forEach(IteratorFn fn) {
    LOSHeader* current = head_;
    while (current) {
        fn(current->object(), current->objectSize);
        current = current->next;
    }
}

inline void LargeObjectSpace::forEachMarked(IteratorFn fn) {
    LOSHeader* current = head_;
    while (current) {
        if (current->marked) {
            fn(current->object(), current->objectSize);
        }
        current = current->next;
    }
}

inline bool LargeObjectSpace::contains(const void* ptr) const {
    const LOSHeader* current = head_;
    while (current) {
        const char* objStart = static_cast<const char*>(current->object());
        const char* objEnd = objStart + current->objectSize;
        if (ptr >= objStart && ptr < objEnd) return true;
        current = current->next;
    }
    return false;
}

inline void LargeObjectSpace::addToList(LOSHeader* header) {
    header->prev = tail_;
    header->next = nullptr;
    if (tail_) tail_->next = header;
    else head_ = header;
    tail_ = header;
}

inline void LargeObjectSpace::removeFromList(LOSHeader* header) {
    if (header->prev) header->prev->next = header->next;
    else head_ = header->next;
    if (header->next) header->next->prev = header->prev;
    else tail_ = header->prev;
    header->prev = nullptr;
    header->next = nullptr;
}

inline void LargeObjectSpace::runFinalizer(LOSHeader* header) {
    if (!header->hasFinalizer) return;
    void* obj = header->object();
    for (auto it = finalizers_.begin(); it != finalizers_.end(); ++it) {
        if (it->object == obj) {
            try { it->fn(obj); } catch (...) {}
            finalizers_.erase(it);
            break;
        }
    }
}

inline void* LargeObjectSpace::platformAllocate(size_t size) {
#ifdef __linux__
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? nullptr : ptr;
#else
    return zepra_aligned_alloc(config_.pageSize, size);
#endif
}

inline void LargeObjectSpace::platformFree(void* ptr, size_t size) {
#ifdef __linux__
    munmap(ptr, size);
#else
    std::free(ptr);
    (void)size;
#endif
}

} // namespace Zepra::Heap

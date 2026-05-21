// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file SizeClassAllocator.h
 * @brief Segregated free-list allocator by size class
 *
 * Objects are allocated from size-class-specific free lists.
 * Each size class maintains its own:
 * - Free list (singly-linked)
 * - Page list (pages dedicated to this size class)
 * - Allocation stats
 *
 * Size classes follow a geometric progression:
 * 8, 16, 24, 32, 48, 64, 80, 96, 128, 160, 192, 256,
 * 320, 384, 512, 640, 768, 1024, 1280, 1536, 2048, ...
 *
 * This dramatically reduces internal fragmentation compared
 * to a single free-list or bump allocator.
 *
 * Thread safety: Each thread uses a ThreadLocalAllocBuffer (TLAB)
 * which pulls batches of objects from the global per-class free list.
 */

#pragma once
#include "zepra_alloc.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>
#include <vector>
#include <mutex>
#include <atomic>
#include <algorithm>

namespace Zepra::Heap {

// =============================================================================
// Size Class Table
// =============================================================================

/**
 * @brief Maps allocation size → size class index
 *
 * Geometric progression with 1.25x growth factor.
 * 64 size classes covering 8 bytes to 32KB.
 */
class SizeClassTable {
public:
    static constexpr size_t NUM_CLASSES = 64;
    static constexpr size_t MIN_SIZE = 8;
    static constexpr size_t MAX_SIZE = 32 * 1024;  // 32KB, larger goes to LOS
    static constexpr size_t ALIGNMENT = 8;

    struct SizeClass {
        size_t size;            // Object size (rounded up)
        size_t objectsPerPage;  // How many objects fit in a page
        size_t pageSize;        // Page size for this class (may be larger for big items)
        size_t transferBatch;   // TLAB refill batch size
        uint16_t index;
    };

    SizeClassTable() { initialize(); }

    const SizeClass& forSize(size_t size) const {
        if (size <= MIN_SIZE) return classes_[0];
        if (size > MAX_SIZE) return classes_[NUM_CLASSES - 1];

        // Fast lookup: use the size-to-class map
        size_t lookupIdx = (size + ALIGNMENT - 1) / ALIGNMENT;
        if (lookupIdx < lookupTableSize_) {
            return classes_[lookupTable_[lookupIdx]];
        }

        // Fallback: binary search
        for (size_t i = 0; i < classCount_; i++) {
            if (classes_[i].size >= size) return classes_[i];
        }
        return classes_[classCount_ - 1];
    }

    uint16_t classIndex(size_t size) const {
        return forSize(size).index;
    }

    size_t classSize(uint16_t index) const {
        return classes_[index].size;
    }

    size_t classCount() const { return classCount_; }

    const SizeClass& classAt(size_t index) const { return classes_[index]; }

private:
    void initialize() {
        classCount_ = 0;

        // Small sizes: 8-byte increments up to 128
        for (size_t s = MIN_SIZE; s <= 128; s += 8) {
            addClass(s);
        }

        // Medium sizes: 32-byte increments up to 512
        for (size_t s = 160; s <= 512; s += 32) {
            addClass(s);
        }

        // Large sizes: 128-byte increments up to 2048
        for (size_t s = 640; s <= 2048; s += 128) {
            addClass(s);
        }

        // Bigger: 256-byte increments up to 4096
        for (size_t s = 2304; s <= 4096; s += 256) {
            addClass(s);
        }

        // Huge: 1024-byte increments up to MAX_SIZE
        for (size_t s = 5120; s <= MAX_SIZE; s += 1024) {
            addClass(s);
        }

        // Build lookup table for fast size→class mapping
        buildLookupTable();
    }

    void addClass(size_t size) {
        if (classCount_ >= NUM_CLASSES) return;

        auto& c = classes_[classCount_];
        c.size = (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        c.objectsPerPage = 4096 / c.size;
        if (c.objectsPerPage == 0) c.objectsPerPage = 1;
        c.pageSize = c.objectsPerPage * c.size;
        // Larger batch for smaller objects (amortize lock cost)
        c.transferBatch = std::min(size_t(64), std::max(size_t(4), 4096 / c.size));
        c.index = static_cast<uint16_t>(classCount_);
        classCount_++;
    }

    void buildLookupTable() {
        if (classCount_ == 0) return;
        size_t maxLookup = classes_[classCount_ - 1].size / ALIGNMENT + 1;
        maxLookup = std::min(maxLookup, size_t(4096));
        lookupTableSize_ = maxLookup;
        lookupTable_.resize(lookupTableSize_, 0);

        size_t ci = 0;
        for (size_t i = 0; i < lookupTableSize_; i++) {
            size_t reqSize = i * ALIGNMENT;
            while (ci + 1 < classCount_ && classes_[ci].size < reqSize) ci++;
            lookupTable_[i] = static_cast<uint16_t>(ci);
        }
    }

    std::array<SizeClass, NUM_CLASSES> classes_{};
    size_t classCount_ = 0;
    std::vector<uint16_t> lookupTable_;
    size_t lookupTableSize_ = 0;
};

// =============================================================================
// Free List Node
// =============================================================================

struct FreeListNode {
    FreeListNode* next;
};

// =============================================================================
// Per-Class Slab
// =============================================================================

/**
 * @brief A page (or group of pages) dedicated to one size class
 *
 * Contains a bump region and a free list of returned objects.
 */
struct ClassSlab {
    char* pageStart;
    char* bumpPointer;
    char* pageEnd;
    FreeListNode* freeList;
    size_t objectSize;
    uint32_t allocatedCount;
    uint32_t freeCount;
    uint32_t liveCount;        // After marking
    ClassSlab* next;           // Linked list of slabs

    void* allocate() {
        // Try free list first
        if (freeList) {
            void* obj = freeList;
            freeList = freeList->next;
            freeCount--;
            allocatedCount++;
            return obj;
        }

        // Bump allocate
        if (bumpPointer + objectSize <= pageEnd) {
            void* obj = bumpPointer;
            bumpPointer += objectSize;
            allocatedCount++;
            return obj;
        }

        return nullptr;  // Slab full
    }

    void deallocate(void* ptr) {
        auto* node = static_cast<FreeListNode*>(ptr);
        node->next = freeList;
        freeList = node;
        freeCount++;
        allocatedCount--;
    }

    bool isEmpty() const { return allocatedCount == 0; }
    bool isFull() const {
        return !freeList && bumpPointer + objectSize > pageEnd;
    }

    size_t capacity() const {
        return static_cast<size_t>(pageEnd - pageStart) / objectSize;
    }

    double utilization() const {
        size_t cap = capacity();
        return cap > 0 ? static_cast<double>(allocatedCount) /
                         static_cast<double>(cap) : 0.0;
    }
};

// =============================================================================
// SizeClassAllocator Statistics
// =============================================================================

struct SizeClassAllocatorStats {
    size_t totalAllocations = 0;
    size_t totalDeallocations = 0;
    size_t totalBytesAllocated = 0;
    size_t totalBytesDeallocated = 0;
    size_t currentObjectCount = 0;
    size_t currentBytesUsed = 0;
    size_t slabCount = 0;
    size_t emptySlabCount = 0;
    size_t fullSlabCount = 0;
    double averageUtilization = 0;

    struct PerClassStats {
        size_t classSize;
        size_t allocations;
        size_t deallocations;
        size_t currentObjects;
        size_t slabs;
    };
    std::vector<PerClassStats> perClass;
};

// =============================================================================
// SizeClassAllocator
// =============================================================================

class SizeClassAllocator {
public:
    SizeClassAllocator();
    ~SizeClassAllocator();

    SizeClassAllocator(const SizeClassAllocator&) = delete;
    SizeClassAllocator& operator=(const SizeClassAllocator&) = delete;

    /**
     * @brief Allocate an object of given size
     * Round up to nearest size class.
     */
    void* allocate(size_t size);

    /**
     * @brief Return an object to its size class free list
     */
    void deallocate(void* ptr, size_t size);

    /**
     * @brief Bulk allocate: fill a transfer batch
     * Used by TLAB to refill its local cache.
     * @return Number of objects allocated
     */
    size_t allocateBatch(size_t classIndex, void** out, size_t count);

    /**
     * @brief Bulk return: add batch back to central list
     */
    void deallocateBatch(size_t classIndex, void** objects, size_t count);

    /**
     * @brief Release empty slabs back to OS
     */
    void releaseEmptySlabs();

    /**
     * @brief After sweep: rebuild free lists from mark bits
     * @param isMarked Check if object at address is marked
     */
    void sweepAndRebuild(std::function<bool(void*)> isMarked);

    /**
     * @brief Check if ptr belongs to this allocator
     */
    bool contains(const void* ptr) const;

    /**
     * @brief Get object size for a pointer
     * Assumes ptr is a valid allocated object.
     */
    size_t objectSizeFor(const void* ptr) const;

    /**
     * @brief Statistics
     */
    SizeClassAllocatorStats computeStats() const;

    /**
     * @brief Iterate all live objects in a class
     */
    void forEachObject(size_t classIndex,
                       std::function<void(void* object)> callback) const;

    /**
     * @brief The size class table
     */
    const SizeClassTable& sizeClasses() const { return table_; }

private:
    ClassSlab* allocateNewSlab(size_t classIndex);
    void freeSlab(ClassSlab* slab);

    SizeClassTable table_;

    // Per-class state
    struct PerClassData {
        ClassSlab* currentSlab = nullptr;   // Slab being allocated from
        ClassSlab* slabList = nullptr;       // All slabs for this class
        size_t slabCount = 0;
        size_t totalAllocations = 0;
        size_t totalDeallocations = 0;
        std::mutex lock;
    };

    std::array<PerClassData, SizeClassTable::NUM_CLASSES> classes_{};
};

// =============================================================================
// Thread-Local Allocation Buffer (TLAB)
// =============================================================================

/**
 * @brief Per-thread allocation cache
 *
 * Each thread has one TLAB. The TLAB maintains small local free lists
 * per size class. Allocation is lock-free while the local cache has
 * objects. When the local cache is empty, it refills from the central
 * allocator in batch (amortized lock cost).
 *
 * This is the primary allocation fast-path — most allocations
 * don't touch any locks.
 */
class ThreadLocalAllocBuffer {
public:
    static constexpr size_t MAX_CACHED_CLASSES = 32;
    static constexpr size_t MAX_CACHE_SIZE = 64;  // Objects per class

    explicit ThreadLocalAllocBuffer(SizeClassAllocator& central);
    ~ThreadLocalAllocBuffer();

    ThreadLocalAllocBuffer(const ThreadLocalAllocBuffer&) = delete;
    ThreadLocalAllocBuffer& operator=(const ThreadLocalAllocBuffer&) = delete;

    /**
     * @brief Allocate (fast path — no locks if cache has objects)
     */
    void* allocate(size_t size);

    /**
     * @brief Deallocate (fast path — returns to local cache)
     */
    void deallocate(void* ptr, size_t size);

    /**
     * @brief Flush all cached objects back to central allocator
     * Called before GC or thread destruction.
     */
    void flushAll();

    /**
     * @brief Statistics
     */
    struct TLABStats {
        uint64_t localAllocations = 0;
        uint64_t localDeallocations = 0;
        uint64_t refills = 0;
        uint64_t flushes = 0;
        double hitRate = 0;
    };

    const TLABStats& stats() const { return stats_; }

    /**
     * @brief Get or create TLAB for current thread
     */
    static ThreadLocalAllocBuffer* current();

private:
    void refillCache(size_t classIndex);
    void flushCache(size_t classIndex);

    struct LocalCache {
        FreeListNode* freeList = nullptr;
        size_t count = 0;
    };

    SizeClassAllocator& central_;
    std::array<LocalCache, MAX_CACHED_CLASSES> caches_{};
    TLABStats stats_;
};

// =============================================================================
// Implementation — SizeClassAllocator
// =============================================================================

inline SizeClassAllocator::SizeClassAllocator() = default;

inline SizeClassAllocator::~SizeClassAllocator() {
    for (auto& pcd : classes_) {
        ClassSlab* slab = pcd.slabList;
        while (slab) {
            ClassSlab* next = slab->next;
            freeSlab(slab);
            slab = next;
        }
    }
}

inline void* SizeClassAllocator::allocate(size_t size) {
    auto& sc = table_.forSize(size);
    auto& pcd = classes_[sc.index];

    std::lock_guard<std::mutex> lock(pcd.lock);

    // Try current slab
    if (pcd.currentSlab) {
        void* obj = pcd.currentSlab->allocate();
        if (obj) {
            pcd.totalAllocations++;
            return obj;
        }
    }

    // Try other non-full slabs
    ClassSlab* slab = pcd.slabList;
    while (slab) {
        if (!slab->isFull()) {
            void* obj = slab->allocate();
            if (obj) {
                pcd.currentSlab = slab;
                pcd.totalAllocations++;
                return obj;
            }
        }
        slab = slab->next;
    }

    // Allocate new slab
    ClassSlab* newSlab = allocateNewSlab(sc.index);
    if (!newSlab) return nullptr;

    void* obj = newSlab->allocate();
    pcd.totalAllocations++;
    return obj;
}

inline void SizeClassAllocator::deallocate(void* ptr, size_t size) {
    auto& sc = table_.forSize(size);
    auto& pcd = classes_[sc.index];

    std::lock_guard<std::mutex> lock(pcd.lock);

    // Find which slab contains this pointer
    ClassSlab* slab = pcd.slabList;
    while (slab) {
        if (ptr >= slab->pageStart && ptr < slab->pageEnd) {
            slab->deallocate(ptr);
            pcd.totalDeallocations++;
            return;
        }
        slab = slab->next;
    }
}

inline size_t SizeClassAllocator::allocateBatch(size_t classIndex,
                                                  void** out, size_t count) {
    if (classIndex >= table_.classCount()) return 0;
    auto& pcd = classes_[classIndex];

    std::lock_guard<std::mutex> lock(pcd.lock);

    size_t allocated = 0;
    while (allocated < count) {
        // Try current slab
        if (pcd.currentSlab) {
            void* obj = pcd.currentSlab->allocate();
            if (obj) {
                out[allocated++] = obj;
                continue;
            }
        }

        // Allocate new slab
        ClassSlab* newSlab = allocateNewSlab(classIndex);
        if (!newSlab) break;
        pcd.currentSlab = newSlab;
    }

    pcd.totalAllocations += allocated;
    return allocated;
}

inline void SizeClassAllocator::deallocateBatch(size_t classIndex,
                                                  void** objects, size_t count) {
    if (classIndex >= table_.classCount()) return;
    auto& pcd = classes_[classIndex];

    std::lock_guard<std::mutex> lock(pcd.lock);

    for (size_t i = 0; i < count; i++) {
        if (!objects[i]) continue;
        ClassSlab* slab = pcd.slabList;
        while (slab) {
            if (objects[i] >= slab->pageStart && objects[i] < slab->pageEnd) {
                slab->deallocate(objects[i]);
                pcd.totalDeallocations++;
                break;
            }
            slab = slab->next;
        }
    }
}

inline ClassSlab* SizeClassAllocator::allocateNewSlab(size_t classIndex) {
    auto& sc = table_.classAt(classIndex);
    auto& pcd = classes_[classIndex];

    // Allocate page for slab
    size_t slabSize = std::max(sc.pageSize, size_t(4096));
    char* page = static_cast<char*>(zepra_aligned_alloc(4096, slabSize));
    if (!page) return nullptr;
    std::memset(page, 0, slabSize);

    auto* slab = new ClassSlab();
    slab->pageStart = page;
    slab->bumpPointer = page;
    slab->pageEnd = page + slabSize;
    slab->freeList = nullptr;
    slab->objectSize = sc.size;
    slab->allocatedCount = 0;
    slab->freeCount = 0;
    slab->liveCount = 0;
    slab->next = pcd.slabList;
    pcd.slabList = slab;
    pcd.currentSlab = slab;
    pcd.slabCount++;

    return slab;
}

inline void SizeClassAllocator::freeSlab(ClassSlab* slab) {
    if (slab->pageStart) std::free(slab->pageStart);
    delete slab;
}

inline void SizeClassAllocator::releaseEmptySlabs() {
    for (size_t ci = 0; ci < table_.classCount(); ci++) {
        auto& pcd = classes_[ci];
        std::lock_guard<std::mutex> lock(pcd.lock);

        ClassSlab** prev = &pcd.slabList;
        ClassSlab* slab = pcd.slabList;
        while (slab) {
            ClassSlab* next = slab->next;
            if (slab->isEmpty() && slab != pcd.currentSlab) {
                *prev = next;
                freeSlab(slab);
                pcd.slabCount--;
            } else {
                prev = &slab->next;
            }
            slab = next;
        }
    }
}

inline void SizeClassAllocator::sweepAndRebuild(
    std::function<bool(void*)> isMarked
) {
    for (size_t ci = 0; ci < table_.classCount(); ci++) {
        auto& pcd = classes_[ci];
        std::lock_guard<std::mutex> lock(pcd.lock);

        ClassSlab* slab = pcd.slabList;
        while (slab) {
            slab->freeList = nullptr;
            slab->freeCount = 0;
            slab->allocatedCount = 0;
            slab->liveCount = 0;

            char* ptr = slab->pageStart;
            while (ptr + slab->objectSize <= slab->bumpPointer) {
                if (isMarked(ptr)) {
                    slab->liveCount++;
                    slab->allocatedCount++;
                } else {
                    // Dead — add to free list
                    auto* node = reinterpret_cast<FreeListNode*>(ptr);
                    node->next = slab->freeList;
                    slab->freeList = node;
                    slab->freeCount++;
                }
                ptr += slab->objectSize;
            }

            slab = slab->next;
        }
    }
}

inline bool SizeClassAllocator::contains(const void* ptr) const {
    for (size_t ci = 0; ci < table_.classCount(); ci++) {
        ClassSlab* slab = classes_[ci].slabList;
        while (slab) {
            if (ptr >= slab->pageStart && ptr < slab->pageEnd) return true;
            slab = slab->next;
        }
    }
    return false;
}

inline size_t SizeClassAllocator::objectSizeFor(const void* ptr) const {
    for (size_t ci = 0; ci < table_.classCount(); ci++) {
        ClassSlab* slab = classes_[ci].slabList;
        while (slab) {
            if (ptr >= slab->pageStart && ptr < slab->pageEnd) {
                return slab->objectSize;
            }
            slab = slab->next;
        }
    }
    return 0;
}

inline SizeClassAllocatorStats SizeClassAllocator::computeStats() const {
    SizeClassAllocatorStats stats;
    for (size_t ci = 0; ci < table_.classCount(); ci++) {
        const auto& pcd = classes_[ci];

        SizeClassAllocatorStats::PerClassStats pcs;
        pcs.classSize = table_.classAt(ci).size;
        pcs.allocations = pcd.totalAllocations;
        pcs.deallocations = pcd.totalDeallocations;
        pcs.currentObjects = pcd.totalAllocations - pcd.totalDeallocations;
        pcs.slabs = pcd.slabCount;

        stats.totalAllocations += pcs.allocations;
        stats.totalDeallocations += pcs.deallocations;
        stats.currentObjectCount += pcs.currentObjects;
        stats.currentBytesUsed += pcs.currentObjects * pcs.classSize;
        stats.slabCount += pcs.slabs;

        stats.perClass.push_back(pcs);
    }
    return stats;
}

inline void SizeClassAllocator::forEachObject(
    size_t classIndex, std::function<void(void* object)> callback
) const {
    if (classIndex >= table_.classCount()) return;
    ClassSlab* slab = classes_[classIndex].slabList;
    while (slab) {
        char* ptr = slab->pageStart;
        while (ptr + slab->objectSize <= slab->bumpPointer) {
            callback(ptr);
            ptr += slab->objectSize;
        }
        slab = slab->next;
    }
}

// =============================================================================
// Implementation — ThreadLocalAllocBuffer
// =============================================================================

inline ThreadLocalAllocBuffer::ThreadLocalAllocBuffer(
    SizeClassAllocator& central) : central_(central) {}

inline ThreadLocalAllocBuffer::~ThreadLocalAllocBuffer() {
    flushAll();
}

inline void* ThreadLocalAllocBuffer::allocate(size_t size) {
    uint16_t ci = central_.sizeClasses().classIndex(size);
    if (ci >= MAX_CACHED_CLASSES) {
        return central_.allocate(size);
    }

    auto& cache = caches_[ci];
    if (!cache.freeList) {
        refillCache(ci);
        if (!cache.freeList) return central_.allocate(size);
    }

    void* obj = cache.freeList;
    cache.freeList = cache.freeList->next;
    cache.count--;
    stats_.localAllocations++;
    return obj;
}

inline void ThreadLocalAllocBuffer::deallocate(void* ptr, size_t size) {
    uint16_t ci = central_.sizeClasses().classIndex(size);
    if (ci >= MAX_CACHED_CLASSES) {
        central_.deallocate(ptr, size);
        return;
    }

    auto& cache = caches_[ci];
    if (cache.count >= MAX_CACHE_SIZE) {
        flushCache(ci);
    }

    auto* node = static_cast<FreeListNode*>(ptr);
    node->next = cache.freeList;
    cache.freeList = node;
    cache.count++;
    stats_.localDeallocations++;
}

inline void ThreadLocalAllocBuffer::flushAll() {
    for (size_t ci = 0; ci < MAX_CACHED_CLASSES; ci++) {
        flushCache(ci);
    }
    stats_.flushes++;
}

inline void ThreadLocalAllocBuffer::refillCache(size_t classIndex) {
    size_t batchSize = central_.sizeClasses().classAt(classIndex).transferBatch;
    void* batch[64];
    size_t got = central_.allocateBatch(classIndex, batch,
                                         std::min(batchSize, size_t(64)));

    auto& cache = caches_[classIndex];
    for (size_t i = 0; i < got; i++) {
        auto* node = static_cast<FreeListNode*>(batch[i]);
        node->next = cache.freeList;
        cache.freeList = node;
        cache.count++;
    }
    stats_.refills++;
}

inline void ThreadLocalAllocBuffer::flushCache(size_t classIndex) {
    auto& cache = caches_[classIndex];
    if (!cache.freeList) return;

    void* batch[MAX_CACHE_SIZE];
    size_t count = 0;
    while (cache.freeList && count < MAX_CACHE_SIZE) {
        batch[count++] = cache.freeList;
        cache.freeList = cache.freeList->next;
    }
    cache.count = 0;

    central_.deallocateBatch(classIndex, batch, count);
}

} // namespace Zepra::Heap

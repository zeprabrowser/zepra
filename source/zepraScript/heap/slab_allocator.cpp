// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file slab_allocator.cpp
 * @brief Slab allocator for fixed-size GC metadata objects
 *
 * Many GC data structures allocate lots of small, fixed-size
 * objects (handles, weak cells, IC entries, stack maps, etc.).
 * The system allocator (malloc) adds per-object overhead and
 * fragments memory for these workloads.
 *
 * The slab allocator solves this:
 * - Allocates large slabs (pages) from the OS via mmap
 * - Divides each slab into equal-size slots
 * - Uses a free list for O(1) alloc/dealloc
 * - No per-object header overhead
 * - Excellent cache locality (objects are packed)
 *
 * Size classes:
 * - Each SlabPool handles one fixed object size
 * - SlabAllocator maps sizes to SlabPools
 * - Size classes: 16, 32, 48, 64, 96, 128, 192, 256, 384, 512
 *
 * Thread safety:
 * - Per-pool lock (no global contention)
 * - Could be extended with per-thread magazining
 */

#include "zepra_alloc.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <memory>
#include <algorithm>
#include <array>

#ifdef __linux__
#include <sys/mman.h>
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#endif

namespace Zepra::Heap {

// =============================================================================
// Constants
// =============================================================================

static constexpr size_t SLAB_PAGE_SIZE = 64 * 1024;  // 64KB slabs
static constexpr size_t NUM_SIZE_CLASSES = 10;
static constexpr size_t SIZE_CLASSES[NUM_SIZE_CLASSES] = {
    16, 32, 48, 64, 96, 128, 192, 256, 384, 512
};

// =============================================================================
// Slab Header
// =============================================================================

/**
 * @brief Header at the start of each slab page
 *
 * Stored at the beginning of the mmap'd page.
 * The rest of the page is divided into slots.
 */
struct SlabHeader {
    SlabHeader* next;       // Next slab in the pool
    SlabHeader* prev;       // Prev slab in the pool
    size_t slotSize;        // Size of each slot
    size_t totalSlots;      // Total number of slots
    size_t usedSlots;       // Currently allocated slots
    void* freeHead;         // Head of per-slab free list

    bool isEmpty() const { return usedSlots == 0; }
    bool isFull() const { return usedSlots == totalSlots; }
    double occupancy() const {
        return totalSlots > 0
            ? static_cast<double>(usedSlots) / static_cast<double>(totalSlots)
            : 0;
    }

    uintptr_t dataStart() const {
        // Data starts after the header, aligned to slotSize
        uintptr_t base = reinterpret_cast<uintptr_t>(this) + sizeof(SlabHeader);
        return (base + slotSize - 1) & ~(slotSize - 1);
    }

    uintptr_t dataEnd() const {
        return reinterpret_cast<uintptr_t>(this) + SLAB_PAGE_SIZE;
    }

    bool containsAddress(uintptr_t addr) const {
        return addr >= dataStart() && addr < dataEnd();
    }
};

// =============================================================================
// Free List Node (embedded in free slots)
// =============================================================================

struct FreeSlot {
    FreeSlot* next;
};

// =============================================================================
// Slab Pool (one size class)
// =============================================================================

/**
 * @brief Pool of slabs for a single size class
 *
 * Maintains:
 * - Partial slabs (have free slots — preferred for allocation)
 * - Full slabs (no free slots)
 * - Empty slabs (all slots free — candidates for release)
 */
class SlabPool {
public:
    struct Stats {
        size_t slotSize;
        size_t totalSlabs;
        size_t partialSlabs;
        size_t fullSlabs;
        size_t emptySlabs;
        size_t totalSlots;
        size_t usedSlots;
        size_t peakUsedSlots;
    };

    explicit SlabPool(size_t slotSize)
        : slotSize_(slotSize)
        , partialHead_(nullptr)
        , fullHead_(nullptr)
        , totalSlabs_(0)
        , totalAllocations_(0)
        , peakUsed_(0) {}

    ~SlabPool() {
        releaseAll();
    }

    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;

    /**
     * @brief Allocate a slot
     */
    void* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);

        // Try partial slab
        if (partialHead_) {
            void* slot = allocateFromSlab(partialHead_);
            if (slot) {
                if (partialHead_->isFull()) {
                    moveToFull(partialHead_);
                }
                totalAllocations_++;
                updatePeak();
                return slot;
            }
        }

        // Allocate new slab
        SlabHeader* newSlab = allocateSlab();
        if (!newSlab) return nullptr;

        insertIntoPartial(newSlab);

        void* slot = allocateFromSlab(newSlab);
        if (slot) {
            totalAllocations_++;
            updatePeak();
        }
        return slot;
    }

    /**
     * @brief Deallocate a slot
     */
    void deallocate(void* ptr) {
        if (!ptr) return;

        std::lock_guard<std::mutex> lock(mutex_);

        // Find which slab contains this pointer
        SlabHeader* slab = findSlab(ptr);
        if (!slab) {
            fprintf(stderr, "[slab] deallocate: address %p not in any slab\n", ptr);
            return;
        }

        bool wasFull = slab->isFull();

        // Push onto per-slab free list
        auto* freeSlot = static_cast<FreeSlot*>(ptr);
        freeSlot->next = static_cast<FreeSlot*>(slab->freeHead);
        slab->freeHead = freeSlot;
        slab->usedSlots--;

        if (wasFull) {
            // Move from full to partial
            removeFromFull(slab);
            insertIntoPartial(slab);
        }

        if (slab->isEmpty()) {
            // Could release slab to OS, or keep for reuse
            // Keep one empty slab, release extras
            if (countEmptySlabs() > 1) {
                removeFromPartial(slab);
                releaseSlab(slab);
            }
        }
    }

    Stats computeStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Stats stats{};
        stats.slotSize = slotSize_;
        stats.totalSlabs = totalSlabs_;
        stats.peakUsedSlots = peakUsed_;

        SlabHeader* s = partialHead_;
        while (s) {
            stats.partialSlabs++;
            stats.totalSlots += s->totalSlots;
            stats.usedSlots += s->usedSlots;
            if (s->isEmpty()) stats.emptySlabs++;
            s = s->next;
        }

        s = fullHead_;
        while (s) {
            stats.fullSlabs++;
            stats.totalSlots += s->totalSlots;
            stats.usedSlots += s->usedSlots;
            s = s->next;
        }

        return stats;
    }

    size_t slotSize() const { return slotSize_; }

private:
    void* allocateFromSlab(SlabHeader* slab) {
        if (!slab->freeHead) return nullptr;

        auto* slot = static_cast<FreeSlot*>(slab->freeHead);
        slab->freeHead = slot->next;
        slab->usedSlots++;

        // Zero out the slot (security: don't leak old data)
        std::memset(slot, 0, slotSize_);
        return slot;
    }

    SlabHeader* allocateSlab() {
#ifdef __linux__
        void* mem = mmap(nullptr, SLAB_PAGE_SIZE,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return nullptr;
#else
        void* mem = zepra_aligned_alloc(4096, SLAB_PAGE_SIZE);
        if (!mem) return nullptr;
        std::memset(mem, 0, SLAB_PAGE_SIZE);
#endif

        auto* slab = static_cast<SlabHeader*>(mem);
        slab->next = nullptr;
        slab->prev = nullptr;
        slab->slotSize = slotSize_;
        slab->usedSlots = 0;
        slab->freeHead = nullptr;

        // Build free list
        uintptr_t start = slab->dataStart();
        uintptr_t end = slab->dataEnd();
        size_t slotCount = 0;

        FreeSlot* prevFree = nullptr;
        for (uintptr_t addr = start; addr + slotSize_ <= end;
             addr += slotSize_) {
            auto* slot = reinterpret_cast<FreeSlot*>(addr);
            slot->next = prevFree;
            prevFree = slot;
            slotCount++;
        }

        slab->freeHead = prevFree;
        slab->totalSlots = slotCount;

        totalSlabs_++;
        allSlabs_.push_back(slab);

        return slab;
    }

    void releaseSlab(SlabHeader* slab) {
        allSlabs_.erase(
            std::remove(allSlabs_.begin(), allSlabs_.end(), slab),
            allSlabs_.end());
        totalSlabs_--;

#ifdef __linux__
        munmap(slab, SLAB_PAGE_SIZE);
#else
        std::free(slab);
#endif
    }

    void releaseAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* slab : allSlabs_) {
#ifdef __linux__
            munmap(slab, SLAB_PAGE_SIZE);
#else
            std::free(slab);
#endif
        }
        allSlabs_.clear();
        partialHead_ = nullptr;
        fullHead_ = nullptr;
        totalSlabs_ = 0;
    }

    SlabHeader* findSlab(void* ptr) {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        for (auto* slab : allSlabs_) {
            if (slab->containsAddress(addr)) return slab;
        }
        return nullptr;
    }

    // Linked list operations
    void insertIntoPartial(SlabHeader* slab) {
        slab->next = partialHead_;
        slab->prev = nullptr;
        if (partialHead_) partialHead_->prev = slab;
        partialHead_ = slab;
    }

    void removeFromPartial(SlabHeader* slab) {
        if (slab->prev) slab->prev->next = slab->next;
        else partialHead_ = slab->next;
        if (slab->next) slab->next->prev = slab->prev;
        slab->next = nullptr;
        slab->prev = nullptr;
    }

    void moveToFull(SlabHeader* slab) {
        removeFromPartial(slab);
        slab->next = fullHead_;
        slab->prev = nullptr;
        if (fullHead_) fullHead_->prev = slab;
        fullHead_ = slab;
    }

    void removeFromFull(SlabHeader* slab) {
        if (slab->prev) slab->prev->next = slab->next;
        else fullHead_ = slab->next;
        if (slab->next) slab->next->prev = slab->prev;
        slab->next = nullptr;
        slab->prev = nullptr;
    }

    size_t countEmptySlabs() const {
        size_t count = 0;
        SlabHeader* s = partialHead_;
        while (s) {
            if (s->isEmpty()) count++;
            s = s->next;
        }
        return count;
    }

    void updatePeak() {
        size_t used = 0;
        for (auto* slab : allSlabs_) used += slab->usedSlots;
        if (used > peakUsed_) peakUsed_ = used;
    }

    size_t slotSize_;
    mutable std::mutex mutex_;
    SlabHeader* partialHead_;
    SlabHeader* fullHead_;
    std::vector<SlabHeader*> allSlabs_;
    size_t totalSlabs_;
    uint64_t totalAllocations_;
    size_t peakUsed_;
};

// =============================================================================
// Slab Allocator (multi-size-class)
// =============================================================================

class SlabAllocator {
public:
    SlabAllocator() {
        for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
            pools_[i] = std::make_unique<SlabPool>(SIZE_CLASSES[i]);
        }
    }

    /**
     * @brief Allocate an object of given size
     *
     * Rounds up to the nearest size class.
     * Falls back to malloc for sizes > 512.
     */
    void* allocate(size_t size) {
        int classIdx = sizeClassIndex(size);
        if (classIdx < 0) {
            // Too large for slab allocator
            return std::malloc(size);
        }
        return pools_[classIdx]->allocate();
    }

    /**
     * @brief Deallocate an object
     *
     * Must be called with the same size used for allocation.
     */
    void deallocate(void* ptr, size_t size) {
        if (!ptr) return;
        int classIdx = sizeClassIndex(size);
        if (classIdx < 0) {
            std::free(ptr);
            return;
        }
        pools_[classIdx]->deallocate(ptr);
    }

    /**
     * @brief Get per-pool stats
     */
    std::array<SlabPool::Stats, NUM_SIZE_CLASSES> allStats() const {
        std::array<SlabPool::Stats, NUM_SIZE_CLASSES> stats;
        for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
            stats[i] = pools_[i]->computeStats();
        }
        return stats;
    }

    /**
     * @brief Print allocation stats
     */
    void printStats(FILE* out) const {
        fprintf(out, "Slab Allocator Stats:\n");
        fprintf(out, "%6s %6s %8s %8s %8s %8s\n",
            "Size", "Slabs", "Total", "Used", "Peak", "Occ%");

        for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
            auto stats = pools_[i]->computeStats();
            double occ = stats.totalSlots > 0
                ? 100.0 * static_cast<double>(stats.usedSlots) /
                  static_cast<double>(stats.totalSlots) : 0;

            fprintf(out, "%6zu %6zu %8zu %8zu %8zu %7.1f\n",
                stats.slotSize, stats.totalSlabs,
                stats.totalSlots, stats.usedSlots,
                stats.peakUsedSlots, occ);
        }
    }

private:
    static int sizeClassIndex(size_t size) {
        for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
            if (size <= SIZE_CLASSES[i]) return static_cast<int>(i);
        }
        return -1;  // Too large
    }

    std::array<std::unique_ptr<SlabPool>, NUM_SIZE_CLASSES> pools_;
};

} // namespace Zepra::Heap

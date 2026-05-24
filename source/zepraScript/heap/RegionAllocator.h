// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file RegionAllocator.h
 * @brief Region-based heap allocation for ZepraScript GC
 *
 * Divides the heap into fixed-size regions (typically 256KB).
 * Each region has its own:
 * - Bump allocator (fast sequential allocation)
 * - Mark bitmap (for parallel marking)
 * - Live byte count (for evacuation decisions)
 *
 * Regions enable:
 * - Parallel per-region sweeping
 * - Selective evacuation (only copy sparse regions)
 * - Better cache locality than malloc-based free-lists
 * - Incremental memory return to OS
 *
 * This is the backbone allocator for old generation.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <algorithm>

#ifdef __linux__
#include <sys/mman.h>
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#endif

namespace Zepra::Heap {

// =============================================================================
// Region Configuration
// =============================================================================

struct RegionConfig {
    size_t regionSize = 256 * 1024;          // 256KB per region
    size_t maxRegions = 2048;                 // 512MB max (2048 * 256KB)
    size_t objectAlignment = 8;
    double evacuationThreshold = 0.5;         // Evacuate if <50% live
    bool useGuardPages = false;
    bool adviseUnused = true;                 // MADV_DONTNEED on free regions
};

// =============================================================================
// Region State
// =============================================================================

enum class RegionState : uint8_t {
    Free,           // Available for allocation
    Active,         // Currently being allocated into
    Full,           // No more space
    Sweeping,       // Being swept
    Evacuating,     // Being compacted (live objects copied out)
    PendingRelease  // Scheduled for release to OS
};

// =============================================================================
// Region Header
// =============================================================================

/**
 * @brief Metadata for a single region
 *
 * Lives outside the region itself to avoid polluting cache lines
 * during allocation.
 */
struct RegionHeader {
    // Identity
    uint32_t index;                 // Region index
    RegionState state;

    // Allocation
    char* start;                    // Region start address
    char* current;                  // Bump pointer (next allocation)
    char* end;                      // Region end address

    // GC metadata
    size_t liveBytes;               // Bytes marked as live
    size_t allocatedBytes;          // Total bytes allocated
    uint32_t gcAge;                 // Number of GC cycles survived
    bool hasFragmentation;          // Fragmentation detected

    // Lock for concurrent allocation
    std::atomic<bool> lock{false};

    // Allocation
    void* allocate(size_t size, size_t alignment) {
        size_t aligned = (size + alignment - 1) & ~(alignment - 1);
        char* result = current;
        char* newCurrent = current + aligned;
        if (newCurrent > end) return nullptr;
        current = newCurrent;
        allocatedBytes += aligned;
        return result;
    }

    // Stats
    size_t used() const { return static_cast<size_t>(current - start); }
    size_t available() const { return static_cast<size_t>(end - current); }
    size_t capacity() const { return static_cast<size_t>(end - start); }
    double liveRatio() const {
        return allocatedBytes > 0
            ? static_cast<double>(liveBytes) / static_cast<double>(allocatedBytes)
            : 0.0;
    }
    bool shouldEvacuate(double threshold) const {
        return liveRatio() < threshold && state == RegionState::Full;
    }

    void reset() {
        current = start;
        liveBytes = 0;
        allocatedBytes = 0;
        gcAge = 0;
        hasFragmentation = false;
        state = RegionState::Free;
    }

    // Spin-lock for concurrent access
    void acquire() {
        while (lock.exchange(true, std::memory_order_acquire)) {
            while (lock.load(std::memory_order_relaxed)) {
                // Spin
            }
        }
    }
    void release() { lock.store(false, std::memory_order_release); }
};

// =============================================================================
// Region Mark Bitmap
// =============================================================================

/**
 * @brief Per-region mark bitmap
 *
 * One bit per allocation granule (8 bytes default).
 * 256KB region / 8 byte granule = 32K bits = 4KB bitmap.
 */
class RegionBitmap {
public:
    RegionBitmap() = default;

    void initialize(size_t regionSize, size_t alignment) {
        size_t numBits = regionSize / alignment;
        size_t numWords = (numBits + 63) / 64;
        bits_.resize(numWords, 0);
        alignment_ = alignment;
    }

    bool mark(size_t offset) {
        size_t idx = offset / alignment_;
        size_t word = idx / 64;
        size_t bit = idx % 64;
        if (word >= bits_.size()) return false;
        uint64_t mask = uint64_t(1) << bit;
        bool wasUnmarked = (bits_[word] & mask) == 0;
        bits_[word] |= mask;
        return wasUnmarked;
    }

    bool isMarked(size_t offset) const {
        size_t idx = offset / alignment_;
        size_t word = idx / 64;
        size_t bit = idx % 64;
        if (word >= bits_.size()) return false;
        return (bits_[word] & (uint64_t(1) << bit)) != 0;
    }

    void clear() {
        std::fill(bits_.begin(), bits_.end(), 0);
    }

    size_t markedCount() const {
        size_t count = 0;
        for (uint64_t w : bits_) {
            count += __builtin_popcountll(w);
        }
        return count;
    }

    /**
     * @brief Iterate marked objects
     */
    template<typename Callback>
    void forEachMarked(char* regionBase, Callback cb) const {
        for (size_t w = 0; w < bits_.size(); w++) {
            if (bits_[w] == 0) continue;
            for (size_t b = 0; b < 64; b++) {
                if (bits_[w] & (uint64_t(1) << b)) {
                    size_t offset = (w * 64 + b) * alignment_;
                    cb(regionBase + offset);
                }
            }
        }
    }

private:
    std::vector<uint64_t> bits_;
    size_t alignment_ = 8;
};

// =============================================================================
// Region Allocator Statistics
// =============================================================================

struct RegionAllocatorStats {
    size_t totalRegions = 0;
    size_t activeRegions = 0;
    size_t fullRegions = 0;
    size_t freeRegions = 0;
    size_t totalAllocated = 0;
    size_t totalLive = 0;
    size_t totalCapacity = 0;
    size_t evacuationCount = 0;
    size_t regionsReleased = 0;

    double overallLiveRatio() const {
        return totalCapacity > 0
            ? static_cast<double>(totalLive) / static_cast<double>(totalCapacity)
            : 0.0;
    }

    double fragmentation() const {
        return totalAllocated > 0
            ? 1.0 - static_cast<double>(totalLive) / static_cast<double>(totalAllocated)
            : 0.0;
    }
};

// =============================================================================
// Region Allocator
// =============================================================================

class RegionAllocator {
public:
    explicit RegionAllocator(const RegionConfig& config = RegionConfig{});
    ~RegionAllocator();

    RegionAllocator(const RegionAllocator&) = delete;
    RegionAllocator& operator=(const RegionAllocator&) = delete;

    // -------------------------------------------------------------------------
    // Allocation
    // -------------------------------------------------------------------------

    /**
     * @brief Allocate memory (sequential bump in active region)
     */
    void* allocate(size_t size);

    /**
     * @brief Allocate in a specific region (for evacuation)
     */
    void* allocateInRegion(uint32_t regionIndex, size_t size);

    // -------------------------------------------------------------------------
    // GC Integration
    // -------------------------------------------------------------------------

    /**
     * @brief Mark an object at given address
     * @return true if newly marked
     */
    bool markObject(void* object);

    /**
     * @brief Check if object is marked
     */
    bool isMarked(void* object) const;

    /**
     * @brief Clear all marks before new GC cycle
     */
    void clearAllMarks();

    /**
     * @brief Sweep: release unmarked objects, compute live bytes
     */
    void sweep();

    /**
     * @brief Selective evacuation: copy live objects from sparse regions
     * @return Number of objects moved
     */
    size_t evacuate(std::function<void(void* oldAddr, void* newAddr, size_t size)> relocator);

    /**
     * @brief Release empty regions back to OS
     */
    void releaseEmptyRegions();

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    /**
     * @brief Check if address belongs to this allocator
     */
    bool contains(const void* ptr) const;

    /**
     * @brief Get region containing an address
     * @return Region index, or -1 if not found
     */
    int32_t regionIndexFor(const void* ptr) const;

    /**
     * @brief Current statistics
     */
    RegionAllocatorStats computeStats() const;

    /**
     * @brief Iterate all live (marked) objects
     */
    void forEachLiveObject(std::function<void(void* object)> callback);

    /**
     * @brief Number of regions
     */
    size_t regionCount() const { return regions_.size(); }

    /**
     * @brief Total allocated bytes
     */
    size_t totalAllocated() const;

    /**
     * @brief Available bytes (free regions + remainder of active)
     */
    size_t availableBytes() const;

private:
    /**
     * @brief Get or create an active region for allocation
     */
    RegionHeader* getActiveRegion(size_t requiredSize);

    /**
     * @brief Allocate a new empty region
     */
    RegionHeader* allocateNewRegion();

    /**
     * @brief Free a region's backing memory
     */
    void freeRegion(RegionHeader* region);

    RegionConfig config_;

    // Backing memory (contiguous virtual address range)
    char* heapBase_ = nullptr;
    size_t heapSize_ = 0;

    // Region tracking
    std::vector<RegionHeader> regions_;
    std::vector<RegionBitmap> bitmaps_;
    uint32_t activeRegionIndex_ = UINT32_MAX;
    uint32_t nextRegionIndex_ = 0;

    // Free region list
    std::vector<uint32_t> freeList_;

    mutable std::mutex mutex_;
};

// =============================================================================
// Implementation
// =============================================================================

inline RegionAllocator::RegionAllocator(const RegionConfig& config)
    : config_(config) {
    heapSize_ = config_.regionSize * config_.maxRegions;

#ifdef __linux__
    // Reserve virtual address space (no physical pages yet)
    heapBase_ = static_cast<char*>(mmap(
        nullptr, heapSize_,
        PROT_NONE,  // No access initially
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
        -1, 0));
    if (heapBase_ == MAP_FAILED) heapBase_ = nullptr;
#else
    heapBase_ = static_cast<char*>(std::calloc(1, heapSize_));
#endif

    regions_.reserve(config_.maxRegions);
    bitmaps_.reserve(config_.maxRegions);
}

inline RegionAllocator::~RegionAllocator() {
    if (heapBase_) {
#ifdef __linux__
        munmap(heapBase_, heapSize_);
#else
        std::free(heapBase_);
#endif
    }
}

inline void* RegionAllocator::allocate(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t aligned = (size + config_.objectAlignment - 1) &
                     ~(config_.objectAlignment - 1);

    RegionHeader* region = getActiveRegion(aligned);
    if (!region) return nullptr;

    void* result = region->allocate(aligned, config_.objectAlignment);
    if (!result) {
        // Region full — mark as full, get new one
        region->state = RegionState::Full;
        region = getActiveRegion(aligned);
        if (!region) return nullptr;
        result = region->allocate(aligned, config_.objectAlignment);
    }

    return result;
}

inline RegionHeader* RegionAllocator::getActiveRegion(size_t requiredSize) {
    if (activeRegionIndex_ < regions_.size()) {
        auto& region = regions_[activeRegionIndex_];
        if (region.available() >= requiredSize) {
            return &region;
        }
        region.state = RegionState::Full;
    }

    // Try free list
    if (!freeList_.empty()) {
        uint32_t idx = freeList_.back();
        freeList_.pop_back();
        regions_[idx].state = RegionState::Active;
        regions_[idx].current = regions_[idx].start;
        activeRegionIndex_ = idx;
        return &regions_[idx];
    }

    // Allocate new region
    auto* newRegion = allocateNewRegion();
    if (newRegion) {
        newRegion->state = RegionState::Active;
        activeRegionIndex_ = newRegion->index;
    }
    return newRegion;
}

inline RegionHeader* RegionAllocator::allocateNewRegion() {
    if (!heapBase_) return nullptr;
    if (nextRegionIndex_ >= config_.maxRegions) return nullptr;

    uint32_t idx = nextRegionIndex_++;
    char* regionStart = heapBase_ + static_cast<size_t>(idx) * config_.regionSize;

#ifdef __linux__
    // Commit physical pages
    mprotect(regionStart, config_.regionSize, PROT_READ | PROT_WRITE);
#endif

    RegionHeader header{};
    header.index = idx;
    header.state = RegionState::Free;
    header.start = regionStart;
    header.current = regionStart;
    header.end = regionStart + config_.regionSize;
    header.liveBytes = 0;
    header.allocatedBytes = 0;
    header.gcAge = 0;
    header.hasFragmentation = false;

    regions_.push_back(header);

    RegionBitmap bitmap;
    bitmap.initialize(config_.regionSize, config_.objectAlignment);
    bitmaps_.push_back(std::move(bitmap));

    return &regions_.back();
}

inline bool RegionAllocator::markObject(void* object) {
    int32_t idx = regionIndexFor(object);
    if (idx < 0) return false;

    size_t offset = static_cast<size_t>(
        static_cast<char*>(object) - regions_[static_cast<size_t>(idx)].start);
    return bitmaps_[static_cast<size_t>(idx)].mark(offset);
}

inline bool RegionAllocator::isMarked(void* object) const {
    int32_t idx = regionIndexFor(object);
    if (idx < 0) return false;

    size_t offset = static_cast<size_t>(
        static_cast<char*>(object) - regions_[static_cast<size_t>(idx)].start);
    return bitmaps_[static_cast<size_t>(idx)].isMarked(offset);
}

inline void RegionAllocator::clearAllMarks() {
    for (auto& bm : bitmaps_) {
        bm.clear();
    }
    for (auto& region : regions_) {
        region.liveBytes = 0;
    }
}

inline void RegionAllocator::sweep() {
    for (size_t i = 0; i < regions_.size(); i++) {
        auto& region = regions_[i];
        if (region.state == RegionState::Free ||
            region.state == RegionState::PendingRelease) continue;

        // Count live bytes
        region.liveBytes = bitmaps_[i].markedCount() * config_.objectAlignment;

        // If no live objects, return to free list
        if (region.liveBytes == 0 && region.state != RegionState::Active) {
            region.reset();
            freeList_.push_back(region.index);
        }
    }
}

inline size_t RegionAllocator::evacuate(
    std::function<void(void* oldAddr, void* newAddr, size_t size)> relocator
) {
    size_t objectsMoved = 0;

    for (size_t i = 0; i < regions_.size(); i++) {
        auto& region = regions_[i];
        if (!region.shouldEvacuate(config_.evacuationThreshold)) continue;

        region.state = RegionState::Evacuating;

        // Copy live objects to another region
        bitmaps_[i].forEachMarked(region.start, [&](char* objAddr) {
            // Determine object size (simplified — would need object header)
            size_t objSize = config_.objectAlignment;  // Minimum

            void* newAddr = allocate(objSize);
            if (newAddr) {
                std::memcpy(newAddr, objAddr, objSize);
                relocator(objAddr, newAddr, objSize);
                objectsMoved++;
            }
        });

        // Region is now empty
        region.reset();
        freeList_.push_back(region.index);
    }

    return objectsMoved;
}

inline void RegionAllocator::releaseEmptyRegions() {
#ifdef __linux__
    for (auto& region : regions_) {
        if (region.state == RegionState::Free && config_.adviseUnused) {
            madvise(region.start, config_.regionSize, MADV_DONTNEED);
            region.state = RegionState::PendingRelease;
        }
    }
#endif
}

inline bool RegionAllocator::contains(const void* ptr) const {
    if (!heapBase_) return false;
    auto* p = static_cast<const char*>(ptr);
    return p >= heapBase_ && p < heapBase_ + heapSize_;
}

inline int32_t RegionAllocator::regionIndexFor(const void* ptr) const {
    if (!contains(ptr)) return -1;
    size_t offset = static_cast<size_t>(
        static_cast<const char*>(ptr) - heapBase_);
    uint32_t idx = static_cast<uint32_t>(offset / config_.regionSize);
    return idx < regions_.size() ? static_cast<int32_t>(idx) : -1;
}

inline RegionAllocatorStats RegionAllocator::computeStats() const {
    RegionAllocatorStats stats;
    stats.totalRegions = regions_.size();

    for (const auto& region : regions_) {
        switch (region.state) {
            case RegionState::Active: stats.activeRegions++; break;
            case RegionState::Full: stats.fullRegions++; break;
            case RegionState::Free:
            case RegionState::PendingRelease:
                stats.freeRegions++; break;
            default: break;
        }
        stats.totalAllocated += region.allocatedBytes;
        stats.totalLive += region.liveBytes;
        stats.totalCapacity += region.capacity();
    }

    return stats;
}

inline void RegionAllocator::forEachLiveObject(
    std::function<void(void* object)> callback
) {
    for (size_t i = 0; i < regions_.size(); i++) {
        if (regions_[i].state == RegionState::Free) continue;
        bitmaps_[i].forEachMarked(regions_[i].start, [&](char* obj) {
            callback(obj);
        });
    }
}

inline size_t RegionAllocator::totalAllocated() const {
    size_t total = 0;
    for (const auto& r : regions_) total += r.allocatedBytes;
    return total;
}

inline size_t RegionAllocator::availableBytes() const {
    size_t avail = freeList_.size() * config_.regionSize;
    if (activeRegionIndex_ < regions_.size()) {
        avail += regions_[activeRegionIndex_].available();
    }
    avail += (config_.maxRegions - nextRegionIndex_) * config_.regionSize;
    return avail;
}

} // namespace Zepra::Heap

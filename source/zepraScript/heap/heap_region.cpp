// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file heap_region.cpp
 * @brief Region-based heap management
 *
 * The heap is divided into fixed-size regions (256KB). Each region:
 * - Is typed: nursery, old-gen, large-object, code, free
 * - Has its own mark bitmap, live byte count, and allocation cursor
 * - Can be independently collected, compacted, or released
 *
 * RegionTable: central directory of all regions
 * - Maps any heap address to its owning region in O(1)
 * - Uses base address + shift to compute region index
 * - Stores region metadata in a side table (not in the region itself)
 *
 * Region lifecycle:
 * 1. Uncommitted: virtual address reserved, no physical pages
 * 2. Free: physical pages committed but no allocations
 * 3. Active: currently being allocated into
 * 4. Full: no more space, waiting for GC
 * 5. Evacuating: GC is moving objects out
 * 6. Released: physical pages returned to OS via madvise
 *
 * Cross-region references:
 * - Tracked via per-region remembered sets (card table + slot set)
 * - Old→young references recorded by write barrier
 * - During minor GC, only regions with dirty cards are scanned
 */

#include "zepra_alloc.h"
#include <atomic>
#include <mutex>
#include <shared_mutex>
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
#include <bitset>
#include <array>

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Zepra::Heap {

// =============================================================================
// Region Constants
// =============================================================================

static constexpr size_t REGION_SIZE_SHIFT = 18;
static constexpr size_t REGION_SIZE = size_t(1) << REGION_SIZE_SHIFT;  // 256KB
static constexpr size_t REGION_ALIGNMENT = REGION_SIZE;

// Mark bitmap: 1 bit per 8-byte cell in the region
static constexpr size_t CELLS_PER_REGION = REGION_SIZE / 8;
static constexpr size_t MARK_BITMAP_BYTES = CELLS_PER_REGION / 8; // 4KB

// Card table: 1 byte per 512-byte card
static constexpr size_t CARD_SIZE = 512;
static constexpr size_t CARDS_PER_REGION = REGION_SIZE / CARD_SIZE; // 512

// =============================================================================
// Region Type
// =============================================================================

enum class RegionType : uint8_t {
    Free,           // No allocations, available for reuse
    Nursery,        // Young generation (minor GC scope)
    Eden,           // Sub-region of nursery for bump allocation
    Survivor,       // Objects that survived one minor GC
    OldGen,         // Tenured objects
    LargeObject,    // Single large object (> REGION_SIZE/2)
    Code,           // JIT code
    Map,            // Hidden class / shape descriptors
    External,       // External (ArrayBuffer/WASM) tracking only
    Uncommitted,    // VA reserved but no physical pages
};

static const char* regionTypeName(RegionType type) {
    switch (type) {
        case RegionType::Free: return "Free";
        case RegionType::Nursery: return "Nursery";
        case RegionType::Eden: return "Eden";
        case RegionType::Survivor: return "Survivor";
        case RegionType::OldGen: return "OldGen";
        case RegionType::LargeObject: return "LargeObject";
        case RegionType::Code: return "Code";
        case RegionType::Map: return "Map";
        case RegionType::External: return "External";
        case RegionType::Uncommitted: return "Uncommitted";
    }
    return "Unknown";
}

// =============================================================================
// Region State
// =============================================================================

enum class RegionState : uint8_t {
    Uncommitted,
    Free,
    Active,     // Currently allocating into
    Full,       // No space, awaiting GC
    Marking,    // GC is marking objects in this region
    Sweeping,   // GC is sweeping this region
    Evacuating, // GC is moving objects out
    Released,   // Physical pages returned to OS
};

// =============================================================================
// Region Descriptor
// =============================================================================

/**
 * @brief Metadata for a single heap region
 *
 * Stored in the side table, NOT in the region memory itself.
 * This avoids polluting the heap with metadata and allows
 * the region memory to be entirely for objects.
 */
struct RegionDescriptor {
    // Identity
    uint32_t index;
    uintptr_t baseAddr;

    // Type and state
    RegionType type;
    RegionState state;

    // Allocation
    size_t allocCursor;     // Offset of next free byte
    size_t liveBytes;       // Live bytes after last GC
    uint32_t liveObjects;   // Live object count after last GC

    // GC data
    uint8_t age;            // Number of GCs survived (for nursery objects)
    bool pinned;            // Contains pinned objects (no evacuation)
    bool hasFinalizers;     // Contains objects with destructors
    bool dirty;             // Has outgoing cross-region references
    uint64_t lastGCCycleId; // When this region was last collected

    // Per-region mark bitmap
    uint8_t markBitmap[MARK_BITMAP_BYTES];

    // Per-region card table
    uint8_t cardTable[CARDS_PER_REGION];

    // Derived
    size_t usedBytes() const { return allocCursor; }
    size_t freeBytes() const { return REGION_SIZE > allocCursor
                                       ? REGION_SIZE - allocCursor : 0; }
    double occupancy() const {
        return allocCursor > 0
            ? static_cast<double>(liveBytes) / static_cast<double>(allocCursor)
            : 0;
    }
    double utilization() const {
        return static_cast<double>(liveBytes) / static_cast<double>(REGION_SIZE);
    }
    bool isEmpty() const { return liveBytes == 0; }
    bool isFull() const { return freeBytes() < 64; }  // < min object size

    uintptr_t endAddr() const { return baseAddr + REGION_SIZE; }
    bool containsAddress(uintptr_t addr) const {
        return addr >= baseAddr && addr < endAddr();
    }

    // Mark bitmap operations
    void markCell(size_t cellIndex) {
        markBitmap[cellIndex / 8] |= (1 << (cellIndex % 8));
    }
    bool isCellMarked(size_t cellIndex) const {
        return (markBitmap[cellIndex / 8] & (1 << (cellIndex % 8))) != 0;
    }
    void clearMarkBitmap() {
        std::memset(markBitmap, 0, sizeof(markBitmap));
    }

    // Card table operations
    void dirtyCard(size_t cardIndex) {
        cardTable[cardIndex] = 1;
    }
    bool isCardDirty(size_t cardIndex) const {
        return cardTable[cardIndex] != 0;
    }
    void clearCardTable() {
        std::memset(cardTable, 0, sizeof(cardTable));
    }

    size_t dirtyCardCount() const {
        size_t count = 0;
        for (size_t i = 0; i < CARDS_PER_REGION; i++) {
            if (cardTable[i]) count++;
        }
        return count;
    }

    // Address → cell/card conversions
    size_t addrToCell(uintptr_t addr) const {
        return (addr - baseAddr) / 8;
    }
    size_t addrToCard(uintptr_t addr) const {
        return (addr - baseAddr) / CARD_SIZE;
    }

    void initialize(uint32_t idx, uintptr_t base) {
        index = idx;
        baseAddr = base;
        type = RegionType::Free;
        state = RegionState::Free;
        allocCursor = 0;
        liveBytes = 0;
        liveObjects = 0;
        age = 0;
        pinned = false;
        hasFinalizers = false;
        dirty = false;
        lastGCCycleId = 0;
        clearMarkBitmap();
        clearCardTable();
    }
};

// =============================================================================
// Region Table
// =============================================================================

/**
 * @brief Central directory of all heap regions
 *
 * Maps any heap address to its RegionDescriptor in O(1):
 *   regionIndex = (addr - heapBase) >> REGION_SIZE_SHIFT
 *
 * Manages the pool of free regions and hands them out.
 */
class RegionTable {
public:
    struct Config {
        size_t maxRegions;          // Maximum number of regions
        size_t initialReserve;     // Regions to reserve VA for
        Config()
            : maxRegions(16384)     // 16K * 256KB = 4GB max heap
            , initialReserve(256) {}// 64MB initial reserve
    };

    struct Stats {
        size_t totalRegions;
        size_t freeRegions;
        size_t nurseryRegions;
        size_t oldGenRegions;
        size_t codeRegions;
        size_t largeObjectRegions;
        size_t committedBytes;
        size_t liveBytes;
    };

    explicit RegionTable(const Config& config = Config{})
        : config_(config)
        , heapBase_(0)
        , heapSize_(0)
        , committed_(0) {
        descriptors_.resize(config.maxRegions);
    }

    ~RegionTable() {
        releaseAll();
    }

    RegionTable(const RegionTable&) = delete;
    RegionTable& operator=(const RegionTable&) = delete;

    /**
     * @brief Initialize the region table with a contiguous VA reservation
     */
    bool initialize() {
        size_t reserveSize = config_.initialReserve * REGION_SIZE;

#ifdef __linux__
        // Reserve VA space — no physical pages yet
        void* base = mmap(nullptr, reserveSize,
                          PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                          -1, 0);
        if (base == MAP_FAILED) return false;
        heapBase_ = reinterpret_cast<uintptr_t>(base);
#else
        void* base = zepra_aligned_alloc(REGION_ALIGNMENT, reserveSize);
        if (!base) return false;
        heapBase_ = reinterpret_cast<uintptr_t>(base);
#endif

        heapSize_ = reserveSize;

        // Initialize all descriptors as uncommitted
        for (size_t i = 0; i < config_.initialReserve; i++) {
            descriptors_[i].initialize(static_cast<uint32_t>(i),
                                        heapBase_ + i * REGION_SIZE);
            descriptors_[i].type = RegionType::Uncommitted;
            descriptors_[i].state = RegionState::Uncommitted;
        }

        regionCount_ = config_.initialReserve;
        return true;
    }

    /**
     * @brief Allocate a region of the given type
     */
    RegionDescriptor* allocateRegion(RegionType type) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Find a free or uncommitted region
        for (size_t i = 0; i < regionCount_; i++) {
            auto& desc = descriptors_[i];
            if (desc.state == RegionState::Uncommitted) {
                commitRegion(i);
                desc.type = type;
                desc.state = RegionState::Active;
                desc.allocCursor = 0;
                desc.liveBytes = 0;
                desc.liveObjects = 0;
                desc.clearMarkBitmap();
                desc.clearCardTable();
                return &desc;
            }
            if (desc.state == RegionState::Free) {
                desc.type = type;
                desc.state = RegionState::Active;
                desc.allocCursor = 0;
                desc.liveBytes = 0;
                desc.liveObjects = 0;
                desc.clearMarkBitmap();
                desc.clearCardTable();
                return &desc;
            }
        }

        return nullptr;  // No regions available
    }

    /**
     * @brief Release a region back to the free pool
     */
    void releaseRegion(uint32_t regionIndex) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (regionIndex >= regionCount_) return;

        auto& desc = descriptors_[regionIndex];

#ifdef __linux__
        // Release physical pages but keep VA
        madvise(reinterpret_cast<void*>(desc.baseAddr),
                REGION_SIZE, MADV_DONTNEED);
#endif

        desc.type = RegionType::Free;
        desc.state = RegionState::Free;
        desc.allocCursor = 0;
        desc.liveBytes = 0;
        desc.liveObjects = 0;
        desc.age = 0;
        desc.pinned = false;
        desc.hasFinalizers = false;
        desc.dirty = false;
        desc.clearMarkBitmap();
        desc.clearCardTable();
    }

    /**
     * @brief Lookup region by address (O(1))
     */
    RegionDescriptor* findRegion(uintptr_t addr) {
        if (addr < heapBase_ || addr >= heapBase_ + heapSize_) return nullptr;
        uint32_t index = static_cast<uint32_t>(
            (addr - heapBase_) >> REGION_SIZE_SHIFT);
        if (index >= regionCount_) return nullptr;
        return &descriptors_[index];
    }

    const RegionDescriptor* findRegion(uintptr_t addr) const {
        if (addr < heapBase_ || addr >= heapBase_ + heapSize_) return nullptr;
        uint32_t index = static_cast<uint32_t>(
            (addr - heapBase_) >> REGION_SIZE_SHIFT);
        if (index >= regionCount_) return nullptr;
        return &descriptors_[index];
    }

    /**
     * @brief Check if an address is in the heap
     */
    bool isHeapAddress(uintptr_t addr) const {
        return addr >= heapBase_ && addr < heapBase_ + heapSize_;
    }

    /**
     * @brief Bump-allocate within a region
     * @return Allocated address or 0 on failure
     */
    uintptr_t allocateInRegion(uint32_t regionIndex, size_t size) {
        if (regionIndex >= regionCount_) return 0;

        auto& desc = descriptors_[regionIndex];
        size = (size + 7) & ~size_t(7);  // Align to 8

        if (desc.allocCursor + size > REGION_SIZE) return 0;

        uintptr_t addr = desc.baseAddr + desc.allocCursor;
        desc.allocCursor += size;
        return addr;
    }

    /**
     * @brief Iterate regions by type
     */
    void forEachByType(RegionType type,
                        std::function<void(RegionDescriptor&)> visitor) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < regionCount_; i++) {
            if (descriptors_[i].type == type) {
                visitor(descriptors_[i]);
            }
        }
    }

    /**
     * @brief Iterate all active (non-free, non-uncommitted) regions
     */
    void forEachActive(std::function<void(RegionDescriptor&)> visitor) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < regionCount_; i++) {
            if (descriptors_[i].state != RegionState::Uncommitted &&
                descriptors_[i].state != RegionState::Free &&
                descriptors_[i].state != RegionState::Released) {
                visitor(descriptors_[i]);
            }
        }
    }

    Stats computeStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Stats stats{};
        stats.totalRegions = regionCount_;

        for (size_t i = 0; i < regionCount_; i++) {
            const auto& d = descriptors_[i];
            switch (d.type) {
                case RegionType::Free:
                case RegionType::Uncommitted:
                    stats.freeRegions++;
                    break;
                case RegionType::Nursery:
                case RegionType::Eden:
                case RegionType::Survivor:
                    stats.nurseryRegions++;
                    stats.committedBytes += REGION_SIZE;
                    stats.liveBytes += d.liveBytes;
                    break;
                case RegionType::OldGen:
                    stats.oldGenRegions++;
                    stats.committedBytes += REGION_SIZE;
                    stats.liveBytes += d.liveBytes;
                    break;
                case RegionType::Code:
                    stats.codeRegions++;
                    stats.committedBytes += REGION_SIZE;
                    break;
                case RegionType::LargeObject:
                    stats.largeObjectRegions++;
                    stats.committedBytes += REGION_SIZE;
                    stats.liveBytes += d.liveBytes;
                    break;
                default:
                    break;
            }
        }

        return stats;
    }

    uintptr_t heapBase() const { return heapBase_; }
    size_t heapSize() const { return heapSize_; }
    size_t regionCount() const { return regionCount_; }

private:
    void commitRegion(size_t index) {
        uintptr_t addr = heapBase_ + index * REGION_SIZE;

#ifdef __linux__
        mprotect(reinterpret_cast<void*>(addr), REGION_SIZE,
                 PROT_READ | PROT_WRITE);
#endif

        committed_ += REGION_SIZE;
    }

    void releaseAll() {
        if (heapBase_ == 0) return;

#ifdef __linux__
        munmap(reinterpret_cast<void*>(heapBase_), heapSize_);
#else
        std::free(reinterpret_cast<void*>(heapBase_));
#endif

        heapBase_ = 0;
        heapSize_ = 0;
    }

    Config config_;
    mutable std::mutex mutex_;

    uintptr_t heapBase_;
    size_t heapSize_;
    size_t regionCount_ = 0;
    size_t committed_ = 0;

    std::vector<RegionDescriptor> descriptors_;
};

} // namespace Zepra::Heap

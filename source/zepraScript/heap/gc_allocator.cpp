// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file gc_allocator.cpp
 * @brief Unified allocator — integrates all allocation paths
 *
 * This file ties together:
 * - NurseryAllocator (bump pointer, Cheney semi-space)
 * - SizeClassAllocator (segregated free lists for old gen)
 * - LargeObjectSpace (mmap-based for >8KB objects)
 * - CodeSpace (executable pages for JIT)
 * - TLAB management (per-thread buffers)
 *
 * Allocation fast-path:
 *   TLAB → SizeClass → Nursery → OldGen → LOS → OOM
 *
 * Slow-path:
 *   Request GC → Retry → Grow heap → OOM
 */

#include "zepra_alloc.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <deque>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <unordered_map>

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Zepra::Heap {

// =============================================================================
// Allocation Policy
// =============================================================================

enum class AllocationPolicy : uint8_t {
    Default,            // Normal allocation: nursery → old gen
    PreTenured,         // Skip nursery: allocate directly in old gen
    Pinned,             // Must not be moved (FFI, native code)
    Executable,         // For JIT code objects
    Transient,          // Short-lived: guaranteed nursery only
    LargeObject,        // Always routed to LOS
};

// =============================================================================
// Large Object Space
// =============================================================================

/**
 * @brief Manages objects too large for size-class allocation
 *
 * Each large object gets its own mmap region:
 * - Guard pages before and after
 * - Direct mmap/munmap lifecycle
 * - No free-list fragmentation
 * - Individually sweepable
 *
 * Threshold: objects >= 8KB (configurable)
 * Maximum: 256MB per object (configurable)
 */
class LargeObjectSpace {
public:
    struct LOSConfig {
        size_t threshold;       // Min size for LOS
        size_t maxObjectSize;   // Max per-object
        size_t maxTotalSize;    // Max total LOS memory
        bool useGuardPages;

        LOSConfig()
            : threshold(8 * 1024)
            , maxObjectSize(256 * 1024 * 1024)
            , maxTotalSize(1024ULL * 1024 * 1024)
            , useGuardPages(true) {}
    };

    struct LOSEntry {
        void* baseAddr;         // mmap base (may include guard page)
        void* objectAddr;       // Actual object start
        size_t objectSize;      // Requested size
        size_t mappedSize;      // Total mapped (incl. guards)
        uint32_t typeId;
        bool marked;

        // Metadata
        uint64_t allocTime;
        uint32_t gcSurvivalCount;
    };

    struct LOSStats {
        size_t objectCount = 0;
        size_t totalBytes = 0;
        size_t peakBytes = 0;
        uint64_t totalAllocations = 0;
        uint64_t totalDeallocations = 0;
        size_t largestObject = 0;
        size_t smallestObject = SIZE_MAX;
    };

    explicit LargeObjectSpace(const LOSConfig& config = LOSConfig{});
    ~LargeObjectSpace();

    LargeObjectSpace(const LargeObjectSpace&) = delete;
    LargeObjectSpace& operator=(const LargeObjectSpace&) = delete;

    /**
     * @brief Allocate a large object
     */
    void* allocate(size_t size, uint32_t typeId = 0);

    /**
     * @brief Free a large object
     */
    void free(void* addr);

    /**
     * @brief Deallocate all unmarked objects
     */
    size_t sweep();

    /**
     * @brief Mark a large object
     */
    bool mark(void* addr);

    /**
     * @brief Check if marked
     */
    bool isMarked(void* addr) const;

    /**
     * @brief Clear all marks
     */
    void clearMarks();

    /**
     * @brief Check if address is in LOS
     */
    bool contains(const void* addr) const;

    /**
     * @brief Iterate all live objects
     */
    void forEach(std::function<void(void* addr, size_t size)> callback) const;

    /**
     * @brief Get statistics
     */
    LOSStats computeStats() const;

    size_t totalSize() const { return totalSize_; }
    size_t objectCount() const { return entries_.size(); }

private:
    size_t alignToPage(size_t size) const {
        return (size + pageSize_ - 1) & ~(pageSize_ - 1);
    }

    LOSConfig config_;
    size_t pageSize_;
    size_t totalSize_ = 0;
    size_t peakSize_ = 0;
    uint64_t totalAllocations_ = 0;
    uint64_t totalDeallocations_ = 0;

    std::vector<LOSEntry> entries_;
    mutable std::mutex mutex_;
};

// LOS Implementation

inline LargeObjectSpace::LargeObjectSpace(const LOSConfig& config)
    : config_(config) {
#ifdef __linux__
    pageSize_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#else
    pageSize_ = 4096;
#endif
}

inline LargeObjectSpace::~LargeObjectSpace() {
    for (auto& entry : entries_) {
#ifdef __linux__
        munmap(entry.baseAddr, entry.mappedSize);
#else
        std::free(entry.baseAddr);
#endif
    }
}

inline void* LargeObjectSpace::allocate(size_t size, uint32_t typeId) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (size > config_.maxObjectSize) return nullptr;
    if (totalSize_ + size > config_.maxTotalSize) return nullptr;

    size_t mappedSize = alignToPage(size);
    size_t guardSize = config_.useGuardPages ? pageSize_ : 0;
    size_t totalMapped = mappedSize + 2 * guardSize;

    void* base = nullptr;
#ifdef __linux__
    base = mmap(nullptr, totalMapped, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return nullptr;

    // Commit the middle portion
    void* objectBase = static_cast<char*>(base) + guardSize;
    if (mprotect(objectBase, mappedSize, PROT_READ | PROT_WRITE) != 0) {
        munmap(base, totalMapped);
        return nullptr;
    }
#else
    base = zepra_aligned_alloc(pageSize_, totalMapped);
    if (!base) return nullptr;
    void* objectBase = static_cast<char*>(base) + guardSize;
    std::memset(objectBase, 0, mappedSize);
#endif

    LOSEntry entry;
    entry.baseAddr = base;
    entry.objectAddr = objectBase;
    entry.objectSize = size;
    entry.mappedSize = totalMapped;
    entry.typeId = typeId;
    entry.marked = false;
    entry.allocTime = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    entry.gcSurvivalCount = 0;

    entries_.push_back(entry);
    totalSize_ += size;
    totalAllocations_++;

    if (totalSize_ > peakSize_) peakSize_ = totalSize_;

    return objectBase;
}

inline void LargeObjectSpace::free(void* addr) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->objectAddr == addr) {
            totalSize_ -= it->objectSize;
            totalDeallocations_++;

#ifdef __linux__
            munmap(it->baseAddr, it->mappedSize);
#else
            std::free(it->baseAddr);
#endif
            entries_.erase(it);
            return;
        }
    }
}

inline size_t LargeObjectSpace::sweep() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t freed = 0;

    auto it = entries_.begin();
    while (it != entries_.end()) {
        if (!it->marked) {
            freed += it->objectSize;
            totalSize_ -= it->objectSize;
            totalDeallocations_++;

#ifdef __linux__
            munmap(it->baseAddr, it->mappedSize);
#else
            std::free(it->baseAddr);
#endif
            it = entries_.erase(it);
        } else {
            it->marked = false;
            it->gcSurvivalCount++;
            ++it;
        }
    }

    return freed;
}

inline bool LargeObjectSpace::mark(void* addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        if (entry.objectAddr == addr) {
            bool wasMarked = entry.marked;
            entry.marked = true;
            return !wasMarked;
        }
    }
    return false;
}

inline bool LargeObjectSpace::isMarked(void* addr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (entry.objectAddr == addr) return entry.marked;
    }
    return false;
}

inline void LargeObjectSpace::clearMarks() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        entry.marked = false;
    }
}

inline bool LargeObjectSpace::contains(const void* addr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (addr >= entry.objectAddr &&
            addr < static_cast<const char*>(entry.objectAddr) + entry.objectSize) {
            return true;
        }
    }
    return false;
}

inline void LargeObjectSpace::forEach(
    std::function<void(void* addr, size_t size)> callback
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        callback(entry.objectAddr, entry.objectSize);
    }
}

inline LargeObjectSpace::LOSStats LargeObjectSpace::computeStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    LOSStats s;
    s.objectCount = entries_.size();
    s.totalBytes = totalSize_;
    s.peakBytes = peakSize_;
    s.totalAllocations = totalAllocations_;
    s.totalDeallocations = totalDeallocations_;

    for (const auto& entry : entries_) {
        if (entry.objectSize > s.largestObject) s.largestObject = entry.objectSize;
        if (entry.objectSize < s.smallestObject) s.smallestObject = entry.objectSize;
    }
    if (entries_.empty()) s.smallestObject = 0;

    return s;
}

// =============================================================================
// Code Space
// =============================================================================

/**
 * @brief Executable memory space for JIT-compiled code
 *
 * Manages memory with PROT_READ | PROT_EXEC permissions.
 * Uses W^X (write XOR execute) protection:
 * - Allocate as writable (PROT_READ | PROT_WRITE)
 * - After JIT compilation, switch to executable (PROT_READ | PROT_EXEC)
 * - Never both writable and executable simultaneously
 */
class CodeSpace {
public:
    struct CodeBlock {
        void* addr;
        size_t size;
        size_t usedSize;            // Code may not fill entire allocation
        bool isExecutable;
        bool isAlive;               // GC tracking
        const char* name;           // Debug name (e.g. function name)
    };

    struct CodeConfig {
        size_t initialSize;
        size_t maxSize;
        size_t blockAlignment;

        CodeConfig()
            : initialSize(4 * 1024 * 1024)
            , maxSize(128 * 1024 * 1024)
            , blockAlignment(64) {}
    };

    explicit CodeSpace(const CodeConfig& config = CodeConfig{});
    ~CodeSpace();

    CodeSpace(const CodeSpace&) = delete;
    CodeSpace& operator=(const CodeSpace&) = delete;

    /**
     * @brief Allocate writable code buffer
     */
    void* allocateWritable(size_t size);

    /**
     * @brief Make code region executable (W^X transition)
     */
    bool makeExecutable(void* addr, size_t size);

    /**
     * @brief Make code region writable again (for patching)
     */
    bool makeWritable(void* addr, size_t size);

    /**
     * @brief Free a code block
     */
    void freeBlock(void* addr);

    /**
     * @brief Check if address is in code space
     */
    bool contains(const void* addr) const;

    /**
     * @brief Flush instruction cache
     */
    static void flushICache(void* addr, size_t size);

    size_t totalSize() const { return totalMapped_; }
    size_t usedSize() const { return usedBytes_; }

private:
    CodeConfig config_;
    size_t pageSize_;
    size_t totalMapped_ = 0;
    size_t usedBytes_ = 0;

    struct MappedRegion {
        void* base;
        size_t size;
        char* cursor;
    };
    std::vector<MappedRegion> regions_;
    std::vector<CodeBlock> blocks_;
    mutable std::mutex mutex_;
};

inline CodeSpace::CodeSpace(const CodeConfig& config)
    : config_(config) {
#ifdef __linux__
    pageSize_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#else
    pageSize_ = 4096;
#endif
}

inline CodeSpace::~CodeSpace() {
    for (auto& region : regions_) {
#ifdef __linux__
        munmap(region.base, region.size);
#else
        std::free(region.base);
#endif
    }
}

inline void* CodeSpace::allocateWritable(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    size = (size + config_.blockAlignment - 1) & ~(config_.blockAlignment - 1);

    // Try existing regions
    for (auto& region : regions_) {
        size_t remaining = static_cast<size_t>(
            static_cast<char*>(region.base) + region.size - region.cursor);
        if (remaining >= size) {
            void* result = region.cursor;
            region.cursor += size;
            usedBytes_ += size;

            CodeBlock block;
            block.addr = result;
            block.size = size;
            block.usedSize = 0;
            block.isExecutable = false;
            block.isAlive = true;
            block.name = nullptr;
            blocks_.push_back(block);

            return result;
        }
    }

    // Allocate new region
    size_t regionSize = std::max(config_.initialSize, (size + pageSize_ - 1) & ~(pageSize_ - 1));
    if (totalMapped_ + regionSize > config_.maxSize) return nullptr;

    void* base = nullptr;
#ifdef __linux__
    base = mmap(nullptr, regionSize, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return nullptr;
#else
    base = zepra_aligned_alloc(pageSize_, regionSize);
    if (!base) return nullptr;
    std::memset(base, 0, regionSize);
#endif

    MappedRegion region;
    region.base = base;
    region.size = regionSize;
    region.cursor = static_cast<char*>(base) + size;
    regions_.push_back(region);
    totalMapped_ += regionSize;
    usedBytes_ += size;

    CodeBlock block;
    block.addr = base;
    block.size = size;
    block.usedSize = 0;
    block.isExecutable = false;
    block.isAlive = true;
    block.name = nullptr;
    blocks_.push_back(block);

    return base;
}

inline bool CodeSpace::makeExecutable(void* addr, size_t size) {
#ifdef __linux__
    // Align to page boundaries
    uintptr_t start = reinterpret_cast<uintptr_t>(addr) & ~(pageSize_ - 1);
    uintptr_t end = (reinterpret_cast<uintptr_t>(addr) + size + pageSize_ - 1) & ~(pageSize_ - 1);
    return mprotect(reinterpret_cast<void*>(start),
                    end - start, PROT_READ | PROT_EXEC) == 0;
#else
    (void)addr; (void)size;
    return true;
#endif
}

inline bool CodeSpace::makeWritable(void* addr, size_t size) {
#ifdef __linux__
    uintptr_t start = reinterpret_cast<uintptr_t>(addr) & ~(pageSize_ - 1);
    uintptr_t end = (reinterpret_cast<uintptr_t>(addr) + size + pageSize_ - 1) & ~(pageSize_ - 1);
    return mprotect(reinterpret_cast<void*>(start),
                    end - start, PROT_READ | PROT_WRITE) == 0;
#else
    (void)addr; (void)size;
    return true;
#endif
}

inline void CodeSpace::freeBlock(void* addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& block : blocks_) {
        if (block.addr == addr) {
            block.isAlive = false;
            break;
        }
    }
}

inline bool CodeSpace::contains(const void* addr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& region : regions_) {
        if (addr >= region.base &&
            addr < static_cast<const char*>(region.base) + region.size) {
            return true;
        }
    }
    return false;
}

inline void CodeSpace::flushICache(void* addr, size_t size) {
#ifdef __GNUC__
    __builtin___clear_cache(static_cast<char*>(addr),
                            static_cast<char*>(addr) + size);
#else
    (void)addr; (void)size;
#endif
}

// =============================================================================
// Card Table (for remembered set)
// =============================================================================

/**
 * @brief Card table for generational write barrier
 *
 * Divides old-gen into "cards" (512 bytes each).
 * When a write barrier fires (old→young reference stored),
 * the card containing the source is marked dirty.
 *
 * During minor GC, only dirty cards are scanned to find
 * old→young references, avoiding a full old-gen scan.
 *
 * Card size is chosen to balance:
 * - Overhead: 1 byte per 512 bytes = 0.2%
 * - Scanning: dirty cards are scanned fully
 */
class CardTable {
public:
    static constexpr size_t CARD_SIZE = 512;
    static constexpr uint8_t CLEAN = 0;
    static constexpr uint8_t DIRTY = 1;
    static constexpr uint8_t YOUNG = 2;     // Card in young gen

    CardTable() = default;

    void initialize(void* heapBase, size_t heapSize) {
        base_ = static_cast<char*>(heapBase);
        cardCount_ = (heapSize + CARD_SIZE - 1) / CARD_SIZE;
        cards_ = std::make_unique<std::atomic<uint8_t>[]>(cardCount_);
        for (size_t i = 0; i < cardCount_; i++) {
            cards_[i].store(CLEAN, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Mark the card containing addr as dirty
     */
    void markDirty(void* addr) {
        size_t card = cardIndex(addr);
        if (card < cardCount_) {
            cards_[card].store(DIRTY, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Check if card is dirty
     */
    bool isDirty(size_t cardIdx) const {
        if (cardIdx >= cardCount_) return false;
        return cards_[cardIdx].load(std::memory_order_relaxed) == DIRTY;
    }

    /**
     * @brief Clear a single card
     */
    void clearCard(size_t cardIdx) {
        if (cardIdx < cardCount_) {
            cards_[cardIdx].store(CLEAN, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Clear all cards
     */
    void clearAll() {
        for (size_t i = 0; i < cardCount_; i++) {
            cards_[i].store(CLEAN, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Iterate dirty cards and visit their address ranges
     */
    void forEachDirtyCard(std::function<void(void* start, void* end)> callback) {
        for (size_t i = 0; i < cardCount_; i++) {
            if (cards_[i].load(std::memory_order_relaxed) == DIRTY) {
                void* start = base_ + i * CARD_SIZE;
                void* end = base_ + (i + 1) * CARD_SIZE;
                callback(start, end);
            }
        }
    }

    /**
     * @brief Count dirty cards
     */
    size_t dirtyCount() const {
        size_t count = 0;
        for (size_t i = 0; i < cardCount_; i++) {
            if (cards_[i].load(std::memory_order_relaxed) == DIRTY) count++;
        }
        return count;
    }

    size_t cardCount() const { return cardCount_; }
    size_t cardIndex(const void* addr) const {
        return static_cast<size_t>(static_cast<const char*>(addr) - base_) / CARD_SIZE;
    }

    void* cardAddress(size_t cardIdx) const {
        return base_ + cardIdx * CARD_SIZE;
    }

private:
    char* base_ = nullptr;
    size_t cardCount_ = 0;
    std::unique_ptr<std::atomic<uint8_t>[]> cards_;
};

// =============================================================================
// Unified Allocator
// =============================================================================

/**
 * @brief Top-level allocator integrating all allocation paths
 */
class UnifiedAllocator {
public:
    struct AllocatorConfig {
        size_t nurserySize;
        size_t oldGenInitialSize;
        size_t losThreshold;
        size_t codeSpaceSize;
        size_t tlabSize;
        size_t maxHeapSize;

        AllocatorConfig()
            : nurserySize(2 * 1024 * 1024)
            , oldGenInitialSize(16 * 1024 * 1024)
            , losThreshold(8 * 1024)
            , codeSpaceSize(4 * 1024 * 1024)
            , tlabSize(32 * 1024)
            , maxHeapSize(512 * 1024 * 1024) {}
    };

    struct AllocationStats {
        uint64_t nurseryAllocations = 0;
        uint64_t nurseryBytes = 0;
        uint64_t oldGenAllocations = 0;
        uint64_t oldGenBytes = 0;
        uint64_t losAllocations = 0;
        uint64_t losBytes = 0;
        uint64_t codeAllocations = 0;
        uint64_t codeBytes = 0;
        uint64_t allocationFailures = 0;
        uint64_t gcTriggered = 0;
    };

    explicit UnifiedAllocator(const AllocatorConfig& config = AllocatorConfig{});
    ~UnifiedAllocator();

    UnifiedAllocator(const UnifiedAllocator&) = delete;
    UnifiedAllocator& operator=(const UnifiedAllocator&) = delete;

    /**
     * @brief Primary allocation entry point
     */
    void* allocate(size_t size, AllocationPolicy policy = AllocationPolicy::Default);

    /**
     * @brief Allocate executable memory
     */
    void* allocateCode(size_t size);

    /**
     * @brief Report allocation failure (triggers GC)
     */
    using GCTriggerFn = std::function<void(void)>;
    void setGCTrigger(GCTriggerFn trigger) { gcTrigger_ = std::move(trigger); }

    /**
     * @brief Access sub-allocators
     */
    LargeObjectSpace& los() { return los_; }
    CodeSpace& codeSpace() { return codeSpace_; }
    CardTable& cardTable() { return cardTable_; }

    const AllocationStats& stats() const { return stats_; }

    /**
     * @brief Total allocated memory
     */
    size_t totalAllocated() const;

    /**
     * @brief Check if address is in managed heap
     */
    bool isHeapAddress(const void* addr) const;

private:
    void* allocateNurseryFastPath(size_t size);
    void* allocateOldGenFastPath(size_t size);
    void* allocateSlowPath(size_t size, AllocationPolicy policy);

    AllocatorConfig config_;
    LargeObjectSpace los_;
    CodeSpace codeSpace_;
    CardTable cardTable_;

    // Nursery bump
    std::atomic<char*> nurseryCursor_{nullptr};
    char* nurseryLimit_ = nullptr;
    char* nurseryBase_ = nullptr;

    AllocationStats stats_;
    GCTriggerFn gcTrigger_;
    mutable std::mutex slowPathMutex_;
};

inline UnifiedAllocator::UnifiedAllocator(const AllocatorConfig& config)
    : config_(config) {
    // Allocate nursery
    nurseryBase_ = static_cast<char*>(zepra_aligned_alloc(4096, config.nurserySize));
    if (nurseryBase_) {
        std::memset(nurseryBase_, 0, config.nurserySize);
        nurseryCursor_ = nurseryBase_;
        nurseryLimit_ = nurseryBase_ + config.nurserySize;
    }
}

inline UnifiedAllocator::~UnifiedAllocator() {
    if (nurseryBase_) std::free(nurseryBase_);
}

inline void* UnifiedAllocator::allocate(size_t size, AllocationPolicy policy) {
    size = (size + 7) & ~size_t(7);

    // Route based on policy
    switch (policy) {
        case AllocationPolicy::LargeObject:
            stats_.losAllocations++;
            stats_.losBytes += size;
            return los_.allocate(size);

        case AllocationPolicy::Executable:
            stats_.codeAllocations++;
            stats_.codeBytes += size;
            return codeSpace_.allocateWritable(size);

        case AllocationPolicy::PreTenured:
        case AllocationPolicy::Pinned:
            return allocateOldGenFastPath(size);

        default:
            break;
    }

    // Size-based routing
    if (size >= config_.losThreshold) {
        stats_.losAllocations++;
        stats_.losBytes += size;
        return los_.allocate(size);
    }

    // Try nursery fast path
    void* result = allocateNurseryFastPath(size);
    if (result) {
        stats_.nurseryAllocations++;
        stats_.nurseryBytes += size;
        return result;
    }

    // Slow path
    return allocateSlowPath(size, policy);
}

inline void* UnifiedAllocator::allocateCode(size_t size) {
    return allocate(size, AllocationPolicy::Executable);
}

inline void* UnifiedAllocator::allocateNurseryFastPath(size_t size) {
    char* result = nurseryCursor_.load(std::memory_order_relaxed);
    char* newCursor = result + size;

    if (newCursor > nurseryLimit_) return nullptr;

    while (!nurseryCursor_.compare_exchange_weak(result, newCursor,
            std::memory_order_relaxed)) {
        newCursor = result + size;
        if (newCursor > nurseryLimit_) return nullptr;
    }

    return result;
}

inline void* UnifiedAllocator::allocateOldGenFastPath(size_t /*size*/) {
    // Would route to SizeClassAllocator TLABs
    // Placeholder for integration
    return nullptr;
}

inline void* UnifiedAllocator::allocateSlowPath(size_t size,
                                                  AllocationPolicy policy) {
    std::lock_guard<std::mutex> lock(slowPathMutex_);

    stats_.allocationFailures++;

    // Try GC
    if (gcTrigger_) {
        gcTrigger_();
        stats_.gcTriggered++;

        // Retry nursery
        if (policy == AllocationPolicy::Default ||
            policy == AllocationPolicy::Transient) {
            void* result = allocateNurseryFastPath(size);
            if (result) return result;
        }
    }

    // Fall back to old gen
    stats_.oldGenAllocations++;
    stats_.oldGenBytes += size;
    return allocateOldGenFastPath(size);
}

inline size_t UnifiedAllocator::totalAllocated() const {
    return stats_.nurseryBytes + stats_.oldGenBytes +
           stats_.losBytes + stats_.codeBytes;
}

inline bool UnifiedAllocator::isHeapAddress(const void* addr) const {
    // Check nursery
    if (addr >= nurseryBase_ && addr < nurseryLimit_) return true;
    // Check LOS
    if (los_.contains(addr)) return true;
    // Check code space
    if (codeSpace_.contains(addr)) return true;
    return false;
}

} // namespace Zepra::Heap

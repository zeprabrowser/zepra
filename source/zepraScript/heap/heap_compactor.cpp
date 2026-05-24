// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file heap_compactor.cpp
 * @brief Page-level evacuation compactor
 *
 * This is the heavy-duty compaction engine that works at the page level.
 * Unlike the incremental compactor (which moves individual objects over
 * multiple cycles), this compactor evacuates entire pages in one pass.
 *
 * Algorithm:
 * 1. After marking, compute per-page live byte counts
 * 2. Sort pages by fragmentation (most sparse first)
 * 3. For each selected source page:
 *    a. Allocate destination space in target pages
 *    b. Copy all live objects (iterate using mark bitmap)
 *    c. Install forwarding pointers at old locations
 * 4. Fix all references (roots + heap) using forwarding table
 * 5. Release evacuated pages via madvise(DONTNEED)
 *
 * Page memory management:
 * - Pages are 256KB aligned regions
 * - Free pages are returned to OS with madvise(DONTNEED), not munmap
 * - This preserves the virtual address space but frees physical memory
 * - Pages are recommitted on first touch (lazy allocation)
 *
 * Parallelism:
 * - Step 3 (copying) can be parallelized per-page
 * - Step 4 (fixup) is parallelized by splitting the heap into ranges
 */

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <memory>
#include <algorithm>
#include <numeric>
#include <unordered_map>

#ifdef __linux__
#include <sys/mman.h>
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#endif

namespace Zepra::Heap {

// =============================================================================
// Page Descriptor
// =============================================================================

static constexpr size_t PAGE_SIZE_SHIFT = 18;   // 256KB
static constexpr size_t GC_PAGE_SIZE = 1 << PAGE_SIZE_SHIFT;

struct PageDescriptor {
    uintptr_t baseAddr;
    size_t pageSize;
    uint32_t pageIndex;

    // Live data (populated by marking phase)
    size_t liveBytes;
    size_t liveObjects;

    // Derived
    double occupancy() const {
        return pageSize > 0
            ? static_cast<double>(liveBytes) / static_cast<double>(pageSize)
            : 0;
    }
    size_t wastedBytes() const {
        return pageSize > liveBytes ? pageSize - liveBytes : 0;
    }

    // Flags
    bool pinned;        // Page contains pinned objects
    bool isCode;        // Page contains JIT code
    bool isLargeObject; // Page is for large object space
    bool selected;      // Selected for evacuation this cycle

    PageDescriptor()
        : baseAddr(0), pageSize(GC_PAGE_SIZE), pageIndex(0)
        , liveBytes(0), liveObjects(0)
        , pinned(false), isCode(false), isLargeObject(false)
        , selected(false) {}
};

// =============================================================================
// Forwarding Table
// =============================================================================

/**
 * @brief Maps old addresses to new addresses after evacuation
 *
 * Two implementations:
 * 1. Hash map: general purpose, handles any layout
 * 2. In-object: overwrite the object header with forwarding pointer
 *    (used during STW when it's safe to mutate dead copies)
 *
 * This implementation uses the hash map approach for safety.
 * The in-object approach is used by the scavenger for nursery objects.
 */
class ForwardingTable {
public:
    void reserve(size_t expectedEntries) {
        table_.reserve(expectedEntries);
    }

    void insert(uintptr_t oldAddr, uintptr_t newAddr) {
        table_[oldAddr] = newAddr;
    }

    uintptr_t lookup(uintptr_t addr) const {
        auto it = table_.find(addr);
        return it != table_.end() ? it->second : 0;
    }

    bool contains(uintptr_t addr) const {
        return table_.count(addr) > 0;
    }

    /**
     * @brief Update a pointer if it points to a forwarded object
     * @return true if the pointer was updated
     */
    bool updatePointer(void** slot) const {
        if (!slot || !*slot) return false;
        auto addr = reinterpret_cast<uintptr_t>(*slot);
        auto newAddr = lookup(addr);
        if (newAddr != 0) {
            *slot = reinterpret_cast<void*>(newAddr);
            return true;
        }
        return false;
    }

    size_t size() const { return table_.size(); }
    void clear() { table_.clear(); }

private:
    std::unordered_map<uintptr_t, uintptr_t> table_;
};

// =============================================================================
// Live Object Iterator
// =============================================================================

/**
 * @brief Iterates live objects in a page using the mark bitmap
 *
 * The mark bitmap has one bit per 8-byte cell in the page.
 * A set bit means the cell is the start of a live object.
 * Object size is obtained via a callback.
 */
class LiveObjectIterator {
public:
    using SizeFn = std::function<size_t(uintptr_t objAddr)>;

    LiveObjectIterator(uintptr_t pageBase, size_t pageSize,
                       const uint8_t* markBitmap, SizeFn sizeFn)
        : pageBase_(pageBase), pageSize_(pageSize)
        , bitmap_(markBitmap), sizeFn_(std::move(sizeFn)) {}

    /**
     * @brief Visit each live object in the page
     */
    void forEach(std::function<void(uintptr_t addr, size_t size)> visitor) {
        if (!bitmap_ || !sizeFn_) return;

        size_t cellCount = pageSize_ / 8;  // 8 bytes per cell
        size_t bitmapBytes = (cellCount + 7) / 8;

        for (size_t byteIdx = 0; byteIdx < bitmapBytes; byteIdx++) {
            uint8_t byte = bitmap_[byteIdx];
            if (byte == 0) continue;

            for (int bit = 0; bit < 8; bit++) {
                if (byte & (1 << bit)) {
                    size_t cellIdx = byteIdx * 8 + bit;
                    uintptr_t objAddr = pageBase_ + cellIdx * 8;

                    if (objAddr >= pageBase_ + pageSize_) return;

                    size_t objSize = sizeFn_(objAddr);
                    if (objSize > 0) {
                        visitor(objAddr, objSize);
                    }
                }
            }
        }
    }

private:
    uintptr_t pageBase_;
    size_t pageSize_;
    const uint8_t* bitmap_;
    SizeFn sizeFn_;
};

// =============================================================================
// Heap Compactor
// =============================================================================

class HeapCompactor {
public:
    struct Config {
        double evacuationThreshold;     // Evacuate pages below this occupancy
        size_t maxPagesToEvacuate;       // Limit per cycle
        size_t parallelWorkers;         // Fixup worker threads
        size_t minWastedBytesToCompact; // Don't compact if wasted < this

        Config()
            : evacuationThreshold(0.65)
            , maxPagesToEvacuate(32)
            , parallelWorkers(2)
            , minWastedBytesToCompact(64 * 1024) {}
    };

    struct Callbacks {
        // Get all page descriptors
        std::function<std::vector<PageDescriptor>()> getPages;

        // Get mark bitmap for a page
        std::function<const uint8_t*(uint32_t pageIndex)> getMarkBitmap;

        // Get object size given address
        std::function<size_t(uintptr_t addr)> objectSize;

        // Allocate space in a target page
        std::function<uintptr_t(size_t size)> allocate;

        // Enumerate roots for fixup
        std::function<void(std::function<void(void** slot)>)> enumerateRoots;

        // Trace object fields for fixup
        std::function<void(uintptr_t obj , std::function<void(void** slot)>)> traceObject;

        // Release a page back to OS
        std::function<void(uint32_t pageIndex)> releasePage;
    };

    struct Stats {
        size_t pagesEvaluated;
        size_t pagesEvacuated;
        size_t pagesReleased;
        size_t objectsMoved;
        size_t bytesMoved;
        size_t referencesFixedUp;
        double evacuationMs;
        double fixupMs;
        double releaseMs;
        double totalMs;
        double fragmentationBefore;
        double fragmentationAfter;
    };

    explicit HeapCompactor(const Config& config = Config{})
        : config_(config) {}

    void setCallbacks(Callbacks callbacks) { cb_ = std::move(callbacks); }

    /**
     * @brief Run full compaction cycle
     */
    Stats compact();

private:
    // Steps
    std::vector<PageDescriptor> evaluatePages(Stats& stats);
    std::vector<PageDescriptor*> selectEvacuationCandidates(
        std::vector<PageDescriptor>& pages, Stats& stats);
    void evacuatePages(const std::vector<PageDescriptor*>& candidates,
                       Stats& stats);
    void fixupAllReferences(Stats& stats);
    void releaseEvacuatedPages(const std::vector<PageDescriptor*>& candidates,
                                Stats& stats);

    Config config_;
    Callbacks cb_;
    ForwardingTable forwarding_;
};

// =============================================================================
// Implementation
// =============================================================================

inline HeapCompactor::Stats HeapCompactor::compact() {
    Stats stats{};
    auto startTime = std::chrono::steady_clock::now();

    forwarding_.clear();

    // 1. Evaluate pages
    auto pages = evaluatePages(stats);

    // 2. Select candidates
    auto candidates = selectEvacuationCandidates(pages, stats);

    if (candidates.empty()) {
        stats.totalMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        return stats;
    }

    // 3. Evacuate (copy live objects out)
    auto evacStart = std::chrono::steady_clock::now();
    evacuatePages(candidates, stats);
    stats.evacuationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - evacStart).count();

    // 4. Fix all references
    auto fixStart = std::chrono::steady_clock::now();
    fixupAllReferences(stats);
    stats.fixupMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - fixStart).count();

    // 5. Release evacuated pages
    auto relStart = std::chrono::steady_clock::now();
    releaseEvacuatedPages(candidates, stats);
    stats.releaseMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - relStart).count();

    stats.totalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();

    return stats;
}

inline std::vector<PageDescriptor> HeapCompactor::evaluatePages(
    Stats& stats
) {
    if (!cb_.getPages) return {};
    auto pages = cb_.getPages();
    stats.pagesEvaluated = pages.size();

    // Compute fragmentation before compaction
    size_t totalLive = 0, totalCapacity = 0;
    for (const auto& p : pages) {
        totalLive += p.liveBytes;
        totalCapacity += p.pageSize;
    }
    stats.fragmentationBefore = totalCapacity > 0
        ? 1.0 - static_cast<double>(totalLive) / static_cast<double>(totalCapacity)
        : 0;

    return pages;
}

inline std::vector<PageDescriptor*> HeapCompactor::selectEvacuationCandidates(
    std::vector<PageDescriptor>& pages, Stats& stats
) {
    // Sort by occupancy ascending (most fragmented first)
    std::vector<size_t> indices(pages.size());
    std::iota(indices.begin(), indices.end(), 0);

    std::sort(indices.begin(), indices.end(),
        [&pages](size_t a, size_t b) {
            return pages[a].occupancy() < pages[b].occupancy();
        });

    std::vector<PageDescriptor*> candidates;

    for (size_t idx : indices) {
        auto& page = pages[idx];

        // Skip non-evacuatable pages
        if (page.pinned || page.isCode || page.isLargeObject) continue;
        if (page.liveBytes == 0) {
            // Empty page — just release, no evacuation needed
            page.selected = true;
            candidates.push_back(&page);
            stats.pagesEvacuated++;
            continue;
        }

        if (page.occupancy() >= config_.evacuationThreshold) break;
        if (page.wastedBytes() < config_.minWastedBytesToCompact) continue;

        if (candidates.size() >= config_.maxPagesToEvacuate) break;

        page.selected = true;
        candidates.push_back(&page);
        stats.pagesEvacuated++;
    }

    return candidates;
}

inline void HeapCompactor::evacuatePages(
    const std::vector<PageDescriptor*>& candidates, Stats& stats
) {
    if (!cb_.getMarkBitmap || !cb_.objectSize || !cb_.allocate) return;

    for (const auto* page : candidates) {
        if (page->liveBytes == 0) continue;  // Empty page, skip

        const uint8_t* bitmap = cb_.getMarkBitmap(page->pageIndex);
        if (!bitmap) continue;

        LiveObjectIterator iter(
            page->baseAddr, page->pageSize,
            bitmap, cb_.objectSize);

        iter.forEach([&](uintptr_t oldAddr, size_t objSize) {
            // Allocate in some non-evacuated page
            uintptr_t newAddr = cb_.allocate(objSize);
            if (newAddr == 0) return;  // Allocation failure

            // Copy the object
            std::memcpy(reinterpret_cast<void*>(newAddr),
                        reinterpret_cast<const void*>(oldAddr),
                        objSize);

            // Record forwarding
            forwarding_.insert(oldAddr, newAddr);

            stats.objectsMoved++;
            stats.bytesMoved += objSize;
        });
    }

    forwarding_.reserve(stats.objectsMoved);
}

inline void HeapCompactor::fixupAllReferences(Stats& stats) {
    if (forwarding_.size() == 0) return;

    auto updateSlot = [&](void** slot) {
        if (forwarding_.updatePointer(slot)) {
            stats.referencesFixedUp++;
        }
    };

    // Fix roots
    if (cb_.enumerateRoots) {
        cb_.enumerateRoots(updateSlot);
    }

    // Fix heap references
    // For production quality we'd iterate all live objects on non-evacuated
    // pages and fix their reference fields. This requires traceObject callback.
    if (cb_.getPages && cb_.traceObject) {
        auto pages = cb_.getPages();
        for (const auto& page : pages) {
            if (page.selected) continue;  // Don't scan evacuated pages
            if (page.liveBytes == 0) continue;

            const uint8_t* bitmap = cb_.getMarkBitmap
                ? cb_.getMarkBitmap(page.pageIndex) : nullptr;
            if (!bitmap) continue;

            LiveObjectIterator iter(
                page.baseAddr, page.pageSize,
                bitmap, cb_.objectSize);

            iter.forEach([&](uintptr_t objAddr, size_t /*size*/) {
                cb_.traceObject(objAddr, updateSlot);
            });
        }
    }
}

inline void HeapCompactor::releaseEvacuatedPages(
    const std::vector<PageDescriptor*>& candidates, Stats& stats
) {
    for (const auto* page : candidates) {
        if (cb_.releasePage) {
            cb_.releasePage(page->pageIndex);
            stats.pagesReleased++;
        }

#ifdef __linux__
        // Also advise the kernel to free the physical memory
        madvise(reinterpret_cast<void*>(page->baseAddr),
                page->pageSize, MADV_DONTNEED);
#endif
    }
}

} // namespace Zepra::Heap

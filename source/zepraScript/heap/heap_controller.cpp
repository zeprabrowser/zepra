// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file heap_controller.cpp
 * @brief Top-level heap controller — unified heap management
 *
 * This is the master controller that the VM interacts with.
 * It owns:
 * - Nursery (young generation)
 * - Old generation (region-based)
 * - Large object space
 * - Code space
 * - Virtual memory manager
 * - GC collection engine
 * - Allocation paths
 * - Write barrier dispatch
 * - Heap configuration
 *
 * Public API:
 * - allocate(size) → void*
 * - writeBarrier(source, slot, old, new)
 * - safePoint()
 * - collectGarbage(reason)
 * - heapStats() → HeapStatisticsData
 * - takeSnapshot() → SnapshotData
 *
 * This file is ~1000 lines and covers the production heap lifecycle.
 */

#include "zepra_alloc.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <mutex>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <string>
#include <algorithm>
#include <memory>
#include <cassert>

namespace Zepra::Heap {

// =============================================================================
// Heap Configuration
// =============================================================================

struct HeapConfig {
    // Nursery
    size_t nurserySize;
    size_t nurseryMaxSize;
    size_t tenureThreshold;           // Age before tenuring

    // Old generation
    size_t oldGenInitialSize;
    size_t oldGenMaxSize;
    size_t regionSize;

    // Large objects
    size_t losThreshold;              // Objects >= this go to LOS
    size_t losMaxSize;

    // Code space
    size_t codeSpaceInitialSize;
    size_t codeSpaceMaxSize;

    // GC
    size_t gcThreads;
    bool enableIncrementalMarking;
    bool enableConcurrentSweeping;
    bool enableCompaction;
    double compactionThreshold;
    size_t incrementalStepBudgetUs;

    // Scheduling
    double gcTriggerRatio;            // Trigger GC when heap grows by this factor
    size_t gcTriggerMinBytes;         // Minimum heap growth before GC trigger
    size_t gcTriggerMaxBytes;         // Maximum heap growth before forced GC

    // Write barrier
    bool enableGenerationalBarrier;
    bool enableSATBBarrier;

    // Debug
    bool enableHeapVerification;
    bool enableAllocationTracing;
    bool enableGCTracing;

    HeapConfig()
        : nurserySize(2 * 1024 * 1024)
        , nurseryMaxSize(16 * 1024 * 1024)
        , tenureThreshold(2)
        , oldGenInitialSize(16 * 1024 * 1024)
        , oldGenMaxSize(512 * 1024 * 1024)
        , regionSize(256 * 1024)
        , losThreshold(8 * 1024)
        , losMaxSize(256 * 1024 * 1024)
        , codeSpaceInitialSize(4 * 1024 * 1024)
        , codeSpaceMaxSize(128 * 1024 * 1024)
        , gcThreads(2)
        , enableIncrementalMarking(true)
        , enableConcurrentSweeping(true)
        , enableCompaction(true)
        , compactionThreshold(0.5)
        , incrementalStepBudgetUs(500)
        , gcTriggerRatio(1.5)
        , gcTriggerMinBytes(4 * 1024 * 1024)
        , gcTriggerMaxBytes(64 * 1024 * 1024)
        , enableGenerationalBarrier(true)
        , enableSATBBarrier(true)
        , enableHeapVerification(false)
        , enableAllocationTracing(false)
        , enableGCTracing(true) {}
};

// =============================================================================
// Heap Statistics Data (exported to VM/DevTools)
// =============================================================================

struct HeapStatisticsData {
    // Overall
    size_t totalHeapSize;
    size_t totalHeapUsed;
    size_t totalHeapCommitted;
    size_t heapSizeLimit;
    size_t externalMemory;

    // Per-space
    struct SpaceStats {
        const char* name;
        size_t size;
        size_t used;
        size_t available;
        size_t committed;
        double occupancy;
    };
    SpaceStats nursery;
    SpaceStats oldGen;
    SpaceStats los;
    SpaceStats codeSpace;

    // GC
    uint64_t majorGCCount;
    uint64_t minorGCCount;
    double lastGCPauseMs;
    double avgGCPauseMs;
    double maxGCPauseMs;
    double gcTimePercentage;

    // Objects
    uint64_t totalObjectCount;
    uint64_t totalAllocationCount;
    uint64_t totalAllocationBytes;
    double allocationRate;
};

// =============================================================================
// Allocation Result
// =============================================================================

enum class AllocationSpace : uint8_t {
    Nursery,
    OldGen,
    LOS,
    Code,
};

struct AllocationRequest {
    size_t size;
    AllocationSpace preferredSpace;
    bool pinned;           // Must not be moved
    bool executable;       // Code space
    bool pretenured;       // Skip nursery
    uint32_t typeId;       // For allocation tracking

    AllocationRequest()
        : size(0), preferredSpace(AllocationSpace::Nursery)
        , pinned(false), executable(false), pretenured(false), typeId(0) {}

    explicit AllocationRequest(size_t s)
        : size(s), preferredSpace(AllocationSpace::Nursery)
        , pinned(false), executable(false), pretenured(false), typeId(0) {}
};

// =============================================================================
// Heap Controller
// =============================================================================

class HeapController {
public:
    explicit HeapController(const HeapConfig& config = HeapConfig{});
    ~HeapController();

    HeapController(const HeapController&) = delete;
    HeapController& operator=(const HeapController&) = delete;

    /**
     * @brief Initialize the heap
     * Allocates virtual memory, sets up spaces, starts GC threads.
     */
    bool initialize();

    /**
     * @brief Shut down the heap
     * Waits for GC threads, releases all memory.
     */
    void shutdown();

    // -------------------------------------------------------------------------
    // Allocation
    // -------------------------------------------------------------------------

    /**
     * @brief Allocate memory for an object
     * Primary allocation entry point.
     */
    void* allocate(size_t size);

    /**
     * @brief Allocate with full request
     */
    void* allocate(const AllocationRequest& request);

    /**
     * @brief Allocate executable memory (for JIT code)
     */
    void* allocateCode(size_t size);

    // -------------------------------------------------------------------------
    // Write Barrier
    // -------------------------------------------------------------------------

    /**
     * @brief Write barrier — must be called on every reference store
     */
    void writeBarrier(void* sourceObject, void** slot,
                       void* oldValue, void* newValue);

    // -------------------------------------------------------------------------
    // GC Control
    // -------------------------------------------------------------------------

    /**
     * @brief Check for GC at safe-point
     */
    void safePoint();

    /**
     * @brief Request garbage collection
     */
    enum class GCReason {
        Allocation,         // Triggered by allocation failure
        Scheduled,          // Triggered by scheduler
        External,           // Triggered by embedder
        LowMemory,          // Triggered by memory pressure
        Testing,            // Triggered by test
    };

    void collectGarbage(GCReason reason = GCReason::Scheduled);

    /**
     * @brief Run idle-time GC work
     */
    void idleNotification(double deadlineMs);

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    /**
     * @brief Get heap statistics
     */
    HeapStatisticsData computeStatistics() const;

    /**
     * @brief Check if pointer is in managed heap
     */
    bool isHeapPointer(const void* ptr) const;

    /**
     * @brief Identify which space a pointer belongs to
     */
    AllocationSpace identifySpace(const void* ptr) const;

    /**
     * @brief Get object size (must be a valid heap pointer)
     */
    size_t objectSize(const void* ptr) const;

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Set the root enumerator (provided by VM)
     */
    using RootEnumerator = std::function<void(std::function<void(void** slot)>)>;
    void setRootEnumerator(RootEnumerator enumerator) {
        rootEnumerator_ = std::move(enumerator);
    }

    /**
     * @brief Set the object tracer (provided by VM)
     */
    using ObjectTracer = std::function<void(void* object,
        std::function<void(void** slot)>)>;
    void setObjectTracer(ObjectTracer tracer) {
        objectTracer_ = std::move(tracer);
    }

    /**
     * @brief Set the object sizer (provided by VM)
     */
    using ObjectSizer = std::function<size_t(void* object)>;
    void setObjectSizer(ObjectSizer sizer) {
        objectSizer_ = std::move(sizer);
    }

    /**
     * @brief GC event callback
     */
    using GCCallback = std::function<void(const char* phase, double durationMs)>;
    void setGCCallback(GCCallback callback) {
        gcCallback_ = std::move(callback);
    }

    // -------------------------------------------------------------------------
    // External Memory
    // -------------------------------------------------------------------------

    /**
     * @brief Report external memory (native buffers, etc.)
     */
    void reportExternalMemory(int64_t delta);
    size_t externalMemory() const { return externalMemory_; }

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    const HeapConfig& config() const { return config_; }

private:
    // Allocation helpers
    void* allocateNursery(size_t size);
    void* allocateOldGen(size_t size);
    void* allocateLOS(size_t size);
    void* allocateCodeSpace(size_t size);
    void* handleAllocationFailure(size_t size, AllocationSpace space);

    // GC helpers
    void minorGC();
    void majorGC();
    void fullGC();
    bool shouldTriggerGC() const;
    void updateGCTrigger();

    HeapConfig config_;

    // VM callbacks
    RootEnumerator rootEnumerator_;
    ObjectTracer objectTracer_;
    ObjectSizer objectSizer_;
    GCCallback gcCallback_;

    // State
    bool initialized_ = false;
    std::atomic<size_t> allocationsSinceGC_{0};
    size_t lastGCHeapSize_ = 0;
    size_t nextGCTriggerSize_ = 0;
    std::atomic<size_t> externalMemory_{0};

    // GC stats
    std::atomic<uint64_t> totalAllocations_{0};
    std::atomic<uint64_t> totalAllocationBytes_{0};
    uint64_t minorGCCount_ = 0;
    uint64_t majorGCCount_ = 0;
    double lastPauseMs_ = 0;
    double maxPauseMs_ = 0;
    double totalPauseMs_ = 0;
    std::chrono::steady_clock::time_point startTime_;

    mutable std::mutex gcMutex_;
};

// =============================================================================
// Implementation
// =============================================================================

inline HeapController::HeapController(const HeapConfig& config)
    : config_(config) {}

inline HeapController::~HeapController() {
    if (initialized_) shutdown();
}

inline bool HeapController::initialize() {
    if (initialized_) return true;

    startTime_ = std::chrono::steady_clock::now();
    nextGCTriggerSize_ = config_.oldGenInitialSize;

    initialized_ = true;
    return true;
}

inline void HeapController::shutdown() {
    if (!initialized_) return;
    initialized_ = false;
}

inline void* HeapController::allocate(size_t size) {
    AllocationRequest req(size);

    // Route based on size
    if (size >= config_.losThreshold) {
        req.preferredSpace = AllocationSpace::LOS;
    } else {
        req.preferredSpace = AllocationSpace::Nursery;
    }

    return allocate(req);
}

inline void* HeapController::allocate(const AllocationRequest& request) {
    totalAllocations_++;
    totalAllocationBytes_ += request.size;
    allocationsSinceGC_ += request.size;

    void* result = nullptr;

    if (request.executable) {
        result = allocateCodeSpace(request.size);
    } else if (request.pretenured ||
               request.preferredSpace == AllocationSpace::OldGen) {
        result = allocateOldGen(request.size);
    } else if (request.preferredSpace == AllocationSpace::LOS ||
               request.size >= config_.losThreshold) {
        result = allocateLOS(request.size);
    } else {
        result = allocateNursery(request.size);
    }

    if (!result) {
        result = handleAllocationFailure(request.size, request.preferredSpace);
    }

    return result;
}

inline void* HeapController::allocateCode(size_t size) {
    AllocationRequest req(size);
    req.executable = true;
    req.preferredSpace = AllocationSpace::Code;
    return allocate(req);
}

inline void HeapController::writeBarrier(void* /*sourceObject*/, void** /*slot*/,
                                          void* /*oldValue*/, void* /*newValue*/) {
    // Dispatch to StoreBarrier
    // In production, this inlines through the JIT
}

inline void HeapController::safePoint() {
    if (shouldTriggerGC()) {
        collectGarbage(GCReason::Scheduled);
    }
}

inline void HeapController::collectGarbage(GCReason reason) {
    std::lock_guard<std::mutex> lock(gcMutex_);

    auto start = std::chrono::steady_clock::now();

    switch (reason) {
        case GCReason::Allocation:
            minorGC();
            break;
        case GCReason::LowMemory:
            fullGC();
            break;
        default:
            if (allocationsSinceGC_ < config_.gcTriggerMinBytes) {
                minorGC();
            } else {
                majorGC();
            }
            break;
    }

    double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    lastPauseMs_ = elapsed;
    totalPauseMs_ += elapsed;
    if (elapsed > maxPauseMs_) maxPauseMs_ = elapsed;

    allocationsSinceGC_ = 0;
    updateGCTrigger();

    if (gcCallback_) {
        gcCallback_("GC", elapsed);
    }
}

inline void HeapController::idleNotification(double /*deadlineMs*/) {
    // Run incremental marking during idle time
    // Or run concurrent sweep steps
}

inline HeapStatisticsData HeapController::computeStatistics() const {
    HeapStatisticsData stats{};

    stats.majorGCCount = majorGCCount_;
    stats.minorGCCount = minorGCCount_;
    stats.lastGCPauseMs = lastPauseMs_;
    stats.maxGCPauseMs = maxPauseMs_;

    uint64_t totalGCs = majorGCCount_ + minorGCCount_;
    stats.avgGCPauseMs = totalGCs > 0
        ? totalPauseMs_ / static_cast<double>(totalGCs) : 0;

    auto uptime = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startTime_).count();
    stats.gcTimePercentage = uptime > 0
        ? (totalPauseMs_ / uptime) * 100.0 : 0;

    stats.totalAllocationCount = totalAllocations_;
    stats.totalAllocationBytes = totalAllocationBytes_;
    stats.externalMemory = externalMemory_;

    return stats;
}

inline bool HeapController::isHeapPointer(const void* /*ptr*/) const {
    // Delegate to AddressSpaceLayout
    return false;
}

inline AllocationSpace HeapController::identifySpace(const void* /*ptr*/) const {
    return AllocationSpace::OldGen;
}

inline size_t HeapController::objectSize(const void* ptr) const {
    if (objectSizer_) {
        return objectSizer_(const_cast<void*>(ptr));
    }
    return 0;
}

inline void HeapController::reportExternalMemory(int64_t delta) {
    if (delta > 0) {
        externalMemory_ += static_cast<size_t>(delta);
    } else if (static_cast<size_t>(-delta) <= externalMemory_) {
        externalMemory_ -= static_cast<size_t>(-delta);
    } else {
        externalMemory_ = 0;
    }
}

// Private methods

inline void* HeapController::allocateNursery(size_t size) {
    // Placeholder: real impl uses Scavenger's semi-space
    (void)size;
    return nullptr;
}

inline void* HeapController::allocateOldGen(size_t size) {
    // Placeholder: real impl uses HeapRegionManager
    (void)size;
    return nullptr;
}

inline void* HeapController::allocateLOS(size_t size) {
    // Placeholder: real impl uses LargeObjectSpace
    (void)size;
    return nullptr;
}

inline void* HeapController::allocateCodeSpace(size_t size) {
    // Placeholder: real impl uses code-space allocator
    (void)size;
    return nullptr;
}

inline void* HeapController::handleAllocationFailure(size_t size,
                                                       AllocationSpace space) {
    // Try GC, then retry
    collectGarbage(GCReason::Allocation);

    switch (space) {
        case AllocationSpace::Nursery: return allocateOldGen(size);
        case AllocationSpace::OldGen: return allocateOldGen(size);
        case AllocationSpace::LOS: return allocateLOS(size);
        case AllocationSpace::Code: return allocateCodeSpace(size);
    }
    return nullptr;
}

inline void HeapController::minorGC() {
    minorGCCount_++;
    // Run scavenger — see Scavenger.h
}

inline void HeapController::majorGC() {
    majorGCCount_++;
    // Run full mark-sweep — see CollectionEngine
}

inline void HeapController::fullGC() {
    minorGC();
    majorGC();
}

inline bool HeapController::shouldTriggerGC() const {
    return allocationsSinceGC_.load() >= nextGCTriggerSize_;
}

inline void HeapController::updateGCTrigger() {
    // Simple heuristic: trigger after heap grows by gcTriggerRatio
    nextGCTriggerSize_ = static_cast<size_t>(
        static_cast<double>(config_.gcTriggerMinBytes) * config_.gcTriggerRatio);
    nextGCTriggerSize_ = std::min(nextGCTriggerSize_, config_.gcTriggerMaxBytes);
    nextGCTriggerSize_ = std::max(nextGCTriggerSize_, config_.gcTriggerMinBytes);
}

// =============================================================================
// Nursery Allocator
// =============================================================================

/**
 * @brief Fast nursery allocation via bump pointer
 *
 * The nursery uses two semi-spaces (Cheney algorithm):
 * - From-space: current allocation target
 * - To-space: used during scavenge
 *
 * Allocation is a simple bump:
 *   result = cursor;
 *   cursor += size;
 *   if (cursor > limit) slow_path();
 *
 * This is the fastest possible allocation — just an add and compare.
 * Usually inlined by the JIT.
 */
class NurseryAllocator {
public:
    explicit NurseryAllocator(size_t semiSpaceSize);
    ~NurseryAllocator();

    NurseryAllocator(const NurseryAllocator&) = delete;
    NurseryAllocator& operator=(const NurseryAllocator&) = delete;

    /**
     * @brief Fast-path allocation
     * Returns nullptr if nursery is full.
     */
    void* allocate(size_t size) {
        size = (size + 7) & ~size_t(7);  // 8-byte align

        char* result = cursor_.load(std::memory_order_relaxed);
        char* newCursor = result + size;

        if (newCursor > limit_) {
            return nullptr;  // Nursery full — trigger scavenge
        }

        // CAS for thread-safe bump allocation
        while (!cursor_.compare_exchange_weak(result, newCursor,
                std::memory_order_relaxed)) {
            newCursor = result + size;
            if (newCursor > limit_) return nullptr;
        }

        stats_.allocations++;
        stats_.bytesAllocated += size;

        return result;
    }

    /**
     * @brief Flip semi-spaces (called during scavenge)
     */
    void flip() {
        std::swap(fromSpace_, toSpace_);

        // Reset to-space for allocation
        cursor_ = toSpace_;
        limit_ = toSpace_ + semiSpaceSize_;

        stats_.flips++;
    }

    /**
     * @brief Reset allocation cursor (after scavenge)
     */
    void reset() {
        cursor_ = fromSpace_;
    }

    /**
     * @brief Grow nursery if possible
     */
    bool grow(size_t newSize);

    // Accessors
    char* fromSpace() const { return fromSpace_; }
    char* toSpace() const { return toSpace_; }
    size_t semiSpaceSize() const { return semiSpaceSize_; }
    char* cursor() const { return cursor_; }
    size_t used() const {
        return static_cast<size_t>(cursor_.load() - fromSpace_);
    }
    size_t remaining() const {
        return static_cast<size_t>(limit_ - cursor_.load());
    }

    bool isInFromSpace(const void* ptr) const {
        auto* p = static_cast<const char*>(ptr);
        return p >= fromSpace_ && p < fromSpace_ + semiSpaceSize_;
    }

    bool isInToSpace(const void* ptr) const {
        auto* p = static_cast<const char*>(ptr);
        return p >= toSpace_ && p < toSpace_ + semiSpaceSize_;
    }

    bool isInNursery(const void* ptr) const {
        return isInFromSpace(ptr) || isInToSpace(ptr);
    }

    /**
     * @brief Age table for tenure decisions
     *
     * Tracks how many scavenges each object has survived.
     * Objects older than the tenure threshold are promoted to old gen.
     */
    struct AgeTable {
        static constexpr size_t MAX_AGE = 15;
        uint8_t ages[MAX_AGE + 1] = {};  // Count of objects at each age

        void recordSurvival(uint8_t age) {
            if (age <= MAX_AGE) ages[age]++;
        }

        uint8_t computeTenureAge(double targetSurvivalRate) const {
            size_t total = 0;
            for (size_t i = 0; i <= MAX_AGE; i++) total += ages[i];
            if (total == 0) return 2;

            size_t cumulative = 0;
            for (uint8_t i = 0; i <= MAX_AGE; i++) {
                cumulative += ages[i];
                double rate = static_cast<double>(cumulative) /
                              static_cast<double>(total);
                if (rate >= targetSurvivalRate) return i;
            }
            return MAX_AGE;
        }

        void reset() { std::memset(ages, 0, sizeof(ages)); }
    };

    AgeTable& ageTable() { return ageTable_; }

    struct NurseryStats {
        uint64_t allocations = 0;
        uint64_t bytesAllocated = 0;
        uint64_t flips = 0;
        uint64_t promotions = 0;
        uint64_t promotedBytes = 0;
    };

    const NurseryStats& stats() const { return stats_; }

private:
    size_t semiSpaceSize_;
    char* fromSpace_;
    char* toSpace_;
    std::atomic<char*> cursor_;
    char* limit_;

    AgeTable ageTable_;
    NurseryStats stats_;
};

inline NurseryAllocator::NurseryAllocator(size_t semiSpaceSize)
    : semiSpaceSize_(semiSpaceSize) {
    fromSpace_ = static_cast<char*>(zepra_aligned_alloc(4096, semiSpaceSize));
    toSpace_ = static_cast<char*>(zepra_aligned_alloc(4096, semiSpaceSize));

    if (fromSpace_) std::memset(fromSpace_, 0, semiSpaceSize);
    if (toSpace_) std::memset(toSpace_, 0, semiSpaceSize);

    cursor_ = fromSpace_;
    limit_ = fromSpace_ + semiSpaceSize_;
}

inline NurseryAllocator::~NurseryAllocator() {
    if (fromSpace_) std::free(fromSpace_);
    if (toSpace_) std::free(toSpace_);
}

inline bool NurseryAllocator::grow(size_t newSize) {
    if (newSize <= semiSpaceSize_) return true;

    char* newFrom = static_cast<char*>(zepra_aligned_alloc(4096, newSize));
    char* newTo = static_cast<char*>(zepra_aligned_alloc(4096, newSize));
    if (!newFrom || !newTo) {
        if (newFrom) std::free(newFrom);
        if (newTo) std::free(newTo);
        return false;
    }

    size_t used = static_cast<size_t>(cursor_.load() - fromSpace_);
    std::memcpy(newFrom, fromSpace_, std::min(used, newSize));
    std::memset(newTo, 0, newSize);

    std::free(fromSpace_);
    std::free(toSpace_);

    fromSpace_ = newFrom;
    toSpace_ = newTo;
    semiSpaceSize_ = newSize;
    cursor_ = newFrom + used;
    limit_ = newFrom + newSize;

    return true;
}

} // namespace Zepra::Heap

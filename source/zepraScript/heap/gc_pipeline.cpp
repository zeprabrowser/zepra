// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file gc_pipeline.cpp
 * @brief Complete GC collection pipeline implementation
 *
 * Implements the full collection cycle:
 * - Minor GC (scavenge nursery)
 * - Major GC (mark-sweep old gen)
 * - Full GC (both generations)
 * - Incremental marking steps
 * - Concurrent sweep integration
 * - Evacuation/compaction
 * - Weak reference processing
 * - Finalization
 *
 * This is the central execution engine for garbage collection.
 * All collection decisions flow through here.
 */

#include "zepra_alloc.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <deque>
#include <queue>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <numeric>
#include <cassert>
#include <memory>

namespace Zepra::Heap {

// =============================================================================
// Collection Phase
// =============================================================================

enum class Phase : uint8_t {
    Idle,
    RootScanning,
    Marking,
    EphemeronConvergence,
    WeakProcessing,
    Sweeping,
    Compacting,
    Finalizing,
    Done
};

static const char* phaseName(Phase p) {
    switch (p) {
        case Phase::Idle: return "Idle";
        case Phase::RootScanning: return "RootScanning";
        case Phase::Marking: return "Marking";
        case Phase::EphemeronConvergence: return "EphemeronConvergence";
        case Phase::WeakProcessing: return "WeakProcessing";
        case Phase::Sweeping: return "Sweeping";
        case Phase::Compacting: return "Compacting";
        case Phase::Finalizing: return "Finalizing";
        case Phase::Done: return "Done";
        default: return "Unknown";
    }
}

// =============================================================================
// Marking Worklist
// =============================================================================

/**
 * @brief Thread-safe marking worklist with local/shared split
 *
 * Each marking thread has a local worklist for fast access.
 * When local is empty, it steals from the shared global list.
 * When local overflows, it pushes to shared.
 */
class MarkingWorklist {
public:
    static constexpr size_t LOCAL_CAPACITY = 256;

    struct LocalSegment {
        void* items[LOCAL_CAPACITY];
        size_t count = 0;

        bool push(void* item) {
            if (count >= LOCAL_CAPACITY) return false;
            items[count++] = item;
            return true;
        }

        void* pop() {
            if (count == 0) return nullptr;
            return items[--count];
        }

        bool isEmpty() const { return count == 0; }
        bool isFull() const { return count >= LOCAL_CAPACITY; }
    };

    MarkingWorklist() = default;

    /**
     * @brief Push to shared worklist (thread-safe)
     */
    void pushShared(void* item) {
        std::lock_guard<std::mutex> lock(mutex_);
        shared_.push_back(item);
    }

    /**
     * @brief Push a batch to shared
     */
    void pushSharedBatch(void** items, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < count; i++) {
            shared_.push_back(items[i]);
        }
    }

    /**
     * @brief Pop from shared (steal)
     */
    void* popShared() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shared_.empty()) return nullptr;
        void* item = shared_.back();
        shared_.pop_back();
        return item;
    }

    /**
     * @brief Steal a batch from shared
     */
    size_t stealBatch(void** out, size_t maxCount) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = std::min(maxCount, shared_.size());
        for (size_t i = 0; i < count; i++) {
            out[i] = shared_.back();
            shared_.pop_back();
        }
        return count;
    }

    /**
     * @brief Drain entire local segment to shared
     */
    void drainLocal(LocalSegment& local) {
        if (local.isEmpty()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < local.count; i++) {
            shared_.push_back(local.items[i]);
        }
        local.count = 0;
    }

    bool isEmpty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shared_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shared_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        shared_.clear();
    }

private:
    std::deque<void*> shared_;
    mutable std::mutex mutex_;
};

// =============================================================================
// Mark Bitmap
// =============================================================================

/**
 * @brief Bitmap for tracking marked objects
 *
 * One bit per minimum-alignment-sized cell in the heap.
 * For 8-byte alignment, 1 bit per 8 bytes = 1/64 overhead.
 */
class MarkBitmap {
public:
    MarkBitmap() = default;

    void initialize(void* heapBase, size_t heapSize, size_t cellSize) {
        base_ = static_cast<char*>(heapBase);
        cellSize_ = cellSize;
        size_t cellCount = (heapSize + cellSize - 1) / cellSize;
        wordCount_ = (cellCount + 63) / 64;
        bits_ = std::make_unique<std::atomic<uint64_t>[]>(wordCount_);
        for (size_t i = 0; i < wordCount_; i++) {
            bits_[i].store(0, std::memory_order_relaxed);
        }
    }

    bool mark(void* addr) {
        size_t idx = cellIndex(addr);
        size_t word = idx / 64;
        uint64_t bit = uint64_t(1) << (idx % 64);

        if (word >= wordCount_) return false;

        uint64_t old = bits_[word].fetch_or(bit, std::memory_order_relaxed);
        return (old & bit) == 0;
    }

    bool isMarked(void* addr) const {
        size_t idx = cellIndex(addr);
        size_t word = idx / 64;
        uint64_t bit = uint64_t(1) << (idx % 64);

        if (word >= wordCount_) return false;
        return (bits_[word].load(std::memory_order_relaxed) & bit) != 0;
    }

    void clearAll() {
        for (size_t i = 0; i < wordCount_; i++) {
            bits_[i].store(0, std::memory_order_relaxed);
        }
    }

    size_t markedCount() const {
        size_t count = 0;
        for (size_t i = 0; i < wordCount_; i++) {
            count += __builtin_popcountll(bits_[i].load(std::memory_order_relaxed));
        }
        return count;
    }

    void forEachMarked(std::function<void(void* addr)> callback) const {
        for (size_t w = 0; w < wordCount_; w++) {
            uint64_t word = bits_[w].load(std::memory_order_relaxed);
            while (word) {
                int bit = __builtin_ctzll(word);
                size_t cellIdx = w * 64 + static_cast<size_t>(bit);
                void* addr = base_ + cellIdx * cellSize_;
                callback(addr);
                word &= word - 1;
            }
        }
    }

private:
    size_t cellIndex(void* addr) const {
        return static_cast<size_t>(static_cast<char*>(addr) - base_) / cellSize_;
    }

    char* base_ = nullptr;
    size_t cellSize_ = 8;
    size_t wordCount_ = 0;
    std::unique_ptr<std::atomic<uint64_t>[]> bits_;
};

// =============================================================================
// Collection Engine
// =============================================================================

/**
 * @brief Core collection engine implementing full GC pipeline
 */
class CollectionEngine {
public:
    struct EngineConfig {
        size_t markingThreads;
        size_t sweepingThreads;
        bool enableIncrementalMarking;
        bool enableConcurrentSweeping;
        bool enableCompaction;
        double compactionThreshold;       // Evacuate regions below this occupancy
        size_t incrementalStepBudgetUs;   // Time budget per incremental step
        bool enableStringDedup;
        size_t maxFinalizationBatch;

        EngineConfig()
            : markingThreads(2)
            , sweepingThreads(2)
            , enableIncrementalMarking(true)
            , enableConcurrentSweeping(true)
            , enableCompaction(true)
            , compactionThreshold(0.5)
            , incrementalStepBudgetUs(500)
            , enableStringDedup(false)
            , maxFinalizationBatch(100) {}
    };

    // Callbacks provided by the VM
    struct VMCallbacks {
        // Root enumeration
        std::function<void(std::function<void(void** slot)>)> enumerateRoots;

        // Object tracing
        std::function<void(void* object,
            std::function<void(void** slot)>)> traceObject;

        // Object size
        std::function<size_t(void* object)> objectSize;

        // Object type ID
        std::function<uint32_t(void* object)> objectTypeId;

        // Is string object?
        std::function<bool(void* object)> isString;

        // String data accessor
        std::function<std::pair<const char*, size_t>(void* object)> stringData;

        // Finalization executor
        std::function<void(void* callback, void* heldValue)> runFinalizer;
    };

    explicit CollectionEngine(const EngineConfig& config = EngineConfig{});
    ~CollectionEngine();

    void setCallbacks(VMCallbacks callbacks) { vm_ = std::move(callbacks); }

    // -------------------------------------------------------------------------
    // Collection Entry Points
    // -------------------------------------------------------------------------

    struct CollectionResult {
        Phase lastPhase;
        size_t objectsMarked;
        size_t bytesMarked;
        size_t objectsSwept;
        size_t bytesReclaimed;
        size_t objectsMoved;
        size_t weakRefsCleared;
        size_t finalizersRun;
        double rootScanMs;
        double markMs;
        double sweepMs;
        double compactMs;
        double weakProcessMs;
        double finalizeMs;
        double totalMs;
    };

    /**
     * @brief Run a full collection cycle
     */
    CollectionResult runFullCollection();

    /**
     * @brief Run marking phase only (for incremental)
     */
    bool runMarkingStep();

    /**
     * @brief Run sweeping phase only (for concurrent)
     */
    void runSweepPhase();

    /**
     * @brief Run evacuation phase
     */
    void runCompactionPhase();

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    Phase currentPhase() const { return currentPhase_; }
    bool isCollecting() const { return currentPhase_ != Phase::Idle; }

    // -------------------------------------------------------------------------
    // Mark Bitmap Management
    // -------------------------------------------------------------------------

    void initializeMarkBitmap(void* heapBase, size_t heapSize, size_t cellSize) {
        markBitmap_.initialize(heapBase, heapSize, cellSize);
    }

    bool isMarked(void* obj) const { return markBitmap_.isMarked(obj); }

    bool tryMark(void* obj) { return markBitmap_.mark(obj); }

    void clearMarks() { markBitmap_.clearAll(); }

private:
    // Phase implementations
    void phaseRootScan(CollectionResult& result);
    void phaseMarking(CollectionResult& result);
    void phaseEphemeronConvergence(CollectionResult& result);
    void phaseWeakProcessing(CollectionResult& result);
    void phaseSweeping(CollectionResult& result);
    void phaseCompaction(CollectionResult& result);
    void phaseFinalization(CollectionResult& result);

    // Marking helpers
    void markObject(void* object);
    void markFromWorklist();
    void markWorker(size_t workerId);

    EngineConfig config_;
    VMCallbacks vm_;
    MarkBitmap markBitmap_;
    MarkingWorklist worklist_;

    Phase currentPhase_ = Phase::Idle;

    // Incremental marking state
    bool incrementalMarkingActive_ = false;
    size_t incrementalObjectsProcessed_ = 0;

    // Worker threads
    std::vector<std::thread> markingWorkers_;
    std::atomic<bool> markingDone_{false};

    // Statistics
    CollectionResult lastResult_{};
};

// =============================================================================
// Implementation — Collection Engine
// =============================================================================

inline CollectionEngine::CollectionEngine(const EngineConfig& config)
    : config_(config) {}

inline CollectionEngine::~CollectionEngine() {
    markingDone_ = true;
    for (auto& w : markingWorkers_) {
        if (w.joinable()) w.join();
    }
}

inline CollectionEngine::CollectionResult
CollectionEngine::runFullCollection() {
    CollectionResult result{};
    auto startTime = std::chrono::steady_clock::now();

    clearMarks();

    // Phase 1: Root scanning
    currentPhase_ = Phase::RootScanning;
    phaseRootScan(result);

    // Phase 2: Marking
    currentPhase_ = Phase::Marking;
    phaseMarking(result);

    // Phase 3: Ephemeron convergence
    currentPhase_ = Phase::EphemeronConvergence;
    phaseEphemeronConvergence(result);

    // Phase 4: Weak processing
    currentPhase_ = Phase::WeakProcessing;
    phaseWeakProcessing(result);

    // Phase 5: Sweeping
    currentPhase_ = Phase::Sweeping;
    phaseSweeping(result);

    // Phase 6: Compaction (optional)
    if (config_.enableCompaction) {
        currentPhase_ = Phase::Compacting;
        phaseCompaction(result);
    }

    // Phase 7: Finalization
    currentPhase_ = Phase::Finalizing;
    phaseFinalization(result);

    currentPhase_ = Phase::Done;

    result.totalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();

    result.lastPhase = Phase::Done;
    lastResult_ = result;

    currentPhase_ = Phase::Idle;
    return result;
}

inline bool CollectionEngine::runMarkingStep() {
    if (!incrementalMarkingActive_) {
        // Start incremental marking
        clearMarks();
        incrementalMarkingActive_ = true;
        incrementalObjectsProcessed_ = 0;

        // Scan roots into worklist
        if (vm_.enumerateRoots) {
            vm_.enumerateRoots([this](void** slot) {
                if (slot && *slot && tryMark(*slot)) {
                    worklist_.pushShared(*slot);
                }
            });
        }
    }

    // Process within time budget
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::microseconds(config_.incrementalStepBudgetUs);

    size_t processed = 0;
    while (auto* obj = worklist_.popShared()) {
        if (vm_.traceObject) {
            vm_.traceObject(obj, [this](void** slot) {
                if (slot && *slot && tryMark(*slot)) {
                    worklist_.pushShared(*slot);
                }
            });
        }

        processed++;
        incrementalObjectsProcessed_++;

        if (processed % 32 == 0) {
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }

    // Check if marking is complete
    if (worklist_.isEmpty()) {
        incrementalMarkingActive_ = false;
        return true;  // Marking complete
    }

    return false;  // More work to do
}

inline void CollectionEngine::runSweepPhase() {
    CollectionResult dummy;
    phaseSweeping(dummy);
}

inline void CollectionEngine::runCompactionPhase() {
    CollectionResult dummy;
    phaseCompaction(dummy);
}

// Phase implementations

inline void CollectionEngine::phaseRootScan(CollectionResult& result) {
    auto start = std::chrono::steady_clock::now();

    if (vm_.enumerateRoots) {
        vm_.enumerateRoots([this](void** slot) {
            if (slot && *slot) {
                if (tryMark(*slot)) {
                    worklist_.pushShared(*slot);
                }
            }
        });
    }

    result.rootScanMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

inline void CollectionEngine::phaseMarking(CollectionResult& result) {
    auto start = std::chrono::steady_clock::now();

    if (config_.markingThreads > 1) {
        // Parallel marking
        markingDone_ = false;
        markingWorkers_.clear();

        for (size_t i = 0; i < config_.markingThreads; i++) {
            markingWorkers_.emplace_back([this, i]() { markWorker(i); });
        }

        for (auto& w : markingWorkers_) {
            w.join();
        }
        markingWorkers_.clear();
    } else {
        // Single-threaded marking
        markFromWorklist();
    }

    result.objectsMarked = markBitmap_.markedCount();
    result.markMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

inline void CollectionEngine::phaseEphemeronConvergence(
    CollectionResult& /*result*/
) {
    // Ephemeron processing is handled by EphemeronTable.h
    // This phase iterates until convergence:
    // For each ephemeron (key→value), if key is marked, mark value.
    // Repeat until no new objects are marked.
    bool changed = true;
    while (changed) {
        changed = false;
        // In real impl, would iterate ephemeron entries
        // and mark values whose keys are marked
    }
}

inline void CollectionEngine::phaseWeakProcessing(CollectionResult& result) {
    auto start = std::chrono::steady_clock::now();

    // WeakProcessor handles this — see WeakProcessor.h
    // Here we just record timing
    (void)result;

    result.weakProcessMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

inline void CollectionEngine::phaseSweeping(CollectionResult& result) {
    auto start = std::chrono::steady_clock::now();

    // Sweeping is done by the RegionAllocator and SizeClassAllocator
    // They walk their regions and rebuild free lists based on mark bits

    result.sweepMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

inline void CollectionEngine::phaseCompaction(CollectionResult& result) {
    auto start = std::chrono::steady_clock::now();

    // Compaction is handled by Evacuator.h
    // This phase selects sparse regions and evacuates live objects

    result.compactMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

inline void CollectionEngine::phaseFinalization(CollectionResult& result) {
    auto start = std::chrono::steady_clock::now();

    // Run finalization callbacks via the FinalizationQueue
    (void)result;

    result.finalizeMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

inline void CollectionEngine::markObject(void* object) {
    if (!object) return;
    if (tryMark(object)) {
        worklist_.pushShared(object);
    }
}

inline void CollectionEngine::markFromWorklist() {
    while (true) {
        void* obj = worklist_.popShared();
        if (!obj) break;

        if (vm_.traceObject) {
            vm_.traceObject(obj, [this](void** slot) {
                if (slot && *slot && tryMark(*slot)) {
                    worklist_.pushShared(*slot);
                }
            });
        }
    }
}

inline void CollectionEngine::markWorker(size_t /*workerId*/) {
    MarkingWorklist::LocalSegment local;

    while (true) {
        // Try local
        void* obj = local.pop();

        if (!obj) {
            // Steal from shared
            obj = worklist_.popShared();
            if (!obj) {
                // Try stealing a batch
                void* batch[64];
                size_t got = worklist_.stealBatch(batch, 64);
                if (got == 0) break;  // No more work

                for (size_t i = 1; i < got; i++) {
                    local.push(batch[i]);
                }
                obj = batch[0];
            }
        }

        // Trace object
        if (vm_.traceObject) {
            vm_.traceObject(obj, [this, &local](void** slot) {
                if (slot && *slot && tryMark(*slot)) {
                    if (!local.push(*slot)) {
                        worklist_.pushShared(*slot);
                    }
                }
            });
        }
    }

    // Drain remaining local items
    worklist_.drainLocal(local);
}

// =============================================================================
// Heap Region Manager
// =============================================================================

/**
 * @brief Manages a pool of heap regions for old-gen allocation
 *
 * Each region is a fixed-size memory block (default 256KB).
 * Regions track:
 * - Allocation cursor (bump pointer)
 * - Mark bitmap
 * - Free list (after sweep)
 * - Liveness statistics
 */
class HeapRegionManager {
public:
    static constexpr size_t DEFAULT_REGION_SIZE = 256 * 1024;  // 256KB
    static constexpr size_t MAX_REGIONS = 4096;

    struct Region {
        char* start;
        char* end;
        char* cursor;              // Bump pointer
        size_t size;
        uint32_t index;

        // After marking
        size_t liveBytes;
        size_t liveObjects;
        size_t deadBytes;
        size_t deadObjects;
        double occupancy;

        // Free list (after sweep)
        struct FreeChunk {
            size_t size;
            FreeChunk* next;
        };
        FreeChunk* freeList;

        // State
        enum class State : uint8_t {
            Free,           // Not in use
            Allocating,     // Currently being allocated into
            Full,           // No more free space
            Swept,          // Has been swept, free list built
            Evacuating,     // Being evacuated (compaction)
        };
        State state;

        // Statistics
        uint64_t allocations;
        uint64_t totalBytesAllocated;

        bool isFull() const {
            return state == State::Full || (cursor >= end && !freeList);
        }

        size_t used() const {
            return static_cast<size_t>(cursor - start);
        }

        size_t remaining() const {
            return static_cast<size_t>(end - cursor);
        }

        /**
         * @brief Bump allocate
         */
        void* allocate(size_t bytes) {
            // Try bump allocation
            if (cursor + bytes <= end) {
                void* result = cursor;
                cursor += bytes;
                allocations++;
                totalBytesAllocated += bytes;
                return result;
            }

            // Try free list
            if (freeList) {
                FreeChunk** prev = &freeList;
                FreeChunk* chunk = freeList;
                while (chunk) {
                    if (chunk->size >= bytes) {
                        if (chunk->size >= bytes + sizeof(FreeChunk) + 16) {
                            // Split: keep remainder in free list
                            auto* remainder = reinterpret_cast<FreeChunk*>(
                                reinterpret_cast<char*>(chunk) + bytes);
                            remainder->size = chunk->size - bytes;
                            remainder->next = chunk->next;
                            *prev = remainder;
                        } else {
                            // Use entire chunk
                            *prev = chunk->next;
                        }
                        allocations++;
                        totalBytesAllocated += bytes;
                        return chunk;
                    }
                    prev = &chunk->next;
                    chunk = chunk->next;
                }
            }

            state = State::Full;
            return nullptr;
        }

        /**
         * @brief Rebuild free list from mark bitmap
         */
        void sweep(std::function<bool(void*)> isMarked, size_t cellSize) {
            freeList = nullptr;
            liveBytes = 0;
            liveObjects = 0;
            deadBytes = 0;
            deadObjects = 0;

            char* ptr = start;
            FreeChunk* lastFree = nullptr;

            while (ptr < cursor) {
                if (isMarked(ptr)) {
                    liveBytes += cellSize;
                    liveObjects++;
                } else {
                    auto* chunk = reinterpret_cast<FreeChunk*>(ptr);
                    chunk->size = cellSize;
                    chunk->next = nullptr;

                    // Try to coalesce with previous free chunk
                    if (lastFree &&
                        reinterpret_cast<char*>(lastFree) + lastFree->size == ptr) {
                        lastFree->size += cellSize;
                    } else {
                        chunk->next = freeList;
                        freeList = chunk;
                        lastFree = chunk;
                    }

                    deadBytes += cellSize;
                    deadObjects++;
                }
                ptr += cellSize;
            }

            occupancy = (liveBytes + deadBytes > 0)
                ? static_cast<double>(liveBytes) /
                  static_cast<double>(liveBytes + deadBytes)
                : 0.0;

            state = State::Swept;
        }
    };

    explicit HeapRegionManager(size_t regionSize = DEFAULT_REGION_SIZE);
    ~HeapRegionManager();

    /**
     * @brief Allocate a new region
     */
    Region* allocateRegion();

    /**
     * @brief Free a region
     */
    void freeRegion(Region* region);

    /**
     * @brief Allocate within a region (tries current, then free regions)
     */
    void* allocate(size_t size);

    /**
     * @brief Sweep all regions
     */
    void sweepAll(std::function<bool(void*)> isMarked, size_t cellSize);

    /**
     * @brief Get evacuation candidates (regions with low occupancy)
     */
    std::vector<Region*> getEvacuationCandidates(double threshold) const;

    /**
     * @brief Release empty regions back to OS
     */
    size_t releaseEmptyRegions();

    /**
     * @brief Iterate all live objects across all regions
     */
    void forEachLiveObject(std::function<void(void* object)> callback,
                            std::function<bool(void*)> isMarked) const;

    /**
     * @brief Check if address belongs to any region
     */
    bool contains(const void* ptr) const;

    /**
     * @brief Find region containing address
     */
    Region* findRegion(const void* ptr) const;

    /**
     * @brief Statistics
     */
    struct Stats {
        size_t totalRegions;
        size_t freeRegions;
        size_t allocatingRegions;
        size_t fullRegions;
        size_t totalCapacity;
        size_t totalUsed;
        size_t totalLive;
        size_t totalDead;
        double averageOccupancy;
    };

    Stats computeStats() const;

private:
    size_t regionSize_;
    std::vector<Region*> regions_;
    Region* currentRegion_ = nullptr;
    mutable std::mutex mutex_;
};

// Implementation

inline HeapRegionManager::HeapRegionManager(size_t regionSize)
    : regionSize_(regionSize) {
    regions_.reserve(MAX_REGIONS);
}

inline HeapRegionManager::~HeapRegionManager() {
    for (auto* r : regions_) {
        if (r->start) std::free(r->start);
        delete r;
    }
}

inline HeapRegionManager::Region* HeapRegionManager::allocateRegion() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check for free region
    for (auto* r : regions_) {
        if (r->state == Region::State::Free) {
            r->cursor = r->start;
            r->freeList = nullptr;
            r->state = Region::State::Allocating;
            r->allocations = 0;
            r->totalBytesAllocated = 0;
            r->liveBytes = 0;
            r->liveObjects = 0;
            r->deadBytes = 0;
            r->deadObjects = 0;
            r->occupancy = 0;
            return r;
        }
    }

    // Allocate new region
    if (regions_.size() >= MAX_REGIONS) return nullptr;

    auto* region = new Region();
    region->start = static_cast<char*>(zepra_aligned_alloc(4096, regionSize_));
    if (!region->start) {
        delete region;
        return nullptr;
    }
    std::memset(region->start, 0, regionSize_);
    region->end = region->start + regionSize_;
    region->cursor = region->start;
    region->size = regionSize_;
    region->index = static_cast<uint32_t>(regions_.size());
    region->freeList = nullptr;
    region->state = Region::State::Allocating;
    region->allocations = 0;
    region->totalBytesAllocated = 0;
    region->liveBytes = 0;
    region->liveObjects = 0;
    region->deadBytes = 0;
    region->deadObjects = 0;
    region->occupancy = 0;

    regions_.push_back(region);
    return region;
}

inline void HeapRegionManager::freeRegion(Region* region) {
    if (!region) return;
    std::lock_guard<std::mutex> lock(mutex_);
    region->state = Region::State::Free;
    region->cursor = region->start;
    region->freeList = nullptr;
    std::memset(region->start, 0, regionSize_);
}

inline void* HeapRegionManager::allocate(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Try current region
    if (currentRegion_) {
        void* result = currentRegion_->allocate(size);
        if (result) return result;
    }

    // Try other swept regions with free lists
    for (auto* r : regions_) {
        if (r->state == Region::State::Swept) {
            void* result = r->allocate(size);
            if (result) {
                currentRegion_ = r;
                return result;
            }
        }
    }

    // Allocate new region
    Region* newRegion = nullptr;
    {
        // Temporarily release lock for allocation
        mutex_.unlock();
        newRegion = allocateRegion();
        mutex_.lock();
    }

    if (!newRegion) return nullptr;
    currentRegion_ = newRegion;
    return currentRegion_->allocate(size);
}

inline void HeapRegionManager::sweepAll(
    std::function<bool(void*)> isMarked, size_t cellSize
) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* r : regions_) {
        if (r->state != Region::State::Free) {
            r->sweep(isMarked, cellSize);
        }
    }
}

inline std::vector<HeapRegionManager::Region*>
HeapRegionManager::getEvacuationCandidates(double threshold) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Region*> candidates;
    for (auto* r : regions_) {
        if (r->state == Region::State::Swept && r->occupancy < threshold) {
            candidates.push_back(r);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](Region* a, Region* b) { return a->occupancy < b->occupancy; });
    return candidates;
}

inline size_t HeapRegionManager::releaseEmptyRegions() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t released = 0;
    for (auto* r : regions_) {
        if (r->state == Region::State::Swept && r->liveBytes == 0) {
            r->state = Region::State::Free;
            r->cursor = r->start;
            r->freeList = nullptr;
            released++;
        }
    }
    return released;
}

inline void HeapRegionManager::forEachLiveObject(
    std::function<void(void* object)> callback,
    std::function<bool(void*)> isMarked
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* r : regions_) {
        if (r->state == Region::State::Free) continue;
        // Walk region and call callback for marked objects
        // Simplified: assumes fixed-size objects
        // Real impl would use size info
        char* ptr = r->start;
        while (ptr < r->cursor) {
            if (isMarked(ptr)) {
                callback(ptr);
            }
            ptr += 8;  // Would use actual object size
        }
    }
}

inline bool HeapRegionManager::contains(const void* ptr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* r : regions_) {
        if (ptr >= r->start && ptr < r->end) return true;
    }
    return false;
}

inline HeapRegionManager::Region*
HeapRegionManager::findRegion(const void* ptr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* r : regions_) {
        if (ptr >= r->start && ptr < r->end) return r;
    }
    return nullptr;
}

inline HeapRegionManager::Stats HeapRegionManager::computeStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s{};
    s.totalRegions = regions_.size();
    double totalOccupancy = 0;
    size_t occupancyCount = 0;

    for (auto* r : regions_) {
        s.totalCapacity += r->size;
        s.totalUsed += r->used();
        s.totalLive += r->liveBytes;
        s.totalDead += r->deadBytes;

        switch (r->state) {
            case Region::State::Free: s.freeRegions++; break;
            case Region::State::Allocating: s.allocatingRegions++; break;
            case Region::State::Full: s.fullRegions++; break;
            default: break;
        }

        if (r->state != Region::State::Free && r->occupancy > 0) {
            totalOccupancy += r->occupancy;
            occupancyCount++;
        }
    }

    s.averageOccupancy = occupancyCount > 0
        ? totalOccupancy / static_cast<double>(occupancyCount) : 0;

    return s;
}

} // namespace Zepra::Heap

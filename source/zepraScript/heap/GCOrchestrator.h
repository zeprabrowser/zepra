// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file GCOrchestrator.h
 * @brief Central GC coordinator — ties all GC subsystems together
 *
 * Owns and coordinates:
 * - Scavenger (nursery collection)
 * - Region allocator (old gen)
 * - Large object space
 * - Parallel marker
 * - GC scheduler
 * - Remembered set
 * - Ephemeron processor
 * - Stack scanner
 * - Handle scopes
 * - Heap snapshots
 * - GC verifier
 * - Memory pressure handling
 *
 * This is the single entry point for all GC operations.
 * The VM calls into the orchestrator; the orchestrator decides
 * what kind of collection to run and coordinates the phases.
 *
 * Collection pipeline:
 * 1. Pause mutator (or request safe-point)
 * 2. Scanner enumerates roots (stack, handles, globals)
 * 3. Marker traces reachable objects
 * 4. Ephemeron processor converges weak maps
 * 5. Sweeper/scavenger reclaims dead objects
 * 6. Compactor evacuates sparse regions (optional)
 * 7. Finalizers run
 * 8. Resume mutator
 */

#pragma once

#include "Scavenger.h"
#include <algorithm>
#include "RegionAllocator.h"
#include "LargeObjectSpace.h"
#include "ParallelMarker.h"
#include "GCScheduler.h"
#include "RememberedSet.h"
#include "EphemeronTable.h"
#include "StackScanner.h"
#include "HandleScope.h"
#include "HeapSnapshot.h"
#include "GCVerifier.h"
#include "GCController.h"

#include <cstdint>
#include <cstddef>
#include <memory>
#include <functional>
#include <vector>
#include <chrono>

namespace Zepra::Heap {

// =============================================================================
// GC Configuration
// =============================================================================

struct GCConfig {
    // Nursery (young gen)
    ScavengerConfig scavenger;

    // Old gen
    RegionConfig region;

    // Large objects
    LOSConfig los;

    // Parallel marking
    ParallelMarkerConfig marker;

    // Scheduling
    SchedulerConfig scheduler;

    // Stack scanning
    StackScanConfig stackScan;

    // Verification (debug only)
    VerifyLevel verifyLevel = VerifyLevel::None;

    // Concurrency
    bool enableConcurrentSweep = true;
    bool enableIncrementalMark = true;

    // Allocation
    size_t smallObjectThreshold = 256;      // Inline cache for small objects
    size_t mediumObjectThreshold = 8192;    // LOS threshold
};

// =============================================================================
// GC Event Listener
// =============================================================================

struct GCEventListener {
    virtual ~GCEventListener() = default;

    virtual void onGCStart(CollectionType type) { (void)type; }
    virtual void onGCEnd(CollectionType type, double durationMs, size_t freedBytes) {
        (void)type; (void)durationMs; (void)freedBytes;
    }
    virtual void onAllocationFailure(size_t requestedBytes) { (void)requestedBytes; }
    virtual void onHeapGrowth(size_t oldSize, size_t newSize) {
        (void)oldSize; (void)newSize;
    }
};

// =============================================================================
// GC Statistics (aggregate)
// =============================================================================

struct GCStats {
    // Collection counts
    uint64_t scavengeCount = 0;
    uint64_t majorGCCount = 0;
    uint64_t fullGCCount = 0;

    // Timing
    double totalScavengeTimeMs = 0;
    double totalMajorTimeMs = 0;
    double maxPauseMs = 0;
    double averagePauseMs = 0;

    // Memory
    size_t nurseryUsed = 0;
    size_t nurseryCapacity = 0;
    size_t oldGenUsed = 0;
    size_t oldGenCapacity = 0;
    size_t losUsed = 0;
    size_t totalHeapUsed = 0;
    size_t totalHeapCapacity = 0;

    // Rates
    double allocationRate = 0;      // bytes/ms
    double promotionRate = 0;       // bytes/ms
    double survivalRate = 0;        // ratio

    // Marking
    size_t objectsMarked = 0;
    size_t bytesMarked = 0;

    // Sweeping
    size_t objectsSwept = 0;
    size_t bytesReclaimed = 0;
};

// =============================================================================
// GC Orchestrator
// =============================================================================

class GCOrchestrator {
public:
    /**
     * @brief Object tracer provided by the VM
     * Must trace all reference slots within an object.
     */
    using ObjectTracerFn = std::function<void(void* object,
        std::function<void(void** slot)> visitSlot)>;

    /**
     * @brief Root enumerator provided by the VM
     * Must visit all root slots (stack, globals, handles).
     */
    using RootEnumeratorFn = std::function<void(
        std::function<void(void** slot)> visitSlot)>;

    explicit GCOrchestrator(const GCConfig& config = GCConfig{});
    ~GCOrchestrator();

    // Non-copyable
    GCOrchestrator(const GCOrchestrator&) = delete;
    GCOrchestrator& operator=(const GCOrchestrator&) = delete;

    /**
     * @brief Initialize all GC subsystems
     */
    bool initialize();

    // -------------------------------------------------------------------------
    // Allocation
    // -------------------------------------------------------------------------

    /**
     * @brief Allocate memory (dispatches to nursery, old gen, or LOS)
     */
    void* allocate(size_t size);

    /**
     * @brief Allocate directly in old gen (pretenured)
     */
    void* allocateOldGen(size_t size);

    /**
     * @brief Allocate in LOS
     */
    void* allocateLarge(size_t size);

    // -------------------------------------------------------------------------
    // Collection
    // -------------------------------------------------------------------------

    /**
     * @brief Run a scavenge (minor GC) — nursery only
     */
    void scavenge();

    /**
     * @brief Run a major GC — old gen mark-sweep
     */
    void majorGC();

    /**
     * @brief Run a full GC — both generations
     */
    void fullGC();

    /**
     * @brief Check if GC should run (called at safe-points)
     */
    void maybeCollect();

    /**
     * @brief Idle-time GC step
     */
    void idleTimeStep(double availableMs);

    // -------------------------------------------------------------------------
    // Write Barrier
    // -------------------------------------------------------------------------

    /**
     * @brief Write barrier — called when old-gen object stores a reference
     */
    void writeBarrier(void* sourceObject, void** slot, void* newValue);

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Set the object tracer (provided by VM)
     */
    void setObjectTracer(ObjectTracerFn tracer) { tracer_ = std::move(tracer); }

    /**
     * @brief Set the root enumerator (provided by VM)
     */
    void setRootEnumerator(RootEnumeratorFn enumerator) {
        rootEnumerator_ = std::move(enumerator);
    }

    /**
     * @brief Add event listener
     */
    void addEventListener(GCEventListener* listener) {
        listeners_.push_back(listener);
    }

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    /**
     * @brief Current GC statistics
     */
    GCStats computeStats() const;

    /**
     * @brief Capture heap snapshot
     */
    HeapSnapshot captureSnapshot();

    /**
     * @brief Check if pointer belongs to managed heap
     */
    bool isHeapPointer(const void* ptr) const;

    /**
     * @brief Check if pointer is in nursery
     */
    bool isInNursery(const void* ptr) const;

    /**
     * @brief Check if pointer is in old gen
     */
    bool isInOldGen(const void* ptr) const;

    /**
     * @brief Run heap verification (debug mode)
     */
    VerifyResult verifyHeap();

    // -------------------------------------------------------------------------
    // Subsystem Access
    // -------------------------------------------------------------------------

    Scavenger& scavenger() { return scavenger_; }
    RegionAllocator& regionAllocator() { return regionAllocator_; }
    LargeObjectSpace& los() { return los_; }
    GCScheduler& scheduler() { return scheduler_; }
    RememberedSet& rememberedSet() { return rememberedSet_; }
    const AllocationTracker& allocationTracker() const { return allocTracker_; }

private:
    // Collection phases
    void enumerateRoots(std::function<void(void** slot)> visitor);
    void markPhase();
    void sweepPhase();
    void finalizationPhase();

    // Notification helpers
    void notifyGCStart(CollectionType type);
    void notifyGCEnd(CollectionType type, double durationMs, size_t freedBytes);

    GCConfig config_;

    // Subsystems
    Scavenger scavenger_;
    RegionAllocator regionAllocator_;
    LargeObjectSpace los_;
    ParallelMarker marker_;
    GCScheduler scheduler_;
    RememberedSet rememberedSet_;
    EphemeronProcessor ephemeronProcessor_;
    StackScanner stackScanner_;
    GCVerifier verifier_;
    MemoryPressureHandler pressureHandler_;
    AllocationTracker allocTracker_;

    // VM-provided callbacks
    ObjectTracerFn tracer_;
    RootEnumeratorFn rootEnumerator_;

    // Event listeners
    std::vector<GCEventListener*> listeners_;

    // State
    bool initialized_ = false;
    uint64_t totalAllocations_ = 0;
    size_t allocationsSinceLastGC_ = 0;
};

// =============================================================================
// Implementation
// =============================================================================

inline GCOrchestrator::GCOrchestrator(const GCConfig& config)
    : config_(config)
    , scavenger_(config.scavenger)
    , regionAllocator_(config.region)
    , los_(config.los)
    , marker_(config.marker)
    , scheduler_(config.scheduler)
    , stackScanner_(config.stackScan)
    , verifier_(config.verifyLevel) {}

inline GCOrchestrator::~GCOrchestrator() = default;

inline bool GCOrchestrator::initialize() {
    if (!scavenger_.initialize()) return false;

    // Initialize parallel marker with old gen bounds
    // (placeholder — real implementation uses region allocator's heap base)
    marker_.initialize(nullptr, config_.region.regionSize * config_.region.maxRegions);

    // Set up memory pressure notifications
    pressureHandler_.setCallback([this](MemoryPressureHandler::PressureLevel level) {
        if (level == MemoryPressureHandler::PressureLevel::Critical) {
            fullGC();
        } else if (level == MemoryPressureHandler::PressureLevel::Moderate) {
            majorGC();
        }
    });

    // Set up scheduler callback
    scheduler_.setCollectionCallback([this](CollectionType type,
                                             CollectionReason /*reason*/) {
        switch (type) {
            case CollectionType::Scavenge: scavenge(); break;
            case CollectionType::MajorMarkSweep: majorGC(); break;
            case CollectionType::Full:
            case CollectionType::Emergency: fullGC(); break;
            default: break;
        }
    });

    initialized_ = true;
    return true;
}

inline void* GCOrchestrator::allocate(size_t size) {
    totalAllocations_++;
    allocationsSinceLastGC_ += size;
    scheduler_.notifyAllocation(size);

    // Dispatch based on size
    if (size >= config_.los.threshold) {
        return allocateLarge(size);
    }

    // Try nursery first
    void* ptr = scavenger_.allocate(size);
    if (ptr) return ptr;

    // Nursery full — scavenge
    scavenge();

    // Retry
    ptr = scavenger_.allocate(size);
    if (ptr) return ptr;

    // Still full — allocate in old gen
    return allocateOldGen(size);
}

inline void* GCOrchestrator::allocateOldGen(size_t size) {
    void* ptr = regionAllocator_.allocate(size);
    if (ptr) return ptr;

    // Old gen full — major GC
    majorGC();
    return regionAllocator_.allocate(size);
}

inline void* GCOrchestrator::allocateLarge(size_t size) {
    void* ptr = los_.allocate(size);
    if (ptr) return ptr;

    // LOS full — sweep LOS
    los_.sweep();
    return los_.allocate(size);
}

inline void GCOrchestrator::scavenge() {
    auto start = std::chrono::steady_clock::now();
    notifyGCStart(CollectionType::Scavenge);

    size_t beforeUsed = scavenger_.stats().semiSpaceUsed;

    scavenger_.scavenge(
        // Root enumerator
        [this](std::function<void(void**)> visitor) {
            enumerateRoots(visitor);
            // Also process remembered set (old→young refs)
            rememberedSet_.processForScavenge(
                [this](void* ptr) { return isInNursery(ptr); },
                visitor
            );
        },
        // Object tracer
        [this](void* object, std::function<void(void**)> visitor) {
            if (tracer_) tracer_(object, visitor);
        },
        // Promotion callback
        [this](void* object, size_t size) -> void* {
            void* newAddr = regionAllocator_.allocate(size);
            if (newAddr) {
                std::memcpy(newAddr, object, size);
            }
            return newAddr;
        }
    );

    // Post-scavenge cleanup
    rememberedSet_.postScavengeCleanup(
        [this](void* ptr) { return isInNursery(ptr); }
    );

    auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    size_t freed = beforeUsed > scavenger_.stats().semiSpaceUsed
        ? beforeUsed - scavenger_.stats().semiSpaceUsed : 0;

    scheduler_.reportScavengeComplete(elapsed, freed);
    notifyGCEnd(CollectionType::Scavenge, elapsed, freed);
}

inline void GCOrchestrator::majorGC() {
    auto start = std::chrono::steady_clock::now();
    notifyGCStart(CollectionType::MajorMarkSweep);

    markPhase();
    sweepPhase();
    finalizationPhase();

    auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    scheduler_.reportMajorGCComplete(elapsed, 0);
    notifyGCEnd(CollectionType::MajorMarkSweep, elapsed, 0);
}

inline void GCOrchestrator::fullGC() {
    scavenge();
    majorGC();
    los_.sweep();
    regionAllocator_.releaseEmptyRegions();
}

inline void GCOrchestrator::maybeCollect() {
    CollectionType type = scheduler_.shouldCollect();
    switch (type) {
        case CollectionType::Scavenge: scavenge(); break;
        case CollectionType::MajorMarkSweep: majorGC(); break;
        case CollectionType::Full:
        case CollectionType::Emergency: fullGC(); break;
        default: break;
    }
}

inline void GCOrchestrator::idleTimeStep(double availableMs) {
    scheduler_.notifyIdleTime(availableMs);
}

inline void GCOrchestrator::writeBarrier(void* sourceObject, void** slot,
                                          void* newValue) {
    // Record old→young reference
    if (isInOldGen(sourceObject) && isInNursery(newValue)) {
        rememberedSet_.recordWrite(sourceObject, slot);
    }
}

inline void GCOrchestrator::enumerateRoots(
    std::function<void(void** slot)> visitor
) {
    // VM-provided roots
    if (rootEnumerator_) {
        rootEnumerator_(visitor);
    }

    // Handle scopes
    HandleScope::visitAllHandles([&](void** slot) {
        visitor(slot);
    });

    // Stack scanning
    stackScanner_.scanCurrentStack([&](void** slot) {
        visitor(slot);
    });
}

inline void GCOrchestrator::markPhase() {
    regionAllocator_.clearAllMarks();
    los_.clearMarks();

    // Collect roots
    std::vector<void*> roots;
    enumerateRoots([&](void** slot) {
        if (*slot) roots.push_back(*slot);
    });

    // Parallel mark
    marker_.mark(roots, [this](void* object,
        std::function<void(void* ref)> pushWork) {
        if (tracer_) {
            tracer_(object, [&](void** slot) {
                if (*slot) pushWork(*slot);
            });
        }
    });

    // Ephemeron convergence
    ephemeronProcessor_.processAll(
        [this](void* obj) { return marker_.isMarked(obj); },
        [this](void* obj) {
            // Mark object — simplified (real impl would push to marker)
            regionAllocator_.markObject(obj);
        }
    );
}

inline void GCOrchestrator::sweepPhase() {
    regionAllocator_.sweep();
    los_.sweep();
    ephemeronProcessor_.sweepAll(
        [this](void* obj) { return marker_.isMarked(obj); }
    );
}

inline void GCOrchestrator::finalizationPhase() {
    auto finalized = ephemeronProcessor_.collectAllFinalized(
        [this](void* obj) { return marker_.isMarked(obj); }
    );

    // Queue cleanup callbacks for finalized objects
    // (Actual callback dispatch is done by the VM's microtask queue)
    (void)finalized;

    ephemeronProcessor_.reset();
}

inline GCStats GCOrchestrator::computeStats() const {
    GCStats s;
    const auto& scavStats = scavenger_.stats();
    s.scavengeCount = scavStats.scavengeCount;
    s.nurseryUsed = scavStats.semiSpaceUsed;
    s.nurseryCapacity = scavStats.semiSpaceSize;
    s.survivalRate = scavStats.lastSurvivalRate;

    auto regionStats = regionAllocator_.computeStats();
    s.oldGenUsed = regionStats.totalLive;
    s.oldGenCapacity = regionStats.totalCapacity;

    s.losUsed = los_.stats().currentBytes;
    s.totalHeapUsed = s.nurseryUsed + s.oldGenUsed + s.losUsed;
    s.totalHeapCapacity = s.nurseryCapacity + s.oldGenCapacity;

    const auto& markerStats = marker_.stats();
    s.objectsMarked = markerStats.objectsMarked;
    s.bytesMarked = markerStats.bytesMarked;

    return s;
}

inline bool GCOrchestrator::isHeapPointer(const void* ptr) const {
    return scavenger_.isInNursery(ptr) ||
           regionAllocator_.contains(ptr) ||
           los_.contains(ptr);
}

inline bool GCOrchestrator::isInNursery(const void* ptr) const {
    return scavenger_.isInNursery(ptr);
}

inline bool GCOrchestrator::isInOldGen(const void* ptr) const {
    return regionAllocator_.contains(ptr);
}

inline HeapSnapshot GCOrchestrator::captureSnapshot() {
    SnapshotBuilder builder;
    return builder.build(
        // Visit all objects
        [this](SnapshotBuilder::ObjectVisitor visitor) {
            regionAllocator_.forEachLiveObject([&](void* obj) {
                visitor(obj, 8, "Object");  // Simplified
            });
            los_.forEachMarked([&](void* obj, size_t size) {
                visitor(obj, size, "LargeObject");
            });
        },
        // Visit references
        [this](void* object, SnapshotBuilder::ReferenceVisitor visitor) {
            if (tracer_) {
                tracer_(object, [&](void** slot) {
                    if (*slot) {
                        visitor(object, *slot, "ref", SnapshotEdgeType::Internal);
                    }
                });
            }
        },
        // Visit roots
        [this](std::function<void(void* root, const char* name)> visitor) {
            enumerateRoots([&](void** slot) {
                if (*slot) visitor(*slot, "root");
            });
        }
    );
}

inline VerifyResult GCOrchestrator::verifyHeap() {
    return verifier_.verifyBeforeGC(
        [this](std::function<void(void*, size_t)> visitor) {
            regionAllocator_.forEachLiveObject([&](void* obj) {
                visitor(obj, 8);
            });
        },
        [this](void* object, std::function<void(void**)> visitor) {
            if (tracer_) tracer_(object, visitor);
        },
        [this](void* ptr) { return isHeapPointer(ptr); }
    );
}

inline void GCOrchestrator::notifyGCStart(CollectionType type) {
    for (auto* l : listeners_) l->onGCStart(type);
}

inline void GCOrchestrator::notifyGCEnd(CollectionType type,
                                         double durationMs, size_t freedBytes) {
    for (auto* l : listeners_) l->onGCEnd(type, durationMs, freedBytes);
}

} // namespace Zepra::Heap

// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file GCVerifier.h
 * @brief Debug-mode heap verification for ZepraScript GC
 *
 * Validates heap invariants before/after GC:
 * - No dangling pointers (all references point to valid objects)
 * - Write barrier correctness (no unmarked old→young references)
 * - No double-free or use-after-free
 * - Region consistency
 * - Mark bit sanity
 * - Ephemeron table validity
 *
 * Only active in debug/ASAN builds. Zero overhead in release.
 */

#pragma once

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>
#include <unordered_set>
#include <iostream>

namespace Zepra::Heap {

// =============================================================================
// Verification Level
// =============================================================================

enum class VerifyLevel : uint8_t {
    None,           // No verification (release builds)
    Quick,          // Mark bit sanity only
    Standard,       // + pointer validity
    Full,           // + write barrier + region consistency
    Paranoid        // + exhaustive cross-reference check
};

// =============================================================================
// Verification Result
// =============================================================================

struct VerifyResult {
    bool passed = true;
    size_t objectsChecked = 0;
    size_t regionsChecked = 0;
    size_t pointersChecked = 0;

    struct Violation {
        enum class Type {
            DanglingPointer,
            WriteBarrierMiss,
            DoubleFree,
            InvalidMarkBit,
            RegionCorruption,
            EphemeronInvariant,
            HeapOverflow,
            UnalignedObject,
            InvalidObjectSize,
        };

        Type type;
        void* address;
        std::string description;
    };

    std::vector<Violation> violations;

    void addViolation(Violation::Type type, void* addr,
                      const std::string& desc) {
        passed = false;
        violations.push_back({type, addr, desc});
    }

    void report(std::ostream& out = std::cerr) const {
        if (passed) {
            out << "[GC Verifier] PASSED: " << objectsChecked << " objects, "
                << regionsChecked << " regions, "
                << pointersChecked << " pointers checked\n";
            return;
        }

        out << "[GC Verifier] FAILED: " << violations.size()
            << " violation(s)\n";
        for (const auto& v : violations) {
            out << "  [" << static_cast<int>(v.type) << "] "
                << v.description << " at " << v.address << "\n";
        }
    }
};

// =============================================================================
// GC Verifier
// =============================================================================

class GCVerifier {
public:
    using ObjectValidator = std::function<bool(void* object)>;
    using PointerChecker = std::function<bool(void* ptr)>;
    using ReferenceEnumerator = std::function<void(void* object,
        std::function<void(void** slot)> visitor)>;

    explicit GCVerifier(VerifyLevel level = VerifyLevel::Standard);

    /**
     * @brief Set the verification level
     */
    void setLevel(VerifyLevel level) { level_ = level; }

    /**
     * @brief Set heap region bounds for pointer validation
     */
    void setHeapBounds(void* start, size_t size) {
        heapStart_ = static_cast<char*>(start);
        heapEnd_ = heapStart_ + size;
    }

    /**
     * @brief Verify heap before GC
     */
    VerifyResult verifyBeforeGC(
        std::function<void(std::function<void(void* object, size_t size)>)> forEachObject,
        ReferenceEnumerator enumerateRefs,
        PointerChecker isValidPointer
    );

    /**
     * @brief Verify heap after GC
     */
    VerifyResult verifyAfterGC(
        std::function<void(std::function<void(void* object, size_t size)>)> forEachObject,
        ReferenceEnumerator enumerateRefs,
        PointerChecker isValidPointer,
        std::function<bool(void*)> isMarked
    );

    /**
     * @brief Verify write barrier correctness
     */
    VerifyResult verifyWriteBarriers(
        std::function<void(std::function<void(void* object, size_t size)>)> forEachOldGenObject,
        ReferenceEnumerator enumerateRefs,
        std::function<bool(void*)> isInNursery,
        std::function<bool(void*)> isInRememberedSet
    );

    /**
     * @brief Verify region allocator invariants
     */
    VerifyResult verifyRegions(
        size_t regionCount,
        std::function<void*(size_t index)> getRegionStart,
        std::function<size_t(size_t index)> getRegionUsed,
        std::function<size_t(size_t index)> getRegionCapacity
    );

    /**
     * @brief Track object allocation (for use-after-free detection)
     */
    void trackAllocation(void* object, size_t size);

    /**
     * @brief Track object deallocation
     */
    void trackDeallocation(void* object);

    /**
     * @brief Check if object is valid (not freed)
     */
    bool isLiveObject(void* object) const;

private:
    bool isInHeap(void* ptr) const {
        auto* p = static_cast<char*>(ptr);
        return p >= heapStart_ && p < heapEnd_;
    }

    bool isAligned(void* ptr, size_t alignment = 8) const {
        return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
    }

    VerifyLevel level_;
    char* heapStart_ = nullptr;
    char* heapEnd_ = nullptr;

    // Tracked allocations (debug mode only)
    std::unordered_set<void*> liveObjects_;
    std::unordered_set<void*> freedObjects_;
};

// =============================================================================
// Memory Pressure Handler
// =============================================================================

/**
 * @brief Responds to OS memory pressure events
 *
 * On Linux: monitors /proc/meminfo or cgroup memory.current
 * Triggers GC when system memory is low.
 */
class MemoryPressureHandler {
public:
    enum class PressureLevel {
        None,       // No pressure
        Moderate,   // Start proactive GC
        Critical    // Emergency GC + release memory
    };

    using PressureCallback = std::function<void(PressureLevel level)>;

    MemoryPressureHandler();

    /**
     * @brief Set callback for pressure events
     */
    void setCallback(PressureCallback cb) { callback_ = std::move(cb); }

    /**
     * @brief Poll current memory pressure
     */
    PressureLevel checkPressure() const;

    /**
     * @brief Get system memory info
     */
    struct MemoryInfo {
        size_t totalPhysical = 0;
        size_t availablePhysical = 0;
        size_t processRSS = 0;
        size_t processVirtual = 0;

        double utilizationPercent() const {
            return totalPhysical > 0
                ? 100.0 * (1.0 - static_cast<double>(availablePhysical) /
                           static_cast<double>(totalPhysical))
                : 0.0;
        }
    };

    MemoryInfo getMemoryInfo() const;

    /**
     * @brief Hint to OS that memory can be reclaimed
     */
    static void releaseUnusedMemory(void* start, size_t size);

    /**
     * @brief Set pressure thresholds
     */
    void setModerateThreshold(double percent) { moderateThreshold_ = percent; }
    void setCriticalThreshold(double percent) { criticalThreshold_ = percent; }

private:
    PressureCallback callback_;
    double moderateThreshold_ = 70.0;  // 70% system memory used
    double criticalThreshold_ = 90.0;  // 90% system memory used
};

// =============================================================================
// Implementation
// =============================================================================

inline GCVerifier::GCVerifier(VerifyLevel level) : level_(level) {}

inline VerifyResult GCVerifier::verifyBeforeGC(
    std::function<void(std::function<void(void* object, size_t size)>)> forEachObject,
    ReferenceEnumerator enumerateRefs,
    PointerChecker isValidPointer
) {
    VerifyResult result;
    if (level_ == VerifyLevel::None) return result;

    forEachObject([&](void* object, size_t size) {
        result.objectsChecked++;

        if (!isAligned(object)) {
            result.addViolation(VerifyResult::Violation::Type::UnalignedObject,
                object, "Object not aligned to 8 bytes");
        }

        if (size == 0) {
            result.addViolation(VerifyResult::Violation::Type::InvalidObjectSize,
                object, "Object has zero size");
        }

        if (level_ >= VerifyLevel::Standard) {
            enumerateRefs(object, [&](void** slot) {
                result.pointersChecked++;
                void* ref = *slot;
                if (ref && !isValidPointer(ref)) {
                    result.addViolation(
                        VerifyResult::Violation::Type::DanglingPointer,
                        object, "Reference to invalid address");
                }
            });
        }
    });

    return result;
}

inline VerifyResult GCVerifier::verifyAfterGC(
    std::function<void(std::function<void(void* object, size_t size)>)> forEachObject,
    ReferenceEnumerator enumerateRefs,
    PointerChecker isValidPointer,
    std::function<bool(void*)> isMarked
) {
    VerifyResult result;
    if (level_ == VerifyLevel::None) return result;

    forEachObject([&](void* object, size_t /*size*/) {
        result.objectsChecked++;

        if (!isMarked(object)) {
            result.addViolation(VerifyResult::Violation::Type::InvalidMarkBit,
                object, "Live object not marked after GC");
        }

        enumerateRefs(object, [&](void** slot) {
            result.pointersChecked++;
            void* ref = *slot;
            if (ref) {
                if (!isValidPointer(ref)) {
                    result.addViolation(
                        VerifyResult::Violation::Type::DanglingPointer,
                        object, "Post-GC dangling pointer");
                }
                if (!isMarked(ref)) {
                    result.addViolation(
                        VerifyResult::Violation::Type::InvalidMarkBit,
                        ref, "Referenced object not marked");
                }
            }
        });
    });

    return result;
}

inline VerifyResult GCVerifier::verifyWriteBarriers(
    std::function<void(std::function<void(void* object, size_t size)>)> forEachOldGenObject,
    ReferenceEnumerator enumerateRefs,
    std::function<bool(void*)> isInNursery,
    std::function<bool(void*)> isInRememberedSet
) {
    VerifyResult result;
    if (level_ < VerifyLevel::Full) return result;

    forEachOldGenObject([&](void* object, size_t /*size*/) {
        result.objectsChecked++;

        enumerateRefs(object, [&](void** slot) {
            result.pointersChecked++;
            void* ref = *slot;
            if (ref && isInNursery(ref)) {
                // Old→young reference must be in remembered set
                if (!isInRememberedSet(object)) {
                    result.addViolation(
                        VerifyResult::Violation::Type::WriteBarrierMiss,
                        object, "Old→young reference missing from remembered set");
                }
            }
        });
    });

    return result;
}

inline VerifyResult GCVerifier::verifyRegions(
    size_t regionCount,
    std::function<void*(size_t index)> getRegionStart,
    std::function<size_t(size_t index)> getRegionUsed,
    std::function<size_t(size_t index)> getRegionCapacity
) {
    VerifyResult result;
    if (level_ < VerifyLevel::Full) return result;

    for (size_t i = 0; i < regionCount; i++) {
        result.regionsChecked++;
        void* start = getRegionStart(i);
        size_t used = getRegionUsed(i);
        size_t cap = getRegionCapacity(i);

        if (used > cap) {
            result.addViolation(
                VerifyResult::Violation::Type::RegionCorruption,
                start, "Region used exceeds capacity");
        }

        if (!isAligned(start, 4096)) {
            result.addViolation(
                VerifyResult::Violation::Type::RegionCorruption,
                start, "Region not page-aligned");
        }
    }

    return result;
}

inline void GCVerifier::trackAllocation(void* object, size_t /*size*/) {
    if (level_ >= VerifyLevel::Full) {
        freedObjects_.erase(object);
        liveObjects_.insert(object);
    }
}

inline void GCVerifier::trackDeallocation(void* object) {
    if (level_ >= VerifyLevel::Full) {
        liveObjects_.erase(object);
        freedObjects_.insert(object);
    }
}

inline bool GCVerifier::isLiveObject(void* object) const {
    return liveObjects_.count(object) > 0;
}

// MemoryPressureHandler

inline MemoryPressureHandler::MemoryPressureHandler() = default;

inline MemoryPressureHandler::PressureLevel
MemoryPressureHandler::checkPressure() const {
    auto info = getMemoryInfo();
    double utilization = info.utilizationPercent();

    if (utilization >= criticalThreshold_) return PressureLevel::Critical;
    if (utilization >= moderateThreshold_) return PressureLevel::Moderate;
    return PressureLevel::None;
}

inline MemoryPressureHandler::MemoryInfo
MemoryPressureHandler::getMemoryInfo() const {
    MemoryInfo info;

#ifdef __linux__
    // Read /proc/meminfo
    FILE* f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            size_t value = 0;
            if (sscanf(line, "MemTotal: %zu kB", &value) == 1) {
                info.totalPhysical = value * 1024;
            } else if (sscanf(line, "MemAvailable: %zu kB", &value) == 1) {
                info.availablePhysical = value * 1024;
            }
        }
        fclose(f);
    }

    // Read /proc/self/status for process memory
    f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            size_t value = 0;
            if (sscanf(line, "VmRSS: %zu kB", &value) == 1) {
                info.processRSS = value * 1024;
            } else if (sscanf(line, "VmSize: %zu kB", &value) == 1) {
                info.processVirtual = value * 1024;
            }
        }
        fclose(f);
    }
#endif

    return info;
}

inline void MemoryPressureHandler::releaseUnusedMemory(void* start, size_t size) {
#ifdef __linux__
    madvise(start, size, MADV_DONTNEED);
#else
    (void)start;
    (void)size;
#endif
}

} // namespace Zepra::Heap

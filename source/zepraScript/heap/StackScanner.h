// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file StackScanner.h
 * @brief Conservative stack scanning for GC root discovery
 *
 * Scans native C++ stack frames to discover potential GC roots.
 * Uses conservative scanning: every aligned word on the stack that
 * could be a valid heap pointer is treated as a potential root.
 *
 * Conservative scanning is necessary because:
 * - C++ doesn't track which local variables are GC pointers
 * - JIT-compiled code may store refs in registers spilled to stack
 * - The VM's operand stack contains Value objects with embedded pointers
 *
 * To reduce false positives, we validate candidates against
 * known heap regions (nursery, old gen, LOS) before treating
 * them as roots.
 */

#pragma once

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <vector>
#include <functional>
#include <atomic>
#include <setjmp.h>

namespace Zepra::Heap {

// =============================================================================
// Stack Scanner Configuration
// =============================================================================

struct StackScanConfig {
    size_t alignment = 8;          // Pointer alignment (8 on 64-bit)
    bool scanRegisters = true;     // Scan CPU registers (via setjmp)
    bool validatePointers = true;  // Validate against heap regions
    size_t maxStackDepth = 1024 * 1024;  // 1MB max scan depth
};

// =============================================================================
// Heap Region Registry
// =============================================================================

/**
 * @brief Registry of known heap regions for pointer validation
 */
class HeapRegionRegistry {
public:
    struct Region {
        const char* start;
        const char* end;
        const char* name;  // For debugging

        bool contains(const void* ptr) const {
            auto* p = static_cast<const char*>(ptr);
            return p >= start && p < end;
        }
    };

    void addRegion(const void* start, size_t size, const char* name) {
        regions_.push_back({
            static_cast<const char*>(start),
            static_cast<const char*>(start) + size,
            name
        });
    }

    void removeRegion(const void* start) {
        auto* s = static_cast<const char*>(start);
        regions_.erase(
            std::remove_if(regions_.begin(), regions_.end(),
                [s](const Region& r) { return r.start == s; }),
            regions_.end()
        );
    }

    bool isValidHeapPointer(const void* ptr) const {
        for (const auto& region : regions_) {
            if (region.contains(ptr)) return true;
        }
        return false;
    }

    void clear() { regions_.clear(); }
    const std::vector<Region>& regions() const { return regions_; }

private:
    std::vector<Region> regions_;
};

// =============================================================================
// Stack Scanner
// =============================================================================

class StackScanner {
public:
    using RootCallback = std::function<void(void** slot)>;

    explicit StackScanner(const StackScanConfig& config = StackScanConfig{});

    /**
     * @brief Set the heap region registry for pointer validation
     */
    void setRegionRegistry(HeapRegionRegistry* registry) {
        registry_ = registry;
    }

    /**
     * @brief Set the stack base (bottom of stack, highest address on x86)
     * Typically captured at thread creation.
     */
    void setStackBase(void* base) { stackBase_ = base; }

    /**
     * @brief Scan the current thread's stack for potential roots
     *
     * Walks from the current stack pointer up to the stack base,
     * checking each aligned word against known heap regions.
     *
     * @param callback Called for each potential root slot found
     * @return Number of potential roots found
     */
    size_t scanCurrentStack(RootCallback callback);

    /**
     * @brief Scan a specific memory range for potential roots
     * Used for scanning the VM's operand stack.
     */
    size_t scanRange(void* start, void* end, RootCallback callback);

    /**
     * @brief Statistics from last scan
     */
    struct ScanStats {
        size_t wordsScanned = 0;
        size_t candidatesFound = 0;
        size_t validatedRoots = 0;
        size_t bytesScanned = 0;
        double scanDurationUs = 0;
    };

    const ScanStats& lastStats() const { return stats_; }

    /**
     * @brief Get the current stack pointer (approximate)
     */
    static void* captureStackPointer();

    /**
     * @brief Capture stack base for the current thread
     * Should be called at thread startup.
     */
    static void* captureStackBase();

private:
    /**
     * @brief Check if a value could be a valid heap pointer
     */
    bool isCandidate(uintptr_t value) const;

    /**
     * @brief Flush registers to stack (via setjmp trick)
     */
    void flushRegisters();

    StackScanConfig config_;
    HeapRegionRegistry* registry_ = nullptr;
    void* stackBase_ = nullptr;
    ScanStats stats_;

    // Register spill buffer
    jmp_buf registerBuffer_;
};

// =============================================================================
// Thread Stack Info
// =============================================================================

/**
 * @brief Per-thread stack information for multi-threaded scanning
 */
struct ThreadStackInfo {
    void* stackBase;
    void* stackTop;        // Current stack pointer (updated at safe-points)
    bool suspended;        // Thread is at a safe-point
    std::atomic<bool> needsScan;

    size_t stackSize() const {
        // Stack grows downward on x86
        return static_cast<size_t>(
            static_cast<char*>(stackBase) - static_cast<char*>(stackTop));
    }
};

// =============================================================================
// Implementation
// =============================================================================

inline StackScanner::StackScanner(const StackScanConfig& config)
    : config_(config) {}

inline void* StackScanner::captureStackPointer() {
    void* sp;
#if defined(__GNUC__) || defined(__clang__)
    #if defined(__x86_64__)
        __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    #elif defined(__aarch64__)
        __asm__ volatile("mov %0, sp" : "=r"(sp));
    #else
        sp = __builtin_frame_address(0);
    #endif
#else
    volatile int marker;
    sp = const_cast<int*>(&marker);
#endif
    return sp;
}

inline void* StackScanner::captureStackBase() {
    // Heuristic: use current frame as base (imprecise but portable)
    // In production, use pthread_attr_getstack or /proc/self/maps
    return captureStackPointer();
}

inline void StackScanner::flushRegisters() {
    // setjmp flushes most callee-saved registers to the jmp_buf
    // which sits on the stack — making them scannable
    setjmp(registerBuffer_);
}

inline bool StackScanner::isCandidate(uintptr_t value) const {
    // Quick rejection: null, small integers, non-aligned
    if (value == 0) return false;
    if (value < 0x1000) return false;  // Below typical mmap range
    if (value & (config_.alignment - 1)) return false;  // Not aligned

    // Validate against known heap regions
    if (config_.validatePointers && registry_) {
        return registry_->isValidHeapPointer(reinterpret_cast<void*>(value));
    }

    // Without validation, accept all plausible-looking pointers
    return true;
}

inline size_t StackScanner::scanCurrentStack(RootCallback callback) {
    stats_ = {};

    // Flush registers to stack
    if (config_.scanRegisters) {
        flushRegisters();
    }

    void* stackTop = captureStackPointer();
    void* base = stackBase_ ? stackBase_ : stackTop;

    // Ensure base > top (stack grows downward on x86)
    if (static_cast<char*>(base) < static_cast<char*>(stackTop)) {
        std::swap(base, stackTop);
    }

    return scanRange(stackTop, base, callback);
}

inline size_t StackScanner::scanRange(void* start, void* end, RootCallback callback) {
    auto* low = static_cast<uintptr_t*>(start);
    auto* high = static_cast<uintptr_t*>(end);

    if (low > high) std::swap(low, high);

    // Limit scan depth
    size_t maxWords = config_.maxStackDepth / sizeof(uintptr_t);
    size_t words = static_cast<size_t>(high - low);
    if (words > maxWords) {
        high = low + maxWords;
        words = maxWords;
    }

    stats_.wordsScanned = words;
    stats_.bytesScanned = words * sizeof(uintptr_t);

    size_t rootCount = 0;

    for (auto* ptr = low; ptr < high; ptr++) {
        uintptr_t value = *ptr;

        if (isCandidate(value)) {
            stats_.candidatesFound++;

            // Pass the stack slot address (so GC can update the pointer)
            callback(reinterpret_cast<void**>(ptr));
            rootCount++;
        }
    }

    stats_.validatedRoots = rootCount;
    return rootCount;
}

} // namespace Zepra::Heap

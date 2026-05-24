// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_conservative_scanner.cpp — Stack and register scanning for GC roots

#include <cstdint>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <vector>
#include <functional>
#include <algorithm>

#ifdef __linux__
#include <setjmp.h>
#include <pthread.h>
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#endif

namespace Zepra::Heap {

struct HeapRegion {
    uintptr_t base;
    size_t size;

    bool contains(uintptr_t addr) const {
        return addr >= base && addr < base + size;
    }
};

class ConservativeScanner {
public:
    struct Callbacks {
        std::function<bool(uintptr_t addr)> isHeapPointer;
        std::function<void*(uintptr_t addr)> findContainingCell;
        std::function<void(void* cell)> addRoot;
    };

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    void addHeapRegion(uintptr_t base, size_t size) {
        heapRegions_.push_back({base, size});
        // Sort for binary search.
        std::sort(heapRegions_.begin(), heapRegions_.end(),
            [](const HeapRegion& a, const HeapRegion& b) { return a.base < b.base; });
    }

    void removeHeapRegion(uintptr_t base) {
        heapRegions_.erase(
            std::remove_if(heapRegions_.begin(), heapRegions_.end(),
                [base](const HeapRegion& r) { return r.base == base; }),
            heapRegions_.end());
    }

    // Scan the current thread's stack for potential heap pointers.
    size_t scanStack() {
        size_t found = 0;

#ifdef __linux__
        // Capture registers into a jmp_buf to make them scannable.
        jmp_buf registers;
        setjmp(registers);

        // Scan the register buffer.
        found += scanRange(reinterpret_cast<uintptr_t*>(&registers),
                          sizeof(registers) / sizeof(uintptr_t));

        // Get stack bounds.
        void* stackTop = __builtin_frame_address(0);

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_getattr_np(pthread_self(), &attr);

        void* stackBase = nullptr;
        size_t stackSize = 0;
        pthread_attr_getstack(&attr, &stackBase, &stackSize);
        pthread_attr_destroy(&attr);

        void* stackBottom = static_cast<uint8_t*>(stackBase) + stackSize;

        // Stack grows downward on x86-64.
        uintptr_t* scanStart = static_cast<uintptr_t*>(stackTop);
        uintptr_t* scanEnd = static_cast<uintptr_t*>(stackBottom);

        if (scanStart > scanEnd) std::swap(scanStart, scanEnd);

        found += scanRange(scanStart, static_cast<size_t>(scanEnd - scanStart));
#endif

        stats_.stackScans++;
        stats_.stackRootsFound += found;
        return found;
    }

    // Scan a given memory range for heap pointers.
    size_t scanRange(const uintptr_t* start, size_t wordCount) {
        size_t found = 0;

        for (size_t i = 0; i < wordCount; i++) {
            uintptr_t value = start[i];

            // Quick filter: skip values outside any heap region.
            if (!couldBePointer(value)) continue;

            // Detailed check.
            if (cb_.isHeapPointer && cb_.isHeapPointer(value)) {
                void* cell = nullptr;
                if (cb_.findContainingCell) {
                    cell = cb_.findContainingCell(value);
                }

                if (cell && cb_.addRoot) {
                    cb_.addRoot(cell);
                    found++;
                }
            }
        }

        return found;
    }

    // Scan a specific buffer (e.g., JIT frame, signal handler frame).
    size_t scanBuffer(const void* buffer, size_t byteSize) {
        if (!buffer || byteSize < sizeof(uintptr_t)) return 0;

        const uintptr_t* words = static_cast<const uintptr_t*>(buffer);
        size_t wordCount = byteSize / sizeof(uintptr_t);
        return scanRange(words, wordCount);
    }

    struct Stats {
        uint64_t stackScans = 0;
        uint64_t stackRootsFound = 0;
        uint64_t totalWordsScanned = 0;
        uint64_t candidatesChecked = 0;
        uint64_t falsePositives = 0;
    };

    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = {}; }

    size_t heapRegionCount() const { return heapRegions_.size(); }

private:
    bool couldBePointer(uintptr_t value) const {
        // Quick checks: alignment, non-zero, plausible address range.
        if (value == 0) return false;
        if (value & 0x07) return false;  // Must be 8-byte aligned.
        if (value < 0x10000) return false;  // Below typical heap start.

        // Binary search through heap regions.
        for (const auto& region : heapRegions_) {
            if (region.contains(value)) {
                stats_.candidatesChecked++;
                return true;
            }
        }
        return false;
    }

    Callbacks cb_;
    std::vector<HeapRegion> heapRegions_;
    mutable Stats stats_;
};

} // namespace Zepra::Heap

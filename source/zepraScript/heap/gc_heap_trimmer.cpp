// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_heap_trimmer.cpp — Return unused heap to OS

#include <mutex>
#include <algorithm>
#include <vector>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cassert>

namespace Zepra::Heap {

// Periodically scans the heap for empty pages/regions and returns
// them to the OS. Reduces resident memory when workload decreases.

class HeapTrimmer {
public:
    struct Backend {
        // Get list of empty (no live objects) pages.
        std::function<std::vector<std::pair<uintptr_t, size_t>>()> getEmptyPages;
        // Decommit a page (MADV_DONTNEED).
        std::function<bool(uintptr_t, size_t)> decommit;
        // Release a segment entirely (munmap).
        std::function<void(uintptr_t, size_t)> release;
        // Get current heap capacity.
        std::function<size_t()> heapCapacity;
        // Get current live bytes.
        std::function<size_t()> liveBytes;
    };

    void setBackend(Backend b) { backend_ = std::move(b); }

    // Trim the heap — decommit empty pages.
    size_t trim() {
        if (!backend_.getEmptyPages) return 0;

        auto emptyPages = backend_.getEmptyPages();
        size_t trimmed = 0;

        for (auto& [addr, size] : emptyPages) {
            if (backend_.decommit && backend_.decommit(addr, size)) {
                trimmed += size;
                stats_.pagesTrimmed++;
            }
        }

        stats_.bytesTrimmed += trimmed;
        return trimmed;
    }

    // Aggressive trim: also release entire empty segments.
    size_t aggressiveTrim() {
        size_t trimmed = trim();

        if (!backend_.getEmptyPages || !backend_.release) return trimmed;

        auto emptyPages = backend_.getEmptyPages();
        for (auto& [addr, size] : emptyPages) {
            // Release large empty regions.
            if (size >= 256 * 1024) {
                backend_.release(addr, size);
                trimmed += size;
                stats_.segmentsReleased++;
            }
        }

        stats_.bytesTrimmed += trimmed;
        return trimmed;
    }

    // Should we trim? Check occupancy ratio.
    bool shouldTrim() const {
        if (!backend_.heapCapacity || !backend_.liveBytes) return false;
        size_t capacity = backend_.heapCapacity();
        size_t live = backend_.liveBytes();
        if (capacity == 0) return false;
        // Trim if less than 30% occupied.
        return static_cast<double>(live) / capacity < 0.30;
    }

    struct Stats { uint64_t pagesTrimmed; uint64_t segmentsReleased; uint64_t bytesTrimmed; };
    Stats stats() const { return stats_; }

private:
    Backend backend_;
    Stats stats_{};
};

} // namespace Zepra::Heap

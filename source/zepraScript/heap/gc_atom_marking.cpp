// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_atom_marking.cpp — Concurrent-safe atom mark bitmap, atom sweep

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <cstring>

namespace Zepra::Heap {

static constexpr size_t kAtomBitmapSize = 65536;  // Max atoms tracked in bitmap
static constexpr size_t kBitmapWords = kAtomBitmapSize / 64;

class AtomMarkBitmap {
public:
    AtomMarkBitmap() { clear(); }

    // Atomically mark an atom as reachable. Returns true if newly marked.
    bool mark(uint32_t atomIndex) {
        assert(atomIndex < kAtomBitmapSize);
        size_t word = atomIndex / 64;
        uint64_t bit = 1ULL << (atomIndex % 64);

        uint64_t old = bitmap_[word].fetch_or(bit, std::memory_order_relaxed);
        return !(old & bit);
    }

    bool isMarked(uint32_t atomIndex) const {
        assert(atomIndex < kAtomBitmapSize);
        size_t word = atomIndex / 64;
        uint64_t bit = 1ULL << (atomIndex % 64);
        return bitmap_[word].load(std::memory_order_relaxed) & bit;
    }

    void clear() {
        for (size_t i = 0; i < kBitmapWords; i++) {
            bitmap_[i].store(0, std::memory_order_relaxed);
        }
    }

    // Count total marked atoms.
    size_t markedCount() const {
        size_t count = 0;
        for (size_t i = 0; i < kBitmapWords; i++) {
            uint64_t word = bitmap_[i].load(std::memory_order_relaxed);
            count += __builtin_popcountll(word);
        }
        return count;
    }

    // Iterate all marked atoms.
    template<typename Fn>
    void forEachMarked(Fn&& fn) const {
        for (size_t w = 0; w < kBitmapWords; w++) {
            uint64_t word = bitmap_[w].load(std::memory_order_relaxed);
            while (word) {
                uint32_t bit = __builtin_ctzll(word);
                fn(static_cast<uint32_t>(w * 64 + bit));
                word &= word - 1;
            }
        }
    }

    // Iterate all unmarked atoms (for sweep).
    template<typename Fn>
    void forEachUnmarked(size_t totalAtoms, Fn&& fn) const {
        for (uint32_t i = 0; i < totalAtoms && i < kAtomBitmapSize; i++) {
            if (!isMarked(i)) fn(i);
        }
    }

private:
    std::atomic<uint64_t> bitmap_[kBitmapWords];
};

struct AtomEntry {
    const char* data;
    uint32_t length;
    uint32_t hash;
    bool pinned;    // Permanent atoms (keywords, builtins)

    AtomEntry() : data(nullptr), length(0), hash(0), pinned(false) {}
};

class AtomMarker {
public:
    struct Callbacks {
        std::function<void(uint32_t atomIndex)> onAtomSwept;
        std::function<void(uint32_t atomIndex)> freeAtomData;
    };

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    void registerAtom(uint32_t index, const char* data, uint32_t length, uint32_t hash) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= atoms_.size()) atoms_.resize(index + 1);
        atoms_[index].data = data;
        atoms_[index].length = length;
        atoms_[index].hash = hash;
        atoms_[index].pinned = false;
        totalAtoms_ = std::max(totalAtoms_, static_cast<size_t>(index + 1));
    }

    void pinAtom(uint32_t index) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < atoms_.size()) atoms_[index].pinned = true;
    }

    // Mark an atom as reachable (called from tracer).
    void markAtom(uint32_t index) {
        bitmap_.mark(index);
    }

    // Sweep unmarked atoms after marking phase.
    size_t sweep() {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t swept = 0;

        bitmap_.forEachUnmarked(totalAtoms_, [&](uint32_t index) {
            if (index >= atoms_.size()) return;
            if (atoms_[index].pinned) return;

            if (cb_.freeAtomData) cb_.freeAtomData(index);
            if (cb_.onAtomSwept) cb_.onAtomSwept(index);
            atoms_[index] = {};
            swept++;
        });

        bitmap_.clear();
        stats_.sweepCount++;
        stats_.totalSwept += swept;
        return swept;
    }

    void prepareForMarking() { bitmap_.clear(); }

    size_t markedCount() const { return bitmap_.markedCount(); }
    size_t totalAtoms() const { return totalAtoms_; }
    size_t liveAtoms() const { return bitmap_.markedCount(); }

    const AtomEntry* atom(uint32_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return index < atoms_.size() ? &atoms_[index] : nullptr;
    }

    struct Stats {
        uint64_t sweepCount = 0;
        uint64_t totalSwept = 0;
    };

    const Stats& stats() const { return stats_; }

private:
    mutable std::mutex mutex_;
    AtomMarkBitmap bitmap_;
    std::vector<AtomEntry> atoms_;
    size_t totalAtoms_ = 0;
    Callbacks cb_;
    Stats stats_;
};

} // namespace Zepra::Heap

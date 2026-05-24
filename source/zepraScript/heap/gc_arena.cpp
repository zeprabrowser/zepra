// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_arena.cpp — Arena page (16KB), cell layout, per-arena free list

#include "zepra_alloc.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <atomic>
#include <mutex>
#include <functional>
#include <algorithm>

#ifdef __linux__
#include <sys/mman.h>
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#endif

namespace Zepra::Heap {

static constexpr size_t kArenaSize = 16384;           // 16KB
static constexpr size_t kArenaHeaderSize = 128;        // Reserved for metadata
static constexpr size_t kMaxCellsPerArena = 1024;
static constexpr size_t kMinCellSize = 16;

struct FreeCell {
    FreeCell* next;
};

struct ArenaHeader {
    uint32_t zoneId;
    uint16_t sizeClass;            // Cell size in this arena
    uint16_t totalCells;           // Total cell capacity
    uint16_t liveCells;
    uint16_t markedCells;
    uint8_t  generation;           // 0 = nursery, 1+ = old-gen
    uint8_t  flags;
    uint8_t  padding[2];
    FreeCell* freeList;
    ArenaHeader* next;             // Next arena in list
    ArenaHeader* prev;
    uint64_t markBitmap[kMaxCellsPerArena / 64];  // 1 bit per cell

    static constexpr uint8_t kFlagCompactable = 0x01;
    static constexpr uint8_t kFlagDecommitted = 0x02;
    static constexpr uint8_t kFlagFull        = 0x04;
    static constexpr uint8_t kFlagSweepNeeded = 0x08;

    bool isCompactable() const { return flags & kFlagCompactable; }
    bool isDecommitted() const { return flags & kFlagDecommitted; }
    bool isFull() const { return flags & kFlagFull; }
    bool needsSweep() const { return flags & kFlagSweepNeeded; }

    void setFlag(uint8_t f) { flags |= f; }
    void clearFlag(uint8_t f) { flags &= ~f; }
};

class Arena {
public:
    static ArenaHeader* allocateArena(uint32_t zoneId, uint16_t sizeClass) {
        assert(sizeClass >= kMinCellSize);
        assert(sizeClass <= kArenaSize - kArenaHeaderSize);

        void* mem = nullptr;
#ifdef __linux__
        mem = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return nullptr;
#else
        mem = zepra_aligned_alloc(kArenaSize, kArenaSize);
        if (!mem) return nullptr;
        memset(mem, 0, kArenaSize);
#endif

        auto* header = static_cast<ArenaHeader*>(mem);
        header->zoneId = zoneId;
        header->sizeClass = sizeClass;
        header->totalCells = static_cast<uint16_t>((kArenaSize - kArenaHeaderSize) / sizeClass);
        if (header->totalCells > kMaxCellsPerArena)
            header->totalCells = kMaxCellsPerArena;
        header->liveCells = 0;
        header->markedCells = 0;
        header->generation = 0;
        header->flags = ArenaHeader::kFlagCompactable;
        header->freeList = nullptr;
        header->next = nullptr;
        header->prev = nullptr;
        memset(header->markBitmap, 0, sizeof(header->markBitmap));

        buildFreeList(header);
        return header;
    }

    static void deallocateArena(ArenaHeader* arena) {
        if (!arena) return;
#ifdef __linux__
        munmap(arena, kArenaSize);
#else
        free(arena);
#endif
    }

    static void* allocateCell(ArenaHeader* arena) {
        if (!arena->freeList) return nullptr;

        FreeCell* cell = arena->freeList;
        arena->freeList = cell->next;
        arena->liveCells++;

        if (!arena->freeList) {
            arena->setFlag(ArenaHeader::kFlagFull);
        }

        return cell;
    }

    static void freeCell(ArenaHeader* arena, void* cell) {
        auto* fc = static_cast<FreeCell*>(cell);
        fc->next = arena->freeList;
        arena->freeList = fc;
        if (arena->liveCells > 0) arena->liveCells--;
        arena->clearFlag(ArenaHeader::kFlagFull);
    }

    // Mark a cell by index.
    static void markCell(ArenaHeader* arena, uint16_t cellIndex) {
        assert(cellIndex < arena->totalCells);
        size_t word = cellIndex / 64;
        size_t bit = cellIndex % 64;
        arena->markBitmap[word] |= (1ULL << bit);
        arena->markedCells++;
    }

    static void unmarkCell(ArenaHeader* arena, uint16_t cellIndex) {
        assert(cellIndex < arena->totalCells);
        size_t word = cellIndex / 64;
        size_t bit = cellIndex % 64;
        arena->markBitmap[word] &= ~(1ULL << bit);
        if (arena->markedCells > 0) arena->markedCells--;
    }

    static bool isCellMarked(const ArenaHeader* arena, uint16_t cellIndex) {
        size_t word = cellIndex / 64;
        size_t bit = cellIndex % 64;
        return (arena->markBitmap[word] >> bit) & 1;
    }

    static void clearAllMarks(ArenaHeader* arena) {
        memset(arena->markBitmap, 0, sizeof(arena->markBitmap));
        arena->markedCells = 0;
    }

    // Sweep: free all unmarked cells, rebuild free list.
    static size_t sweep(ArenaHeader* arena) {
        size_t freedBytes = 0;
        uint8_t* cellBase = reinterpret_cast<uint8_t*>(arena) + kArenaHeaderSize;

        arena->freeList = nullptr;
        arena->liveCells = 0;
        arena->clearFlag(ArenaHeader::kFlagFull);

        for (uint16_t i = 0; i < arena->totalCells; i++) {
            if (isCellMarked(arena, i)) {
                arena->liveCells++;
            } else {
                void* cell = cellBase + (i * arena->sizeClass);
                auto* fc = static_cast<FreeCell*>(cell);
                fc->next = arena->freeList;
                arena->freeList = fc;
                freedBytes += arena->sizeClass;
            }
        }

        clearAllMarks(arena);
        arena->clearFlag(ArenaHeader::kFlagSweepNeeded);

        if (!arena->freeList) arena->setFlag(ArenaHeader::kFlagFull);

        return freedBytes;
    }

    // Get cell index from pointer.
    static int32_t cellIndex(const ArenaHeader* arena, const void* cell) {
        const uint8_t* cellBase = reinterpret_cast<const uint8_t*>(arena) + kArenaHeaderSize;
        const uint8_t* cellPtr = static_cast<const uint8_t*>(cell);

        if (cellPtr < cellBase) return -1;
        size_t offset = static_cast<size_t>(cellPtr - cellBase);
        if (offset % arena->sizeClass != 0) return -1;

        uint16_t idx = static_cast<uint16_t>(offset / arena->sizeClass);
        return idx < arena->totalCells ? static_cast<int32_t>(idx) : -1;
    }

    // Get cell pointer from index.
    static void* cellAt(ArenaHeader* arena, uint16_t index) {
        assert(index < arena->totalCells);
        uint8_t* cellBase = reinterpret_cast<uint8_t*>(arena) + kArenaHeaderSize;
        return cellBase + (index * arena->sizeClass);
    }

    // Check if pointer belongs to this arena.
    static bool contains(const ArenaHeader* arena, const void* ptr) {
        const uint8_t* base = reinterpret_cast<const uint8_t*>(arena);
        const uint8_t* p = static_cast<const uint8_t*>(ptr);
        return p >= base + kArenaHeaderSize && p < base + kArenaSize;
    }

    static double occupancy(const ArenaHeader* arena) {
        return arena->totalCells > 0
            ? static_cast<double>(arena->liveCells) / arena->totalCells
            : 0;
    }

    static bool isEmpty(const ArenaHeader* arena) { return arena->liveCells == 0; }

    // Decommit arena memory (release physical pages back to OS).
    static void decommit(ArenaHeader* arena) {
#ifdef __linux__
        uint8_t* cellBase = reinterpret_cast<uint8_t*>(arena) + kArenaHeaderSize;
        size_t cellRegionSize = kArenaSize - kArenaHeaderSize;
        madvise(cellBase, cellRegionSize, MADV_DONTNEED);
#endif
        arena->setFlag(ArenaHeader::kFlagDecommitted);
    }

    // Recommit arena memory.
    static void recommit(ArenaHeader* arena) {
        arena->clearFlag(ArenaHeader::kFlagDecommitted);
        buildFreeList(arena);
    }

private:
    static void buildFreeList(ArenaHeader* arena) {
        uint8_t* cellBase = reinterpret_cast<uint8_t*>(arena) + kArenaHeaderSize;
        arena->freeList = nullptr;

        for (int i = arena->totalCells - 1; i >= 0; i--) {
            auto* cell = reinterpret_cast<FreeCell*>(cellBase + (i * arena->sizeClass));
            cell->next = arena->freeList;
            arena->freeList = cell;
        }
    }
};

// Arena pool: pre-allocates arenas for reuse, reducing mmap syscall frequency.
class ArenaPool {
public:
    ArenaHeader* acquire(uint32_t zoneId, uint16_t sizeClass) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Try to reuse a pooled arena.
        for (auto it = pool_.begin(); it != pool_.end(); ++it) {
            if ((*it)->sizeClass == sizeClass) {
                ArenaHeader* arena = *it;
                pool_.erase(it);
                arena->zoneId = zoneId;
                arena->liveCells = 0;
                arena->markedCells = 0;
                Arena::clearAllMarks(arena);
                Arena::recommit(arena);
                return arena;
            }
        }

        return Arena::allocateArena(zoneId, sizeClass);
    }

    void release(ArenaHeader* arena) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (pool_.size() >= maxPooled_) {
            Arena::deallocateArena(arena);
            return;
        }

        Arena::decommit(arena);
        pool_.push_back(arena);
    }

    void purge() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* a : pool_) {
            Arena::deallocateArena(a);
        }
        pool_.clear();
    }

    size_t pooledCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pool_.size();
    }

    void setMaxPooled(size_t max) { maxPooled_ = max; }

private:
    mutable std::mutex mutex_;
    std::vector<ArenaHeader*> pool_;
    size_t maxPooled_ = 64;
};

} // namespace Zepra::Heap

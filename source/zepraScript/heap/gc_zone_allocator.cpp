// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_zone_allocator.cpp — Zone-scoped bump allocator with arena request

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <mutex>
#include <atomic>
#include <functional>
#include <vector>
#include <memory>

namespace Zepra::Heap {

static constexpr size_t kArenaPageSize = 16384;  // 16KB per arena page
static constexpr size_t kCellAlignment = 8;

struct ArenaPage;  // Forward declaration

class ZoneAllocator {
public:
    struct Callbacks {
        std::function<ArenaPage*(uint32_t zoneId, size_t sizeClass)> requestArena;
        std::function<void(ArenaPage* arena)> returnArena;
        std::function<void(uint32_t zoneId, size_t bytes)> onAllocation;
    };

    explicit ZoneAllocator(uint32_t zoneId) : zoneId_(zoneId), cursor_(nullptr)
        , limit_(nullptr), currentArena_(nullptr), totalAllocated_(0) {}

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // Fast-path bump allocation within current arena.
    void* allocate(size_t size) {
        size = align(size);

        if (cursor_ + size <= limit_) {
            void* result = cursor_;
            cursor_ += size;
            totalAllocated_ += size;
            allocCount_++;
            if (cb_.onAllocation) cb_.onAllocation(zoneId_, size);
            return result;
        }

        return allocateSlow(size);
    }

    // Slow path: request new arena and retry.
    void* allocateSlow(size_t size) {
        if (!cb_.requestArena) return nullptr;

        size_t sizeClass = sizeClassFor(size);
        ArenaPage* arena = cb_.requestArena(zoneId_, sizeClass);
        if (!arena) return nullptr;

        installArena(arena);
        return allocate(size);
    }

    void installArena(ArenaPage* arena) {
        if (currentArena_) {
            usedArenas_.push_back(currentArena_);
        }
        currentArena_ = arena;
        cursor_ = reinterpret_cast<uint8_t*>(arena) + kArenaPageSize;
        limit_ = reinterpret_cast<uint8_t*>(arena) + kArenaPageSize;
    }

    void reset() {
        for (auto* a : usedArenas_) {
            if (cb_.returnArena) cb_.returnArena(a);
        }
        usedArenas_.clear();
        if (currentArena_ && cb_.returnArena) {
            cb_.returnArena(currentArena_);
        }
        currentArena_ = nullptr;
        cursor_ = nullptr;
        limit_ = nullptr;
        totalAllocated_ = 0;
        allocCount_ = 0;
    }

    uint32_t zoneId() const { return zoneId_; }
    size_t totalAllocated() const { return totalAllocated_; }
    size_t allocCount() const { return allocCount_; }
    size_t arenaCount() const { return usedArenas_.size() + (currentArena_ ? 1 : 0); }

    size_t remainingInCurrentArena() const {
        return cursor_ && limit_ ? static_cast<size_t>(limit_ - cursor_) : 0;
    }

    bool hasCapacity(size_t bytes) const {
        return remainingInCurrentArena() >= align(bytes);
    }

private:
    static size_t align(size_t size) {
        return (size + kCellAlignment - 1) & ~(kCellAlignment - 1);
    }

    static size_t sizeClassFor(size_t size) {
        if (size <= 16) return 16;
        if (size <= 32) return 32;
        if (size <= 48) return 48;
        if (size <= 64) return 64;
        if (size <= 128) return 128;
        if (size <= 256) return 256;
        if (size <= 512) return 512;
        return 1024;
    }

    uint32_t zoneId_;
    Callbacks cb_;
    uint8_t* cursor_;
    uint8_t* limit_;
    ArenaPage* currentArena_;
    std::vector<ArenaPage*> usedArenas_;
    size_t totalAllocated_ = 0;
    size_t allocCount_ = 0;
};

// Per-zone allocator pool: one ZoneAllocator per zone, thread-safe lookup.
class ZoneAllocatorPool {
public:
    ZoneAllocator* getOrCreate(uint32_t zoneId) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& a : allocators_) {
            if (a->zoneId() == zoneId) return a.get();
        }
        allocators_.push_back(std::make_unique<ZoneAllocator>(zoneId));
        return allocators_.back().get();
    }

    void remove(uint32_t zoneId) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = allocators_.begin(); it != allocators_.end(); ++it) {
            if ((*it)->zoneId() == zoneId) {
                (*it)->reset();
                allocators_.erase(it);
                return;
            }
        }
    }

    void resetAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& a : allocators_) a->reset();
    }

    size_t totalAllocated() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (auto& a : allocators_) total += a->totalAllocated();
        return total;
    }

    size_t totalArenaCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (auto& a : allocators_) total += a->arenaCount();
        return total;
    }

    template<typename Fn>
    void forEach(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& a : allocators_) fn(a.get());
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<ZoneAllocator>> allocators_;
};

} // namespace Zepra::Heap

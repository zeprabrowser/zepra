// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_thread_state.cpp — Per-thread GC state, allocation context, safepoint flag

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <atomic>
#include <mutex>
#include <vector>
#include <thread>
#include <functional>

namespace Zepra::Heap {

enum class ThreadGCState : uint8_t {
    Mutating,       // Normal execution
    SafepointWait,  // Waiting at safepoint for GC
    GCMarking,      // Participating in parallel marking
    GCSweeping,     // Participating in parallel sweeping
    NativeCall,     // In native/FFI code — GC-safe
    Blocked,        // Blocked on I/O or lock
};

class ThreadAllocContext {
public:
    ThreadAllocContext() : nurseryBump_(0), nurseryEnd_(0), nurseryStart_(0)
        , allocatedBytes_(0), allocationCount_(0) {}

    // Bump-allocate from thread-local nursery segment.
    void* allocateNursery(size_t size) {
        size = (size + 7) & ~7;  // 8-byte align.
        uintptr_t result = nurseryBump_;
        uintptr_t newBump = result + size;

        if (newBump > nurseryEnd_) return nullptr;  // Need slow path.

        nurseryBump_ = newBump;
        allocatedBytes_ += size;
        allocationCount_++;
        return reinterpret_cast<void*>(result);
    }

    void setNurserySegment(uintptr_t start, uintptr_t end) {
        nurseryStart_ = start;
        nurseryBump_ = start;
        nurseryEnd_ = end;
    }

    void resetNursery() {
        nurseryBump_ = nurseryStart_;
    }

    uintptr_t nurseryBump() const { return nurseryBump_; }
    size_t nurseryUsed() const { return nurseryBump_ - nurseryStart_; }
    size_t nurseryRemaining() const { return nurseryEnd_ > nurseryBump_ ? nurseryEnd_ - nurseryBump_ : 0; }
    bool nurseryExhausted() const { return nurseryBump_ >= nurseryEnd_; }

    size_t allocatedBytes() const { return allocatedBytes_; }
    uint64_t allocationCount() const { return allocationCount_; }

    void resetStats() { allocatedBytes_ = 0; allocationCount_ = 0; }

private:
    uintptr_t nurseryBump_;
    uintptr_t nurseryEnd_;
    uintptr_t nurseryStart_;
    size_t allocatedBytes_;
    uint64_t allocationCount_;
};

struct GCThreadState {
    uint32_t id;
    std::thread::id osThreadId;
    std::atomic<ThreadGCState> state{ThreadGCState::Mutating};
    std::atomic<bool> safepointRequested{false};
    ThreadAllocContext allocContext;
    void* stackBase;
    void* stackTop;
    uint64_t safepoints_hit;

    GCThreadState() : id(0), stackBase(nullptr), stackTop(nullptr), safepoints_hit(0) {}

    bool isAtSafepoint() const { return state.load() == ThreadGCState::SafepointWait; }
    bool isInNative() const { return state.load() == ThreadGCState::NativeCall; }
    bool isBlocked() const { return state.load() == ThreadGCState::Blocked; }
    bool isGCSafe() const {
        ThreadGCState s = state.load();
        return s != ThreadGCState::Mutating;
    }
};

class ThreadStateRegistry {
public:
    uint32_t registerThread() {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t id = static_cast<uint32_t>(threads_.size());
        auto thr = std::make_unique<GCThreadState>();
        thr->id = id;
        thr->osThreadId = std::this_thread::get_id();
        threads_.push_back(std::move(thr));
        return id;
    }

    void unregisterThread(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id < threads_.size()) {
            threads_[id].reset();
        }
    }

    GCThreadState* threadState(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return id < threads_.size() ? threads_[id].get() : nullptr;
    }

    // Check if all registered threads are GC-safe.
    bool allThreadsGCSafe() const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : threads_) {
            if (t && !t->isGCSafe()) return false;
        }
        return true;
    }

    // Iterate all active threads.
    template<typename Fn>
    void forEach(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : threads_) {
            if (t) fn(t.get());
        }
    }

    // Request all threads to reach safepoint.
    void requestSafepointAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : threads_) {
            if (t) t->safepointRequested.store(true, std::memory_order_release);
        }
    }

    void clearSafepointAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : threads_) {
            if (t) t->safepointRequested.store(false, std::memory_order_release);
        }
    }

    uint32_t activeCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t count = 0;
        for (auto& t : threads_) {
            if (t) count++;
        }
        return count;
    }

    // Aggregate allocation stats across all threads.
    size_t totalAllocatedBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (auto& t : threads_) {
            if (t) total += t->allocContext.allocatedBytes();
        }
        return total;
    }

    uint64_t totalAllocations() const {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t total = 0;
        for (auto& t : threads_) {
            if (t) total += t->allocContext.allocationCount();
        }
        return total;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<GCThreadState>> threads_;
};

} // namespace Zepra::Heap

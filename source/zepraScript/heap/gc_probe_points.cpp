// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_probe_points.cpp — Probe points for external GC tooling

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <thread>

namespace Zepra::Heap {

enum class ProbeEvent : uint8_t {
    GCStart,
    GCEnd,
    MarkStart,
    MarkEnd,
    SweepStart,
    SweepEnd,
    CompactStart,
    CompactEnd,
    AllocSlowPath,
    NurseryPromotion,
    LargeObjectAlloc,
    ArenaAllocated,
    ArenaReleased,
    StoreBufferOverflow,
    MemoryPressure,
    SafepointEnter,
    SafepointExit,
    Count,
};

struct ProbeData {
    ProbeEvent event;
    uint64_t timestamp;
    uint64_t arg1;
    uint64_t arg2;
    uint32_t threadId;

    ProbeData() : event{}, timestamp(0), arg1(0), arg2(0), threadId(0) {}
};

using ProbeFn = std::function<void(const ProbeData& data)>;

class GCProbePoints {
public:
    GCProbePoints() : enabled_(false) {
        for (int i = 0; i < static_cast<int>(ProbeEvent::Count); i++) {
            eventCounters_[i].store(0, std::memory_order_relaxed);
        }
    }

    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }

    // Register a probe handler for a specific event.
    void registerProbe(ProbeEvent event, ProbeFn fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t idx = static_cast<size_t>(event);
        if (idx < static_cast<size_t>(ProbeEvent::Count)) {
            probes_[idx].push_back(std::move(fn));
        }
    }

    // Register a handler for all events.
    void registerGlobalProbe(ProbeFn fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        globalProbes_.push_back(std::move(fn));
    }

    // Fire a probe event.
    void fire(ProbeEvent event, uint64_t arg1 = 0, uint64_t arg2 = 0) {
        size_t idx = static_cast<size_t>(event);
        eventCounters_[idx].fetch_add(1, std::memory_order_relaxed);

        if (!enabled_) return;

        ProbeData data;
        data.event = event;
        data.timestamp = currentTimestamp();
        data.arg1 = arg1;
        data.arg2 = arg2;
        data.threadId = currentThreadId();

        std::lock_guard<std::mutex> lock(mutex_);

        for (auto& fn : probes_[idx]) fn(data);
        for (auto& fn : globalProbes_) fn(data);

        // Ring buffer.
        if (ringBuffer_.size() >= ringBufferSize_) {
            ringBuffer_.erase(ringBuffer_.begin());
        }
        ringBuffer_.push_back(data);
    }

    // Shorthand fire methods.
    void fireGCStart(uint64_t heapBytes) { fire(ProbeEvent::GCStart, heapBytes); }
    void fireGCEnd(uint64_t freed) { fire(ProbeEvent::GCEnd, freed); }
    void fireMarkStart() { fire(ProbeEvent::MarkStart); }
    void fireMarkEnd(uint64_t marked) { fire(ProbeEvent::MarkEnd, marked); }
    void fireSweepStart() { fire(ProbeEvent::SweepStart); }
    void fireSweepEnd(uint64_t freed) { fire(ProbeEvent::SweepEnd, freed); }
    void fireAllocSlowPath(uint64_t size) { fire(ProbeEvent::AllocSlowPath, size); }
    void fireNurseryPromotion(uint64_t bytes) { fire(ProbeEvent::NurseryPromotion, bytes); }
    void fireLargeObjectAlloc(uint64_t size) { fire(ProbeEvent::LargeObjectAlloc, size); }
    void fireStoreBufferOverflow() { fire(ProbeEvent::StoreBufferOverflow); }
    void fireMemoryPressure(uint64_t level) { fire(ProbeEvent::MemoryPressure, level); }

    uint64_t eventCount(ProbeEvent event) const {
        return eventCounters_[static_cast<size_t>(event)].load(std::memory_order_relaxed);
    }

    const std::vector<ProbeData>& ringBuffer() const { return ringBuffer_; }
    void setRingBufferSize(size_t size) { ringBufferSize_ = size; }

    void clearHandlers() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < static_cast<int>(ProbeEvent::Count); i++) probes_[i].clear();
        globalProbes_.clear();
    }

private:
    static uint64_t currentTimestamp() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    static uint32_t currentThreadId() {
        return static_cast<uint32_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    bool enabled_;
    std::mutex mutex_;
    std::vector<ProbeFn> probes_[static_cast<size_t>(ProbeEvent::Count)];
    std::vector<ProbeFn> globalProbes_;
    std::atomic<uint64_t> eventCounters_[static_cast<size_t>(ProbeEvent::Count)];
    std::vector<ProbeData> ringBuffer_;
    size_t ringBufferSize_ = 4096;
};

} // namespace Zepra::Heap

// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_activity_callback.cpp — Eden/full GC activity callbacks for embedder

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <mutex>
#include <vector>
#include <functional>
#include <chrono>

namespace Zepra::Heap {

enum class GCActivity : uint8_t {
    MinorGCStart,
    MinorGCEnd,
    MajorGCStart,
    MajorGCEnd,
    IncrementalMarkStart,
    IncrementalMarkStep,
    IncrementalMarkEnd,
    CompactionStart,
    CompactionEnd,
    MemoryPressureGC,
    IdleGC,
};

struct GCActivityEvent {
    GCActivity activity;
    uint64_t timestampMs;
    size_t heapBytesBeforeGC;
    size_t heapBytesAfterGC;
    double durationMs;
    size_t freedBytes;

    GCActivityEvent() : activity{}, timestampMs(0), heapBytesBeforeGC(0)
        , heapBytesAfterGC(0), durationMs(0), freedBytes(0) {}
};

using ActivityCallback = std::function<void(const GCActivityEvent& event)>;

class GCActivityCallbackManager {
public:
    struct RegisteredCallback {
        uint32_t id;
        ActivityCallback callback;
        uint32_t activityMask;  // Bitmask of GCActivity types to listen for

        bool listensFor(GCActivity activity) const {
            return activityMask & (1u << static_cast<uint8_t>(activity));
        }
    };

    // Register a callback with activity filter.
    uint32_t registerCallback(ActivityCallback cb, uint32_t activityMask = 0xFFFFFFFF) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t id = nextId_++;
        callbacks_.push_back({id, std::move(cb), activityMask});
        return id;
    }

    void unregisterCallback(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.erase(
            std::remove_if(callbacks_.begin(), callbacks_.end(),
                [id](const RegisteredCallback& c) { return c.id == id; }),
            callbacks_.end());
    }

    // Fire an activity event to all matching listeners.
    void fireEvent(GCActivity activity, size_t heapBefore, size_t heapAfter,
                   double durationMs) {
        GCActivityEvent event;
        event.activity = activity;
        event.timestampMs = currentTimestampMs();
        event.heapBytesBeforeGC = heapBefore;
        event.heapBytesAfterGC = heapAfter;
        event.durationMs = durationMs;
        event.freedBytes = heapBefore > heapAfter ? heapBefore - heapAfter : 0;

        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& cb : callbacks_) {
            if (cb.listensFor(activity)) {
                cb.callback(event);
            }
        }

        history_.push_back(event);
        if (history_.size() > maxHistory_) {
            history_.erase(history_.begin());
        }
    }

    // Helper for firing start/end pairs.
    void fireMinorGCStart(size_t heapBytes) {
        fireEvent(GCActivity::MinorGCStart, heapBytes, heapBytes, 0);
    }

    void fireMinorGCEnd(size_t heapBefore, size_t heapAfter, double ms) {
        fireEvent(GCActivity::MinorGCEnd, heapBefore, heapAfter, ms);
    }

    void fireMajorGCStart(size_t heapBytes) {
        fireEvent(GCActivity::MajorGCStart, heapBytes, heapBytes, 0);
    }

    void fireMajorGCEnd(size_t heapBefore, size_t heapAfter, double ms) {
        fireEvent(GCActivity::MajorGCEnd, heapBefore, heapAfter, ms);
    }

    const std::vector<GCActivityEvent>& history() const { return history_; }
    void setMaxHistory(size_t max) { maxHistory_ = max; }

    size_t callbackCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return callbacks_.size();
    }

private:
    static uint64_t currentTimestampMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    mutable std::mutex mutex_;
    std::vector<RegisteredCallback> callbacks_;
    std::vector<GCActivityEvent> history_;
    uint32_t nextId_ = 1;
    size_t maxHistory_ = 1024;
};

} // namespace Zepra::Heap

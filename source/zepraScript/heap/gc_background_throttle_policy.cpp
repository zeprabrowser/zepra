// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_background_throttle_policy.cpp — Background task footprint minimizer

#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <chrono>

namespace Zepra::Heap {

class BackgroundThrottlePolicy {
public:
    struct Config {
        size_t maxBytesPerSecond;       // Allocation rate cap for background tabs
        uint32_t timerThrottleMs;       // Minimum timer interval in background
        uint32_t idleDetectionSec;      // Seconds of no allocation = idle
        bool allowIncrementalGCOnly;    // No full GC in background

        Config()
            : maxBytesPerSecond(1024 * 1024)  // 1MB/s
            , timerThrottleMs(1000)
            , idleDetectionSec(30)
            , allowIncrementalGCOnly(true) {}
    };

    struct TabThrottleState {
        uint64_t tabId;
        size_t bytesThisWindow;
        uint64_t windowStartMs;
        uint64_t lastAllocMs;
        bool isIdle;
        bool isThrottled;

        TabThrottleState()
            : tabId(0), bytesThisWindow(0), windowStartMs(0)
            , lastAllocMs(0), isIdle(false), isThrottled(false) {}
    };

    struct Callbacks {
        std::function<void(uint64_t tabId)> onIdleDetected;
        std::function<void(uint64_t tabId)> onThrottleEngaged;
    };

    explicit BackgroundThrottlePolicy(const Config& config = Config{})
        : config_(config) {}

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    void registerTab(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        TabThrottleState s;
        s.tabId = tabId;
        s.windowStartMs = nowMs();
        tabs_[tabId] = s;
    }

    void unregisterTab(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        tabs_.erase(tabId);
    }

    // Returns true if allocation is allowed, false if throttled.
    bool checkAllocation(uint64_t tabId, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        if (it == tabs_.end()) return true;

        auto& s = it->second;
        uint64_t now = nowMs();

        // Reset window every second.
        if (now - s.windowStartMs >= 1000) {
            s.bytesThisWindow = 0;
            s.windowStartMs = now;
        }

        if (s.bytesThisWindow + bytes > config_.maxBytesPerSecond) {
            if (!s.isThrottled) {
                s.isThrottled = true;
                if (cb_.onThrottleEngaged) cb_.onThrottleEngaged(tabId);
            }
            stats_.throttledAllocations++;
            return false;
        }

        s.bytesThisWindow += bytes;
        s.lastAllocMs = now;
        s.isThrottled = false;
        s.isIdle = false;
        return true;
    }

    // Check for idle background tabs.
    void checkIdleTabs() {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t now = nowMs();

        for (auto& [id, s] : tabs_) {
            if (s.isIdle) continue;

            uint64_t sinceLastAlloc = now - s.lastAllocMs;
            if (sinceLastAlloc >= config_.idleDetectionSec * 1000) {
                s.isIdle = true;
                if (cb_.onIdleDetected) cb_.onIdleDetected(id);
                stats_.idleDetections++;
            }
        }
    }

    uint32_t throttledTimerInterval() const { return config_.timerThrottleMs; }
    bool allowFullGC() const { return !config_.allowIncrementalGCOnly; }

    bool isTabIdle(uint64_t tabId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        return it != tabs_.end() ? it->second.isIdle : false;
    }

    bool isTabThrottled(uint64_t tabId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        return it != tabs_.end() ? it->second.isThrottled : false;
    }

    struct Stats { uint64_t throttledAllocations; uint64_t idleDetections; };
    Stats stats() const { return stats_; }

private:
    static uint64_t nowMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    Config config_;
    Callbacks cb_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, TabThrottleState> tabs_;
    Stats stats_{};
};

} // namespace Zepra::Heap

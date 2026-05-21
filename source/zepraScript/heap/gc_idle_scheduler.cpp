// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_idle_scheduler.cpp — GC during browser idle periods

#include <atomic>
#include <algorithm>
#include <mutex>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <chrono>

namespace Zepra::Heap {

// Schedule GC work during idle time to minimize user-visible jank.
// Idle periods: no animation frames, no input events, no network I/O.
// This reduces perceived latency and keeps RAM usage low.

class IdleGCScheduler {
public:
    struct Config {
        double idleDeadlineMs;       // Max GC work per idle callback
        double minIdlePeriodMs;      // Ignore idle periods shorter than this
        size_t maxIncrementalBytes;  // Max bytes to mark per idle step
        bool aggressiveOnBackground; // Full GC when tab backgrounded
        double backgroundDelayMs;    // Wait before background GC

        Config()
            : idleDeadlineMs(5.0)
            , minIdlePeriodMs(1.0)
            , maxIncrementalBytes(16 * 1024)
            , aggressiveOnBackground(true)
            , backgroundDelayMs(5000.0) {}
    };

    struct GCWork {
        std::function<size_t(size_t budget)> incrementalMark;
        std::function<void()> incrementalSweep;
        std::function<void()> trimHeap;
        std::function<void()> decommitEmptyPages;
        std::function<void()> fullCollect;
    };

    explicit IdleGCScheduler(const Config& config = Config{})
        : config_(config) {}

    void setGCWork(GCWork work) { work_ = std::move(work); }

    // Called by the browser event loop during requestIdleCallback.
    void onIdle(double deadlineMs) {
        if (deadlineMs < config_.minIdlePeriodMs) return;

        auto start = std::chrono::steady_clock::now();
        double remaining = deadlineMs;

        // Priority 1: Incremental marking.
        if (remaining > 1.0 && work_.incrementalMark) {
            work_.incrementalMark(config_.maxIncrementalBytes);
            remaining = updateRemaining(start, deadlineMs);
            stats_.idleMarkSteps++;
        }

        // Priority 2: Incremental sweeping.
        if (remaining > 1.0 && work_.incrementalSweep) {
            work_.incrementalSweep();
            remaining = updateRemaining(start, deadlineMs);
        }

        // Priority 3: Trim heap (return pages to OS).
        if (remaining > 0.5 && work_.trimHeap) {
            work_.trimHeap();
        }

        stats_.idleCallbacks++;
    }

    // Called when tab goes to background.
    void onTabBackground() {
        tabBackgrounded_ = true;
        backgroundTime_ = std::chrono::steady_clock::now();
    }

    // Called when tab returns to foreground.
    void onTabForeground() {
        tabBackgrounded_ = false;
    }

    // Periodic check: if backgrounded long enough, do full GC.
    void tick() {
        if (!tabBackgrounded_ || !config_.aggressiveOnBackground) return;

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(
            now - backgroundTime_).count();

        if (elapsed >= config_.backgroundDelayMs) {
            if (work_.fullCollect) {
                work_.fullCollect();
                stats_.backgroundCollections++;
            }
            if (work_.decommitEmptyPages) {
                work_.decommitEmptyPages();
            }
            tabBackgrounded_ = false;
        }
    }

    struct Stats {
        uint64_t idleCallbacks;
        uint64_t idleMarkSteps;
        uint64_t backgroundCollections;
    };

    const Stats& stats() const { return stats_; }

private:
    double updateRemaining(
            std::chrono::steady_clock::time_point start, double deadline) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(
            now - start).count();
        return deadline - elapsed;
    }

    Config config_;
    GCWork work_;
    Stats stats_{};
    bool tabBackgrounded_ = false;
    std::chrono::steady_clock::time_point backgroundTime_;
};

} // namespace Zepra::Heap

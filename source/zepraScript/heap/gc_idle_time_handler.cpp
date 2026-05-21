// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_idle_time_handler.cpp — Idle-time GC trigger for background collection

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <chrono>
#include <atomic>
#include <functional>

namespace Zepra::Heap {

class IdleTimeHandler {
public:
    struct Config {
        double minIdleMs;            // Minimum idle period to start GC
        double maxIdleSliceMs;       // Max GC work per idle notification
        size_t minBytesToCollect;    // Don't bother if below this threshold
        bool enableIdleCompaction;
        double compactionIdleMs;     // Minimum idle time for compaction

        Config() : minIdleMs(5.0), maxIdleSliceMs(4.0), minBytesToCollect(64 * 1024)
            , enableIdleCompaction(true), compactionIdleMs(50.0) {}
    };

    struct Callbacks {
        std::function<size_t()> heapAllocatedBytes;
        std::function<size_t()> heapLiveBytes;
        std::function<bool()> hasIncrementalWork;
        std::function<void(double budgetMs)> doIncrementalWork;
        std::function<void()> triggerMinorGC;
        std::function<void()> triggerCompaction;
        std::function<double()> predictedGCDurationMs;
    };

    explicit IdleTimeHandler(const Config& config = Config{}) : config_(config)
        , totalIdleMs_(0) {}

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // Called by embedder when idle time is available.
    // `deadlineMs` = how much idle time is available.
    void notifyIdle(double deadlineMs) {
        if (deadlineMs < config_.minIdleMs) return;

        stats_.idleNotifications++;
        totalIdleMs_ += deadlineMs;

        double budget = std::min(deadlineMs, config_.maxIdleSliceMs);

        // Priority 1: Continue incremental marking.
        if (cb_.hasIncrementalWork && cb_.hasIncrementalWork()) {
            if (cb_.doIncrementalWork) {
                cb_.doIncrementalWork(budget);
                stats_.incrementalSlices++;
                return;
            }
        }

        // Priority 2: Minor GC if nursery has accumulated garbage.
        size_t allocated = cb_.heapAllocatedBytes ? cb_.heapAllocatedBytes() : 0;
        size_t live = cb_.heapLiveBytes ? cb_.heapLiveBytes() : 0;
        size_t garbage = allocated > live ? allocated - live : 0;

        if (garbage >= config_.minBytesToCollect) {
            double predicted = cb_.predictedGCDurationMs ? cb_.predictedGCDurationMs() : 2.0;
            if (predicted <= budget) {
                if (cb_.triggerMinorGC) {
                    cb_.triggerMinorGC();
                    stats_.idleMinorGCs++;
                    return;
                }
            }
        }

        // Priority 3: Compaction if enough idle time.
        if (config_.enableIdleCompaction && deadlineMs >= config_.compactionIdleMs) {
            if (cb_.triggerCompaction) {
                cb_.triggerCompaction();
                stats_.idleCompactions++;
                return;
            }
        }

        stats_.skippedIdles++;
    }

    // Report utilization of idle time.
    double idleUtilization() const {
        return stats_.idleNotifications > 0
            ? static_cast<double>(stats_.idleNotifications - stats_.skippedIdles) / stats_.idleNotifications
            : 0;
    }

    void setConfig(const Config& config) { config_ = config; }
    const Config& config() const { return config_; }

    struct Stats {
        uint64_t idleNotifications = 0;
        uint64_t incrementalSlices = 0;
        uint64_t idleMinorGCs = 0;
        uint64_t idleCompactions = 0;
        uint64_t skippedIdles = 0;
    };

    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = {}; }

private:
    Config config_;
    Callbacks cb_;
    double totalIdleMs_;
    Stats stats_;
};

} // namespace Zepra::Heap

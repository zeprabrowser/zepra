// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_media_tab_protector.cpp — Media-playing tab keep-alive and GC pause capping

#include <mutex>
#include <algorithm>
#include <unordered_set>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <chrono>

namespace Zepra::Heap {

class MediaTabProtector {
public:
    struct Config {
        size_t minMediaBudgetMB;     // Minimum budget for media tabs
        double maxGCPauseMs;         // Max GC pause during media playback
        bool preventAutoSuspend;     // Never auto-suspend media tabs
        bool preventEviction;        // Never evict media tabs

        Config()
            : minMediaBudgetMB(128)
            , maxGCPauseMs(2.0)
            , preventAutoSuspend(true)
            , preventEviction(true) {}
    };

    struct Callbacks {
        std::function<void(uint64_t tabId, double maxPauseMs)> setGCPauseBudget;
        std::function<void(uint64_t tabId, size_t minBudget)> setMinBudget;
    };

    explicit MediaTabProtector(const Config& config = Config{})
        : config_(config) {}

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    void onMediaStart(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        mediaTabs_.insert(tabId);

        if (cb_.setGCPauseBudget) {
            cb_.setGCPauseBudget(tabId, config_.maxGCPauseMs);
        }
        if (cb_.setMinBudget) {
            cb_.setMinBudget(tabId, config_.minMediaBudgetMB * 1024 * 1024);
        }

        stats_.mediaStarts++;
    }

    void onMediaStop(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        mediaTabs_.erase(tabId);
        stats_.mediaStops++;
    }

    bool isMediaPlaying(uint64_t tabId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mediaTabs_.count(tabId) > 0;
    }

    bool canSuspend(uint64_t tabId) const {
        if (!config_.preventAutoSuspend) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        return mediaTabs_.count(tabId) == 0;
    }

    bool canEvict(uint64_t tabId) const {
        if (!config_.preventEviction) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        return mediaTabs_.count(tabId) == 0;
    }

    double maxGCPauseMs(uint64_t tabId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mediaTabs_.count(tabId) > 0) {
            return config_.maxGCPauseMs;
        }
        return 0;  // 0 = no cap
    }

    size_t mediaTabCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mediaTabs_.size();
    }

    struct Stats { uint64_t mediaStarts; uint64_t mediaStops; };
    Stats stats() const { return stats_; }

private:
    Config config_;
    Callbacks cb_;
    mutable std::mutex mutex_;
    std::unordered_set<uint64_t> mediaTabs_;
    Stats stats_{};
};

} // namespace Zepra::Heap

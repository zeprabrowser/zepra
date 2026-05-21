// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_tab_memory_policy.cpp — Per-tab memory caps and GC frequency by state

#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <chrono>

namespace Zepra::Heap {

enum class TabState : uint8_t {
    Active,
    Background,
    Suspended,
    MediaPlaying,
};

struct TabPolicy {
    size_t heapCapMB;
    uint32_t gcMinIntervalMs;    // Minimum time between GCs
    bool allowFullGC;
    bool allowCompaction;
    double allocationRateLimit;  // Bytes per second, 0 = unlimited

    static TabPolicy forState(TabState state) {
        TabPolicy p;
        switch (state) {
            case TabState::Active:
                p.heapCapMB = 512;
                p.gcMinIntervalMs = 100;
                p.allowFullGC = true;
                p.allowCompaction = true;
                p.allocationRateLimit = 0;
                break;
            case TabState::Background:
                p.heapCapMB = 128;
                p.gcMinIntervalMs = 5000;
                p.allowFullGC = false;
                p.allowCompaction = false;
                p.allocationRateLimit = 1024 * 1024;  // 1MB/s
                break;
            case TabState::Suspended:
                p.heapCapMB = 32;
                p.gcMinIntervalMs = 30000;
                p.allowFullGC = false;
                p.allowCompaction = false;
                p.allocationRateLimit = 0;  // No allocation allowed
                break;
            case TabState::MediaPlaying:
                p.heapCapMB = 256;
                p.gcMinIntervalMs = 500;
                p.allowFullGC = true;
                p.allowCompaction = false;  // Avoid latency spikes during playback
                p.allocationRateLimit = 0;
                break;
        }
        return p;
    }
};

class TabMemoryPolicy {
public:
    struct TabRecord {
        uint64_t tabId;
        TabState state;
        TabPolicy policy;
        uint64_t lastGCTimeMs;
        uint64_t stateChangeTimeMs;
        uint64_t backgroundSinceMs;  // When tab went to background

        TabRecord() : tabId(0), state(TabState::Active), lastGCTimeMs(0)
            , stateChangeTimeMs(0), backgroundSinceMs(0) {}
    };

    struct Callbacks {
        std::function<void(uint64_t tabId, TabState newState)> onStateChange;
        std::function<void(uint64_t tabId)> onSuspendRecommended;
    };

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    void registerTab(uint64_t tabId, TabState initialState) {
        std::lock_guard<std::mutex> lock(mutex_);
        TabRecord r;
        r.tabId = tabId;
        r.state = initialState;
        r.policy = TabPolicy::forState(initialState);
        r.stateChangeTimeMs = nowMs();
        tabs_[tabId] = r;
    }

    void unregisterTab(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        tabs_.erase(tabId);
    }

    void setTabState(uint64_t tabId, TabState newState) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        if (it == tabs_.end()) return;

        auto& r = it->second;
        if (r.state == newState) return;

        r.state = newState;
        r.policy = TabPolicy::forState(newState);
        r.stateChangeTimeMs = nowMs();

        if (newState == TabState::Background) {
            r.backgroundSinceMs = nowMs();
        }

        if (cb_.onStateChange) cb_.onStateChange(tabId, newState);
    }

    const TabPolicy* getPolicy(uint64_t tabId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        return it != tabs_.end() ? &it->second.policy : nullptr;
    }

    bool canGCNow(uint64_t tabId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        if (it == tabs_.end()) return false;
        uint64_t elapsed = nowMs() - it->second.lastGCTimeMs;
        return elapsed >= it->second.policy.gcMinIntervalMs;
    }

    void recordGC(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        if (it != tabs_.end()) {
            it->second.lastGCTimeMs = nowMs();
        }
    }

    // Check background tabs for demotion timeline.
    void checkDemotions() {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t now = nowMs();

        for (auto& [id, r] : tabs_) {
            if (r.state != TabState::Background) continue;

            uint64_t bgDuration = now - r.backgroundSinceMs;

            // 30s background → reduce budget (already handled by policy).
            // 120s background → recommend suspension.
            if (bgDuration > 120000 && cb_.onSuspendRecommended) {
                cb_.onSuspendRecommended(id);
            }
        }
    }

private:
    static uint64_t nowMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    Callbacks cb_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, TabRecord> tabs_;
};

} // namespace Zepra::Heap

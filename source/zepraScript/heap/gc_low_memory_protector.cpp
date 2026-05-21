// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_low_memory_protector.cpp — Emergency actions for low-memory

#include <atomic>
#include <algorithm>
#include <mutex>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <vector>

namespace Zepra::Heap {

// When the system is critically low on memory, the protector
// takes progressive emergency actions to keep the browser alive.

enum class MemoryLevel : uint8_t {
    Normal,           // Do nothing
    Low,              // Trim caches
    Critical,         // Full GC + decommit
    OOMImminent       // Kill non-essential tabs
};

class LowMemoryProtector {
public:
    struct Actions {
        std::function<void()> trimICCaches;
        std::function<void()> flushJITCode;
        std::function<void()> fullGC;
        std::function<void()> decommitEmpty;
        std::function<void()> shrinkNursery;
        std::function<void(uint64_t tabId)> suspendTab;
        std::function<std::vector<uint64_t>()> getNonEssentialTabs;
    };

    void setActions(Actions a) { actions_ = std::move(a); }

    void onMemoryLevel(MemoryLevel level) {
        currentLevel_ = level;

        switch (level) {
            case MemoryLevel::Normal:
                break;

            case MemoryLevel::Low:
                // Trim caches, discard JIT code for cold functions.
                if (actions_.trimICCaches) actions_.trimICCaches();
                stats_.lowEvents++;
                break;

            case MemoryLevel::Critical:
                // Full GC, aggressive decommit, shrink nursery.
                if (actions_.fullGC) actions_.fullGC();
                if (actions_.decommitEmpty) actions_.decommitEmpty();
                if (actions_.shrinkNursery) actions_.shrinkNursery();
                if (actions_.flushJITCode) actions_.flushJITCode();
                stats_.criticalEvents++;
                break;

            case MemoryLevel::OOMImminent:
                // Last resort: suspend non-essential tabs.
                if (actions_.fullGC) actions_.fullGC();
                if (actions_.decommitEmpty) actions_.decommitEmpty();

                if (actions_.getNonEssentialTabs && actions_.suspendTab) {
                    auto tabs = actions_.getNonEssentialTabs();
                    for (auto tabId : tabs) {
                        actions_.suspendTab(tabId);
                        stats_.tabsSuspended++;
                    }
                }
                stats_.oomEvents++;
                break;
        }
    }

    MemoryLevel currentLevel() const { return currentLevel_; }

    struct Stats {
        uint64_t lowEvents;
        uint64_t criticalEvents;
        uint64_t oomEvents;
        uint64_t tabsSuspended;
    };

    Stats stats() const { return stats_; }

private:
    Actions actions_;
    MemoryLevel currentLevel_ = MemoryLevel::Normal;
    Stats stats_{};
};

} // namespace Zepra::Heap

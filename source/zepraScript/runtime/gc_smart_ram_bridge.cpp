// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_smart_ram_bridge.cpp — Runtime hook connecting Smart RAM to GC pipeline

#include <functional>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <mutex>

namespace Zepra::Runtime {

class SmartRAMBridge {
public:
    // Pressure levels matching Zepra::Heap::PressureLevel.
    enum class Pressure : uint8_t { None, Low, Moderate, Critical, OOM };

    struct GCActions {
        std::function<void()> triggerIncrementalGC;
        std::function<void()> triggerFullGC;
        std::function<void()> triggerCompaction;
        std::function<void()> flushCodeCaches;
        std::function<void()> shrinkHeap;
    };

    struct TabActions {
        std::function<void(uint64_t tabId)> triggerTabGC;
        std::function<void(uint64_t tabId)> recommendSuspend;
        std::function<void(uint64_t tabId)> recommendEvict;
    };

    struct AllocationHooks {
        // Returns false to deny allocation (budget exceeded).
        std::function<bool(uint64_t tabId, size_t bytes)> checkBudget;
        // Called post-allocation to update tab usage.
        std::function<void(uint64_t tabId, size_t bytes)> reportUsage;
    };

    void setGCActions(GCActions a) { gcActions_ = std::move(a); }
    void setTabActions(TabActions a) { tabActions_ = std::move(a); }
    void setAllocationHooks(AllocationHooks h) { allocHooks_ = std::move(h); }

    // Called by MemoryPressureMonitor when system pressure changes.
    void onPressureChange(Pressure level) {
        std::lock_guard<std::mutex> lock(mutex_);
        lastPressure_ = level;
        stats_.pressureChanges++;

        switch (level) {
            case Pressure::None:
            case Pressure::Low:
                break;

            case Pressure::Moderate:
                if (gcActions_.triggerIncrementalGC) gcActions_.triggerIncrementalGC();
                break;

            case Pressure::Critical:
                if (gcActions_.triggerFullGC) gcActions_.triggerFullGC();
                if (gcActions_.flushCodeCaches) gcActions_.flushCodeCaches();
                if (gcActions_.shrinkHeap) gcActions_.shrinkHeap();
                break;

            case Pressure::OOM:
                if (gcActions_.triggerFullGC) gcActions_.triggerFullGC();
                if (gcActions_.triggerCompaction) gcActions_.triggerCompaction();
                if (gcActions_.flushCodeCaches) gcActions_.flushCodeCaches();
                if (gcActions_.shrinkHeap) gcActions_.shrinkHeap();
                break;
        }
    }

    // Called when a tab exceeds its memory budget.
    void onTabBudgetExceeded(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.budgetExceeded++;

        // First try: GC the tab.
        if (tabActions_.triggerTabGC) tabActions_.triggerTabGC(tabId);
    }

    // Allocation-time budget check (called from VM hooks).
    bool checkAllocation(uint64_t tabId, size_t bytes) {
        if (allocHooks_.checkBudget) {
            return allocHooks_.checkBudget(tabId, bytes);
        }
        return true;
    }

    void reportAllocation(uint64_t tabId, size_t bytes) {
        if (allocHooks_.reportUsage) {
            allocHooks_.reportUsage(tabId, bytes);
        }
    }

    Pressure currentPressure() const { return lastPressure_; }

    struct Stats { uint64_t pressureChanges; uint64_t budgetExceeded; };
    Stats stats() const { return stats_; }

private:
    GCActions gcActions_;
    TabActions tabActions_;
    AllocationHooks allocHooks_;
    Pressure lastPressure_ = Pressure::None;
    Stats stats_{};
    std::mutex mutex_;
};

} // namespace Zepra::Runtime

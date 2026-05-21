// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_tab_lifecycle.cpp — Tab lifecycle events → GC/allocator coordination

#include <mutex>
#include <algorithm>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <unordered_map>

namespace Zepra::Runtime {

class TabLifecycleManager {
public:
    struct Handlers {
        // TabIsolator calls
        std::function<bool(uint64_t tabId, size_t initialHeapSize)> createTabHeap;
        std::function<void(uint64_t tabId)> destroyTabHeap;
        std::function<void(uint64_t tabId, uintptr_t base, size_t size)> secureWipe;

        // PerTabAllocator calls
        std::function<void(uint64_t tabId, uintptr_t base, size_t size, size_t maxBytes)> initTabAllocator;
        std::function<void(uint64_t tabId)> removeTabAllocator;

        // SmartRAMAllocator calls
        std::function<void(uint64_t tabId, uint8_t priority)> registerTabRAM;
        std::function<void(uint64_t tabId)> unregisterTabRAM;
        std::function<void(uint64_t tabId, uint8_t priority)> setTabPriority;

        // TabMemoryPolicy calls
        std::function<void(uint64_t tabId, uint8_t state)> registerTabPolicy;
        std::function<void(uint64_t tabId)> unregisterTabPolicy;
        std::function<void(uint64_t tabId, uint8_t state)> setTabState;

        // MediaTabProtector calls
        std::function<void(uint64_t tabId)> mediaStart;
        std::function<void(uint64_t tabId)> mediaStop;
    };

    void setHandlers(Handlers h) { handlers_ = std::move(h); }

    void onCreate(uint64_t tabId, size_t initialHeapSize) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (handlers_.createTabHeap) handlers_.createTabHeap(tabId, initialHeapSize);
        if (handlers_.registerTabRAM) handlers_.registerTabRAM(tabId, 0); // Active
        if (handlers_.registerTabPolicy) handlers_.registerTabPolicy(tabId, 0); // Active

        tabStates_[tabId] = State::Active;
        stats_.created++;
    }

    void onActivate(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tabStates_.find(tabId) == tabStates_.end()) return;

        tabStates_[tabId] = State::Active;
        if (handlers_.setTabPriority) handlers_.setTabPriority(tabId, 0);
        if (handlers_.setTabState) handlers_.setTabState(tabId, 0);
    }

    void onDeactivate(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tabStates_.find(tabId) == tabStates_.end()) return;

        tabStates_[tabId] = State::Background;
        if (handlers_.setTabPriority) handlers_.setTabPriority(tabId, 2); // Background
        if (handlers_.setTabState) handlers_.setTabState(tabId, 1); // Background
    }

    void onSuspend(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tabStates_.find(tabId) == tabStates_.end()) return;

        tabStates_[tabId] = State::Suspended;
        if (handlers_.setTabPriority) handlers_.setTabPriority(tabId, 3); // Suspended
        if (handlers_.setTabState) handlers_.setTabState(tabId, 2); // Suspended
    }

    void onDestroy(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (handlers_.destroyTabHeap) handlers_.destroyTabHeap(tabId);
        if (handlers_.removeTabAllocator) handlers_.removeTabAllocator(tabId);
        if (handlers_.unregisterTabRAM) handlers_.unregisterTabRAM(tabId);
        if (handlers_.unregisterTabPolicy) handlers_.unregisterTabPolicy(tabId);

        tabStates_.erase(tabId);
        stats_.destroyed++;
    }

    void onMediaStart(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tabStates_.find(tabId) == tabStates_.end()) return;

        tabStates_[tabId] = State::MediaPlaying;
        if (handlers_.setTabPriority) handlers_.setTabPriority(tabId, 1); // MediaPlaying
        if (handlers_.setTabState) handlers_.setTabState(tabId, 3); // MediaPlaying
        if (handlers_.mediaStart) handlers_.mediaStart(tabId);
    }

    void onMediaStop(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tabStates_.find(tabId) == tabStates_.end()) return;

        if (handlers_.mediaStop) handlers_.mediaStop(tabId);
        // Revert to previous active/background state.
        tabStates_[tabId] = State::Active;
        if (handlers_.setTabPriority) handlers_.setTabPriority(tabId, 0);
        if (handlers_.setTabState) handlers_.setTabState(tabId, 0);
    }

    size_t activeTabCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tabStates_.size();
    }

    struct Stats { uint64_t created; uint64_t destroyed; };
    Stats stats() const { return stats_; }

private:
    enum class State : uint8_t { Active, Background, Suspended, MediaPlaying };

    Handlers handlers_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, State> tabStates_;
    Stats stats_{};
};

} // namespace Zepra::Runtime

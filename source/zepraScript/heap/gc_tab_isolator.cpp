// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_tab_isolator.cpp — Per-tab heap isolation for security

#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cassert>

namespace Zepra::Heap {

// Each browser tab gets its own isolated heap region. This prevents:
// - Cross-tab data leaks via heap reuse
// - One tab's GC affecting another tab's latency
// - Spectre-style side-channel attacks between tab heaps

struct TabHeap {
    uint64_t tabId;
    uintptr_t heapBase;
    size_t heapSize;
    size_t used;
    bool active;

    TabHeap() : tabId(0), heapBase(0), heapSize(0), used(0), active(false) {}
};

class TabIsolator {
public:
    struct Backend {
        std::function<uintptr_t(size_t)> allocateSegment;
        std::function<void(uintptr_t, size_t)> freeSegment;
        std::function<void(uintptr_t, size_t)> secureWipe;
    };

    void setBackend(Backend b) { backend_ = std::move(b); }

    // Create an isolated heap for a new tab.
    bool createTabHeap(uint64_t tabId, size_t initialSize) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tabs_.count(tabId)) return false;

        uintptr_t base = 0;
        if (backend_.allocateSegment) {
            base = backend_.allocateSegment(initialSize);
            if (base == 0) return false;
        }

        TabHeap t;
        t.tabId = tabId;
        t.heapBase = base;
        t.heapSize = initialSize;
        t.used = 0;
        t.active = true;
        tabs_[tabId] = t;

        return true;
    }

    // Destroy a tab's heap — wipe then free.
    void destroyTabHeap(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        if (it == tabs_.end()) return;

        auto& tab = it->second;

        // Security: zero all memory before freeing.
        if (backend_.secureWipe) {
            backend_.secureWipe(tab.heapBase, tab.heapSize);
        }

        if (backend_.freeSegment) {
            backend_.freeSegment(tab.heapBase, tab.heapSize);
        }

        tabs_.erase(it);
        stats_.tabsDestroyed++;
    }

    // Check if an address belongs to a specific tab.
    bool belongsToTab(uint64_t tabId, uintptr_t addr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        if (it == tabs_.end()) return false;
        auto& tab = it->second;
        return addr >= tab.heapBase && addr < tab.heapBase + tab.heapSize;
    }

    // Deny cross-tab pointer stores (security invariant).
    bool validateStore(uint64_t srcTabId, uintptr_t destAddr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        // Dest must be in same tab or in shared heap.
        auto srcIt = tabs_.find(srcTabId);
        if (srcIt == tabs_.end()) return true;

        for (auto& [tabId, tab] : tabs_) {
            if (tabId != srcTabId && tab.active) {
                if (destAddr >= tab.heapBase &&
                    destAddr < tab.heapBase + tab.heapSize) {
                    return false;  // Cross-tab reference blocked.
                }
            }
        }
        return true;
    }

    size_t activeTabCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tabs_.size();
    }

    // Get per-tab memory usage.
    std::vector<std::pair<uint64_t, size_t>> tabMemoryUsage() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<uint64_t, size_t>> result;
        for (auto& [id, t] : tabs_) {
            result.push_back({id, t.used});
        }
        return result;
    }

    struct Stats { uint64_t tabsDestroyed; };
    Stats stats() const { return stats_; }

private:
    mutable std::mutex mutex_;
    Backend backend_;
    std::unordered_map<uint64_t, TabHeap> tabs_;
    Stats stats_{};
};

} // namespace Zepra::Heap

// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — sandbox.cpp — Tab-isolated sandbox enforcement

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <functional>
#include <atomic>
#include <memory>
#include <memory>

namespace Zepra::Runtime {

enum class SandboxPermission : uint32_t {
    None            = 0,
    Network         = 1 << 0,   // fetch, XMLHttpRequest
    Storage         = 1 << 1,   // localStorage, indexedDB
    Clipboard       = 1 << 2,   // navigator.clipboard
    Geolocation     = 1 << 3,
    Camera          = 1 << 4,
    Microphone      = 1 << 5,
    Notifications   = 1 << 6,
    WebWorkers      = 1 << 7,
    SharedMemory    = 1 << 8,   // SharedArrayBuffer
    Eval            = 1 << 9,   // eval(), Function()
    DynamicImport   = 1 << 10,
    WasmCompile     = 1 << 11,
    FileAccess      = 1 << 12,
    AllPermissions  = 0xFFFFFFFF,
};

inline SandboxPermission operator|(SandboxPermission a, SandboxPermission b) {
    return static_cast<SandboxPermission>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool hasPermission(SandboxPermission set, SandboxPermission perm) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(perm)) != 0;
}

struct SandboxConfig {
    uint32_t tabId;
    std::string origin;
    SandboxPermission permissions;
    size_t memoryLimitBytes;       // Per-tab heap limit
    size_t stackLimitBytes;        // Max stack size
    uint32_t maxCallDepth;
    double cpuTimeLimitMs;         // Max CPU time per task
    bool allowCrossOriginAccess;
    bool strictMode;

    SandboxConfig() : tabId(0), permissions(SandboxPermission::None)
        , memoryLimitBytes(64 * 1024 * 1024), stackLimitBytes(1024 * 1024)
        , maxCallDepth(512), cpuTimeLimitMs(30000.0)
        , allowCrossOriginAccess(false), strictMode(false) {}
};

class Sandbox {
public:
    explicit Sandbox(const SandboxConfig& config) : config_(config)
        , memoryUsed_(0), violated_(false) {}

    bool checkPermission(SandboxPermission perm) const {
        if (!hasPermission(config_.permissions, perm)) {
            stats_.permissionDenials++;
            return false;
        }
        return true;
    }

    bool checkMemoryAllocation(size_t bytes) {
        size_t current = memoryUsed_.load(std::memory_order_relaxed);
        if (current + bytes > config_.memoryLimitBytes) {
            stats_.memoryViolations++;
            violated_ = true;
            return false;
        }
        memoryUsed_.fetch_add(bytes, std::memory_order_relaxed);
        return true;
    }

    void releaseMemory(size_t bytes) {
        size_t current = memoryUsed_.load(std::memory_order_relaxed);
        size_t release = bytes > current ? current : bytes;
        memoryUsed_.fetch_sub(release, std::memory_order_relaxed);
    }

    bool checkCallDepth(uint32_t depth) const {
        if (depth >= config_.maxCallDepth) {
            stats_.stackOverflows++;
            return false;
        }
        return true;
    }

    // Cross-origin check: block access to other tab's heap.
    bool checkCrossOriginAccess(const std::string& targetOrigin) const {
        if (config_.allowCrossOriginAccess) return true;
        if (targetOrigin == config_.origin) return true;
        stats_.crossOriginBlocks++;
        return false;
    }

    // API surface restriction: block disallowed globals.
    bool isAPIAllowed(const std::string& apiName) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (blockedAPIs_.count(apiName)) {
            stats_.apiBlocks++;
            return false;
        }
        return true;
    }

    void blockAPI(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        blockedAPIs_.insert(name);
    }

    void unblockAPI(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        blockedAPIs_.erase(name);
    }

    uint32_t tabId() const { return config_.tabId; }
    const std::string& origin() const { return config_.origin; }
    size_t memoryUsed() const { return memoryUsed_.load(std::memory_order_relaxed); }
    size_t memoryLimit() const { return config_.memoryLimitBytes; }
    bool isViolated() const { return violated_; }
    const SandboxConfig& config() const { return config_; }

    struct Stats {
        mutable uint64_t permissionDenials = 0;
        mutable uint64_t memoryViolations = 0;
        mutable uint64_t stackOverflows = 0;
        mutable uint64_t crossOriginBlocks = 0;
        mutable uint64_t apiBlocks = 0;
    };

    const Stats& stats() const { return stats_; }

private:
    SandboxConfig config_;
    std::atomic<size_t> memoryUsed_;
    bool violated_;
    mutable std::mutex mutex_;
    std::unordered_set<std::string> blockedAPIs_;
    Stats stats_;
};

// Per-tab sandbox registry.
class SandboxRegistry {
public:
    Sandbox* createSandbox(const SandboxConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto sb = std::make_unique<Sandbox>(config);
        Sandbox* ptr = sb.get();
        sandboxes_[config.tabId] = std::move(sb);
        return ptr;
    }

    void destroySandbox(uint32_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        sandboxes_.erase(tabId);
    }

    Sandbox* getSandbox(uint32_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sandboxes_.find(tabId);
        return it != sandboxes_.end() ? it->second.get() : nullptr;
    }

    size_t totalMemoryUsed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (auto& [id, sb] : sandboxes_) total += sb->memoryUsed();
        return total;
    }

    size_t sandboxCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sandboxes_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<Sandbox>> sandboxes_;
};

} // namespace Zepra::Runtime

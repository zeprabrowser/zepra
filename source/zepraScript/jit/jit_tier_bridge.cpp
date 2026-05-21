// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — jit_tier_bridge.cpp — Connects profiler, tier policy, and JIT compiler

#include "jit/jit_profiler.hpp"
#include <algorithm>
#include "jit/baseline_jit.hpp"
#include "jit/osr.hpp"
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <chrono>

namespace Zepra::JIT {

// =============================================================================
// Tier-up Bridge — orchestrates profiler → policy → compiler transitions
// =============================================================================

class TierUpBridge {
public:
    explicit TierUpBridge(Runtime::VM* vm)
        : vm_(vm)
        , baselineJIT_(vm) {}

    // Called from the interpreter dispatch loop on every function entry.
    // Returns native entry point if the function was JIT-compiled, nullptr otherwise.
    void* onFunctionEntry(uintptr_t functionId, const Bytecode::BytecodeChunk* chunk) {
        // Fast path: already compiled — return cached entry point.
        {
            auto it = compiledEntries_.find(functionId);
            if (it != compiledEntries_.end()) {
                return it->second;
            }
        }

        // Record the call in the profiler.
        JITProfiler& profiler = getProfiler();
        if (!profiler.isEnabled()) return nullptr;

        bool becameHot = profiler.recordCall(functionId);
        if (!becameHot) return nullptr;

        // Function crossed hot threshold — attempt baseline compilation.
        return attemptCompilation(functionId, chunk);
    }

    // Called on loop back-edges for OSR candidates.
    void* onLoopBackEdge(uintptr_t functionId, const Bytecode::BytecodeChunk* chunk,
                          uint32_t bytecodeOffset) {
        JITProfiler& profiler = getProfiler();
        if (!profiler.isEnabled()) return nullptr;

        profiler.recordLoopIteration(functionId);

        const FunctionProfile* profile = profiler.getProfile(functionId);
        if (!profile) return nullptr;

        // OSR threshold: 200 loop iterations on a hot function.
        if (profile->loopIterations >= 200 && profile->isHot()) {
            auto it = compiledEntries_.find(functionId);
            if (it != compiledEntries_.end()) {
                return it->second;
            }
            return attemptCompilation(functionId, chunk);
        }

        return nullptr;
    }

    // Called when a deoptimization occurs — blacklists function after budget exhaustion.
    void onDeoptimization(uintptr_t functionId) {
        std::lock_guard<std::mutex> lock(mutex_);

        deoptCounts_[functionId]++;
        if (deoptCounts_[functionId] >= kMaxDeopts) {
            blacklisted_.insert(functionId);
            compiledEntries_.erase(functionId);
            compiledCode_.erase(functionId);
        }
    }

    // Statistics
    struct Stats {
        uint64_t compilationAttempts = 0;
        uint64_t compilationSuccesses = 0;
        uint64_t compilationFailures = 0;
        uint64_t deoptimizations = 0;
        double totalCompileTimeMs = 0.0;
    };

    const Stats& stats() const { return stats_; }

    // Check if a function has been JIT-compiled.
    bool isCompiled(uintptr_t functionId) const {
        return compiledEntries_.find(functionId) != compiledEntries_.end();
    }

    // Discard all compiled code (e.g. on debugger attach).
    void invalidateAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        compiledEntries_.clear();
        compiledCode_.clear();
    }

private:
    static constexpr uint32_t kMaxDeopts = 10;

    void* attemptCompilation(uintptr_t functionId, const Bytecode::BytecodeChunk* chunk) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Double-check after lock.
        if (blacklisted_.count(functionId)) return nullptr;
        auto it = compiledEntries_.find(functionId);
        if (it != compiledEntries_.end()) return it->second;

        stats_.compilationAttempts++;

        auto start = std::chrono::high_resolution_clock::now();
        auto compiled = baselineJIT_.compile(chunk);
        auto end = std::chrono::high_resolution_clock::now();

        double compileMs = std::chrono::duration<double, std::milli>(end - start).count();
        stats_.totalCompileTimeMs += compileMs;

        if (!compiled) {
            stats_.compilationFailures++;
            return nullptr;
        }

        void* entry = compiled->entryPoint();
        compiledCode_[functionId] = std::move(compiled);
        compiledEntries_[functionId] = entry;
        stats_.compilationSuccesses++;

        return entry;
    }

    Runtime::VM* vm_;
    BaselineJIT baselineJIT_;
    std::mutex mutex_;

    // Cached native entry points for fast lookup (no lock needed for reads).
    std::unordered_map<uintptr_t, void*> compiledEntries_;

    // Owns the compiled code lifetime.
    std::unordered_map<uintptr_t, std::unique_ptr<CompiledCode>> compiledCode_;

    // Deopt tracking.
    std::unordered_map<uintptr_t, uint32_t> deoptCounts_;
    std::unordered_set<uintptr_t> blacklisted_;

    Stats stats_;
};

// =============================================================================
// Singleton accessor
// =============================================================================

static TierUpBridge* g_bridge = nullptr;

void initTierUpBridge(Runtime::VM* vm) {
    if (!g_bridge) {
        g_bridge = new TierUpBridge(vm);
    }
}

void shutdownTierUpBridge() {
    delete g_bridge;
    g_bridge = nullptr;
}

TierUpBridge* getTierUpBridge() {
    return g_bridge;
}

} // namespace Zepra::JIT

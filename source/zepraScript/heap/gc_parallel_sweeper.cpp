// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_parallel_sweeper.cpp — Concurrent/parallel page sweeping with per-zone tasks

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <chrono>
#include <queue>

namespace Zepra::Heap {

struct SweepTask {
    uint32_t zoneId;
    uintptr_t arenaBase;
    uint16_t sizeClass;
    uint32_t totalCells;
    bool hasFinalizers;

    SweepTask() : zoneId(0), arenaBase(0), sizeClass(0), totalCells(0), hasFinalizers(false) {}
};

struct SweepResult {
    uint32_t zoneId;
    size_t freedBytes;
    uint32_t freedCells;
    uint32_t survivingCells;
    double durationMs;
    bool arenaEmpty;

    SweepResult() : zoneId(0), freedBytes(0), freedCells(0), survivingCells(0)
        , durationMs(0), arenaEmpty(false) {}
};

class ParallelSweeper {
public:
    struct Config {
        uint32_t maxConcurrentTasks;
        bool sweepOnMainThread;   // Fallback for single-threaded mode
        bool deferFinalizerArenas; // Push finalizer arenas to end of sweep

        Config() : maxConcurrentTasks(4), sweepOnMainThread(false)
            , deferFinalizerArenas(true) {}
    };

    struct Callbacks {
        std::function<size_t(uintptr_t arenaBase, uint16_t sizeClass)> sweepArena;
        std::function<void(uintptr_t arenaBase)> callFinalizers;
        std::function<void(uintptr_t arenaBase)> releaseArena;
        std::function<void(uint32_t zoneId, const SweepResult&)> onArenaSwept;
    };

    explicit ParallelSweeper(const Config& config = Config{}) : config_(config)
        , totalFreed_(0), totalArenas_(0), completedArenas_(0) {}

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // Queue arenas for sweeping.
    void addTask(const SweepTask& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (task.hasFinalizers && config_.deferFinalizerArenas) {
            finalizerQueue_.push(task);
        } else {
            taskQueue_.push(task);
        }
        totalArenas_++;
    }

    // Sweep a single arena (callable from any thread).
    SweepResult sweepOne(const SweepTask& task) {
        SweepResult result;
        result.zoneId = task.zoneId;

        auto start = std::chrono::steady_clock::now();

        if (task.hasFinalizers && cb_.callFinalizers) {
            cb_.callFinalizers(task.arenaBase);
        }

        if (cb_.sweepArena) {
            result.freedBytes = cb_.sweepArena(task.arenaBase, task.sizeClass);
        }

        result.freedCells = result.freedBytes > 0 && task.sizeClass > 0
            ? static_cast<uint32_t>(result.freedBytes / task.sizeClass) : 0;
        result.survivingCells = task.totalCells - result.freedCells;
        result.arenaEmpty = result.survivingCells == 0;

        auto end = std::chrono::steady_clock::now();
        result.durationMs = std::chrono::duration<double, std::milli>(end - start).count();

        // Release empty arenas.
        if (result.arenaEmpty && cb_.releaseArena) {
            cb_.releaseArena(task.arenaBase);
        }

        return result;
    }

    // Sweep all queued arenas sequentially (main thread or single worker).
    size_t sweepAll() {
        totalFreed_ = 0;
        completedArenas_ = 0;

        auto start = std::chrono::steady_clock::now();

        // Regular arenas first.
        while (!taskQueue_.empty()) {
            SweepTask task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (taskQueue_.empty()) break;
                task = taskQueue_.front();
                taskQueue_.pop();
            }

            SweepResult result = sweepOne(task);
            totalFreed_ += result.freedBytes;
            completedArenas_++;
            results_.push_back(result);

            if (cb_.onArenaSwept) cb_.onArenaSwept(task.zoneId, result);
        }

        // Finalizer arenas last.
        while (!finalizerQueue_.empty()) {
            SweepTask task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (finalizerQueue_.empty()) break;
                task = finalizerQueue_.front();
                finalizerQueue_.pop();
            }

            SweepResult result = sweepOne(task);
            totalFreed_ += result.freedBytes;
            completedArenas_++;
            results_.push_back(result);

            if (cb_.onArenaSwept) cb_.onArenaSwept(task.zoneId, result);
        }

        auto end = std::chrono::steady_clock::now();
        totalDurationMs_ = std::chrono::duration<double, std::milli>(end - start).count();

        return totalFreed_;
    }

    // Get next pending task for external thread pool execution.
    bool getNextTask(SweepTask& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!taskQueue_.empty()) {
            task = taskQueue_.front();
            taskQueue_.pop();
            return true;
        }
        if (!finalizerQueue_.empty()) {
            task = finalizerQueue_.front();
            finalizerQueue_.pop();
            return true;
        }
        return false;
    }

    void recordResult(const SweepResult& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        totalFreed_ += result.freedBytes;
        completedArenas_++;
        results_.push_back(result);
    }

    size_t totalFreed() const { return totalFreed_; }
    size_t totalArenas() const { return totalArenas_; }
    size_t completedArenas() const { return completedArenas_; }
    double totalDurationMs() const { return totalDurationMs_; }
    bool isComplete() const { return completedArenas_ >= totalArenas_; }

    size_t pendingTasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return taskQueue_.size() + finalizerQueue_.size();
    }

    const std::vector<SweepResult>& results() const { return results_; }

    // Per-zone summary.
    size_t freedInZone(uint32_t zoneId) const {
        size_t total = 0;
        for (auto& r : results_) {
            if (r.zoneId == zoneId) total += r.freedBytes;
        }
        return total;
    }

    size_t emptyArenasInZone(uint32_t zoneId) const {
        size_t count = 0;
        for (auto& r : results_) {
            if (r.zoneId == zoneId && r.arenaEmpty) count++;
        }
        return count;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!taskQueue_.empty()) taskQueue_.pop();
        while (!finalizerQueue_.empty()) finalizerQueue_.pop();
        results_.clear();
        totalFreed_ = 0;
        totalArenas_ = 0;
        completedArenas_ = 0;
        totalDurationMs_ = 0;
    }

private:
    Config config_;
    Callbacks cb_;
    mutable std::mutex mutex_;
    std::queue<SweepTask> taskQueue_;
    std::queue<SweepTask> finalizerQueue_;
    std::vector<SweepResult> results_;
    size_t totalFreed_;
    size_t totalArenas_;
    size_t completedArenas_;
    double totalDurationMs_ = 0;
};

} // namespace Zepra::Heap

// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_parallel_marker.cpp — Multi-threaded marking with per-thread worklists

#include <cstdint>
#include <cstdio>
#include <cassert>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <memory>
#include <chrono>
#include <thread>

namespace Zepra::Heap {

struct MarkWorkItem {
    void* cell;
    uint16_t cellSize;

    MarkWorkItem() : cell(nullptr), cellSize(0) {}
    MarkWorkItem(void* c, uint16_t s) : cell(c), cellSize(s) {}
};

class MarkWorklist {
public:
    static constexpr size_t kSegmentCapacity = 2048;

    bool push(const MarkWorkItem& item) {
        std::lock_guard<std::mutex> lock(*mutex_);
        if (count_ >= kSegmentCapacity) return false;
        items_[count_++] = item;
        return true;
    }

    bool pop(MarkWorkItem& item) {
        std::lock_guard<std::mutex> lock(*mutex_);
        if (count_ == 0) return false;
        item = items_[--count_];
        return true;
    }

    bool isEmpty() const { return count_ == 0; }
    size_t count() const { return count_; }

    void clear() {
        std::lock_guard<std::mutex> lock(*mutex_);
        count_ = 0;
    }

    // Steal half the items from this worklist.
    size_t stealHalf(MarkWorkItem* dest, size_t maxItems) {
        std::lock_guard<std::mutex> lock(*mutex_);
        size_t toSteal = std::min(count_ / 2, maxItems);
        if (toSteal == 0) return 0;

        for (size_t i = 0; i < toSteal; i++) {
            dest[i] = items_[--count_];
        }
        return toSteal;
    }

private:
    std::unique_ptr<std::mutex> mutex_ = std::make_unique<std::mutex>();
    MarkWorkItem items_[kSegmentCapacity];
    size_t count_ = 0;
};

class ParallelMarker {
public:
    struct Config {
        uint32_t threadCount;
        size_t markedBytesTarget;     // Stop marking after this many bytes
        double maxPauseMs;            // Time budget per marking slice
        bool enableWorkStealing;

        Config() : threadCount(4), markedBytesTarget(0), maxPauseMs(5.0)
            , enableWorkStealing(true) {}
    };

    struct Callbacks {
        std::function<void(void* cell, uint16_t size,
                          std::function<void(void* child, uint16_t childSize)> push)> traceCell;
        std::function<bool(void* cell)> isMarked;
        std::function<void(void* cell)> markCell;
    };

    explicit ParallelMarker(const Config& config = Config{}) : config_(config)
        , totalMarked_(0), totalBytes_(0), terminated_(false) {
        worklists_.resize(config.threadCount);
    }

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // Add root objects to worklist 0.
    void addRoot(void* cell, uint16_t size) {
        if (cell && !cb_.isMarked(cell)) {
            cb_.markCell(cell);
            worklists_[0].push({cell, size});
            rootCount_++;
        }
    }

    // Run parallel marking. Returns total bytes marked.
    size_t mark() {
        terminated_ = false;
        totalMarked_ = 0;
        totalBytes_ = 0;

        auto start = std::chrono::steady_clock::now();

        if (config_.threadCount <= 1) {
            // Single-threaded fast path
            markLoop(0);
        } else {
            // Multi-threaded: spawn N workers, each runs markLoop on its worklist
            std::vector<std::thread> workers;
            workers.reserve(config_.threadCount);

            for (uint32_t w = 0; w < config_.threadCount; ++w) {
                workers.emplace_back([this, w]() { markLoop(w); });
            }

            for (auto& t : workers) {
                t.join();
            }
        }

        auto end = std::chrono::steady_clock::now();
        durationMs_ = std::chrono::duration<double, std::milli>(end - start).count();

        return totalBytes_;
    }

    // Single worker marking loop.
    void markLoop(uint32_t workerId) {
        MarkWorkItem item;

        while (!terminated_) {
            // Try local worklist first.
            if (worklists_[workerId].pop(item)) {
                processItem(item, workerId);
                continue;
            }

            // Try work stealing.
            if (config_.enableWorkStealing && stealWork(workerId)) {
                continue;
            }

            // No more work — check if globally done.
            if (allWorklistsEmpty()) break;
        }
    }

    // Mark a single work slice (for incremental marking).
    size_t markSlice(double budgetMs) {
        auto start = std::chrono::steady_clock::now();
        size_t marked = 0;
        MarkWorkItem item;

        while (worklists_[0].pop(item)) {
            processItem(item, 0);
            marked++;

            auto elapsed = std::chrono::steady_clock::now() - start;
            double ms = std::chrono::duration<double, std::milli>(elapsed).count();
            if (ms >= budgetMs) break;
        }

        return marked;
    }

    void terminate() { terminated_ = true; }

    size_t totalMarked() const { return totalMarked_; }
    size_t totalBytes() const { return totalBytes_; }
    size_t rootCount() const { return rootCount_; }
    double durationMs() const { return durationMs_; }

    bool isComplete() const { return allWorklistsEmpty(); }

    struct Stats {
        size_t marked;
        size_t bytes;
        size_t roots;
        double durationMs;
        uint64_t steals;
    };

    Stats stats() const {
        return {totalMarked_, totalBytes_, rootCount_, durationMs_, stealCount_};
    }

    void reset() {
        for (auto& wl : worklists_) wl.clear();
        totalMarked_ = 0;
        totalBytes_ = 0;
        rootCount_ = 0;
        stealCount_ = 0;
        terminated_ = false;
    }

private:
    void processItem(const MarkWorkItem& item, uint32_t workerId) {
        totalMarked_.fetch_add(1, std::memory_order_relaxed);
        totalBytes_.fetch_add(item.cellSize, std::memory_order_relaxed);

        if (cb_.traceCell) {
            cb_.traceCell(item.cell, item.cellSize,
                [this, workerId](void* child, uint16_t childSize) {
                    if (child && !cb_.isMarked(child)) {
                        cb_.markCell(child);
                        worklists_[workerId].push({child, childSize});
                    }
                });
        }
    }

    bool stealWork(uint32_t workerId) {
        MarkWorkItem stolen[64];

        for (uint32_t i = 0; i < worklists_.size(); i++) {
            if (i == workerId) continue;
            size_t count = worklists_[i].stealHalf(stolen, 64);
            if (count > 0) {
                for (size_t j = 0; j < count; j++) {
                    worklists_[workerId].push(stolen[j]);
                }
                stealCount_++;
                return true;
            }
        }
        return false;
    }

    bool allWorklistsEmpty() const {
        for (auto& wl : worklists_) {
            if (!wl.isEmpty()) return false;
        }
        return true;
    }

    Config config_;
    Callbacks cb_;
    std::vector<MarkWorklist> worklists_;
    std::atomic<size_t> totalMarked_{0};
    std::atomic<size_t> totalBytes_{0};
    size_t rootCount_ = 0;
    double durationMs_ = 0;
    uint64_t stealCount_ = 0;
    bool terminated_ = false;
};

} // namespace Zepra::Heap

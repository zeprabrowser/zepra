// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — scheduler.cpp — Microtask/macrotask queues, priority scheduling

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <queue>
#include <vector>
#include <functional>
#include <mutex>
#include <chrono>
#include <atomic>
#include <memory>
#include <memory>

namespace Zepra::Runtime {

enum class TaskPriority : uint8_t {
    Microtask,           // Promise callbacks, queueMicrotask
    UserBlocking,        // Input handlers, requestAnimationFrame
    UserVisible,         // setTimeout(fn, 0), MessageChannel
    Background,          // setTimeout(fn, N), setInterval
    Idle,                // requestIdleCallback
};

struct ScheduledTask {
    uint64_t id;
    TaskPriority priority;
    std::function<void()> callback;
    double scheduledAtMs;    // When the task was enqueued
    double delayMs;          // Delay before execution (for setTimeout)
    bool cancelled;
    bool repeating;          // For setInterval
    double intervalMs;

    ScheduledTask() : id(0), priority(TaskPriority::Background)
        , scheduledAtMs(0), delayMs(0), cancelled(false)
        , repeating(false), intervalMs(0) {}

    double readyAtMs() const { return scheduledAtMs + delayMs; }
};

struct TaskCompare {
    bool operator()(const ScheduledTask* a, const ScheduledTask* b) const {
        // Microtasks first, then by ready time.
        if (a->priority != b->priority) {
            return static_cast<uint8_t>(a->priority) > static_cast<uint8_t>(b->priority);
        }
        return a->readyAtMs() > b->readyAtMs();
    }
};

class TaskScheduler {
public:
    TaskScheduler() : nextId_(1), draining_(false) {}

    // Enqueue a microtask (Promise, queueMicrotask).
    uint64_t queueMicrotask(std::function<void()> callback) {
        return enqueue(TaskPriority::Microtask, std::move(callback), 0);
    }

    // setTimeout
    uint64_t setTimeout(std::function<void()> callback, double delayMs) {
        TaskPriority prio = delayMs <= 0 ? TaskPriority::UserVisible : TaskPriority::Background;
        return enqueue(prio, std::move(callback), delayMs);
    }

    // setInterval
    uint64_t setInterval(std::function<void()> callback, double intervalMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t id = nextId_++;
        auto task = std::make_unique<ScheduledTask>();
        task->id = id;
        task->priority = TaskPriority::Background;
        task->callback = std::move(callback);
        task->scheduledAtMs = nowMs();
        task->delayMs = intervalMs;
        task->repeating = true;
        task->intervalMs = intervalMs;
        ScheduledTask* ptr = task.get();
        allTasks_[id] = std::move(task);
        macroQueue_.push(ptr);
        return id;
    }

    void clearTimeout(uint64_t id) { cancel(id); }
    void clearInterval(uint64_t id) { cancel(id); }

    void cancel(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = allTasks_.find(id);
        if (it != allTasks_.end()) {
            it->second->cancelled = true;
        }
    }

    // requestAnimationFrame
    uint64_t requestAnimationFrame(std::function<void()> callback) {
        return enqueue(TaskPriority::UserBlocking, std::move(callback), 0);
    }

    // Drain microtask queue completely (per spec: all microtasks run before next macrotask).
    size_t drainMicrotasks() {
        std::lock_guard<std::mutex> lock(mutex_);
        draining_ = true;
        size_t drained = 0;

        while (!microQueue_.empty()) {
            ScheduledTask* task = microQueue_.front();
            microQueue_.erase(microQueue_.begin());

            if (!task->cancelled && task->callback) {
                task->callback();
                drained++;
            }
        }

        draining_ = false;
        stats_.microtasksDrained += drained;
        return drained;
    }

    // Run one macrotask (event loop tick).
    bool runNextMacrotask() {
        std::lock_guard<std::mutex> lock(mutex_);
        double now = nowMs();

        while (!macroQueue_.empty()) {
            ScheduledTask* task = macroQueue_.top();
            macroQueue_.pop();

            if (task->cancelled) continue;
            if (task->readyAtMs() > now) {
                // Not ready yet — push back and return.
                macroQueue_.push(task);
                return false;
            }

            if (task->callback) {
                task->callback();
                stats_.macrotasksRun++;

                // Reschedule repeating tasks.
                if (task->repeating && !task->cancelled) {
                    task->scheduledAtMs = now;
                    task->delayMs = task->intervalMs;
                    macroQueue_.push(task);
                }
                return true;
            }
        }
        return false;
    }

    // Full event loop iteration: drain microtasks → run one macrotask.
    bool tick() {
        drainMicrotasks();
        return runNextMacrotask();
    }

    bool hasPendingWork() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !microQueue_.empty() || !macroQueue_.empty();
    }

    size_t pendingMicrotasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return microQueue_.size();
    }

    size_t pendingMacrotasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return macroQueue_.size();
    }

    struct Stats {
        uint64_t microtasksDrained = 0;
        uint64_t macrotasksRun = 0;
        uint64_t tasksScheduled = 0;
        uint64_t tasksCancelled = 0;
    };

    const Stats& stats() const { return stats_; }

private:
    uint64_t enqueue(TaskPriority priority, std::function<void()> callback, double delayMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t id = nextId_++;
        auto task = std::make_unique<ScheduledTask>();
        task->id = id;
        task->priority = priority;
        task->callback = std::move(callback);
        task->scheduledAtMs = nowMs();
        task->delayMs = delayMs;

        ScheduledTask* ptr = task.get();
        allTasks_[id] = std::move(task);

        if (priority == TaskPriority::Microtask) {
            microQueue_.push_back(ptr);
        } else {
            macroQueue_.push(ptr);
        }

        stats_.tasksScheduled++;
        return id;
    }

    static double nowMs() {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    mutable std::mutex mutex_;
    uint64_t nextId_;
    bool draining_;

    // Microtasks: FIFO order.
    std::vector<ScheduledTask*> microQueue_;

    // Macrotasks: priority queue ordered by priority + ready time.
    std::priority_queue<ScheduledTask*, std::vector<ScheduledTask*>, TaskCompare> macroQueue_;

    // Owns all tasks.
    std::unordered_map<uint64_t, std::unique_ptr<ScheduledTask>> allTasks_;

    Stats stats_;
};

} // namespace Zepra::Runtime

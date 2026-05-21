// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_parallel_task.cpp — GC parallel task abstraction, thread pool integration

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <functional>
#include <chrono>
#include <queue>

namespace Zepra::Heap {

enum class TaskState : uint8_t {
    Pending,
    Running,
    Complete,
    Cancelled,
};

enum class TaskPriority : uint8_t {
    Low,
    Normal,
    High,
    Critical,
};

struct GCTaskStats {
    uint64_t taskId;
    double durationMs;
    size_t workUnits;
    uint32_t threadId;
};

class GCParallelTask {
public:
    using WorkFn = std::function<void(uint32_t workerId)>;

    explicit GCParallelTask(uint64_t id, WorkFn fn, TaskPriority prio = TaskPriority::Normal)
        : id_(id), workFn_(std::move(fn)), priority_(prio)
        , state_(TaskState::Pending), durationMs_(0) {}

    uint64_t id() const { return id_; }
    TaskState state() const { return state_.load(); }
    TaskPriority priority() const { return priority_; }
    double durationMs() const { return durationMs_; }

    void run(uint32_t workerId) {
        state_.store(TaskState::Running);
        auto start = std::chrono::steady_clock::now();

        workFn_(workerId);

        auto end = std::chrono::steady_clock::now();
        durationMs_ = std::chrono::duration<double, std::milli>(end - start).count();
        state_.store(TaskState::Complete);
    }

    void cancel() {
        TaskState expected = TaskState::Pending;
        state_.compare_exchange_strong(expected, TaskState::Cancelled);
    }

    bool isComplete() const { return state_.load() == TaskState::Complete; }
    bool isCancelled() const { return state_.load() == TaskState::Cancelled; }

private:
    uint64_t id_;
    WorkFn workFn_;
    TaskPriority priority_;
    std::atomic<TaskState> state_;
    double durationMs_;
};

class GCThreadPool {
public:
    explicit GCThreadPool(uint32_t numThreads) : running_(false), nextTaskId_(1) {
        threadCount_ = numThreads > 0 ? numThreads : 1;
    }

    ~GCThreadPool() { shutdown(); }

    void start() {
        if (running_) return;
        running_ = true;

        for (uint32_t i = 0; i < threadCount_; i++) {
            threads_.emplace_back([this, i]() { workerLoop(i); });
        }
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();

        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
    }

    uint64_t submit(GCParallelTask::WorkFn fn, TaskPriority prio = TaskPriority::Normal) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t id = nextTaskId_++;
        auto task = std::make_shared<GCParallelTask>(id, std::move(fn), prio);
        taskQueue_.push(task);
        cv_.notify_one();
        return id;
    }

    // Submit and wait for all tasks to complete.
    void submitAndWait(std::vector<GCParallelTask::WorkFn>& tasks) {
        std::vector<std::shared_ptr<GCParallelTask>> submitted;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& fn : tasks) {
                uint64_t id = nextTaskId_++;
                auto task = std::make_shared<GCParallelTask>(id, std::move(fn));
                taskQueue_.push(task);
                submitted.push_back(task);
            }
            cv_.notify_all();
        }

        // Wait for all tasks to complete.
        for (auto& task : submitted) {
            while (!task->isComplete() && !task->isCancelled()) {
                std::this_thread::yield();
            }
        }
    }

    // Fork-join: split work into N chunks and run in parallel.
    void forkJoin(size_t totalWork, std::function<void(size_t begin, size_t end)> fn) {
        if (totalWork == 0) return;

        size_t chunkSize = (totalWork + threadCount_ - 1) / threadCount_;
        std::vector<GCParallelTask::WorkFn> tasks;

        for (size_t offset = 0; offset < totalWork; offset += chunkSize) {
            size_t begin = offset;
            size_t end = std::min(offset + chunkSize, totalWork);
            tasks.push_back([fn, begin, end](uint32_t) { fn(begin, end); });
        }

        submitAndWait(tasks);
    }

    uint32_t threadCount() const { return threadCount_; }
    bool isRunning() const { return running_; }

    uint64_t completedTasks() const { return completedTasks_.load(); }

private:
    void workerLoop(uint32_t workerId) {
        while (true) {
            std::shared_ptr<GCParallelTask> task;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return !taskQueue_.empty() || !running_; });

                if (!running_ && taskQueue_.empty()) return;

                if (!taskQueue_.empty()) {
                    task = taskQueue_.front();
                    taskQueue_.pop();
                }
            }

            if (task && !task->isCancelled()) {
                task->run(workerId);
                completedTasks_.fetch_add(1);
            }
        }
    }

    uint32_t threadCount_;
    std::atomic<bool> running_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::thread> threads_;
    std::queue<std::shared_ptr<GCParallelTask>> taskQueue_;
    uint64_t nextTaskId_;
    std::atomic<uint64_t> completedTasks_{0};
};

} // namespace Zepra::Heap

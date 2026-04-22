// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WorkerIsolation.h
 * @brief Worker crash isolation from main thread
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>

namespace Zepra::Safety {

class WorkerIsolation {
public:
    struct WorkerState {
        uint64_t id;
        std::atomic<bool> alive{true};
        std::atomic<bool> crashed{false};
        std::string crashReason;
    };
    
    uint64_t registerWorker() {
        uint64_t id = nextId_++;
        workers_[id] = std::make_unique<WorkerState>();
        workers_[id]->id = id;
        return id;
    }
    
    void workerCrashed(uint64_t id, const std::string& reason) {
        auto it = workers_.find(id);
        if (it != workers_.end()) {
            it->second->alive = false;
            it->second->crashed = true;
            it->second->crashReason = reason;
        }
        crashCount_++;
    }
    
    bool isWorkerAlive(uint64_t id) const {
        auto it = workers_.find(id);
        return it != workers_.end() && it->second->alive;
    }
    
    void removeWorker(uint64_t id) {
        workers_.erase(id);
    }
    
    size_t activeWorkers() const {
        size_t count = 0;
        for (const auto& [id, w] : workers_) {
            if (w->alive) count++;
        }
        return count;
    }
    
    size_t crashCount() const { return crashCount_; }
    
private:
    std::unordered_map<uint64_t, std::unique_ptr<WorkerState>> workers_;
    std::atomic<uint64_t> nextId_{1};
    std::atomic<size_t> crashCount_{0};
};

} // namespace Zepra::Safety

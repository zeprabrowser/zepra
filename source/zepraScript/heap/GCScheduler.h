// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file GCScheduler.h
 * @brief Heuristic-based GC scheduling
 *
 * Decides when and what kind of collection to run:
 * - Scavenge (minor): when nursery is 75%+ full
 * - Major mark-sweep: when old gen reaches threshold
 * - Full concurrent: when heap pressure exceeds target
 * - Emergency: when OOM is imminent
 *
 * Uses allocation rate, survival rate, and heap growth to predict
 * optimal collection timing. Aims to minimize pause times while
 * keeping memory usage within budget.
 */

#pragma once

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <chrono>
#include <atomic>
#include <functional>
#include <deque>

namespace Zepra::Heap {

// =============================================================================
// Collection Types
// =============================================================================

enum class CollectionType : uint8_t {
    None,
    Scavenge,           // Minor: nursery only
    MajorMarkSweep,     // Major: old gen mark-sweep
    MajorMarkCompact,   // Major: old gen mark-compact
    Full,               // Full: both generations
    Concurrent,         // Background concurrent mark
    Emergency,          // OOM-triggered aggressive collection
    Idle,               // Idle-time incremental step
};

enum class CollectionReason : uint8_t {
    NurseryFull,
    AllocationFailure,
    HeapGrowth,
    TimeBased,
    MemoryPressure,
    ExternalRequest,
    IdleNotification,
    ContextDisposal,
    PromotionFailure,
    LargeObjectAllocation,
};

// =============================================================================
// Heap Metrics
// =============================================================================

struct HeapMetrics {
    // Current state
    size_t nurseryUsed = 0;
    size_t nurseryCapacity = 0;
    size_t oldGenUsed = 0;
    size_t oldGenCapacity = 0;
    size_t losUsed = 0;
    size_t externalMemory = 0;

    // Rates (bytes/ms)
    double allocationRate = 0.0;
    double promotionRate = 0.0;
    double sweepRate = 0.0;

    // GC performance
    double lastScavengeDurationMs = 0.0;
    double lastMajorDurationMs = 0.0;
    double averagePauseMs = 0.0;
    double maxPauseMs = 0.0;

    // Survival / fragmentation
    double nurseSurvivalRate = 0.0;
    double oldGenFragmentation = 0.0;

    // Totals
    size_t totalHeapUsed() const {
        return nurseryUsed + oldGenUsed + losUsed + externalMemory;
    }

    size_t totalHeapCapacity() const {
        return nurseryCapacity + oldGenCapacity;
    }

    double heapUtilization() const {
        size_t cap = totalHeapCapacity();
        return cap > 0 ? static_cast<double>(totalHeapUsed()) / static_cast<double>(cap) : 0.0;
    }
};

// =============================================================================
// Scheduler Configuration
// =============================================================================

struct SchedulerConfig {
    // Target metrics
    size_t heapSizeLimit = 512 * 1024 * 1024;    // 512MB hard limit
    size_t softLimit = 256 * 1024 * 1024;          // 256MB soft limit (trigger major)
    double targetHeapUtilization = 0.7;            // Aim for 70% utilization
    double maxPauseTargetMs = 10.0;                // 10ms max pause target
    double idleTimeTargetMs = 5.0;                 // 5ms idle-time GC steps

    // Scavenge triggers
    double nurseryFullThreshold = 0.75;            // Scavenge at 75% nursery

    // Major GC triggers
    double oldGenGrowthFactor = 1.5;               // Major when old gen grows 1.5x
    size_t oldGenMinThreshold = 4 * 1024 * 1024;   // At least 4MB before major

    // Concurrent marking
    bool enableConcurrentMarking = true;
    double concurrentMarkingThreshold = 0.5;       // Start concurrent at 50% old gen

    // Emergency
    size_t emergencyThreshold = 0;                 // 0 = heapSizeLimit - 10%
    bool enableOOMKill = false;                    // Kill context on true OOM

    // Allocation-rate prediction
    size_t allocationRateWindowMs = 1000;          // 1 second window
    size_t samplingIntervalMs = 100;               // Sample every 100ms
};

// =============================================================================
// Allocation Rate Tracker
// =============================================================================

class AllocationRateTracker {
public:
    void recordAllocation(size_t bytes) {
        currentWindowBytes_ += bytes;
    }

    void sample() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastSampleTime_).count();

        if (elapsed > 0) {
            double rate = static_cast<double>(currentWindowBytes_) /
                         static_cast<double>(elapsed);

            samples_.push_back(rate);
            if (samples_.size() > maxSamples_) {
                samples_.pop_front();
            }

            currentWindowBytes_ = 0;
            lastSampleTime_ = now;
        }
    }

    double averageRate() const {
        if (samples_.empty()) return 0.0;
        double sum = 0;
        for (double s : samples_) sum += s;
        return sum / static_cast<double>(samples_.size());
    }

    double peakRate() const {
        double peak = 0;
        for (double s : samples_) {
            if (s > peak) peak = s;
        }
        return peak;
    }

    /**
     * @brief Predict when old gen will be full
     * @return Estimated milliseconds until full, 0 if rate is zero
     */
    uint64_t predictTimeToFull(size_t availableBytes) const {
        double rate = averageRate();
        if (rate <= 0) return UINT64_MAX;
        return static_cast<uint64_t>(
            static_cast<double>(availableBytes) / rate);
    }

private:
    std::deque<double> samples_;
    size_t maxSamples_ = 20;
    size_t currentWindowBytes_ = 0;
    std::chrono::steady_clock::time_point lastSampleTime_ =
        std::chrono::steady_clock::now();
};

// =============================================================================
// GC Scheduler
// =============================================================================

class GCScheduler {
public:
    using CollectionCallback = std::function<void(CollectionType type,
                                                   CollectionReason reason)>;

    explicit GCScheduler(const SchedulerConfig& config = SchedulerConfig{});

    /**
     * @brief Update heap metrics (call periodically or after GC events)
     */
    void updateMetrics(const HeapMetrics& metrics);

    /**
     * @brief Check if any collection should be triggered
     * @return Type of collection to run, or None
     */
    CollectionType shouldCollect() const;

    /**
     * @brief Report that a scavenge completed
     */
    void reportScavengeComplete(double durationMs, size_t bytesFreed);

    /**
     * @brief Report that a major GC completed
     */
    void reportMajorGCComplete(double durationMs, size_t bytesFreed);

    /**
     * @brief Notify of allocation (for rate tracking)
     */
    void notifyAllocation(size_t bytes);

    /**
     * @brief Notify of external memory change
     */
    void notifyExternalMemoryChange(int64_t delta);

    /**
     * @brief Request idle-time GC step
     * @param availableMs Milliseconds available for GC work
     * @return true if work was requested
     */
    bool notifyIdleTime(double availableMs);

    /**
     * @brief Notify memory pressure from OS
     * @param level 0 = low, 1 = moderate, 2 = critical
     */
    void notifyMemoryPressure(int level);

    /**
     * @brief Register callback for collection triggers
     */
    void setCollectionCallback(CollectionCallback cb) {
        callback_ = std::move(cb);
    }

    /**
     * @brief Current computed heap growth factor
     */
    double computedGrowthFactor() const;

    /**
     * @brief Estimate of time until next major GC (ms)
     */
    uint64_t estimatedTimeToNextMajor() const;

    /**
     * @brief Current metrics snapshot
     */
    const HeapMetrics& metrics() const { return metrics_; }

    /**
     * @brief Get scheduler config (read-only)
     */
    const SchedulerConfig& config() const { return config_; }

private:
    bool shouldScavenge() const;
    bool shouldMajorGC() const;
    bool shouldConcurrentMark() const;
    bool isEmergency() const;

    void triggerCollection(CollectionType type, CollectionReason reason);

    SchedulerConfig config_;
    HeapMetrics metrics_;
    AllocationRateTracker rateTracker_;
    CollectionCallback callback_;

    // State
    size_t oldGenSizeAtLastMajor_ = 0;
    size_t totalScavenges_ = 0;
    size_t totalMajorGCs_ = 0;
    std::atomic<int> memoryPressureLevel_{0};
    bool concurrentMarkingActive_ = false;

    // Pause tracking
    double totalPauseMs_ = 0.0;
    double maxPauseMs_ = 0.0;
};

// =============================================================================
// Implementation
// =============================================================================

inline GCScheduler::GCScheduler(const SchedulerConfig& config)
    : config_(config) {
    if (config_.emergencyThreshold == 0) {
        config_.emergencyThreshold = config_.heapSizeLimit -
            config_.heapSizeLimit / 10;  // 90% of limit
    }
}

inline void GCScheduler::updateMetrics(const HeapMetrics& metrics) {
    metrics_ = metrics;
    rateTracker_.sample();
}

inline CollectionType GCScheduler::shouldCollect() const {
    if (isEmergency()) return CollectionType::Emergency;
    if (shouldScavenge()) return CollectionType::Scavenge;
    if (shouldMajorGC()) return CollectionType::MajorMarkSweep;
    if (shouldConcurrentMark()) return CollectionType::Concurrent;
    return CollectionType::None;
}

inline bool GCScheduler::shouldScavenge() const {
    if (metrics_.nurseryCapacity == 0) return false;
    double usage = static_cast<double>(metrics_.nurseryUsed) /
                   static_cast<double>(metrics_.nurseryCapacity);
    return usage >= config_.nurseryFullThreshold;
}

inline bool GCScheduler::shouldMajorGC() const {
    if (metrics_.oldGenUsed < config_.oldGenMinThreshold) return false;

    // Growth-based trigger
    if (oldGenSizeAtLastMajor_ > 0) {
        double growth = static_cast<double>(metrics_.oldGenUsed) /
                       static_cast<double>(oldGenSizeAtLastMajor_);
        if (growth >= config_.oldGenGrowthFactor) return true;
    }

    // Absolute trigger
    if (metrics_.totalHeapUsed() >= config_.softLimit) return true;

    // Memory pressure
    if (memoryPressureLevel_.load() >= 1) return true;

    return false;
}

inline bool GCScheduler::shouldConcurrentMark() const {
    if (!config_.enableConcurrentMarking) return false;
    if (concurrentMarkingActive_) return false;
    if (metrics_.oldGenCapacity == 0) return false;

    double usage = static_cast<double>(metrics_.oldGenUsed) /
                   static_cast<double>(metrics_.oldGenCapacity);
    return usage >= config_.concurrentMarkingThreshold;
}

inline bool GCScheduler::isEmergency() const {
    return metrics_.totalHeapUsed() >= config_.emergencyThreshold ||
           memoryPressureLevel_.load() >= 2;
}

inline void GCScheduler::reportScavengeComplete(double durationMs, size_t /*bytesFreed*/) {
    totalScavenges_++;
    totalPauseMs_ += durationMs;
    if (durationMs > maxPauseMs_) maxPauseMs_ = durationMs;
    metrics_.lastScavengeDurationMs = durationMs;
    metrics_.averagePauseMs = totalPauseMs_ /
        static_cast<double>(totalScavenges_ + totalMajorGCs_);
    metrics_.maxPauseMs = maxPauseMs_;
}

inline void GCScheduler::reportMajorGCComplete(double durationMs, size_t /*bytesFreed*/) {
    totalMajorGCs_++;
    oldGenSizeAtLastMajor_ = metrics_.oldGenUsed;
    totalPauseMs_ += durationMs;
    if (durationMs > maxPauseMs_) maxPauseMs_ = durationMs;
    metrics_.lastMajorDurationMs = durationMs;
    metrics_.averagePauseMs = totalPauseMs_ /
        static_cast<double>(totalScavenges_ + totalMajorGCs_);
    metrics_.maxPauseMs = maxPauseMs_;
}

inline void GCScheduler::notifyAllocation(size_t bytes) {
    rateTracker_.recordAllocation(bytes);
}

inline void GCScheduler::notifyExternalMemoryChange(int64_t delta) {
    if (delta > 0) {
        metrics_.externalMemory += static_cast<size_t>(delta);
    } else {
        size_t sub = static_cast<size_t>(-delta);
        metrics_.externalMemory = metrics_.externalMemory > sub
            ? metrics_.externalMemory - sub : 0;
    }
}

inline bool GCScheduler::notifyIdleTime(double availableMs) {
    if (availableMs < config_.idleTimeTargetMs) return false;

    // Use idle time for incremental marking or finalization
    if (shouldMajorGC() || shouldConcurrentMark()) {
        triggerCollection(CollectionType::Idle, CollectionReason::IdleNotification);
        return true;
    }
    return false;
}

inline void GCScheduler::notifyMemoryPressure(int level) {
    memoryPressureLevel_ = level;
    if (level >= 2) {
        triggerCollection(CollectionType::Emergency, CollectionReason::MemoryPressure);
    } else if (level >= 1) {
        triggerCollection(CollectionType::MajorMarkSweep, CollectionReason::MemoryPressure);
    }
}

inline double GCScheduler::computedGrowthFactor() const {
    // Dynamic growth factor based on heap utilization
    double util = metrics_.heapUtilization();
    if (util > 0.8) return 1.2;
    if (util > 0.6) return 1.5;
    return 2.0;
}

inline uint64_t GCScheduler::estimatedTimeToNextMajor() const {
    size_t targetSize = static_cast<size_t>(
        static_cast<double>(oldGenSizeAtLastMajor_) * config_.oldGenGrowthFactor);
    if (metrics_.oldGenUsed >= targetSize) return 0;
    return rateTracker_.predictTimeToFull(targetSize - metrics_.oldGenUsed);
}

inline void GCScheduler::triggerCollection(CollectionType type, CollectionReason reason) {
    if (callback_) {
        callback_(type, reason);
    }
}

} // namespace Zepra::Heap

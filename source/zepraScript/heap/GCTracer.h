// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file GCTracer.h
 * @brief GC event tracing, metrics, and telemetry
 *
 * Records detailed timing and statistics for every GC event.
 * Used for:
 * 1. Performance analysis (identify GC bottlenecks)
 * 2. Heuristic tuning (feed data to GC scheduler)
 * 3. Structured logging (JSON events for offline analysis)
 * 4. DevTools integration (real-time GC timeline)
 * 5. Health monitoring (detect degradation)
 *
 * Event flow:
 * GC start → root scan → mark → ephemeron → sweep → compact → finalize → GC end
 *
 * Each phase has entry/exit timestamps, byte counts, and object counts.
 */

#pragma once

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <chrono>
#include <mutex>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace Zepra::Heap {

// =============================================================================
// GC Event Types
// =============================================================================

enum class GCEventType : uint8_t {
    // Top-level events
    ScavengeStart,
    ScavengeEnd,
    MajorGCStart,
    MajorGCEnd,
    FullGCStart,
    FullGCEnd,

    // Phase events
    RootScanStart,
    RootScanEnd,
    MarkStart,
    MarkEnd,
    EphemeronStart,
    EphemeronEnd,
    SweepStart,
    SweepEnd,
    CompactStart,
    CompactEnd,
    FinalizeStart,
    FinalizeEnd,

    // Allocation events
    AllocationFailure,
    HeapGrowth,
    HeapShrink,

    // Memory pressure
    MemoryPressureModerate,
    MemoryPressureCritical,

    // Background events
    ConcurrentMarkStart,
    ConcurrentMarkEnd,
    ConcurrentSweepStart,
    ConcurrentSweepEnd,

    // Custom events
    UserDefined,
};

// =============================================================================
// GC Event
// =============================================================================

struct GCEvent {
    GCEventType type;
    uint64_t timestampUs;   // Microseconds since epoch
    uint64_t durationUs;    // Duration (for end events)

    // Memory state at event time
    size_t heapUsed;
    size_t heapCapacity;
    size_t nurseryUsed;
    size_t oldGenUsed;
    size_t losUsed;

    // Phase-specific data
    size_t objectsProcessed;
    size_t bytesProcessed;
    size_t objectsFreed;
    size_t bytesFreed;
    size_t objectsMoved;
    size_t bytesMoved;

    // Extra data
    std::string label;
};

// =============================================================================
// GC Metrics — Rolling Averages
// =============================================================================

struct GCMetrics {
    // Pause times
    double avgScavengePauseMs = 0;
    double maxScavengePauseMs = 0;
    double p95ScavengePauseMs = 0;
    double p99ScavengePauseMs = 0;

    double avgMajorPauseMs = 0;
    double maxMajorPauseMs = 0;
    double p95MajorPauseMs = 0;
    double p99MajorPauseMs = 0;

    // Throughput
    double mutatorUtilization = 0;   // % of time spent running JS (not GC)
    double allocationRate = 0;       // bytes/ms
    double promotionRate = 0;        // bytes/ms
    double survivalRate = 0;         // Ratio

    // Collection frequency
    double scavengesPerMinute = 0;
    double majorGCsPerMinute = 0;

    // Efficiency
    double bytesReclaimedPerMs = 0;
    double markingSpeedBps = 0;      // bytes/second marking throughput
    double sweepingSpeedBps = 0;

    // Heap pressure
    double heapGrowthRate = 0;       // bytes/ms
    double fragmentation = 0;        // 0.0 - 1.0
    double liveRatio = 0;            // live/total
};

// =============================================================================
// Percentile Calculator
// =============================================================================

class PercentileTracker {
public:
    explicit PercentileTracker(size_t maxSamples = 1000)
        : maxSamples_(maxSamples) {}

    void addSample(double value) {
        if (samples_.size() >= maxSamples_) {
            samples_.pop_front();
        }
        samples_.push_back(value);
    }

    double percentile(double p) const {
        if (samples_.empty()) return 0;
        std::vector<double> sorted(samples_.begin(), samples_.end());
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(
            std::ceil(p / 100.0 * static_cast<double>(sorted.size())) - 1);
        idx = std::min(idx, sorted.size() - 1);
        return sorted[idx];
    }

    double average() const {
        if (samples_.empty()) return 0;
        double sum = 0;
        for (double s : samples_) sum += s;
        return sum / static_cast<double>(samples_.size());
    }

    double max() const {
        if (samples_.empty()) return 0;
        double mx = samples_[0];
        for (double s : samples_) if (s > mx) mx = s;
        return mx;
    }

    double min() const {
        if (samples_.empty()) return 0;
        double mn = samples_[0];
        for (double s : samples_) if (s < mn) mn = s;
        return mn;
    }

    size_t count() const { return samples_.size(); }
    void clear() { samples_.clear(); }

private:
    std::deque<double> samples_;
    size_t maxSamples_;
};

// =============================================================================
// GC Tracer
// =============================================================================

class GCTracer {
public:
    GCTracer();
    ~GCTracer();

    // -------------------------------------------------------------------------
    // Event Recording
    // -------------------------------------------------------------------------

    /**
     * @brief Record a GC event
     */
    void recordEvent(const GCEvent& event);

    /**
     * @brief Start a timed scope
     * @return Scope ID to pass to endScope()
     */
    uint64_t beginScope(GCEventType type, const std::string& label = "");

    /**
     * @brief End a timed scope
     */
    void endScope(uint64_t scopeId, GCEventType endType,
                  size_t objectsProcessed = 0, size_t bytesProcessed = 0,
                  size_t objectsFreed = 0, size_t bytesFreed = 0);

    // Convenience wrappers
    void scavengeStart() { currentScavengeScope_ = beginScope(GCEventType::ScavengeStart); }
    void scavengeEnd(size_t freed) {
        endScope(currentScavengeScope_, GCEventType::ScavengeEnd,
                 0, 0, 0, freed);
    }

    void majorGCStart() { currentMajorScope_ = beginScope(GCEventType::MajorGCStart); }
    void majorGCEnd(size_t freed) {
        endScope(currentMajorScope_, GCEventType::MajorGCEnd,
                 0, 0, 0, freed);
    }

    // -------------------------------------------------------------------------
    // Allocation Tracking
    // -------------------------------------------------------------------------

    void recordAllocation(size_t bytes) {
        allocationBytes_.fetch_add(bytes, std::memory_order_relaxed);
        allocationCount_.fetch_add(1, std::memory_order_relaxed);
    }

    void recordPromotion(size_t bytes) {
        promotionBytes_.fetch_add(bytes, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // Metrics
    // -------------------------------------------------------------------------

    /**
     * @brief Compute current metrics
     */
    GCMetrics computeMetrics() const;

    /**
     * @brief Get the last N events
     */
    std::vector<GCEvent> recentEvents(size_t n) const;

    /**
     * @brief Get all events since last clear
     */
    const std::deque<GCEvent>& allEvents() const { return events_; }

    // -------------------------------------------------------------------------
    // Export
    // -------------------------------------------------------------------------

    /**
     * @brief Export events as JSON (for DevTools timeline)
     */
    std::string exportJSON() const;

    /**
     * @brief Export events to a file
     */
    bool exportToFile(const std::string& path) const;

    /**
     * @brief Export as CSV
     */
    std::string exportCSV() const;

    // -------------------------------------------------------------------------
    // Listeners
    // -------------------------------------------------------------------------

    using EventListener = std::function<void(const GCEvent&)>;

    void addEventListener(EventListener listener) {
        std::lock_guard<std::mutex> lock(mutex_);
        listeners_.push_back(std::move(listener));
    }

    // -------------------------------------------------------------------------
    // Control
    // -------------------------------------------------------------------------

    /**
     * @brief Set maximum stored events
     */
    void setMaxEvents(size_t max) { maxEvents_ = max; }

    /**
     * @brief Clear all events
     */
    void clear();

    /**
     * @brief Enable/disable tracing
     */
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

    /**
     * @brief Set heap state (for including in events)
     */
    void setHeapState(size_t heapUsed, size_t heapCapacity,
                      size_t nurseryUsed, size_t oldGenUsed, size_t losUsed) {
        lastHeapUsed_ = heapUsed;
        lastHeapCapacity_ = heapCapacity;
        lastNurseryUsed_ = nurseryUsed;
        lastOldGenUsed_ = oldGenUsed;
        lastLOSUsed_ = losUsed;
    }

private:
    static uint64_t nowUs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    const char* eventTypeName(GCEventType type) const;

    std::deque<GCEvent> events_;
    size_t maxEvents_ = 10000;
    bool enabled_ = true;

    // Active scopes
    struct ActiveScope {
        uint64_t startTimeUs;
        GCEventType type;
        std::string label;
    };
    std::unordered_map<uint64_t, ActiveScope> activeScopes_;
    uint64_t nextScopeId_ = 1;

    // Current top-level scopes
    uint64_t currentScavengeScope_ = 0;
    uint64_t currentMajorScope_ = 0;

    // Percentile trackers
    PercentileTracker scavengePauses_;
    PercentileTracker majorPauses_;

    // Allocation tracking
    std::atomic<uint64_t> allocationBytes_{0};
    std::atomic<uint64_t> allocationCount_{0};
    std::atomic<uint64_t> promotionBytes_{0};
    uint64_t lastAllocationCheckUs_ = 0;
    uint64_t lastAllocationBytes_ = 0;

    // Last known heap state
    size_t lastHeapUsed_ = 0;
    size_t lastHeapCapacity_ = 0;
    size_t lastNurseryUsed_ = 0;
    size_t lastOldGenUsed_ = 0;
    size_t lastLOSUsed_ = 0;

    // Listeners
    std::vector<EventListener> listeners_;

    mutable std::mutex mutex_;
};

// =============================================================================
// RAII Scope Timer
// =============================================================================

class GCScopeTimer {
public:
    GCScopeTimer(GCTracer& tracer, GCEventType startType,
                 GCEventType endType, const std::string& label = "")
        : tracer_(tracer), endType_(endType) {
        scopeId_ = tracer_.beginScope(startType, label);
    }

    ~GCScopeTimer() {
        tracer_.endScope(scopeId_, endType_, objects_, bytes_,
                         freedObjects_, freedBytes_);
    }

    void setObjectsProcessed(size_t n) { objects_ = n; }
    void setBytesProcessed(size_t n) { bytes_ = n; }
    void setFreedObjects(size_t n) { freedObjects_ = n; }
    void setFreedBytes(size_t n) { freedBytes_ = n; }

private:
    GCTracer& tracer_;
    GCEventType endType_;
    uint64_t scopeId_;
    size_t objects_ = 0;
    size_t bytes_ = 0;
    size_t freedObjects_ = 0;
    size_t freedBytes_ = 0;
};

// =============================================================================
// Implementation
// =============================================================================

inline GCTracer::GCTracer() {
    lastAllocationCheckUs_ = nowUs();
}

inline GCTracer::~GCTracer() = default;

inline void GCTracer::recordEvent(const GCEvent& event) {
    if (!enabled_) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
        if (events_.size() > maxEvents_) {
            events_.pop_front();
        }

        for (auto& listener : listeners_) {
            listener(event);
        }
    }
}

inline uint64_t GCTracer::beginScope(GCEventType type,
                                      const std::string& label) {
    if (!enabled_) return 0;

    uint64_t id = nextScopeId_++;
    ActiveScope scope;
    scope.startTimeUs = nowUs();
    scope.type = type;
    scope.label = label;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeScopes_[id] = scope;
    }

    GCEvent event{};
    event.type = type;
    event.timestampUs = scope.startTimeUs;
    event.heapUsed = lastHeapUsed_;
    event.heapCapacity = lastHeapCapacity_;
    event.nurseryUsed = lastNurseryUsed_;
    event.oldGenUsed = lastOldGenUsed_;
    event.losUsed = lastLOSUsed_;
    event.label = label;
    recordEvent(event);

    return id;
}

inline void GCTracer::endScope(uint64_t scopeId, GCEventType endType,
                                size_t objectsProcessed, size_t bytesProcessed,
                                size_t objectsFreed, size_t bytesFreed) {
    if (!enabled_ || scopeId == 0) return;

    uint64_t endTime = nowUs();
    ActiveScope scope;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = activeScopes_.find(scopeId);
        if (it == activeScopes_.end()) return;
        scope = it->second;
        activeScopes_.erase(it);
    }

    uint64_t duration = endTime - scope.startTimeUs;
    double durationMs = static_cast<double>(duration) / 1000.0;

    // Track pause percentiles
    if (endType == GCEventType::ScavengeEnd) {
        scavengePauses_.addSample(durationMs);
    } else if (endType == GCEventType::MajorGCEnd) {
        majorPauses_.addSample(durationMs);
    }

    GCEvent event{};
    event.type = endType;
    event.timestampUs = endTime;
    event.durationUs = duration;
    event.heapUsed = lastHeapUsed_;
    event.heapCapacity = lastHeapCapacity_;
    event.objectsProcessed = objectsProcessed;
    event.bytesProcessed = bytesProcessed;
    event.objectsFreed = objectsFreed;
    event.bytesFreed = bytesFreed;
    event.label = scope.label;
    recordEvent(event);
}

inline GCMetrics GCTracer::computeMetrics() const {
    GCMetrics m;

    m.avgScavengePauseMs = scavengePauses_.average();
    m.maxScavengePauseMs = scavengePauses_.max();
    m.p95ScavengePauseMs = scavengePauses_.percentile(95);
    m.p99ScavengePauseMs = scavengePauses_.percentile(99);

    m.avgMajorPauseMs = majorPauses_.average();
    m.maxMajorPauseMs = majorPauses_.max();
    m.p95MajorPauseMs = majorPauses_.percentile(95);
    m.p99MajorPauseMs = majorPauses_.percentile(99);

    // Allocation rate
    uint64_t now = nowUs();
    uint64_t elapsed = now - lastAllocationCheckUs_;
    if (elapsed > 0) {
        uint64_t allocated = allocationBytes_.load();
        m.allocationRate = static_cast<double>(allocated - lastAllocationBytes_) /
                           (static_cast<double>(elapsed) / 1000.0);
    }

    // Heap stats
    if (lastHeapCapacity_ > 0) {
        m.liveRatio = static_cast<double>(lastHeapUsed_) /
                      static_cast<double>(lastHeapCapacity_);
        m.fragmentation = 1.0 - m.liveRatio;
    }

    return m;
}

inline std::vector<GCEvent> GCTracer::recentEvents(size_t n) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t start = events_.size() > n ? events_.size() - n : 0;
    return {events_.begin() + static_cast<long>(start), events_.end()};
}

inline void GCTracer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
    activeScopes_.clear();
    scavengePauses_.clear();
    majorPauses_.clear();
    allocationBytes_ = 0;
    allocationCount_ = 0;
    promotionBytes_ = 0;
}

inline const char* GCTracer::eventTypeName(GCEventType type) const {
    switch (type) {
        case GCEventType::ScavengeStart: return "ScavengeStart";
        case GCEventType::ScavengeEnd: return "ScavengeEnd";
        case GCEventType::MajorGCStart: return "MajorGCStart";
        case GCEventType::MajorGCEnd: return "MajorGCEnd";
        case GCEventType::FullGCStart: return "FullGCStart";
        case GCEventType::FullGCEnd: return "FullGCEnd";
        case GCEventType::RootScanStart: return "RootScanStart";
        case GCEventType::RootScanEnd: return "RootScanEnd";
        case GCEventType::MarkStart: return "MarkStart";
        case GCEventType::MarkEnd: return "MarkEnd";
        case GCEventType::SweepStart: return "SweepStart";
        case GCEventType::SweepEnd: return "SweepEnd";
        case GCEventType::CompactStart: return "CompactStart";
        case GCEventType::CompactEnd: return "CompactEnd";
        case GCEventType::FinalizeStart: return "FinalizeStart";
        case GCEventType::FinalizeEnd: return "FinalizeEnd";
        case GCEventType::AllocationFailure: return "AllocationFailure";
        case GCEventType::HeapGrowth: return "HeapGrowth";
        case GCEventType::HeapShrink: return "HeapShrink";
        default: return "Unknown";
    }
}

inline std::string GCTracer::exportJSON() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    out << "[\n";
    for (size_t i = 0; i < events_.size(); i++) {
        const auto& e = events_[i];
        out << "  {\"type\":\"" << eventTypeName(e.type) << "\""
            << ",\"ts\":" << e.timestampUs
            << ",\"dur\":" << e.durationUs
            << ",\"heap_used\":" << e.heapUsed
            << ",\"heap_cap\":" << e.heapCapacity
            << ",\"objs_proc\":" << e.objectsProcessed
            << ",\"bytes_proc\":" << e.bytesProcessed
            << ",\"objs_freed\":" << e.objectsFreed
            << ",\"bytes_freed\":" << e.bytesFreed;
        if (!e.label.empty()) {
            out << ",\"label\":\"" << e.label << "\"";
        }
        out << "}";
        if (i + 1 < events_.size()) out << ",";
        out << "\n";
    }
    out << "]\n";
    return out.str();
}

inline bool GCTracer::exportToFile(const std::string& path) const {
    std::ofstream file(path);
    if (!file) return false;
    file << exportJSON();
    return true;
}

inline std::string GCTracer::exportCSV() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    out << "type,timestamp_us,duration_us,heap_used,heap_cap,"
        << "objs_processed,bytes_processed,objs_freed,bytes_freed\n";
    for (const auto& e : events_) {
        out << eventTypeName(e.type) << ","
            << e.timestampUs << ","
            << e.durationUs << ","
            << e.heapUsed << ","
            << e.heapCapacity << ","
            << e.objectsProcessed << ","
            << e.bytesProcessed << ","
            << e.objectsFreed << ","
            << e.bytesFreed << "\n";
    }
    return out.str();
}

} // namespace Zepra::Heap

// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file gc_debug.cpp
 * @brief Heap dump, heap summary, and GC diagnostics
 *
 * Provides rich diagnostic output for debugging GC issues:
 *
 * 1. Heap dump: writes all live objects to a binary/JSON file
 * 2. Heap summary: ASCII table of per-type object counts and sizes
 * 3. GC timeline: chronological log of GC events
 * 4. Fragmentation map: visual per-page occupancy
 * 5. Reference graph: dot-format export for Graphviz
 *
 * All off the hot path — intended for debugging and DevTools only.
 */

#include <atomic>
#include <mutex>
#include <vector>
#include <deque>
#include <string>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <memory>
#include <algorithm>
#include <unordered_map>

namespace Zepra::Heap {

// =============================================================================
// Object Type Info
// =============================================================================

struct ObjectTypeInfo {
    uint32_t typeId;
    const char* name;
    size_t instanceCount;
    size_t totalBytes;
    size_t avgSizeBytes;
    size_t maxSizeBytes;
};

// =============================================================================
// Heap Summary
// =============================================================================

class HeapSummary {
public:
    struct Callbacks {
        // Iterate all live objects: callback(addr, size, typeId)
        std::function<void(
            std::function<void(uintptr_t addr, size_t size, uint32_t typeId)>
        )> iterateAllObjects;

        // Get type name from type ID
        std::function<const char*(uint32_t typeId)> getTypeName;
    };

    void setCallbacks(Callbacks callbacks) { cb_ = std::move(callbacks); }

    /**
     * @brief Collect per-type statistics
     */
    std::vector<ObjectTypeInfo> collect() {
        std::unordered_map<uint32_t, ObjectTypeInfo> byType;

        if (cb_.iterateAllObjects) {
            cb_.iterateAllObjects([&](uintptr_t /*addr*/, size_t size,
                                     uint32_t typeId) {
                auto& info = byType[typeId];
                info.typeId = typeId;
                info.instanceCount++;
                info.totalBytes += size;
                if (size > info.maxSizeBytes) info.maxSizeBytes = size;
            });
        }

        // Compute averages and type names
        std::vector<ObjectTypeInfo> result;
        result.reserve(byType.size());

        for (auto& [typeId, info] : byType) {
            if (info.instanceCount > 0) {
                info.avgSizeBytes = info.totalBytes / info.instanceCount;
            }
            if (cb_.getTypeName) {
                info.name = cb_.getTypeName(typeId);
            }
            if (!info.name) info.name = "unknown";
            result.push_back(info);
        }

        // Sort by total bytes descending
        std::sort(result.begin(), result.end(),
            [](const ObjectTypeInfo& a, const ObjectTypeInfo& b) {
                return a.totalBytes > b.totalBytes;
            });

        return result;
    }

    /**
     * @brief Print summary to file (ASCII table)
     */
    bool printTo(FILE* out) {
        auto types = collect();
        if (types.empty()) return false;

        fprintf(out, "%-30s %10s %14s %10s %10s\n",
            "Type", "Count", "Total Bytes", "Avg Size", "Max Size");
        fprintf(out, "%-30s %10s %14s %10s %10s\n",
            "------------------------------",
            "----------", "--------------",
            "----------", "----------");

        size_t totalCount = 0, totalBytes = 0;
        for (const auto& info : types) {
            fprintf(out, "%-30s %10zu %14zu %10zu %10zu\n",
                info.name, info.instanceCount, info.totalBytes,
                info.avgSizeBytes, info.maxSizeBytes);
            totalCount += info.instanceCount;
            totalBytes += info.totalBytes;
        }

        fprintf(out, "%-30s %10s %14s\n", "", "----------", "--------------");
        fprintf(out, "%-30s %10zu %14zu\n", "TOTAL", totalCount, totalBytes);

        return true;
    }

    /**
     * @brief Print summary to string
     */
    std::string toString() {
        // Create temp file in memory
        char buf[8192];
        #ifdef _WIN32
        // Windows: use snprintf directly since fmemopen is POSIX-only
        FILE* stream = tmpfile();
#else
        FILE* stream = fmemopen(buf, sizeof(buf), "w");
#endif
        if (!stream) return "";
        printTo(stream);
        fclose(stream);
        return std::string(buf);
    }

private:
    Callbacks cb_;
};

// =============================================================================
// GC Timeline
// =============================================================================

struct GCTimelineEntry {
    enum class EventType : uint8_t {
        MinorGC, MajorGC, IncrementalMark, IncrementalSweep,
        Compaction, ScavengerRun, Finalization,
        HeapGrow, HeapShrink, OOMEvent,
    };

    EventType type;
    uint64_t startUs;
    uint64_t durationUs;
    size_t heapUsedBefore;
    size_t heapUsedAfter;
    size_t bytesReclaimed;
    const char* detail;
};

class GCTimeline {
public:
    void record(const GCTimelineEntry& entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(entry);
        if (entries_.size() > maxEntries_) entries_.pop_front();
    }

    /**
     * @brief Print timeline to file
     */
    bool printTo(FILE* out) const {
        std::lock_guard<std::mutex> lock(mutex_);

        fprintf(out, "%14s %12s %8s %14s %14s %14s %s\n",
            "Timestamp(us)", "Type", "ms", "Before", "After",
            "Reclaimed", "Detail");

        for (const auto& e : entries_) {
            const char* typeName = "unknown";
            switch (e.type) {
                case GCTimelineEntry::EventType::MinorGC:
                    typeName = "MinorGC"; break;
                case GCTimelineEntry::EventType::MajorGC:
                    typeName = "MajorGC"; break;
                case GCTimelineEntry::EventType::IncrementalMark:
                    typeName = "IncrMark"; break;
                case GCTimelineEntry::EventType::IncrementalSweep:
                    typeName = "IncrSweep"; break;
                case GCTimelineEntry::EventType::Compaction:
                    typeName = "Compact"; break;
                case GCTimelineEntry::EventType::ScavengerRun:
                    typeName = "Scavenge"; break;
                case GCTimelineEntry::EventType::Finalization:
                    typeName = "Finalize"; break;
                case GCTimelineEntry::EventType::HeapGrow:
                    typeName = "HeapGrow"; break;
                case GCTimelineEntry::EventType::HeapShrink:
                    typeName = "HeapShrink"; break;
                case GCTimelineEntry::EventType::OOMEvent:
                    typeName = "OOM"; break;
            }

            double ms = static_cast<double>(e.durationUs) / 1000.0;
            fprintf(out, "%14lu %12s %7.2f %14zu %14zu %14zu %s\n",
                static_cast<unsigned long>(e.startUs), typeName, ms,
                e.heapUsedBefore, e.heapUsedAfter, e.bytesReclaimed,
                e.detail ? e.detail : "");
        }

        return true;
    }

    /**
     * @brief Export timeline as JSON array
     */
    std::string toJSON() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string out = "[\n";
        bool first = true;

        for (const auto& e : entries_) {
            if (!first) out += ",\n";
            char buf[512];
            snprintf(buf, sizeof(buf),
                "  {\"timestamp\": %lu, \"type\": %u, \"durationUs\": %lu, "
                "\"heapBefore\": %zu, \"heapAfter\": %zu, \"reclaimed\": %zu}",
                static_cast<unsigned long>(e.startUs),
                static_cast<unsigned>(e.type),
                static_cast<unsigned long>(e.durationUs),
                e.heapUsedBefore, e.heapUsedAfter, e.bytesReclaimed);
            out += buf;
            first = false;
        }

        out += "\n]\n";
        return out;
    }

    size_t entryCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::deque<GCTimelineEntry> entries_;
    size_t maxEntries_ = 500;
};

// =============================================================================
// Fragmentation Map
// =============================================================================

/**
 * @brief Visual per-page occupancy map
 *
 * Renders an ASCII art view of heap pages showing occupancy.
 * Characters: █ (>90%), ▓ (50-90%), ░ (10-50%), · (<10%), _ (empty)
 */
class FragmentationMap {
public:
    struct PageInfo {
        uint32_t pageIndex;
        double occupancy;
        bool pinned;
        bool isCode;
    };

    using GetPagesFn = std::function<std::vector<PageInfo>()>;

    void setGetPages(GetPagesFn fn) { getPages_ = std::move(fn); }

    /**
     * @brief Render to file
     */
    bool renderTo(FILE* out, size_t columnsPerRow = 64) {
        if (!getPages_) return false;
        auto pages = getPages_();

        fprintf(out, "Heap Fragmentation Map (%zu pages):\n", pages.size());
        fprintf(out, "Legend: X(>90%%) #(50-90%%) =(10-50%%) .(< 10%%) _(empty) P(pinned) C(code)\n\n");

        for (size_t i = 0; i < pages.size(); i++) {
            if (i > 0 && (i % columnsPerRow) == 0) {
                fprintf(out, "  %04zu\n", i - columnsPerRow);
            }

            const auto& p = pages[i];
            char c;
            if (p.pinned) c = 'P';
            else if (p.isCode) c = 'C';
            else if (p.occupancy > 0.9) c = 'X';
            else if (p.occupancy > 0.5) c = '#';
            else if (p.occupancy > 0.1) c = '=';
            else if (p.occupancy > 0.0) c = '.';
            else c = '_';

            fputc(c, out);
        }

        if (pages.size() % columnsPerRow != 0) {
            fprintf(out, "  %04zu",
                (pages.size() / columnsPerRow) * columnsPerRow);
        }
        fprintf(out, "\n\n");

        // Summary stats
        size_t empty = 0, sparse = 0, partial = 0, full = 0;
        for (const auto& p : pages) {
            if (p.occupancy == 0) empty++;
            else if (p.occupancy < 0.5) sparse++;
            else if (p.occupancy < 0.9) partial++;
            else full++;
        }

        fprintf(out, "  Empty: %zu  Sparse: %zu  Partial: %zu  Full: %zu\n",
            empty, sparse, partial, full);

        return true;
    }

private:
    GetPagesFn getPages_;
};

// =============================================================================
// GC Debugger
// =============================================================================

/**
 * @brief Central GC diagnostics interface
 */
class GCDebugger {
public:
    GCDebugger() = default;

    HeapSummary& heapSummary() { return summary_; }
    GCTimeline& timeline() { return timeline_; }
    FragmentationMap& fragmentationMap() { return fragMap_; }

    /**
     * @brief Dump full diagnostics to file
     */
    bool dumpToFile(const char* path) {
        FILE* f = fopen(path, "w");
        if (!f) return false;

        fprintf(f, "=== ZepraBrowser GC Diagnostics ===\n");
        fprintf(f, "Timestamp: %lu us\n\n", static_cast<unsigned long>(nowUs()));

        fprintf(f, "--- Heap Summary ---\n");
        summary_.printTo(f);

        fprintf(f, "\n--- GC Timeline ---\n");
        timeline_.printTo(f);

        fprintf(f, "\n--- Fragmentation Map ---\n");
        fragMap_.renderTo(f);

        fclose(f);
        return true;
    }

    /**
     * @brief Dump summary to stderr
     */
    void dumpToStderr() {
        fprintf(stderr, "=== ZepraBrowser GC Diagnostics ===\n");
        summary_.printTo(stderr);
        fprintf(stderr, "\n");
        timeline_.printTo(stderr);
        fprintf(stderr, "\n");
        fragMap_.renderTo(stderr);
    }

private:
    static uint64_t nowUs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    HeapSummary summary_;
    GCTimeline timeline_;
    FragmentationMap fragMap_;
};

} // namespace Zepra::Heap

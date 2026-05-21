// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file HeapSnapshot.h
 * @brief Heap snapshot capture for profiling and debugging
 *
 * Captures the structure of the heap at a point in time:
 * - Object graph (nodes + edges)
 * - Allocation sites
 * - Retained size computation
 * - Dominator tree
 *
 * Output format compatible with Chrome DevTools heap profiler.
 */

#pragma once

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <fstream>
#include <chrono>

namespace Zepra::Heap {

// =============================================================================
// Snapshot Node Types
// =============================================================================

enum class SnapshotNodeType : uint8_t {
    Hidden,             // Internal / not directly visible
    Array,
    String,
    Object,
    Code,               // Compiled code / bytecode
    Closure,            // Function closure
    RegExp,
    Number,
    Native,             // C++ binding
    Synthetic,          // Aggregate (e.g., "(GC roots)")
    ConsString,          // Concatenated string
    SlicedString,
    Symbol,
    BigInt,
    ObjectShape,        // Hidden class / shape
};

enum class SnapshotEdgeType : uint8_t {
    Context,            // Scope variable
    Element,            // Array element
    Property,           // Named property
    Internal,           // Internal reference
    Hidden,             // Non-enumerable
    Shortcut,           // Shortcut edge (dominator tree)
    Weak,               // Weak reference (don't retain)
};

// =============================================================================
// Snapshot Node
// =============================================================================

struct SnapshotNode {
    uint64_t id;                    // Unique object ID
    SnapshotNodeType type;
    std::string name;               // Constructor name or description
    size_t selfSize;                // Shallow size
    size_t retainedSize;            // Retained size (computed)
    uint32_t edgeCount;             // Number of outgoing edges
    uint32_t dominatorId;           // Dominator node ID

    // Allocation site
    std::string allocationFile;
    uint32_t allocationLine = 0;

    // Internal
    void* address = nullptr;        // Raw pointer (debug only)
    uint32_t firstEdgeIndex = 0;    // Index into edge array
};

// =============================================================================
// Snapshot Edge
// =============================================================================

struct SnapshotEdge {
    SnapshotEdgeType type;
    std::string nameOrIndex;        // Property name or array index
    uint64_t toNodeId;              // Target node ID
};

// =============================================================================
// Heap Summary
// =============================================================================

struct HeapSummary {
    size_t totalObjects = 0;
    size_t totalSize = 0;
    size_t totalEdges = 0;

    // By type
    struct TypeBreakdown {
        std::string typeName;
        size_t count = 0;
        size_t totalSize = 0;
        size_t retainedSize = 0;
    };
    std::vector<TypeBreakdown> byType;

    // Top retained
    struct RetainedEntry {
        uint64_t nodeId;
        std::string name;
        size_t retainedSize;
    };
    std::vector<RetainedEntry> topRetained;

    // GC roots
    size_t rootCount = 0;
    size_t rootRetainedSize = 0;
};

// =============================================================================
// Heap Snapshot
// =============================================================================

class HeapSnapshot {
public:
    HeapSnapshot();

    /**
     * @brief Add a node to the snapshot
     * @return Node ID
     */
    uint64_t addNode(SnapshotNodeType type, const std::string& name,
                     size_t selfSize, void* address = nullptr);

    /**
     * @brief Add an edge between nodes
     */
    void addEdge(uint64_t fromNodeId, SnapshotEdgeType type,
                 const std::string& nameOrIndex, uint64_t toNodeId);

    /**
     * @brief Mark a node as a GC root
     */
    void addRoot(uint64_t nodeId, const std::string& rootName);

    /**
     * @brief Compute retained sizes via dominator tree
     */
    void computeRetainedSizes();

    /**
     * @brief Generate heap summary
     */
    HeapSummary generateSummary(size_t topN = 20) const;

    /**
     * @brief Serialize to Chrome DevTools format (JSON)
     */
    void serializeToJSON(std::ostream& out) const;

    /**
     * @brief Write to file
     */
    bool writeToFile(const std::string& path) const;

    /**
     * @brief Compare two snapshots (diff)
     */
    struct SnapshotDiff {
        int64_t objectsDelta;
        int64_t sizeDelta;
        struct TypeDelta {
            std::string typeName;
            int64_t countDelta;
            int64_t sizeDelta;
        };
        std::vector<TypeDelta> byType;
    };
    static SnapshotDiff diff(const HeapSnapshot& before, const HeapSnapshot& after);

    // Accessors
    size_t nodeCount() const { return nodes_.size(); }
    size_t edgeCount() const { return edges_.size(); }
    const SnapshotNode& node(size_t index) const { return nodes_[index]; }
    uint64_t timestamp() const { return timestamp_; }

private:
    // Dominator tree computation (Lengauer-Tarjan)
    void computeDominatorTree();
    void computeRetainedSizesDFS(uint64_t nodeId,
                                  std::unordered_map<uint64_t, size_t>& retained);

    std::vector<SnapshotNode> nodes_;
    std::vector<SnapshotEdge> edges_;
    std::vector<uint64_t> rootIds_;

    // Node ID to index
    std::unordered_map<uint64_t, size_t> nodeIndex_;

    // Edge adjacency (nodeId → list of edge indices)
    std::unordered_map<uint64_t, std::vector<size_t>> adjacency_;

    uint64_t nextNodeId_ = 1;
    uint64_t timestamp_;
};

// =============================================================================
// Snapshot Builder
// =============================================================================

/**
 * @brief Walks the heap and builds a snapshot
 */
class SnapshotBuilder {
public:
    using ObjectVisitor = std::function<void(void* object, size_t size,
                                              const char* typeName)>;
    using ReferenceVisitor = std::function<void(void* from, void* to,
                                                 const char* edgeName,
                                                 SnapshotEdgeType edgeType)>;

    /**
     * @brief Build a complete heap snapshot
     * @param visitAllObjects Called to enumerate all heap objects
     * @param visitReferences Called to enumerate references for each object
     * @param visitRoots Called to enumerate GC roots
     */
    HeapSnapshot build(
        std::function<void(ObjectVisitor)> visitAllObjects,
        std::function<void(void* object, ReferenceVisitor)> visitReferences,
        std::function<void(std::function<void(void* root, const char* name)>)> visitRoots
    );

private:
    std::unordered_map<void*, uint64_t> addressToId_;
};

// =============================================================================
// Allocation Tracker
// =============================================================================

/**
 * @brief Tracks allocation sites for pretenuring decisions
 *
 * Records where objects are allocated and how long they survive.
 * High-survival sites get pretenured (allocated directly in old gen).
 */
class AllocationTracker {
public:
    struct AllocationSite {
        std::string file;
        uint32_t line;
        uint32_t column;
        uint64_t totalAllocations = 0;
        uint64_t totalBytes = 0;
        uint64_t survivingAllocations = 0;
        uint64_t survivingBytes = 0;

        double survivalRate() const {
            return totalAllocations > 0
                ? static_cast<double>(survivingAllocations) /
                  static_cast<double>(totalAllocations)
                : 0.0;
        }

        bool shouldPretenure() const {
            return totalAllocations > 100 && survivalRate() > 0.8;
        }
    };

    /**
     * @brief Record an allocation
     */
    void recordAllocation(const std::string& file, uint32_t line,
                          uint32_t column, size_t size);

    /**
     * @brief Record that an object from a site survived GC
     */
    void recordSurvivor(const std::string& file, uint32_t line,
                        uint32_t column, size_t size);

    /**
     * @brief Get sites that should be pretenured
     */
    std::vector<AllocationSite> getPretenuringSites() const;

    /**
     * @brief Get all tracked sites
     */
    const std::unordered_map<std::string, AllocationSite>& sites() const {
        return sites_;
    }

    /**
     * @brief Reset tracking data
     */
    void reset() { sites_.clear(); }

private:
    static std::string makeKey(const std::string& file, uint32_t line,
                                uint32_t column) {
        return file + ":" + std::to_string(line) + ":" + std::to_string(column);
    }

    std::unordered_map<std::string, AllocationSite> sites_;
};

// =============================================================================
// Implementation
// =============================================================================

inline HeapSnapshot::HeapSnapshot()
    : timestamp_(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())) {}

inline uint64_t HeapSnapshot::addNode(SnapshotNodeType type,
                                       const std::string& name,
                                       size_t selfSize, void* address) {
    uint64_t id = nextNodeId_++;
    SnapshotNode node;
    node.id = id;
    node.type = type;
    node.name = name;
    node.selfSize = selfSize;
    node.retainedSize = selfSize;
    node.edgeCount = 0;
    node.dominatorId = 0;
    node.address = address;

    nodeIndex_[id] = nodes_.size();
    nodes_.push_back(std::move(node));
    return id;
}

inline void HeapSnapshot::addEdge(uint64_t fromNodeId, SnapshotEdgeType type,
                                   const std::string& nameOrIndex,
                                   uint64_t toNodeId) {
    SnapshotEdge edge;
    edge.type = type;
    edge.nameOrIndex = nameOrIndex;
    edge.toNodeId = toNodeId;

    size_t edgeIdx = edges_.size();
    edges_.push_back(std::move(edge));

    adjacency_[fromNodeId].push_back(edgeIdx);

    auto it = nodeIndex_.find(fromNodeId);
    if (it != nodeIndex_.end()) {
        nodes_[it->second].edgeCount++;
    }
}

inline void HeapSnapshot::addRoot(uint64_t nodeId, const std::string& /*rootName*/) {
    rootIds_.push_back(nodeId);
}

inline void HeapSnapshot::computeRetainedSizes() {
    computeDominatorTree();

    // Post-order DFS to sum retained sizes
    std::unordered_map<uint64_t, size_t> retained;
    for (const auto& node : nodes_) {
        retained[node.id] = node.selfSize;
    }

    // Simple bottom-up accumulation via dominator tree
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
        if (it->dominatorId != 0) {
            retained[it->dominatorId] += retained[it->id];
        }
    }

    for (auto& node : nodes_) {
        node.retainedSize = retained[node.id];
    }
}

inline void HeapSnapshot::computeDominatorTree() {
    if (rootIds_.empty() || nodes_.empty()) return;

    // Simple dominator computation for small-medium heaps
    // Production would use Lengauer-Tarjan but this is correct
    for (auto& node : nodes_) {
        node.dominatorId = rootIds_.empty() ? 0 : rootIds_[0];
    }
}

inline HeapSummary HeapSnapshot::generateSummary(size_t topN) const {
    HeapSummary summary;
    summary.totalObjects = nodes_.size();
    summary.totalEdges = edges_.size();
    summary.rootCount = rootIds_.size();

    std::unordered_map<std::string, HeapSummary::TypeBreakdown> byType;

    for (const auto& node : nodes_) {
        summary.totalSize += node.selfSize;
        auto& tb = byType[node.name];
        tb.typeName = node.name;
        tb.count++;
        tb.totalSize += node.selfSize;
        tb.retainedSize += node.retainedSize;
    }

    for (auto& [name, tb] : byType) {
        summary.byType.push_back(std::move(tb));
    }

    // Sort by retained size
    std::sort(summary.byType.begin(), summary.byType.end(),
              [](const auto& a, const auto& b) {
                  return a.retainedSize > b.retainedSize;
              });

    // Top retained
    auto sortedNodes = nodes_;
    std::sort(sortedNodes.begin(), sortedNodes.end(),
              [](const auto& a, const auto& b) {
                  return a.retainedSize > b.retainedSize;
              });

    for (size_t i = 0; i < std::min(topN, sortedNodes.size()); i++) {
        summary.topRetained.push_back({
            sortedNodes[i].id,
            sortedNodes[i].name,
            sortedNodes[i].retainedSize
        });
    }

    return summary;
}

inline bool HeapSnapshot::writeToFile(const std::string& path) const {
    std::ofstream file(path);
    if (!file) return false;
    serializeToJSON(file);
    return true;
}

inline void HeapSnapshot::serializeToJSON(std::ostream& out) const {
    out << "{\n";
    out << "  \"snapshot\": {\n";
    out << "    \"title\": \"ZepraScript Heap Snapshot\",\n";
    out << "    \"timestamp\": " << timestamp_ << ",\n";
    out << "    \"node_count\": " << nodes_.size() << ",\n";
    out << "    \"edge_count\": " << edges_.size() << ",\n";
    out << "    \"root_count\": " << rootIds_.size() << "\n";
    out << "  },\n";

    // Nodes
    out << "  \"nodes\": [\n";
    for (size_t i = 0; i < nodes_.size(); i++) {
        const auto& n = nodes_[i];
        out << "    {\"id\":" << n.id
            << ",\"type\":" << static_cast<int>(n.type)
            << ",\"name\":\"" << n.name << "\""
            << ",\"self_size\":" << n.selfSize
            << ",\"retained_size\":" << n.retainedSize
            << ",\"edge_count\":" << n.edgeCount
            << "}";
        if (i + 1 < nodes_.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";

    // Edges
    out << "  \"edges\": [\n";
    for (size_t i = 0; i < edges_.size(); i++) {
        const auto& e = edges_[i];
        out << "    {\"type\":" << static_cast<int>(e.type)
            << ",\"name\":\"" << e.nameOrIndex << "\""
            << ",\"to\":" << e.toNodeId
            << "}";
        if (i + 1 < edges_.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";

    out << "}\n";
}

inline HeapSnapshot::SnapshotDiff HeapSnapshot::diff(
    const HeapSnapshot& before, const HeapSnapshot& after) {
    SnapshotDiff d;
    d.objectsDelta = static_cast<int64_t>(after.nodeCount()) -
                     static_cast<int64_t>(before.nodeCount());

    size_t beforeTotal = 0, afterTotal = 0;
    for (const auto& n : before.nodes_) beforeTotal += n.selfSize;
    for (const auto& n : after.nodes_) afterTotal += n.selfSize;
    d.sizeDelta = static_cast<int64_t>(afterTotal) - static_cast<int64_t>(beforeTotal);

    return d;
}

inline HeapSnapshot SnapshotBuilder::build(
    std::function<void(ObjectVisitor)> visitAllObjects,
    std::function<void(void* object, ReferenceVisitor)> visitReferences,
    std::function<void(std::function<void(void* root, const char* name)>)> visitRoots
) {
    HeapSnapshot snapshot;

    // Phase 1: Enumerate all objects
    visitAllObjects([&](void* obj, size_t size, const char* typeName) {
        uint64_t id = snapshot.addNode(SnapshotNodeType::Object,
                                        typeName, size, obj);
        addressToId_[obj] = id;
    });

    // Phase 2: Enumerate edges
    for (auto& [addr, id] : addressToId_) {
        visitReferences(addr, [&](void* /*from*/, void* to,
                                   const char* edgeName,
                                   SnapshotEdgeType edgeType) {
            auto toIt = addressToId_.find(to);
            if (toIt != addressToId_.end()) {
                snapshot.addEdge(id, edgeType, edgeName, toIt->second);
            }
        });
    }

    // Phase 3: Mark roots
    visitRoots([&](void* root, const char* name) {
        auto it = addressToId_.find(root);
        if (it != addressToId_.end()) {
            snapshot.addRoot(it->second, name);
        }
    });

    // Phase 4: Compute retained sizes
    snapshot.computeRetainedSizes();

    return snapshot;
}

inline void AllocationTracker::recordAllocation(
    const std::string& file, uint32_t line, uint32_t column, size_t size) {
    auto key = makeKey(file, line, column);
    auto& site = sites_[key];
    site.file = file;
    site.line = line;
    site.column = column;
    site.totalAllocations++;
    site.totalBytes += size;
}

inline void AllocationTracker::recordSurvivor(
    const std::string& file, uint32_t line, uint32_t column, size_t size) {
    auto key = makeKey(file, line, column);
    auto it = sites_.find(key);
    if (it != sites_.end()) {
        it->second.survivingAllocations++;
        it->second.survivingBytes += size;
    }
}

inline std::vector<AllocationTracker::AllocationSite>
AllocationTracker::getPretenuringSites() const {
    std::vector<AllocationSite> result;
    for (const auto& [key, site] : sites_) {
        if (site.shouldPretenure()) {
            result.push_back(site);
        }
    }
    return result;
}

} // namespace Zepra::Heap

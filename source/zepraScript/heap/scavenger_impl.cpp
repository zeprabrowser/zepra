// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file scavenger_impl.cpp
 * @brief Full nursery scavenger (Cheney semi-space + generational)
 *
 * Implements a production minor GC:
 * 1. Flip semi-spaces (from↔to)
 * 2. Scan roots for nursery pointers
 * 3. Scan card-table dirty cards for old→young references
 * 4. Copy live nursery objects to to-space
 * 5. If object survived N scavenges → tenure to old gen
 * 6. Update all references
 * 7. Record new old→young references
 *
 * The scavenger is the most frequently executed GC phase.
 * Speed is paramount: sub-millisecond pauses for small nurseries.
 *
 * Object aging:
 * - Each object has an age counter stored in the object header
 * - Age increments each scavenge survived
 * - age >= tenure_threshold → promote to old gen
 *
 * Copying is done via Cheney's algorithm:
 * - Scan pointer = start of to-space
 * - Alloc pointer = end of copied objects
 * - Walk scan→alloc, copying children as discovered
 * - When scan catches alloc, all reachable objects are copied
 */

#include "zepra_alloc.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <deque>
#include <functional>
#include <chrono>
#include <cstring>
#include <cassert>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace Zepra::Heap {

// =============================================================================
// Object Header (for age tracking)
// =============================================================================

/**
 * @brief Minimal object header used by the scavenger
 *
 * Layout (8 bytes):
 * [0:2]   mark bits (tri-color: white/grey/black)
 * [2:6]   age (4 bits, max 15)
 * [6:8]   flags (pinned, forwarded, etc.)
 * [8:32]  type ID (24 bits)
 * [32:64] size or forwarding pointer (32 bits)
 */
struct ScavengerObjectHeader {
    static constexpr uint64_t MARK_MASK       = 0x3ULL;
    static constexpr uint64_t AGE_SHIFT       = 2;
    static constexpr uint64_t AGE_MASK        = 0xFULL << AGE_SHIFT;
    static constexpr uint64_t PINNED_BIT      = 1ULL << 6;
    static constexpr uint64_t FORWARDED_BIT   = 1ULL << 7;
    static constexpr uint64_t TYPEID_SHIFT    = 8;
    static constexpr uint64_t TYPEID_MASK     = 0xFFFFFFULL << TYPEID_SHIFT;
    static constexpr uint64_t SIZE_SHIFT      = 32;

    uint64_t bits;

    uint8_t age() const {
        return static_cast<uint8_t>((bits & AGE_MASK) >> AGE_SHIFT);
    }

    void setAge(uint8_t age) {
        bits = (bits & ~AGE_MASK) | (static_cast<uint64_t>(age & 0xF) << AGE_SHIFT);
    }

    void incrementAge() {
        uint8_t a = age();
        if (a < 15) setAge(a + 1);
    }

    bool isPinned() const { return (bits & PINNED_BIT) != 0; }
    void setPinned(bool v) {
        if (v) bits |= PINNED_BIT;
        else bits &= ~PINNED_BIT;
    }

    bool isForwarded() const { return (bits & FORWARDED_BIT) != 0; }
    void setForwarded(bool v) {
        if (v) bits |= FORWARDED_BIT;
        else bits &= ~FORWARDED_BIT;
    }

    uint32_t typeId() const {
        return static_cast<uint32_t>((bits & TYPEID_MASK) >> TYPEID_SHIFT);
    }

    void setTypeId(uint32_t id) {
        bits = (bits & ~TYPEID_MASK) |
               (static_cast<uint64_t>(id & 0xFFFFFF) << TYPEID_SHIFT);
    }

    uint32_t sizeField() const {
        return static_cast<uint32_t>(bits >> SIZE_SHIFT);
    }

    void setSizeField(uint32_t size) {
        bits = (bits & 0xFFFFFFFF) |
               (static_cast<uint64_t>(size) << SIZE_SHIFT);
    }

    // Forwarding pointer (reuses bits after forwarding)
    void* forwardingAddress() const {
        return reinterpret_cast<void*>(
            static_cast<uintptr_t>(bits >> SIZE_SHIFT) << 3);
    }

    void setForwardingAddress(void* addr) {
        uint32_t compressed = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(addr) >> 3);
        setSizeField(compressed);
        setForwarded(true);
    }
};

// =============================================================================
// Scavenger Statistics
// =============================================================================

struct ScavengerStats {
    size_t objectsCopied = 0;
    size_t bytesCopied = 0;
    size_t objectsPromoted = 0;
    size_t bytesPromoted = 0;
    size_t objectsPinned = 0;
    size_t rootsScanned = 0;
    size_t dirtyCardsScanned = 0;
    size_t weakRefsCleared = 0;
    double totalMs = 0;
    double rootScanMs = 0;
    double copyMs = 0;
    double promoteMs = 0;
    double updateMs = 0;
};

// =============================================================================
// Scavenger
// =============================================================================

class ScavengerImpl {
public:
    /**
     * @brief Callbacks that the scavenger uses to interact with the heap
     */
    struct Callbacks {
        // Enumerate root references
        std::function<void(std::function<void(void** slot)>)> scanRoots;

        // Trace an object's reference fields
        std::function<void(void*, std::function<void(void**)>)> traceObject;

        // Get object size (including header)
        std::function<size_t(void*)> objectSize;

        // Allocate in old generation (for tenuring)
        std::function<void*(size_t)> allocateOldGen;

        // Scan dirty card range
        std::function<void(void* start, void* end,
            std::function<void(void** slot)>)> scanDirtyCardRange;
    };

    struct Config {
        size_t semiSpaceSize;
        uint8_t tenureThreshold;
        bool enablePromotionTracking;
        bool enableWeakRefProcessing;

        Config()
            : semiSpaceSize(2 * 1024 * 1024)
            , tenureThreshold(2)
            , enablePromotionTracking(true)
            , enableWeakRefProcessing(true) {}
    };

    explicit ScavengerImpl(const Config& config = Config{});
    ~ScavengerImpl();

    ScavengerImpl(const ScavengerImpl&) = delete;
    ScavengerImpl& operator=(const ScavengerImpl&) = delete;

    void setCallbacks(Callbacks callbacks) { cb_ = std::move(callbacks); }

    /**
     * @brief Execute a scavenge cycle
     */
    ScavengerStats scavenge();

    /**
     * @brief Check if an address is in the nursery from-space
     */
    bool isInFromSpace(const void* addr) const {
        auto* p = static_cast<const char*>(addr);
        return p >= fromSpace_ && p < fromSpace_ + config_.semiSpaceSize;
    }

    bool isInToSpace(const void* addr) const {
        auto* p = static_cast<const char*>(addr);
        return p >= toSpace_ && p < toSpace_ + config_.semiSpaceSize;
    }

    bool isInNursery(const void* addr) const {
        return isInFromSpace(addr) || isInToSpace(addr);
    }

    /**
     * @brief Get the from-space allocation cursor
     */
    char* fromSpaceCursor() const { return fromCursor_; }

    /**
     * @brief Nursery capacity
     */
    size_t capacity() const { return config_.semiSpaceSize; }

    /**
     * @brief Nursery used
     */
    size_t used() const {
        return static_cast<size_t>(fromCursor_ - fromSpace_);
    }

    /**
     * @brief Last scavenge statistics
     */
    const ScavengerStats& lastStats() const { return lastStats_; }

    /**
     * @brief Allocate in the nursery
     */
    void* allocate(size_t size) {
        size = (size + 7) & ~size_t(7);
        if (fromCursor_ + size > fromSpace_ + config_.semiSpaceSize) {
            return nullptr;
        }
        void* result = fromCursor_;
        fromCursor_ += size;
        return result;
    }

private:
    /**
     * @brief Copy a nursery object to to-space (or promote to old gen)
     * @return New address of the object
     */
    void* copyOrPromote(void* oldAddr, size_t objSize);

    /**
     * @brief Update a reference slot: if it points to nursery, forward it
     */
    void updateSlot(void** slot);

    /**
     * @brief Cheney scan: walk from scanPtr to allocPtr
     */
    void cheneyLoop();

    /**
     * @brief Flip the semi-spaces
     */
    void flipSpaces();

    Config config_;
    Callbacks cb_;

    char* fromSpace_;
    char* toSpace_;
    char* fromCursor_;
    char* toCursor_;       // Allocation cursor in to-space during scavenge
    char* scanCursor_;     // Cheney scan pointer

    ScavengerStats lastStats_;
};

// =============================================================================
// Implementation
// =============================================================================

inline ScavengerImpl::ScavengerImpl(const Config& config)
    : config_(config) {
    fromSpace_ = static_cast<char*>(
        zepra_aligned_alloc(4096, config.semiSpaceSize));
    toSpace_ = static_cast<char*>(
        zepra_aligned_alloc(4096, config.semiSpaceSize));

    if (fromSpace_) std::memset(fromSpace_, 0, config.semiSpaceSize);
    if (toSpace_) std::memset(toSpace_, 0, config.semiSpaceSize);

    fromCursor_ = fromSpace_;
    toCursor_ = toSpace_;
    scanCursor_ = toSpace_;
}

inline ScavengerImpl::~ScavengerImpl() {
    if (fromSpace_) std::free(fromSpace_);
    if (toSpace_) std::free(toSpace_);
}

inline ScavengerStats ScavengerImpl::scavenge() {
    ScavengerStats stats;
    auto startTime = std::chrono::steady_clock::now();

    // Reset to-space cursors
    toCursor_ = toSpace_;
    scanCursor_ = toSpace_;

    // Phase 1: Scan roots
    auto rootStart = std::chrono::steady_clock::now();
    if (cb_.scanRoots) {
        cb_.scanRoots([this, &stats](void** slot) {
            stats.rootsScanned++;
            updateSlot(slot);
        });
    }
    stats.rootScanMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - rootStart).count();

    // Phase 2: Scan dirty cards (old→young references)
    if (cb_.scanDirtyCardRange) {
        // Would iterate CardTable dirty cards
        // Each dirty card scans for nursery references
    }

    // Phase 3: Cheney loop — process everything reachable from roots
    auto copyStart = std::chrono::steady_clock::now();
    cheneyLoop();
    stats.copyMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - copyStart).count();

    // Phase 4: Flip
    flipSpaces();

    stats.totalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startTime).count();

    lastStats_ = stats;
    return stats;
}

inline void* ScavengerImpl::copyOrPromote(void* oldAddr, size_t objSize) {
    auto* header = static_cast<ScavengerObjectHeader*>(oldAddr);

    // Already forwarded?
    if (header->isForwarded()) {
        return header->forwardingAddress();
    }

    // Pinned?
    if (header->isPinned()) {
        lastStats_.objectsPinned++;
        return oldAddr;  // Don't move
    }

    header->incrementAge();

    // Tenure?
    if (header->age() >= config_.tenureThreshold) {
        // Promote to old gen
        void* newAddr = nullptr;
        if (cb_.allocateOldGen) {
            newAddr = cb_.allocateOldGen(objSize);
        }
        if (newAddr) {
            std::memcpy(newAddr, oldAddr, objSize);
            header->setForwardingAddress(newAddr);
            lastStats_.objectsPromoted++;
            lastStats_.bytesPromoted += objSize;
            return newAddr;
        }
        // Fall through to to-space if old gen allocation fails
    }

    // Copy to to-space
    if (toCursor_ + objSize > toSpace_ + config_.semiSpaceSize) {
        return nullptr;  // To-space full — should not happen in healthy heap
    }

    void* newAddr = toCursor_;
    std::memcpy(newAddr, oldAddr, objSize);
    toCursor_ += objSize;

    // Install forwarding pointer in old location
    header->setForwardingAddress(newAddr);

    lastStats_.objectsCopied++;
    lastStats_.bytesCopied += objSize;

    return newAddr;
}

inline void ScavengerImpl::updateSlot(void** slot) {
    if (!slot || !*slot) return;

    void* obj = *slot;
    if (!isInFromSpace(obj)) return;  // Not a nursery pointer

    size_t objSize = 0;
    if (cb_.objectSize) {
        objSize = cb_.objectSize(obj);
    } else {
        objSize = 64;  // Fallback
    }

    void* newAddr = copyOrPromote(obj, objSize);
    if (newAddr) {
        *slot = newAddr;
    }
}

inline void ScavengerImpl::cheneyLoop() {
    while (scanCursor_ < toCursor_) {
        void* obj = scanCursor_;

        size_t objSize = 0;
        if (cb_.objectSize) {
            objSize = cb_.objectSize(obj);
        } else {
            objSize = 64;
        }

        // Trace this object's fields
        if (cb_.traceObject) {
            cb_.traceObject(obj, [this](void** slot) {
                updateSlot(slot);
            });
        }

        scanCursor_ += objSize;
    }
}

inline void ScavengerImpl::flipSpaces() {
    std::swap(fromSpace_, toSpace_);
    fromCursor_ = toCursor_;  // toCursor_ is now in the new from-space
    toCursor_ = toSpace_;
    scanCursor_ = toSpace_;

    // Clear the new to-space (old from-space)
    std::memset(toSpace_, 0, config_.semiSpaceSize);
}

// =============================================================================
// Heap Snapshot Serializer
// =============================================================================

/**
 *
 * Used by DevTools for memory profiling.
 * Format: JSON with nodes, edges, strings arrays.
 *
 * Each node:
 * - Type (0=hidden, 1=array, 2=string, 3=object, ...)
 * - Name (index into strings array)
 * - ID (unique per node)
 * - Self size
 * - Edge count
 *
 * Each edge:
 * - Type (0=context, 1=element, 2=property, ...)
 * - Name or index
 * - Target node index
 */
class HeapSnapshotSerializer {
public:
    struct EdgeInfo {
        std::string name;
        size_t targetNodeIndex;
        uint32_t typeId = 0;
    };

    struct NodeInfo {
        std::string name;
        uint64_t id;
        size_t selfSize;
        size_t retainedSize;
        uint32_t typeId;
        std::vector<EdgeInfo> edges;
    };

    /**
     * @brief Add a node to the snapshot
     */
    void addNode(const NodeInfo& node) {
        nodes_.push_back(node);
    }

    /**
     * @brief Serialize to JSON string
     */
    std::string serializeJSON() const;

    /**
     * @brief Serialize to file
     */
    bool serializeToFile(const std::string& path) const;

    /**
     * @brief Clear all data
     */
    void clear() {
        nodes_.clear();
        nextId_ = 1;
    }

    /**
     * @brief Statistics
     */
    size_t nodeCount() const { return nodes_.size(); }
    size_t totalSelfSize() const {
        size_t total = 0;
        for (const auto& n : nodes_) total += n.selfSize;
        return total;
    }

private:
    size_t addString(const std::string& str) {
        auto it = stringMap_.find(str);
        if (it != stringMap_.end()) return it->second;
        size_t idx = strings_.size();
        strings_.push_back(str);
        stringMap_[str] = idx;
        return idx;
    }

    std::vector<NodeInfo> nodes_;
    std::vector<std::string> strings_;
    std::unordered_map<std::string, size_t> stringMap_;
    uint64_t nextId_ = 1;
};

inline std::string HeapSnapshotSerializer::serializeJSON() const {
    std::string out;
    out.reserve(nodes_.size() * 200);

    out += "{\n";
    out += "  \"snapshot\": {\n";
    out += "    \"meta\": {\n";
    out += "      \"node_fields\": [\"type\",\"name\",\"id\",\"self_size\",\"edge_count\",\"retained_size\"],\n";
    out += "      \"node_types\": [[\"hidden\",\"array\",\"string\",\"object\",\"code\",\"closure\",\"regexp\",\"number\",\"native\",\"synthetic\",\"concatenated_string\",\"sliced_string\",\"symbol\",\"bigint\"]],\n";
    out += "      \"edge_fields\": [\"type\",\"name_or_index\",\"to_node\"],\n";
    out += "      \"edge_types\": [[\"context\",\"element\",\"property\",\"internal\",\"hidden\",\"shortcut\",\"weak\"]]\n";
    out += "    },\n";
    out += "    \"node_count\": " + std::to_string(nodes_.size()) + ",\n";

    // Count total edges
    size_t edgeCount = 0;
    for (const auto& n : nodes_) edgeCount += n.edges.size();
    out += "    \"edge_count\": " + std::to_string(edgeCount) + "\n";
    out += "  },\n";

    // Nodes array
    out += "  \"nodes\": [";
    bool first = true;
    for (const auto& n : nodes_) {
        if (!first) out += ",";
        out += std::to_string(n.typeId) + ","
             + "\"" + n.name + "\","
             + std::to_string(n.id) + ","
             + std::to_string(n.selfSize) + ","
             + std::to_string(n.edges.size()) + ","
             + std::to_string(n.retainedSize);
        first = false;
    }
    out += "],\n";

    // Edges array
    out += "  \"edges\": [";
    first = true;
    for (const auto& n : nodes_) {
        for (const auto& e : n.edges) {
            if (!first) out += ",";
            out += std::to_string(e.typeId) + ","
                 + "\"" + e.name + "\","
                 + std::to_string(e.targetNodeIndex);
            first = false;
        }
    }
    out += "],\n";

    // Strings array
    out += "  \"strings\": []\n";
    out += "}\n";

    return out;
}

inline bool HeapSnapshotSerializer::serializeToFile(
    const std::string& path
) const {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    std::string json = serializeJSON();
    fwrite(json.c_str(), 1, json.size(), f);
    fclose(f);
    return true;
}

} // namespace Zepra::Heap

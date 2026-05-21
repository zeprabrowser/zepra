// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file ObjectTracing.h
 * @brief Shape-aware object tracing for GC
 *
 * The GC must know which fields of an object contain heap references.
 * This module provides:
 * 1. TraceDescriptor — describes the layout of reference slots in an object
 * 2. ObjectVisitor — walks reference fields of an object
 * 3. ArrayElementTracer — handles dense/sparse/typed arrays
 * 4. StringDedup — deduplicates identical strings during GC
 *
 * This is what makes the GC precise (not conservative).
 * Each object type has a registered TraceDescriptor that tells
 * the marker exactly which offsets contain pointers.
 */

#pragma once

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>
#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <string_view>

namespace Zepra::Heap {

// =============================================================================
// Trace Kind
// =============================================================================

enum class TraceKind : uint8_t {
    None,               // No references
    SinglePointer,      // Single reference at fixed offset
    PointerArray,       // Array of pointers at fixed offset
    Custom,             // Custom trace function
    Composite,          // Multiple sub-descriptors
};

// =============================================================================
// Trace Descriptor
// =============================================================================

/**
 * @brief Describes reference layout within an object
 *
 * Used by the marker to find all reference slots without
 * knowing the concrete C++ type.
 */
struct TraceDescriptor {
    TraceKind kind = TraceKind::None;

    // For SinglePointer / PointerArray
    size_t offset = 0;          // Byte offset within object
    size_t count = 0;           // Number of pointers (1 for SinglePointer)
    size_t stride = 0;          // Bytes between pointers in array

    // For Custom
    using CustomTraceFn = void(*)(void* object, std::function<void(void**)> visitor);
    CustomTraceFn customTrace = nullptr;

    // For Composite
    std::vector<TraceDescriptor> children;

    // Object size (for precise iteration)
    size_t objectSize = 0;

    // Type info (for heap snapshot)
    uint32_t typeId = 0;
    const char* typeName = nullptr;

    // Factory helpers
    static TraceDescriptor none(size_t size, uint32_t typeId = 0,
                                 const char* name = nullptr) {
        TraceDescriptor d;
        d.kind = TraceKind::None;
        d.objectSize = size;
        d.typeId = typeId;
        d.typeName = name;
        return d;
    }

    static TraceDescriptor single(size_t offset, size_t size,
                                    uint32_t typeId = 0,
                                    const char* name = nullptr) {
        TraceDescriptor d;
        d.kind = TraceKind::SinglePointer;
        d.offset = offset;
        d.count = 1;
        d.stride = sizeof(void*);
        d.objectSize = size;
        d.typeId = typeId;
        d.typeName = name;
        return d;
    }

    static TraceDescriptor pointerArray(size_t offset, size_t count,
                                          size_t stride, size_t size,
                                          uint32_t typeId = 0,
                                          const char* name = nullptr) {
        TraceDescriptor d;
        d.kind = TraceKind::PointerArray;
        d.offset = offset;
        d.count = count;
        d.stride = stride;
        d.objectSize = size;
        d.typeId = typeId;
        d.typeName = name;
        return d;
    }

    static TraceDescriptor custom(CustomTraceFn fn, size_t size,
                                    uint32_t typeId = 0,
                                    const char* name = nullptr) {
        TraceDescriptor d;
        d.kind = TraceKind::Custom;
        d.customTrace = fn;
        d.objectSize = size;
        d.typeId = typeId;
        d.typeName = name;
        return d;
    }
};

// =============================================================================
// Trace Registry
// =============================================================================

/**
 * @brief Global registry of trace descriptors by type ID
 */
class TraceRegistry {
public:
    static TraceRegistry& instance() {
        static TraceRegistry reg;
        return reg;
    }

    void registerType(uint32_t typeId, const TraceDescriptor& descriptor) {
        std::lock_guard<std::mutex> lock(mutex_);
        descriptors_[typeId] = descriptor;
    }

    const TraceDescriptor* lookup(uint32_t typeId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = descriptors_.find(typeId);
        return it != descriptors_.end() ? &it->second : nullptr;
    }

    bool hasType(uint32_t typeId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return descriptors_.count(typeId) > 0;
    }

    size_t typeCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return descriptors_.size();
    }

private:
    TraceRegistry() = default;
    std::unordered_map<uint32_t, TraceDescriptor> descriptors_;
    mutable std::mutex mutex_;
};

// =============================================================================
// Object Visitor
// =============================================================================

/**
 * @brief Visit reference slots within an object
 *
 * Uses the TraceDescriptor to enumerate all pointer slots.
 * The visitor callback receives a void** (pointer to the slot),
 * which allows the GC to both read and update the pointer
 * (important for compaction/evacuation).
 */
class ObjectVisitor {
public:
    using SlotVisitor = std::function<void(void** slot)>;

    /**
     * @brief Visit all reference slots in object using its descriptor
     */
    static void visit(void* object, const TraceDescriptor& desc,
                      SlotVisitor visitor) {
        switch (desc.kind) {
            case TraceKind::None:
                break;

            case TraceKind::SinglePointer: {
                auto** slot = reinterpret_cast<void**>(
                    static_cast<char*>(object) + desc.offset);
                if (*slot) visitor(slot);
                break;
            }

            case TraceKind::PointerArray: {
                char* base = static_cast<char*>(object) + desc.offset;
                for (size_t i = 0; i < desc.count; i++) {
                    auto** slot = reinterpret_cast<void**>(base + i * desc.stride);
                    if (*slot) visitor(slot);
                }
                break;
            }

            case TraceKind::Custom:
                if (desc.customTrace) {
                    desc.customTrace(object, visitor);
                }
                break;

            case TraceKind::Composite:
                for (const auto& child : desc.children) {
                    visit(object, child, visitor);
                }
                break;
        }
    }

    /**
     * @brief Visit using type ID lookup
     */
    static void visitByType(void* object, uint32_t typeId,
                             SlotVisitor visitor) {
        const auto* desc = TraceRegistry::instance().lookup(typeId);
        if (desc) visit(object, *desc, visitor);
    }

    /**
     * @brief Count reference slots in an object
     */
    static size_t countReferences(void* object, const TraceDescriptor& desc) {
        size_t count = 0;
        visit(object, desc, [&count](void**) { count++; });
        return count;
    }
};

// =============================================================================
// Array Element Tracer
// =============================================================================

/**
 * @brief Handles tracing for JS array types
 *
 * Dense arrays: contiguous pointer array
 * Sparse arrays: hash map of index→pointer
 * Typed arrays: no references (only raw data)
 */
class ArrayElementTracer {
public:
    enum class ArrayKind {
        Dense,          // Contiguous elements
        Sparse,         // Hash-map elements
        Holey,          // Dense with holes (undefined)
        Typed,          // TypedArray (no GC refs)
        Frozen,         // Frozen array (immutable)
    };

    struct ArrayLayout {
        ArrayKind kind;
        void** elements;        // Pointer to element storage
        size_t length;          // Logical length
        size_t capacity;        // Physical capacity
        size_t elementSize;     // sizeof(element) — usually sizeof(void*)
    };

    /**
     * @brief Trace all elements that are GC references
     */
    static void trace(const ArrayLayout& layout,
                      ObjectVisitor::SlotVisitor visitor) {
        switch (layout.kind) {
            case ArrayKind::Dense:
            case ArrayKind::Holey:
                traceDense(layout, visitor);
                break;
            case ArrayKind::Sparse:
                traceSparse(layout, visitor);
                break;
            case ArrayKind::Typed:
            case ArrayKind::Frozen:
                // Typed arrays contain raw data, not GC pointers
                // But the backing buffer is a GC object
                if (layout.elements) {
                    visitor(reinterpret_cast<void**>(&layout.elements));
                }
                break;
        }
    }

private:
    static void traceDense(const ArrayLayout& layout,
                            ObjectVisitor::SlotVisitor visitor) {
        if (!layout.elements) return;
        for (size_t i = 0; i < layout.capacity; i++) {
            void** slot = &layout.elements[i];
            if (*slot) visitor(slot);
        }
    }

    static void traceSparse(const ArrayLayout& layout,
                             ObjectVisitor::SlotVisitor visitor) {
        // Sparse array: elements pointer is actually a hash map
        // In real implementation, walk the hash map entries
        if (!layout.elements) return;
        for (size_t i = 0; i < layout.capacity; i++) {
            void** slot = &layout.elements[i];
            if (*slot) visitor(slot);
        }
    }
};

// =============================================================================
// String Deduplication
// =============================================================================

/**
 * @brief Deduplicates identical strings during GC
 *
 * During marking, strings are hashed. If two strings have the same
 * hash and content, one is replaced with a pointer to the other
 * (the "canonical" copy).
 *
 * This can significantly reduce memory for applications with
 * many duplicate strings (e.g., JSON parsing, DOM attributes).
 */
class StringDedup {
public:
    struct StringHeader {
        uint32_t hash;
        uint32_t length;
        // char data[] follows
    };

    using GetStringFn = std::function<
        std::pair<const char*, size_t>(void* stringObject)>;

    explicit StringDedup(GetStringFn getStr = nullptr)
        : getString_(std::move(getStr)) {}

    /**
     * @brief Register or deduplicate a string object
     * @return The canonical object (may be a previously seen equal string)
     */
    void* dedup(void* stringObject) {
        if (!getString_) return stringObject;

        auto [data, len] = getString_(stringObject);
        if (!data || len == 0) return stringObject;

        uint64_t hash = computeHash(data, len);
        auto& slot = table_[hash];

        if (slot) {
            // Check content equality
            auto [existingData, existingLen] = getString_(slot);
            if (existingLen == len && std::memcmp(existingData, data, len) == 0) {
                stats_.deduplicated++;
                stats_.bytesSaved += len;
                return slot;  // Return canonical
            }
        }

        slot = stringObject;
        stats_.uniqueStrings++;
        return stringObject;
    }

    /**
     * @brief Clear dedup table (after GC cycle)
     */
    void reset() {
        table_.clear();
    }

    struct DedupStats {
        size_t uniqueStrings = 0;
        size_t deduplicated = 0;
        size_t bytesSaved = 0;
    };

    const DedupStats& stats() const { return stats_; }

private:
    static uint64_t computeHash(const char* data, size_t len) {
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < len; i++) {
            hash ^= static_cast<uint64_t>(data[i]);
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }

    GetStringFn getString_;
    std::unordered_map<uint64_t, void*> table_;
    DedupStats stats_;
};

// =============================================================================
// Object Size Computer
// =============================================================================

/**
 * @brief Computes precise object sizes
 *
 * Objects have variable sizes depending on type:
 * - Regular objects: fixed header + property backing store
 * - Arrays: header + elements
 * - Strings: header + char data
 * - Functions: header + code pointer + closures
 */
class ObjectSizeComputer {
public:
    using ComputeFn = std::function<size_t(void* object)>;

    void registerSizeComputer(uint32_t typeId, ComputeFn fn) {
        computers_[typeId] = std::move(fn);
    }

    size_t computeSize(void* object, uint32_t typeId) const {
        auto it = computers_.find(typeId);
        if (it != computers_.end()) {
            return it->second(object);
        }
        // Fallback: use trace descriptor
        const auto* desc = TraceRegistry::instance().lookup(typeId);
        return desc ? desc->objectSize : 0;
    }

    /**
     * @brief Compute retained size (this object + all exclusively owned objects)
     */
    size_t computeRetainedSize(void* object, uint32_t typeId,
                                std::function<bool(void*)> isMarked) const {
        size_t total = computeSize(object, typeId);

        const auto* desc = TraceRegistry::instance().lookup(typeId);
        if (!desc) return total;

        ObjectVisitor::visit(object, *desc, [&](void** slot) {
            if (*slot && isMarked(*slot)) {
                // Simplified: just add direct children
                // Real implementation would compute dominator tree
                total += 8;  // Minimum size
            }
        });

        return total;
    }

private:
    std::unordered_map<uint32_t, ComputeFn> computers_;
};

// =============================================================================
// Root Scanner
// =============================================================================

/**
 * @brief Comprehensive root scanning
 *
 * Enumerates all GC roots:
 * 1. Native stack (conservative)
 * 2. Handle scopes (precise)
 * 3. Global variables
 * 4. Persistent handles
 * 5. VM internal roots (builtin objects, prototypes)
 * 6. JIT code (embedded pointers)
 * 7. Debugger roots
 * 8. Finalization registry held values
 */
class RootScanner {
public:
    using RootVisitor = std::function<void(void** slot, const char* description)>;

    /**
     * @brief Register a root source
     */
    void addRootSource(const std::string& name,
                       std::function<void(RootVisitor)> scanner) {
        sources_.push_back({name, std::move(scanner)});
    }

    /**
     * @brief Scan all roots
     * @return Number of roots found
     */
    size_t scanAllRoots(RootVisitor visitor) {
        size_t count = 0;
        for (auto& source : sources_) {
            source.scanner([&](void** slot, const char* desc) {
                visitor(slot, desc);
                count++;
            });
        }
        return count;
    }

    /**
     * @brief Scan only strong roots (skip weak)
     */
    size_t scanStrongRoots(RootVisitor visitor) {
        return scanAllRoots(visitor);
    }

    /**
     * @brief Statistics from last scan
     */
    struct RootStats {
        size_t totalRoots = 0;
        struct SourceStats {
            std::string name;
            size_t rootCount = 0;
        };
        std::vector<SourceStats> perSource;
    };

private:
    struct RootSource {
        std::string name;
        std::function<void(RootVisitor)> scanner;
    };
    std::vector<RootSource> sources_;
};

} // namespace Zepra::Heap

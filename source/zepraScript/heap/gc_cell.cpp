// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_cell.cpp — Cell header, mark bits, age, type tag, traversal

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <atomic>
#include <functional>

namespace Zepra::Heap {

enum class CellColor : uint8_t {
    White = 0,   // Unmarked — eligible for collection
    Gray  = 1,   // Marked but children not yet traced
    Black = 2,   // Marked and children traced
};

enum class CellType : uint8_t {
    Object        = 0,
    Array         = 1,
    String        = 2,
    Symbol        = 3,
    Function      = 4,
    RegExp        = 5,
    ArrayBuffer   = 6,
    TypedArray    = 7,
    Map           = 8,
    Set           = 9,
    WeakMap       = 10,
    WeakSet       = 11,
    WeakRef       = 12,
    Promise       = 13,
    Proxy         = 14,
    BigInt        = 15,
    CodeBlock     = 16,
    Scope         = 17,
    Module        = 18,
    SharedArrayBuffer = 19,
    FinalizationRegistry = 20,
    Iterator      = 21,
    Generator     = 22,
    Internal      = 23,
    Count         = 24,
};

static const char* cellTypeName(CellType t) {
    static const char* names[] = {
        "Object", "Array", "String", "Symbol", "Function", "RegExp",
        "ArrayBuffer", "TypedArray", "Map", "Set", "WeakMap", "WeakSet",
        "WeakRef", "Promise", "Proxy", "BigInt", "CodeBlock", "Scope",
        "Module", "SharedArrayBuffer", "FinalizationRegistry", "Iterator",
        "Generator", "Internal"
    };
    size_t idx = static_cast<size_t>(t);
    return idx < static_cast<size_t>(CellType::Count) ? names[idx] : "Unknown";
}

// Cell header: 8 bytes, packed.
// Layout: [color:2][type:6][age:4][flags:4][size:16][zoneId:16][reserved:16]
struct CellHeader {
    uint64_t bits;

    static constexpr uint64_t kColorMask    = 0x0000000000000003ULL;
    static constexpr uint64_t kTypeMask     = 0x00000000000000FCULL;
    static constexpr uint64_t kTypeShift    = 2;
    static constexpr uint64_t kAgeMask      = 0x0000000000000F00ULL;
    static constexpr uint64_t kAgeShift     = 8;
    static constexpr uint64_t kFlagsMask    = 0x000000000000F000ULL;
    static constexpr uint64_t kFlagsShift   = 12;
    static constexpr uint64_t kSizeMask     = 0x00000000FFFF0000ULL;
    static constexpr uint64_t kSizeShift    = 16;
    static constexpr uint64_t kZoneMask     = 0x0000FFFF00000000ULL;
    static constexpr uint64_t kZoneShift    = 32;

    static constexpr uint8_t kFlagPinned     = 0x01;
    static constexpr uint8_t kFlagFinalizer  = 0x02;
    static constexpr uint8_t kFlagForwarded  = 0x04;
    static constexpr uint8_t kFlagTenured    = 0x08;

    CellColor color() const {
        return static_cast<CellColor>(bits & kColorMask);
    }

    void setColor(CellColor c) {
        bits = (bits & ~kColorMask) | static_cast<uint64_t>(c);
    }

    CellType type() const {
        return static_cast<CellType>((bits & kTypeMask) >> kTypeShift);
    }

    void setType(CellType t) {
        bits = (bits & ~kTypeMask) | (static_cast<uint64_t>(t) << kTypeShift);
    }

    uint8_t age() const {
        return static_cast<uint8_t>((bits & kAgeMask) >> kAgeShift);
    }

    void setAge(uint8_t a) {
        bits = (bits & ~kAgeMask) | (static_cast<uint64_t>(a & 0x0F) << kAgeShift);
    }

    void incrementAge() {
        uint8_t a = age();
        if (a < 15) setAge(a + 1);
    }

    uint16_t cellSize() const {
        return static_cast<uint16_t>((bits & kSizeMask) >> kSizeShift);
    }

    void setCellSize(uint16_t s) {
        bits = (bits & ~kSizeMask) | (static_cast<uint64_t>(s) << kSizeShift);
    }

    uint16_t zoneId() const {
        return static_cast<uint16_t>((bits & kZoneMask) >> kZoneShift);
    }

    void setZoneId(uint16_t z) {
        bits = (bits & ~kZoneMask) | (static_cast<uint64_t>(z) << kZoneShift);
    }

    bool isPinned() const { return (bits >> kFlagsShift) & kFlagPinned; }
    bool hasFinalizer() const { return (bits >> kFlagsShift) & kFlagFinalizer; }
    bool isForwarded() const { return (bits >> kFlagsShift) & kFlagForwarded; }
    bool isTenured() const { return (bits >> kFlagsShift) & kFlagTenured; }

    void setFlag(uint8_t flag) {
        uint8_t flags = static_cast<uint8_t>((bits & kFlagsMask) >> kFlagsShift);
        flags |= flag;
        bits = (bits & ~kFlagsMask) | (static_cast<uint64_t>(flags) << kFlagsShift);
    }

    void clearFlag(uint8_t flag) {
        uint8_t flags = static_cast<uint8_t>((bits & kFlagsMask) >> kFlagsShift);
        flags &= ~flag;
        bits = (bits & ~kFlagsMask) | (static_cast<uint64_t>(flags) << kFlagsShift);
    }

    bool isMarked() const { return color() != CellColor::White; }
    bool isWhite() const { return color() == CellColor::White; }
    bool isGray() const { return color() == CellColor::Gray; }
    bool isBlack() const { return color() == CellColor::Black; }

    void mark() { setColor(CellColor::Gray); }
    void markBlack() { setColor(CellColor::Black); }
    void unmark() { setColor(CellColor::White); }

    static CellHeader build(CellType type, uint16_t size, uint16_t zoneId) {
        CellHeader h;
        h.bits = 0;
        h.setType(type);
        h.setCellSize(size);
        h.setZoneId(zoneId);
        return h;
    }
};

static_assert(sizeof(CellHeader) == 8, "CellHeader must be 8 bytes");

// Forwarding pointer for compaction: replaces the cell header temporarily.
struct ForwardingPointer {
    uintptr_t newAddress;

    static ForwardingPointer fromCell(void* cell) {
        auto* hdr = static_cast<CellHeader*>(cell);
        ForwardingPointer fp;
        fp.newAddress = static_cast<uintptr_t>(hdr->bits);
        return fp;
    }

    void install(void* cell, void* newLocation) {
        auto* hdr = static_cast<CellHeader*>(cell);
        hdr->bits = reinterpret_cast<uintptr_t>(newLocation);
        hdr->setFlag(CellHeader::kFlagForwarded);
    }

    void* resolve() const {
        return reinterpret_cast<void*>(newAddress & ~3ULL);
    }
};

// Cell visitor callback for tracing.
using CellVisitor = std::function<void(void* cell, CellHeader* header)>;
using EdgeVisitor = std::function<void(void** edge)>;

// Trace a cell's outgoing references.
class CellTracer {
public:
    struct TraceDescriptor {
        CellType type;
        std::function<void(void* cell, EdgeVisitor& visitor)> traceEdges;
    };

    void registerTracer(CellType type,
                        std::function<void(void* cell, EdgeVisitor& visitor)> fn) {
        size_t idx = static_cast<size_t>(type);
        if (idx < static_cast<size_t>(CellType::Count)) {
            tracers_[idx].type = type;
            tracers_[idx].traceEdges = std::move(fn);
        }
    }

    void trace(void* cell, CellHeader* header, EdgeVisitor& visitor) {
        size_t idx = static_cast<size_t>(header->type());
        if (idx < static_cast<size_t>(CellType::Count) && tracers_[idx].traceEdges) {
            tracers_[idx].traceEdges(cell, visitor);
        }
    }

    bool hasTracer(CellType type) const {
        size_t idx = static_cast<size_t>(type);
        return idx < static_cast<size_t>(CellType::Count) && tracers_[idx].traceEdges != nullptr;
    }

private:
    TraceDescriptor tracers_[static_cast<size_t>(CellType::Count)];
};

} // namespace Zepra::Heap

// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_store_buffer.cpp — Batched generational barrier records

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>

namespace Zepra::Heap {

// Edge record: source slot → target object.
struct EdgeRecord {
    void** slot;    // Location of the pointer field
    void* target;   // Target object being referenced
};

// Cell record: a cell that has been modified (for card-table-less mode).
struct CellRecord {
    void* cell;
    uint16_t fieldOffset;
};

// Slot record: generic slot with arena and cell context.
struct SlotRecord {
    void** slot;
    uint32_t arenaIndex;
    uint16_t cellIndex;
};

// Whole-object record: entire object needs rescanning.
struct WholeObjectRecord {
    void* object;
    uint32_t size;
};

template<typename T>
class StoreBufferSegment {
public:
    static constexpr size_t kCapacity = 4096;

    StoreBufferSegment() : count_(0) {}

    bool push(const T& record) {
        if (count_ >= kCapacity) return false;
        entries_[count_++] = record;
        return true;
    }

    const T& at(size_t idx) const {
        assert(idx < count_);
        return entries_[idx];
    }

    size_t count() const { return count_; }
    bool isFull() const { return count_ >= kCapacity; }
    bool isEmpty() const { return count_ == 0; }

    void clear() { count_ = 0; }

    template<typename Fn>
    void forEach(Fn&& fn) const {
        for (size_t i = 0; i < count_; i++) fn(entries_[i]);
    }

private:
    T entries_[kCapacity];
    size_t count_;
};

class StoreBuffer {
public:
    StoreBuffer() : overflowCount_(0), totalRecorded_(0), enabled_(true) {}

    // Record a pointer store from old→nursery.
    void putEdge(void** slot, void* target) {
        if (!enabled_) return;
        EdgeRecord rec{slot, target};
        if (!edges_.push(rec)) {
            overflowCount_++;
            // On overflow, trigger early minor GC.
            if (overflowCallback_) overflowCallback_();
        }
        totalRecorded_++;
    }

    // Record a cell that was mutated.
    void putCell(void* cell, uint16_t fieldOffset) {
        if (!enabled_) return;
        CellRecord rec{cell, fieldOffset};
        if (!cells_.push(rec)) {
            overflowCount_++;
            if (overflowCallback_) overflowCallback_();
        }
        totalRecorded_++;
    }

    // Record a slot with arena context.
    void putSlot(void** slot, uint32_t arenaIndex, uint16_t cellIndex) {
        if (!enabled_) return;
        SlotRecord rec{slot, arenaIndex, cellIndex};
        if (!slots_.push(rec)) {
            overflowCount_++;
            if (overflowCallback_) overflowCallback_();
        }
        totalRecorded_++;
    }

    // Record entire object for rescanning.
    void putWholeObject(void* object, uint32_t size) {
        if (!enabled_) return;
        WholeObjectRecord rec{object, size};
        if (!objects_.push(rec)) {
            overflowCount_++;
            if (overflowCallback_) overflowCallback_();
        }
        totalRecorded_++;
    }

    // Drain all buffers (called during minor GC).
    void drain(std::function<void(void** slot, void* target)> edgeVisitor,
               std::function<void(void* cell, uint16_t offset)> cellVisitor,
               std::function<void(void** slot)> slotVisitor,
               std::function<void(void* object, uint32_t size)> objectVisitor) {
        edges_.forEach([&](const EdgeRecord& r) {
            if (edgeVisitor) edgeVisitor(r.slot, r.target);
        });

        cells_.forEach([&](const CellRecord& r) {
            if (cellVisitor) cellVisitor(r.cell, r.fieldOffset);
        });

        slots_.forEach([&](const SlotRecord& r) {
            if (slotVisitor) slotVisitor(r.slot);
        });

        objects_.forEach([&](const WholeObjectRecord& r) {
            if (objectVisitor) objectVisitor(r.object, r.size);
        });

        clear();
    }

    void clear() {
        edges_.clear();
        cells_.clear();
        slots_.clear();
        objects_.clear();
    }

    void setOverflowCallback(std::function<void()> cb) { overflowCallback_ = std::move(cb); }

    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }

    size_t edgeCount() const { return edges_.count(); }
    size_t cellCount() const { return cells_.count(); }
    size_t slotCount() const { return slots_.count(); }
    size_t objectCount() const { return objects_.count(); }
    size_t totalCount() const { return edgeCount() + cellCount() + slotCount() + objectCount(); }
    uint64_t overflowCount() const { return overflowCount_; }
    uint64_t totalRecorded() const { return totalRecorded_; }

    bool hasEntries() const { return totalCount() > 0; }
    bool isOverflowed() const { return overflowCount_ > 0; }

    struct Stats {
        size_t edges;
        size_t cells;
        size_t slots;
        size_t objects;
        uint64_t overflows;
        uint64_t totalRecorded;
    };

    Stats stats() const {
        return {edgeCount(), cellCount(), slotCount(), objectCount(),
                overflowCount_, totalRecorded_};
    }

private:
    StoreBufferSegment<EdgeRecord> edges_;
    StoreBufferSegment<CellRecord> cells_;
    StoreBufferSegment<SlotRecord> slots_;
    StoreBufferSegment<WholeObjectRecord> objects_;
    std::function<void()> overflowCallback_;
    uint64_t overflowCount_;
    uint64_t totalRecorded_;
    bool enabled_;
};

} // namespace Zepra::Heap

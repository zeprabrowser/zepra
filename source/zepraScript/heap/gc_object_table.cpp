// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_object_table.cpp — Flat array object registry with bitmap free-slot tracking

#include <atomic>
#include <algorithm>
#include <mutex>
#include <vector>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <cstring>

namespace Zepra::Heap {

class ObjectTable {
public:
    struct Entry {
        uintptr_t addr;
        uint32_t sizeBytes;
        uint16_t typeId;
        uint8_t  generation;   // 0 = young, 1 = old
        uint8_t  flags;        // bit0 = marked, bit1 = pinned, bit2 = finalizeable

        bool isAlive() const { return addr != 0; }
        bool isMarked() const { return flags & 0x01; }
        void setMarked() { flags |= 0x01; }
        void clearMarked() { flags &= ~0x01; }
        bool isPinned() const { return flags & 0x02; }
    };

    static constexpr size_t INITIAL_CAPACITY = 80000;
    static constexpr size_t MAX_CAPACITY = 262144;
    static constexpr size_t BITMAP_WORD_BITS = 64;

    explicit ObjectTable(size_t capacity = INITIAL_CAPACITY) { resize(capacity); }

    // O(1) insert — finds first free slot via bitmap.
    uint32_t insert(uintptr_t addr, uint32_t sizeBytes, uint16_t typeId, uint8_t gen) {
        std::lock_guard<std::mutex> lock(mutex_);

        uint32_t slot = findFreeSlot();
        if (slot == UINT32_MAX) return UINT32_MAX;

        entries_[slot].addr = addr;
        entries_[slot].sizeBytes = sizeBytes;
        entries_[slot].typeId = typeId;
        entries_[slot].generation = gen;
        entries_[slot].flags = 0;

        markSlotUsed(slot);
        count_++;
        if (count_ > peakCount_) peakCount_ = count_;
        return slot;
    }

    // O(1) remove by slot index.
    void remove(uint32_t slot) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot >= capacity_ || !entries_[slot].isAlive()) return;

        entries_[slot].addr = 0;
        entries_[slot].sizeBytes = 0;
        entries_[slot].flags = 0;
        markSlotFree(slot);
        count_--;
    }

    // O(1) lookup by slot index.
    const Entry* get(uint32_t slot) const {
        if (slot >= capacity_) return nullptr;
        return entries_[slot].isAlive() ? &entries_[slot] : nullptr;
    }

    Entry* getMutable(uint32_t slot) {
        if (slot >= capacity_) return nullptr;
        return entries_[slot].isAlive() ? &entries_[slot] : nullptr;
    }

    // Grow or shrink the table. Preserves existing entries.
    bool resize(size_t newCapacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (newCapacity < count_) return false;
        newCapacity = std::min(newCapacity, MAX_CAPACITY);

        entries_.resize(newCapacity);
        // Zero-fill new slots.
        for (size_t i = capacity_; i < newCapacity; i++) {
            entries_[i] = {};
        }

        size_t bitmapWords = (newCapacity + BITMAP_WORD_BITS - 1) / BITMAP_WORD_BITS;
        bitmap_.resize(bitmapWords, 0);

        // Rebuild bitmap for new range.
        for (size_t i = capacity_; i < newCapacity; i++) {
            markSlotFree(i);
        }

        capacity_ = newCapacity;
        return true;
    }

    // Iterate all live entries (for marking/sweeping).
    void forEach(std::function<void(uint32_t slot, Entry& entry)> callback) {
        for (size_t i = 0; i < capacity_; i++) {
            if (entries_[i].isAlive()) {
                callback(static_cast<uint32_t>(i), entries_[i]);
            }
        }
    }

    // Clear all marks (pre-mark phase).
    void clearAllMarks() {
        for (size_t i = 0; i < capacity_; i++) {
            entries_[i].clearMarked();
        }
    }

    // Sweep: remove all unmarked live entries, return freed byte count.
    size_t sweep() {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t freed = 0;
        for (size_t i = 0; i < capacity_; i++) {
            if (entries_[i].isAlive() && !entries_[i].isMarked()) {
                freed += entries_[i].sizeBytes;
                entries_[i].addr = 0;
                entries_[i].sizeBytes = 0;
                entries_[i].flags = 0;
                markSlotFree(i);
                count_--;
            }
        }
        return freed;
    }

    size_t count() const { return count_; }
    size_t capacity() const { return capacity_; }
    size_t peakCount() const { return peakCount_; }

    double fillRatio() const {
        return capacity_ > 0 ? static_cast<double>(count_) / capacity_ : 0;
    }

private:
    uint32_t findFreeSlot() const {
        for (size_t w = 0; w < bitmap_.size(); w++) {
            uint64_t word = bitmap_[w];
            // A set bit means free.
            if (word == 0) continue;
            int bit = __builtin_ctzll(word);
            uint32_t slot = static_cast<uint32_t>(w * BITMAP_WORD_BITS + bit);
            if (slot < capacity_) return slot;
        }
        return UINT32_MAX;
    }

    void markSlotUsed(size_t slot) {
        size_t word = slot / BITMAP_WORD_BITS;
        size_t bit = slot % BITMAP_WORD_BITS;
        bitmap_[word] &= ~(1ULL << bit);
    }

    void markSlotFree(size_t slot) {
        size_t word = slot / BITMAP_WORD_BITS;
        size_t bit = slot % BITMAP_WORD_BITS;
        if (word < bitmap_.size()) {
            bitmap_[word] |= (1ULL << bit);
        }
    }

    std::vector<Entry> entries_;
    std::vector<uint64_t> bitmap_;   // 1 = free, 0 = occupied
    size_t capacity_ = 0;
    size_t count_ = 0;
    size_t peakCount_ = 0;
    mutable std::mutex mutex_;
};

} // namespace Zepra::Heap

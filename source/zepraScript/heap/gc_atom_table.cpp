// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_atom_table.cpp — GC-managed atom table with weak entries

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <mutex>
#include <vector>
#include <functional>

namespace Zepra::Heap {

struct AtomSlot {
    const char* data;
    uint32_t length;
    uint32_t hash;
    uint32_t refCount;     // Explicit ref counting for pinned atoms
    bool pinned;
    bool occupied;

    AtomSlot() : data(nullptr), length(0), hash(0), refCount(0)
        , pinned(false), occupied(false) {}

    bool matches(const char* s, uint32_t len, uint32_t h) const {
        if (!occupied || hash != h || length != len) return false;
        return memcmp(data, s, len) == 0;
    }
};

class AtomTable {
public:
    struct Config {
        size_t initialCapacity;
        double loadFactor;         // Rehash when above this
        size_t maxCapacity;

        Config() : initialCapacity(4096), loadFactor(0.75), maxCapacity(1 << 20) {}
    };

    struct Callbacks {
        std::function<char*(size_t size)> allocateString;
        std::function<void(char* data)> freeString;
    };

    explicit AtomTable(const Config& config = Config{})
        : config_(config), count_(0) {
        slots_.resize(config.initialCapacity);
    }

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // Intern a string: return existing atom index or create new one.
    uint32_t intern(const char* data, uint32_t length) {
        std::lock_guard<std::mutex> lock(mutex_);

        uint32_t hash = hashString(data, length);

        // Lookup first.
        uint32_t idx = findSlot(data, length, hash);
        if (idx != UINT32_MAX) {
            slots_[idx].refCount++;
            return idx;
        }

        // Need to insert.
        if (shouldRehash()) rehash();

        return insertNew(data, length, hash);
    }

    // Lookup without interning.
    uint32_t lookup(const char* data, uint32_t length) const {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t hash = hashString(data, length);
        return findSlot(data, length, hash);
    }

    // Pin an atom so it survives GC sweep.
    void pin(uint32_t index) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < slots_.size() && slots_[index].occupied) {
            slots_[index].pinned = true;
        }
    }

    // Remove a specific atom (for GC sweep of unreachable atoms).
    void remove(uint32_t index) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= slots_.size() || !slots_[index].occupied) return;
        if (slots_[index].pinned) return;

        if (cb_.freeString && slots_[index].data) {
            cb_.freeString(const_cast<char*>(slots_[index].data));
        }
        slots_[index] = {};
        count_--;
    }

    // GC sweep: remove all non-pinned, unreachable atoms.
    size_t sweep(std::function<bool(uint32_t index)> isReachable) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t swept = 0;

        for (size_t i = 0; i < slots_.size(); i++) {
            if (!slots_[i].occupied) continue;
            if (slots_[i].pinned) continue;
            if (isReachable && isReachable(static_cast<uint32_t>(i))) continue;

            if (cb_.freeString && slots_[i].data) {
                cb_.freeString(const_cast<char*>(slots_[i].data));
            }
            slots_[i] = {};
            count_--;
            swept++;
        }

        return swept;
    }

    const AtomSlot* slot(uint32_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return index < slots_.size() ? &slots_[index] : nullptr;
    }

    size_t count() const { return count_; }
    size_t capacity() const { return slots_.size(); }

    double loadRatio() const {
        return slots_.size() > 0 ? static_cast<double>(count_) / slots_.size() : 0;
    }

    template<typename Fn>
    void forEach(Fn&& fn) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < slots_.size(); i++) {
            if (slots_[i].occupied) fn(static_cast<uint32_t>(i), slots_[i]);
        }
    }

private:
    static uint32_t hashString(const char* data, uint32_t length) {
        uint32_t hash = 2166136261u;
        for (uint32_t i = 0; i < length; i++) {
            hash ^= static_cast<uint8_t>(data[i]);
            hash *= 16777619u;
        }
        return hash;
    }

    uint32_t findSlot(const char* data, uint32_t length, uint32_t hash) const {
        size_t mask = slots_.size() - 1;
        size_t idx = hash & mask;

        for (size_t probe = 0; probe < slots_.size(); probe++) {
            size_t i = (idx + probe) & mask;
            if (!slots_[i].occupied) return UINT32_MAX;
            if (slots_[i].matches(data, length, hash)) {
                return static_cast<uint32_t>(i);
            }
        }
        return UINT32_MAX;
    }

    uint32_t insertNew(const char* data, uint32_t length, uint32_t hash) {
        size_t mask = slots_.size() - 1;
        size_t idx = hash & mask;

        for (size_t probe = 0; probe < slots_.size(); probe++) {
            size_t i = (idx + probe) & mask;
            if (!slots_[i].occupied) {
                // Copy string data.
                char* copy = nullptr;
                if (cb_.allocateString) {
                    copy = cb_.allocateString(length + 1);
                } else {
                    copy = new char[length + 1];
                }
                memcpy(copy, data, length);
                copy[length] = '\0';

                slots_[i].data = copy;
                slots_[i].length = length;
                slots_[i].hash = hash;
                slots_[i].refCount = 1;
                slots_[i].pinned = false;
                slots_[i].occupied = true;
                count_++;
                return static_cast<uint32_t>(i);
            }
        }

        return UINT32_MAX;  // Table full — should not happen after rehash.
    }

    bool shouldRehash() const {
        return static_cast<double>(count_) / slots_.size() > config_.loadFactor;
    }

    void rehash() {
        size_t newCap = std::min(slots_.size() * 2, config_.maxCapacity);
        if (newCap <= slots_.size()) return;

        std::vector<AtomSlot> oldSlots = std::move(slots_);
        slots_.resize(newCap);
        count_ = 0;

        for (auto& slot : oldSlots) {
            if (slot.occupied) {
                uint32_t idx = insertNew(slot.data, slot.length, slot.hash);
                if (idx != UINT32_MAX) {
                    slots_[idx].pinned = slot.pinned;
                    slots_[idx].refCount = slot.refCount;
                }
            }
        }
    }

    Config config_;
    Callbacks cb_;
    mutable std::mutex mutex_;
    std::vector<AtomSlot> slots_;
    size_t count_;
};

} // namespace Zepra::Heap

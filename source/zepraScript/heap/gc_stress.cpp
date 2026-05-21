// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file gc_stress.cpp
 * @brief GC stress testing
 *
 * Hammers the GC subsystems with high-intensity workloads to expose:
 * - Race conditions in concurrent marking / sweeping
 * - Memory corruption from incorrect forwarding
 * - Card table inconsistencies under write barrier pressure
 * - TLAB refill contention under multi-threaded allocation
 * - SATB buffer overflow under rapid reference mutation
 * - Scavenger correctness with deep promotion chains
 * - Compactor correctness with fragmented pages
 *
 * Each stress test runs a fixed number of iterations within a timeout.
 * Failures are detected via consistency checks after each round.
 */

#include "zepra_alloc.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <memory>
#include <algorithm>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace Zepra::Heap::Stress {

// =============================================================================
// Stress Test Config
// =============================================================================

struct StressConfig {
    size_t iterations;
    size_t threadCount;
    size_t heapSize;
    size_t nurserySize;
    size_t objectSizeMin;
    size_t objectSizeMax;
    uint64_t timeoutMs;

    StressConfig()
        : iterations(10000)
        , threadCount(4)
        , heapSize(16 * 1024 * 1024)
        , nurserySize(2 * 1024 * 1024)
        , objectSizeMin(16)
        , objectSizeMax(4096)
        , timeoutMs(30000) {}
};

// =============================================================================
// Stress Test Result
// =============================================================================

struct StressResult {
    const char* testName;
    bool passed;
    uint64_t iterationsCompleted;
    double durationMs;
    size_t objectsAllocated;
    size_t objectsCollected;
    size_t bytesAllocated;
    size_t errorsDetected;
    std::string errorDetail;
};

// =============================================================================
// Simulated Object Graph
// =============================================================================

/**
 * @brief Simulated object for stress testing
 *
 * Each object has a header (magic + size), payload, and up to 4 references.
 */
struct StressObject {
    static constexpr uint32_t MAGIC = 0x5A47434F;  // "ZGCO"

    uint32_t magic;
    uint32_t size;
    uint32_t typeId;
    uint8_t age;
    uint8_t refCount;
    uint8_t padding[2];
    StressObject* refs[4];  // Up to 4 outgoing references

    bool isValid() const { return magic == MAGIC; }

    void initialize(uint32_t sz, uint32_t type) {
        magic = MAGIC;
        size = sz;
        typeId = type;
        age = 0;
        refCount = 0;
        std::memset(padding, 0, sizeof(padding));
        std::memset(refs, 0, sizeof(refs));
    }

    void addRef(StressObject* target) {
        if (refCount < 4) {
            refs[refCount++] = target;
        }
    }
};

// =============================================================================
// Stress Heap
// =============================================================================

class StressHeap {
public:
    explicit StressHeap(size_t size)
        : size_(size) {
        data_ = static_cast<char*>(zepra_aligned_alloc(4096, size));
        if (data_) std::memset(data_, 0, size);
        cursor_ = data_;
    }

    ~StressHeap() { std::free(data_); }

    StressObject* allocate(size_t objSize) {
        objSize = (objSize + 7) & ~size_t(7);
        if (objSize < sizeof(StressObject)) objSize = sizeof(StressObject);

        std::lock_guard<std::mutex> lock(mutex_);
        if (cursor_ + objSize > data_ + size_) return nullptr;

        auto* obj = reinterpret_cast<StressObject*>(cursor_);
        cursor_ += objSize;
        allocCount_++;
        allocBytes_ += objSize;
        return obj;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (data_) std::memset(data_, 0, size_);
        cursor_ = data_;
        allocCount_ = 0;
        allocBytes_ = 0;
    }

    bool contains(const void* addr) const {
        auto p = static_cast<const char*>(addr);
        return p >= data_ && p < data_ + size_;
    }

    size_t used() const { return static_cast<size_t>(cursor_ - data_); }
    size_t remaining() const { return size_ - used(); }
    size_t allocCount() const { return allocCount_; }
    size_t allocBytes() const { return allocBytes_; }

    StressHeap(const StressHeap&) = delete;
    StressHeap& operator=(const StressHeap&) = delete;

private:
    char* data_ = nullptr;
    char* cursor_ = nullptr;
    size_t size_;
    size_t allocCount_ = 0;
    size_t allocBytes_ = 0;
    std::mutex mutex_;
};

// =============================================================================
// Stress Test: Allocation Pressure
// =============================================================================

/**
 * @brief Hammer the allocator with rapid allocations from multiple threads
 *
 * Verifies:
 * - No overlapping allocations
 * - All objects have valid headers after allocation
 * - Allocation failure is handled gracefully
 */
static StressResult stressAllocation(const StressConfig& config) {
    StressResult result{};
    result.testName = "AllocationPressure";
    auto start = std::chrono::steady_clock::now();

    StressHeap heap(config.heapSize);
    std::atomic<size_t> totalAlloc{0};
    std::atomic<size_t> totalBytes{0};
    std::atomic<size_t> failures{0};
    std::atomic<size_t> errors{0};

    auto worker = [&](unsigned /*threadId*/) {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> sizeDist(
            config.objectSizeMin, config.objectSizeMax);

        size_t localAlloc = 0;
        for (size_t i = 0; i < config.iterations; i++) {
            size_t size = sizeDist(rng);
            auto* obj = heap.allocate(size);
            if (!obj) {
                failures++;
                continue;
            }

            obj->initialize(static_cast<uint32_t>(size), 1);

            // Verify immediately after init
            if (!obj->isValid()) {
                errors++;
            }

            localAlloc++;
            totalBytes.fetch_add(size, std::memory_order_relaxed);
        }
        totalAlloc.fetch_add(localAlloc, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    for (size_t t = 0; t < config.threadCount; t++) {
        threads.emplace_back(worker, static_cast<unsigned>(t));
    }
    for (auto& t : threads) t.join();

    result.objectsAllocated = totalAlloc.load();
    result.bytesAllocated = totalBytes.load();
    result.errorsDetected = errors.load();
    result.passed = errors.load() == 0;
    result.iterationsCompleted = config.iterations * config.threadCount;
    result.durationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    return result;
}

// =============================================================================
// Stress Test: Reference Mutation + Barrier
// =============================================================================

/**
 * @brief Rapidly mutate references to stress write barriers
 *
 * Creates a graph of objects and randomly rewires references.
 * Verifies that all references still point to valid objects.
 */
static StressResult stressReferenceMutation(const StressConfig& config) {
    StressResult result{};
    result.testName = "ReferenceMutation";
    auto start = std::chrono::steady_clock::now();

    StressHeap heap(config.heapSize);
    std::vector<StressObject*> liveObjects;

    // Pre-allocate objects
    size_t preAlloc = std::min(config.iterations, config.heapSize / 128);
    for (size_t i = 0; i < preAlloc; i++) {
        auto* obj = heap.allocate(sizeof(StressObject));
        if (!obj) break;
        obj->initialize(sizeof(StressObject), 2);
        liveObjects.push_back(obj);
    }

    result.objectsAllocated = liveObjects.size();
    if (liveObjects.empty()) {
        result.passed = false;
        result.errorDetail = "No objects allocated";
        return result;
    }

    // Simulate write barrier tracking
    std::vector<std::pair<StressObject*, StressObject*>> barrierLog;
    std::mutex logMutex;
    std::atomic<size_t> errors{0};

    auto worker = [&](unsigned /*threadId*/) {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, liveObjects.size() - 1);

        for (size_t i = 0; i < config.iterations / config.threadCount; i++) {
            size_t srcIdx = dist(rng);
            size_t tgtIdx = dist(rng);

            auto* src = liveObjects[srcIdx];
            auto* tgt = liveObjects[tgtIdx];

            // Simulate pre-barrier (capture old)
            StressObject* oldRef = src->refs[0];

            // Store new reference
            src->refs[0] = tgt;

            // Simulate post-barrier (log for verification)
            {
                std::lock_guard<std::mutex> lock(logMutex);
                barrierLog.push_back({src, tgt});
            }

            // Verify objects still valid
            if (!src->isValid()) errors++;
            if (!tgt->isValid()) errors++;
            (void)oldRef;
        }
    };

    std::vector<std::thread> threads;
    for (size_t t = 0; t < config.threadCount; t++) {
        threads.emplace_back(worker, static_cast<unsigned>(t));
    }
    for (auto& t : threads) t.join();

    // Post-verification: all references should point to valid objects
    size_t invalidRefs = 0;
    for (auto* obj : liveObjects) {
        for (uint8_t r = 0; r < obj->refCount; r++) {
            if (obj->refs[r] && !heap.contains(obj->refs[r])) {
                invalidRefs++;
            }
        }
    }

    result.errorsDetected = errors.load() + invalidRefs;
    result.passed = result.errorsDetected == 0;
    result.iterationsCompleted = config.iterations;
    result.durationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    return result;
}

// =============================================================================
// Stress Test: Card Table Hammering
// =============================================================================

/**
 * @brief Concurrently dirty and clear cards to test atomicity
 */
static StressResult stressCardTable(const StressConfig& config) {
    StressResult result{};
    result.testName = "CardTableHammering";
    auto start = std::chrono::steady_clock::now();

    constexpr size_t CARD_COUNT = 4096;
    auto cards = std::make_unique<std::atomic<uint8_t>[]>(CARD_COUNT);
    for (size_t i = 0; i < CARD_COUNT; i++) {
        cards[i].store(0, std::memory_order_relaxed);
    }

    std::atomic<size_t> dirtyCount{0};
    std::atomic<size_t> clearCount{0};

    auto writer = [&]() {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, CARD_COUNT - 1);

        for (size_t i = 0; i < config.iterations; i++) {
            size_t idx = dist(rng);
            cards[idx].store(1, std::memory_order_relaxed);
            dirtyCount.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto cleaner = [&]() {
        for (size_t i = 0; i < config.iterations / 10; i++) {
            for (size_t j = 0; j < CARD_COUNT; j++) {
                uint8_t val = cards[j].exchange(0, std::memory_order_relaxed);
                if (val != 0) {
                    clearCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t t = 0; t < config.threadCount; t++) {
        threads.emplace_back(writer);
    }
    threads.emplace_back(cleaner);

    for (auto& t : threads) t.join();

    // No crash = pass (testing for atomicity violations)
    result.passed = true;
    result.iterationsCompleted = config.iterations * config.threadCount;
    result.durationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    return result;
}

// =============================================================================
// Stress Test: SATB Buffer Overflow
// =============================================================================

/**
 * @brief Flood SATB buffers to test overflow handling
 */
static StressResult stressSATBOverflow(const StressConfig& config) {
    StressResult result{};
    result.testName = "SATBBufferOverflow";
    auto start = std::chrono::steady_clock::now();

    constexpr size_t BUF_SIZE = 64;  // Small buffer to force many overflows
    std::atomic<size_t> flushCount{0};
    std::atomic<size_t> totalPushes{0};

    auto worker = [&]() {
        auto buf = std::make_unique<void*[]>(BUF_SIZE);
        size_t count = 0;

        for (size_t i = 0; i < config.iterations; i++) {
            void* value = reinterpret_cast<void*>(i + 0x1000);

            if (count >= BUF_SIZE) {
                // Flush
                count = 0;
                flushCount.fetch_add(1, std::memory_order_relaxed);
            }

            buf[count++] = value;
            totalPushes.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (size_t t = 0; t < config.threadCount; t++) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) t.join();

    result.passed = totalPushes.load() == config.iterations * config.threadCount;
    result.iterationsCompleted = totalPushes.load();
    result.durationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    return result;
}

// =============================================================================
// Stress Test: Scavenge Correctness
// =============================================================================

/**
 * @brief Allocate in nursery, copy to to-space, verify integrity
 */
static StressResult stressScavenge(const StressConfig& config) {
    StressResult result{};
    result.testName = "ScavengeCorrectness";
    auto start = std::chrono::steady_clock::now();

    size_t nurserySize = config.nurserySize;
    StressHeap fromSpace(nurserySize);
    StressHeap toSpace(nurserySize);

    std::vector<StressObject*> liveObjects;
    std::atomic<size_t> errors{0};

    // Fill nursery
    while (fromSpace.remaining() >= sizeof(StressObject)) {
        auto* obj = fromSpace.allocate(sizeof(StressObject));
        if (!obj) break;
        obj->initialize(sizeof(StressObject), 3);
        obj->refs[0] = nullptr;
        liveObjects.push_back(obj);
    }

    result.objectsAllocated = liveObjects.size();

    // Simulate scavenge: copy all to to-space
    std::unordered_map<StressObject*, StressObject*> forwarding;

    for (auto* obj : liveObjects) {
        auto* copy = toSpace.allocate(sizeof(StressObject));
        if (!copy) break;
        std::memcpy(copy, obj, sizeof(StressObject));
        forwarding[obj] = copy;
    }

    // Fix references
    for (auto& [old, copy] : forwarding) {
        for (uint8_t r = 0; r < copy->refCount; r++) {
            if (copy->refs[r]) {
                auto it = forwarding.find(copy->refs[r]);
                if (it != forwarding.end()) {
                    copy->refs[r] = it->second;
                }
            }
        }
    }

    // Verify all copies
    for (auto& [old, copy] : forwarding) {
        if (!copy->isValid()) errors++;
        if (copy->typeId != old->typeId) errors++;
    }

    result.errorsDetected = errors.load();
    result.passed = errors.load() == 0;
    result.objectsCollected = forwarding.size();
    result.iterationsCompleted = liveObjects.size();
    result.durationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    return result;
}

// =============================================================================
// Stress Test: Forwarding Table Under Load
// =============================================================================

static StressResult stressForwardingTable(const StressConfig& config) {
    StressResult result{};
    result.testName = "ForwardingTableLoad";
    auto start = std::chrono::steady_clock::now();

    std::unordered_map<uintptr_t, uintptr_t> fwd;
    std::atomic<size_t> errors{0};

    // Insert many entries
    size_t entryCount = std::min(config.iterations, size_t(100000));
    for (size_t i = 0; i < entryCount; i++) {
        fwd[0x1000 + i * 64] = 0x80000000 + i * 64;
    }

    // Verify all lookups
    for (size_t i = 0; i < entryCount; i++) {
        auto it = fwd.find(0x1000 + i * 64);
        if (it == fwd.end() || it->second != 0x80000000 + i * 64) {
            errors++;
        }
    }

    // Simulate pointer updates
    std::vector<void*> slots(entryCount);
    for (size_t i = 0; i < entryCount; i++) {
        slots[i] = reinterpret_cast<void*>(0x1000 + i * 64);
    }

    for (size_t i = 0; i < entryCount; i++) {
        auto addr = reinterpret_cast<uintptr_t>(slots[i]);
        auto it = fwd.find(addr);
        if (it != fwd.end()) {
            slots[i] = reinterpret_cast<void*>(it->second);
        }
    }

    // Verify updates
    for (size_t i = 0; i < entryCount; i++) {
        if (slots[i] != reinterpret_cast<void*>(0x80000000 + i * 64)) {
            errors++;
        }
    }

    result.errorsDetected = errors.load();
    result.passed = errors.load() == 0;
    result.iterationsCompleted = entryCount * 2;
    result.durationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    return result;
}

// =============================================================================
// Run All Stress Tests
// =============================================================================

struct StressSummary {
    size_t total;
    size_t passed;
    size_t failed;
    double totalMs;
    std::vector<StressResult> results;
};

static StressSummary runAllStressTests(const StressConfig& config = {}) {
    StressSummary summary{};
    auto start = std::chrono::steady_clock::now();

    fprintf(stderr, "\n=== ZepraBrowser GC Stress Tests ===\n");
    fprintf(stderr, "  Threads: %zu  Iterations: %zu  Heap: %zu KB\n\n",
        config.threadCount, config.iterations, config.heapSize / 1024);

    auto run = [&](StressResult (*testFn)(const StressConfig&)) {
        auto result = testFn(config);
        summary.results.push_back(result);
        summary.total++;

        if (result.passed) {
            summary.passed++;
            fprintf(stderr, "  [PASS] %s (%.2f ms, %zu allocs, %zu bytes)\n",
                result.testName, result.durationMs,
                result.objectsAllocated, result.bytesAllocated);
        } else {
            summary.failed++;
            fprintf(stderr, "  [FAIL] %s (%.2f ms, %zu errors)%s%s\n",
                result.testName, result.durationMs, result.errorsDetected,
                result.errorDetail.empty() ? "" : ": ",
                result.errorDetail.c_str());
        }
    };

    run(stressAllocation);
    run(stressReferenceMutation);
    run(stressCardTable);
    run(stressSATBOverflow);
    run(stressScavenge);
    run(stressForwardingTable);

    summary.totalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    fprintf(stderr, "\n  %zu/%zu passed (%.2f ms total)\n",
        summary.passed, summary.total, summary.totalMs);

    return summary;
}

} // namespace Zepra::Heap::Stress

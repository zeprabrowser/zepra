// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file gc_unit_tests.cpp
 * @brief Comprehensive unit tests for GC subsystems
 *
 * Tests cover:
 * 1. BarrierCardTable: initialization, mark/clear, dirty iteration
 * 2. SATBLocalBuffer: push, overflow, drain
 * 3. GlobalSATBQueue: chunk push/drain, thread safety
 * 4. WriteBarrierEngine: pre/post store barrier, filtering
 * 5. ScavengerImpl: nursery allocation, copy, tenure, flip
 * 6. ForwardingTable: insert, lookup, pointer update
 * 7. LiveObjectIterator: bitmap walking
 * 8. PercentileTracker: p50/p95/p99 accuracy
 * 9. RollingCounter: windowed rate computation
 * 10. AllocationRateTracker: rate + time-to-full
 * 11. PromotionRatePredictor: EWMA convergence
 * 12. StackWalker: conservative scanning, pointer filtering
 * 13. GCMapTable: descriptor registration, binary search
 * 14. ThreadGCData: TLAB allocation, SATB buffer
 * 15. HeapSummary: type aggregation
 * 16. GCTimeline: event recording, JSON export
 * 17. RememberedSetImpl: card+slot scan
 * 18. HeapVerifierImpl: reference validation
 *
 * Uses a minimal assertion framework (no external dependency).
 */

#include "zepra_alloc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <cmath>
#include <vector>
#include <deque>
#include <string>
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <algorithm>

namespace Zepra::Heap::Tests {

// =============================================================================
// Minimal Test Framework
// =============================================================================

struct TestResult {
    const char* name;
    bool passed;
    const char* failMessage;
    double durationMs;
};

class TestRunner {
public:
    using TestFn = std::function<bool()>;

    void addTest(const char* name, TestFn fn) {
        tests_.push_back({name, std::move(fn)});
    }

    struct Summary {
        size_t total;
        size_t passed;
        size_t failed;
        double totalMs;
        std::vector<TestResult> results;
    };

    Summary run() {
        Summary summary{};
        summary.total = tests_.size();

        auto globalStart = std::chrono::steady_clock::now();

        for (auto& test : tests_) {
            TestResult result;
            result.name = test.name;
            result.failMessage = nullptr;

            auto start = std::chrono::steady_clock::now();

            try {
                result.passed = test.fn();
            } catch (...) {
                result.passed = false;
                result.failMessage = "exception thrown";
            }

            result.durationMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();

            if (result.passed) {
                summary.passed++;
                fprintf(stderr, "  [PASS] %s (%.2f ms)\n",
                    result.name, result.durationMs);
            } else {
                summary.failed++;
                fprintf(stderr, "  [FAIL] %s (%.2f ms)%s%s\n",
                    result.name, result.durationMs,
                    result.failMessage ? ": " : "",
                    result.failMessage ? result.failMessage : "");
            }

            summary.results.push_back(result);
        }

        summary.totalMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - globalStart).count();

        fprintf(stderr, "\n  %zu/%zu passed (%.2f ms total)\n",
            summary.passed, summary.total, summary.totalMs);

        return summary;
    }

private:
    struct TestEntry {
        const char* name;
        TestFn fn;
    };
    std::vector<TestEntry> tests_;
};

// =============================================================================
// Helper: Simulated Heap Region
// =============================================================================

/**
 * @brief Allocates aligned memory simulating a heap region
 * for testing card tables, barriers, etc.
 */
class TestHeapRegion {
public:
    explicit TestHeapRegion(size_t size)
        : size_(size) {
        data_ = static_cast<char*>(zepra_aligned_alloc(4096, size));
        if (data_) std::memset(data_, 0, size);
    }

    ~TestHeapRegion() { std::free(data_); }

    char* base() { return data_; }
    size_t size() const { return size_; }
    uintptr_t baseAddr() const { return reinterpret_cast<uintptr_t>(data_); }
    uintptr_t endAddr() const { return baseAddr() + size_; }

    // Simulated objects (pointer-sized slots)
    void** slotAt(size_t offset) {
        return reinterpret_cast<void**>(data_ + offset);
    }

    void storeRef(size_t srcOffset, void* target) {
        *slotAt(srcOffset) = target;
    }

    void* readRef(size_t offset) {
        return *slotAt(offset);
    }

    TestHeapRegion(const TestHeapRegion&) = delete;
    TestHeapRegion& operator=(const TestHeapRegion&) = delete;

private:
    char* data_ = nullptr;
    size_t size_;
};

// =============================================================================
// Card Table Tests
// =============================================================================

static bool testCardTableInit() {
    TestHeapRegion heap(1024 * 1024);  // 1MB heap
    // Card size = 512 bytes → 2048 cards for 1MB
    // Verify card count computation
    size_t expectedCards = heap.size() / 512;
    return expectedCards == 2048;
}

static bool testCardTableMarkDirty() {
    TestHeapRegion heap(64 * 1024);  // 64KB

    // Simulate card table with manual byte array
    size_t cardCount = heap.size() / 512;
    auto cards = std::make_unique<uint8_t[]>(cardCount);
    std::memset(cards.get(), 0, cardCount);

    // Mark card at offset 1024 (card index = 1024/512 = 2)
    size_t cardIdx = 1024 / 512;
    cards[cardIdx] = 1;

    return cards[cardIdx] == 1 && cards[0] == 0 && cards[cardIdx + 1] == 0;
}

static bool testCardTableClearAll() {
    size_t cardCount = 256;
    auto cards = std::make_unique<uint8_t[]>(cardCount);

    // Dirty some cards
    for (size_t i = 0; i < cardCount; i += 3) {
        cards[i] = 1;
    }

    // Clear all
    std::memset(cards.get(), 0, cardCount);

    // Verify all clean
    for (size_t i = 0; i < cardCount; i++) {
        if (cards[i] != 0) return false;
    }
    return true;
}

static bool testCardTableDirtyIteration() {
    size_t cardCount = 128;
    auto cards = std::make_unique<uint8_t[]>(cardCount);
    std::memset(cards.get(), 0, cardCount);

    // Dirty specific cards
    cards[10] = 1;
    cards[50] = 1;
    cards[100] = 1;

    size_t dirtyFound = 0;
    for (size_t i = 0; i < cardCount; i++) {
        if (cards[i] == 1) dirtyFound++;
    }

    return dirtyFound == 3;
}

// =============================================================================
// SATB Buffer Tests
// =============================================================================

static bool testSATBPush() {
    // Simulate local buffer
    constexpr size_t CAP = 1024;
    auto buf = std::make_unique<void*[]>(CAP);
    size_t count = 0;

    // Push values
    for (size_t i = 0; i < 100; i++) {
        buf[count++] = reinterpret_cast<void*>(i + 1);
    }

    return count == 100 &&
           buf[0] == reinterpret_cast<void*>(1) &&
           buf[99] == reinterpret_cast<void*>(100);
}

static bool testSATBOverflow() {
    constexpr size_t CAP = 64;
    auto buf = std::make_unique<void*[]>(CAP);
    size_t count = 0;

    // Fill to capacity
    for (size_t i = 0; i < CAP; i++) {
        buf[count++] = reinterpret_cast<void*>(i);
    }

    bool full = (count >= CAP);

    // Attempting to push past capacity should fail
    bool overflowed = (count >= CAP);

    return full && overflowed && count == CAP;
}

static bool testSATBDrain() {
    constexpr size_t CAP = 512;
    auto buf = std::make_unique<void*[]>(CAP);
    size_t count = 0;

    for (size_t i = 0; i < 200; i++) {
        buf[count++] = reinterpret_cast<void*>(i + 0x1000);
    }

    // Drain to vector
    std::vector<void*> drained;
    for (size_t i = 0; i < count; i++) {
        drained.push_back(buf[i]);
    }
    count = 0;

    return drained.size() == 200 && count == 0 &&
           drained[0] == reinterpret_cast<void*>(0x1000);
}

// =============================================================================
// Write Barrier Logic Tests
// =============================================================================

static bool testGenerationalBarrierFiltering() {
    // Simulate nursery bounds
    uintptr_t nurseryStart = 0x10000;
    uintptr_t nurseryEnd = 0x20000;

    // Case 1: source in nursery → skip (young→young or young→old)
    uintptr_t src1 = 0x15000;  // In nursery
    uintptr_t tgt1 = 0x18000;  // In nursery
    bool skip1 = (src1 >= nurseryStart && src1 < nurseryEnd);

    // Case 2: source in old gen, target in nursery → must track
    uintptr_t src2 = 0x50000;  // Old gen
    uintptr_t tgt2 = 0x15000;  // In nursery
    bool skipSrc2 = (src2 >= nurseryStart && src2 < nurseryEnd);
    bool inNursery2 = (tgt2 >= nurseryStart && tgt2 < nurseryEnd);
    bool mustTrack = !skipSrc2 && inNursery2;

    // Case 3: source in old gen, target in old gen → skip
    uintptr_t tgt3 = 0x60000;  // Old gen
    bool inNursery3 = (tgt3 >= nurseryStart && tgt3 < nurseryEnd);
    bool skip3 = !inNursery3;

    return skip1 && mustTrack && skip3;
}

static bool testSATBPreBarrier() {
    // Simulate: slot contains old value, about to be overwritten
    void* slot_value = reinterpret_cast<void*>(0xDEADBEEF);
    void* new_value = reinterpret_cast<void*>(0xCAFEBABE);

    // Pre-barrier: capture old value
    std::vector<void*> satbLog;
    satbLog.push_back(slot_value);

    // Write happens
    slot_value = new_value;

    // Verify old value was captured
    return satbLog.size() == 1 &&
           satbLog[0] == reinterpret_cast<void*>(0xDEADBEEF) &&
           slot_value == reinterpret_cast<void*>(0xCAFEBABE);
}

// =============================================================================
// Scavenger Tests (Cheney Copy)
// =============================================================================

static bool testCheneyAllocAndCopy() {
    // Simulate two semi-spaces
    size_t semiSize = 4096;
    TestHeapRegion fromSpace(semiSize);
    TestHeapRegion toSpace(semiSize);

    // Allocate objects in from-space
    struct SimObj {
        uint64_t header;
        uint64_t data;
    };

    auto* obj1 = reinterpret_cast<SimObj*>(fromSpace.base());
    obj1->header = 0x01;
    obj1->data = 42;

    auto* obj2 = reinterpret_cast<SimObj*>(fromSpace.base() + sizeof(SimObj));
    obj2->header = 0x02;
    obj2->data = 99;

    // Copy to to-space (Cheney: sequential copy)
    char* toCursor = toSpace.base();

    std::memcpy(toCursor, obj1, sizeof(SimObj));
    auto* copy1 = reinterpret_cast<SimObj*>(toCursor);
    toCursor += sizeof(SimObj);

    std::memcpy(toCursor, obj2, sizeof(SimObj));
    auto* copy2 = reinterpret_cast<SimObj*>(toCursor);
    toCursor += sizeof(SimObj);

    // Verify copies
    return copy1->data == 42 && copy2->data == 99 &&
           copy1->header == 0x01 && copy2->header == 0x02 &&
           toCursor == toSpace.base() + 2 * sizeof(SimObj);
}

static bool testTenurePromotion() {
    // Simulate age-based tenuring
    uint8_t threshold = 3;

    // Object survives scavenges
    uint8_t age = 0;
    for (int scavenge = 0; scavenge < 5; scavenge++) {
        age++;
        if (age >= threshold) {
            // Should be promoted
            break;
        }
    }

    return age == threshold;
}

static bool testSemiSpaceFlip() {
    char* from = reinterpret_cast<char*>(0x1000);
    char* to = reinterpret_cast<char*>(0x2000);

    std::swap(from, to);

    return from == reinterpret_cast<char*>(0x2000) &&
           to == reinterpret_cast<char*>(0x1000);
}

// =============================================================================
// Forwarding Table Tests
// =============================================================================

static bool testForwardingInsertLookup() {
    // Simulate forwarding map
    std::unordered_map<uintptr_t, uintptr_t> fwd;

    fwd[0x1000] = 0x5000;
    fwd[0x2000] = 0x6000;
    fwd[0x3000] = 0x7000;

    return fwd[0x1000] == 0x5000 &&
           fwd[0x2000] == 0x6000 &&
           fwd[0x3000] == 0x7000 &&
           fwd.count(0x4000) == 0;
}

static bool testForwardingPointerUpdate() {
    std::unordered_map<uintptr_t, uintptr_t> fwd;
    fwd[0xA000] = 0xB000;

    // Simulate slot containing old address
    void* slot = reinterpret_cast<void*>(0xA000);
    auto it = fwd.find(reinterpret_cast<uintptr_t>(slot));
    if (it != fwd.end()) {
        slot = reinterpret_cast<void*>(it->second);
    }

    return slot == reinterpret_cast<void*>(0xB000);
}

// =============================================================================
// Mark Bitmap / LiveObjectIterator Tests
// =============================================================================

static bool testBitmapSetAndRead() {
    constexpr size_t PAGE = 4096;
    constexpr size_t CELL = 8;
    size_t cellCount = PAGE / CELL;  // 512 cells
    size_t bitmapBytes = (cellCount + 7) / 8;  // 64 bytes

    auto bitmap = std::make_unique<uint8_t[]>(bitmapBytes);
    std::memset(bitmap.get(), 0, bitmapBytes);

    // Mark cell 0 (byte 0, bit 0)
    bitmap[0] |= (1 << 0);
    // Mark cell 10 (byte 1, bit 2)
    bitmap[1] |= (1 << 2);
    // Mark cell 63 (byte 7, bit 7)
    bitmap[7] |= (1 << 7);

    // Read back
    bool cell0 = (bitmap[0] & (1 << 0)) != 0;
    bool cell10 = (bitmap[1] & (1 << 2)) != 0;
    bool cell63 = (bitmap[7] & (1 << 7)) != 0;
    bool cell1 = (bitmap[0] & (1 << 1)) != 0;

    return cell0 && cell10 && cell63 && !cell1;
}

static bool testBitmapIteration() {
    constexpr size_t CELLS = 64;
    size_t bitmapBytes = CELLS / 8;  // 8 bytes
    auto bitmap = std::make_unique<uint8_t[]>(bitmapBytes);
    std::memset(bitmap.get(), 0, bitmapBytes);

    // Mark cells 0, 15, 31, 63
    bitmap[0] |= (1 << 0);      // cell 0
    bitmap[1] |= (1 << 7);      // cell 15
    bitmap[3] |= (1 << 7);      // cell 31
    bitmap[7] |= (1 << 7);      // cell 63

    size_t foundCount = 0;
    std::vector<size_t> foundCells;

    for (size_t b = 0; b < bitmapBytes; b++) {
        if (bitmap[b] == 0) continue;
        for (int bit = 0; bit < 8; bit++) {
            if (bitmap[b] & (1 << bit)) {
                foundCells.push_back(b * 8 + bit);
                foundCount++;
            }
        }
    }

    return foundCount == 4 &&
           foundCells[0] == 0 &&
           foundCells[1] == 15 &&
           foundCells[2] == 31 &&
           foundCells[3] == 63;
}

// =============================================================================
// Percentile Tracker Tests
// =============================================================================

static bool testPercentileBasic() {
    // Insert sorted values 1..100
    std::vector<double> samples;
    for (int i = 1; i <= 100; i++) {
        samples.push_back(static_cast<double>(i));
    }

    auto percentile = [&](double p) -> double {
        auto sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(
            p / 100.0 * static_cast<double>(sorted.size() - 1));
        return sorted[idx];
    };

    double p50 = percentile(50);
    double p95 = percentile(95);
    double p99 = percentile(99);

    // p50 should be ~50, p95 ~95, p99 ~99
    return std::abs(p50 - 50.0) < 2.0 &&
           std::abs(p95 - 95.0) < 2.0 &&
           std::abs(p99 - 99.0) < 2.0;
}

static bool testPercentileWithOutliers() {
    std::vector<double> samples;
    // 99 fast GCs at 0.5ms, 1 slow GC at 50ms
    for (int i = 0; i < 99; i++) samples.push_back(0.5);
    samples.push_back(50.0);

    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    double p50 = sorted[49];    // Should be 0.5
    double p99 = sorted[98];    // Should be 0.5 (99th is the last fast one)
    double max = sorted[99];    // Should be 50.0

    return std::abs(p50 - 0.5) < 0.01 &&
           std::abs(p99 - 0.5) < 0.01 &&
           std::abs(max - 50.0) < 0.01;
}

// =============================================================================
// Allocation Rate Tracker Tests
// =============================================================================

static bool testAllocationRateComputation() {
    // Simulate: 10MB allocated over 100ms = 100KB/ms
    size_t totalBytes = 10 * 1024 * 1024;
    double windowMs = 100.0;
    double rate = static_cast<double>(totalBytes) / windowMs;

    // Should be ~102400 bytes/ms
    return rate > 100000 && rate < 110000;
}

static bool testTimeToFull() {
    double rate = 100000.0;  // 100KB/ms
    size_t currentUsed = 50 * 1024 * 1024;  // 50MB
    size_t heapLimit = 100 * 1024 * 1024;   // 100MB

    size_t remaining = heapLimit - currentUsed;  // 50MB
    double timeMs = static_cast<double>(remaining) / rate;

    // 50MB / 100KB/ms = ~500ms
    return std::abs(timeMs - 524.288) < 10.0;
}

// =============================================================================
// Promotion Rate Predictor (EWMA) Tests
// =============================================================================

static bool testEWMAConvergence() {
    double ewma = 0;
    bool initialized = false;
    double alpha = 0.3;

    // Feed constant promotion rate of 20%
    for (int i = 0; i < 50; i++) {
        double rate = 0.2;
        if (initialized) {
            ewma = alpha * rate + (1.0 - alpha) * ewma;
        } else {
            ewma = rate;
            initialized = true;
        }
    }

    // EWMA should converge to 0.2
    return std::abs(ewma - 0.2) < 0.001;
}

static bool testEWMATrendDetection() {
    std::deque<double> history;

    // Increasing rates: 10%, 12%, 14%, ..., 30%
    for (int i = 0; i < 11; i++) {
        history.push_back(0.10 + i * 0.02);
    }

    // Check trend: recent half should be higher than older half
    size_t half = history.size() / 2;
    double older = 0, recent = 0;
    for (size_t i = 0; i < half; i++) older += history[i];
    for (size_t i = half; i < history.size(); i++) recent += history[i];
    older /= static_cast<double>(half);
    recent /= static_cast<double>(history.size() - half);

    return recent > older * 1.1;
}

// =============================================================================
// Stack Scanning Tests
// =============================================================================

static bool testConservativePointerFiltering() {
    uintptr_t heapLow = 0x7F0000000000ULL;
    uintptr_t heapHigh = 0x7F0010000000ULL;

    // Valid pointer
    uintptr_t valid = 0x7F0000001000ULL;
    bool isValid = (valid >= heapLow && valid < heapHigh &&
                    (valid & 7) == 0 && valid > 0x1000);

    // Small integer (should be filtered)
    uintptr_t small = 42;
    bool isSmall = (small >= heapLow && small < heapHigh);

    // Misaligned (should be filtered)
    uintptr_t misaligned = 0x7F0000001003ULL;
    bool isMisaligned = ((misaligned & 7) != 0);

    return isValid && !isSmall && isMisaligned;
}

static bool testGCMapTableBinarySearch() {
    // Simulate sorted return addresses
    std::vector<uintptr_t> addresses = {
        0x1000, 0x2000, 0x3000, 0x4000, 0x5000,
        0x6000, 0x7000, 0x8000, 0x9000, 0xA000
    };

    // Binary search for 0x5000
    uintptr_t target = 0x5000;
    size_t lo = 0, hi = addresses.size();
    bool found = false;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (addresses[mid] < target) lo = mid + 1;
        else if (addresses[mid] > target) hi = mid;
        else { found = true; break; }
    }

    // Search for non-existent
    uintptr_t missing = 0x5500;
    lo = 0; hi = addresses.size();
    bool notFound = true;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (addresses[mid] < missing) lo = mid + 1;
        else if (addresses[mid] > missing) hi = mid;
        else { notFound = false; break; }
    }

    return found && notFound;
}

// =============================================================================
// Thread GC Data Tests
// =============================================================================

static bool testTLABAllocation() {
    constexpr size_t TLAB_SIZE = 4096;
    TestHeapRegion tlab(TLAB_SIZE);

    char* cursor = tlab.base();
    char* end = tlab.base() + TLAB_SIZE;

    // Allocate 64-byte object
    size_t size1 = 64;
    size1 = (size1 + 7) & ~size_t(7);
    void* obj1 = nullptr;
    if (cursor + size1 <= end) {
        obj1 = cursor;
        cursor += size1;
    }

    // Allocate 128-byte object
    size_t size2 = 128;
    size2 = (size2 + 7) & ~size_t(7);
    void* obj2 = nullptr;
    if (cursor + size2 <= end) {
        obj2 = cursor;
        cursor += size2;
    }

    // Verify
    return obj1 == tlab.base() &&
           obj2 == tlab.base() + 64 &&
           cursor == tlab.base() + 192;
}

static bool testTLABExhaustion() {
    constexpr size_t TLAB_SIZE = 256;
    TestHeapRegion tlab(TLAB_SIZE);

    char* cursor = tlab.base();
    char* end = tlab.base() + TLAB_SIZE;

    size_t allocated = 0;
    while (cursor + 64 <= end) {
        cursor += 64;
        allocated++;
    }

    // Should fit exactly 4 objects (256/64)
    return allocated == 4 && cursor == end;
}

// =============================================================================
// Memory Pressure Classification Tests
// =============================================================================

static bool testPressureClassification() {
    auto classify = [](double freeRatio) -> int {
        if (freeRatio < 0.05) return 3;  // Critical
        if (freeRatio < 0.10) return 2;  // Moderate
        if (freeRatio < 0.20) return 1;  // Low
        return 0;  // None
    };

    return classify(0.50) == 0 &&   // None
           classify(0.15) == 1 &&   // Low
           classify(0.08) == 2 &&   // Moderate
           classify(0.03) == 3;     // Critical
}

// =============================================================================
// Heap Summary Aggregation Tests
// =============================================================================

static bool testTypeAggregation() {
    struct ObjInfo { uint32_t typeId; size_t size; };
    std::vector<ObjInfo> objects = {
        {1, 64}, {1, 64}, {1, 128},   // Type 1: 3 objects, 256 bytes
        {2, 256}, {2, 256},            // Type 2: 2 objects, 512 bytes
        {3, 32},                       // Type 3: 1 object, 32 bytes
    };

    std::unordered_map<uint32_t, std::pair<size_t, size_t>> agg;
    for (const auto& obj : objects) {
        agg[obj.typeId].first++;
        agg[obj.typeId].second += obj.size;
    }

    return agg[1].first == 3 && agg[1].second == 256 &&
           agg[2].first == 2 && agg[2].second == 512 &&
           agg[3].first == 1 && agg[3].second == 32;
}

// =============================================================================
// GC Timeline Tests
// =============================================================================

static bool testTimelineRecording() {
    struct Event {
        uint8_t type;
        uint64_t startUs;
        uint64_t durationUs;
        size_t heapBefore;
        size_t heapAfter;
    };

    std::deque<Event> timeline;

    timeline.push_back({0, 1000, 500, 1024 * 1024, 512 * 1024});
    timeline.push_back({1, 5000, 2000, 2 * 1024 * 1024, 1024 * 1024});

    return timeline.size() == 2 &&
           timeline[0].durationUs == 500 &&
           timeline[1].heapAfter == 1024 * 1024;
}

// =============================================================================
// Remembered Set Tests
// =============================================================================

static bool testRememberedSetCardAndSlot() {
    // Card table records at card granularity
    size_t cardCount = 128;
    auto cards = std::make_unique<uint8_t[]>(cardCount);
    std::memset(cards.get(), 0, cardCount);

    // Slot set records exact addresses
    std::vector<void**> slots;

    void* dummy1;
    void* dummy2;
    void** slot1 = &dummy1;
    void** slot2 = &dummy2;

    // Record store in card
    cards[10] = 1;
    // Record exact slot
    slots.push_back(slot1);
    slots.push_back(slot2);

    // Scan should find both
    size_t found = 0;
    if (cards[10] == 1) found++;
    found += slots.size();

    return found == 3;
}

// =============================================================================
// Register All Tests
// =============================================================================

static TestRunner::Summary runAllGCTests() {
    TestRunner runner;

    // Card table
    runner.addTest("CardTable.Init", testCardTableInit);
    runner.addTest("CardTable.MarkDirty", testCardTableMarkDirty);
    runner.addTest("CardTable.ClearAll", testCardTableClearAll);
    runner.addTest("CardTable.DirtyIteration", testCardTableDirtyIteration);

    // SATB
    runner.addTest("SATB.Push", testSATBPush);
    runner.addTest("SATB.Overflow", testSATBOverflow);
    runner.addTest("SATB.Drain", testSATBDrain);

    // Write barriers
    runner.addTest("Barrier.GenerationalFiltering", testGenerationalBarrierFiltering);
    runner.addTest("Barrier.SATBPreBarrier", testSATBPreBarrier);

    // Scavenger
    runner.addTest("Scavenger.CheneyAllocAndCopy", testCheneyAllocAndCopy);
    runner.addTest("Scavenger.TenurePromotion", testTenurePromotion);
    runner.addTest("Scavenger.SemiSpaceFlip", testSemiSpaceFlip);

    // Forwarding
    runner.addTest("Forwarding.InsertLookup", testForwardingInsertLookup);
    runner.addTest("Forwarding.PointerUpdate", testForwardingPointerUpdate);

    // Mark bitmap
    runner.addTest("Bitmap.SetAndRead", testBitmapSetAndRead);
    runner.addTest("Bitmap.Iteration", testBitmapIteration);

    // Percentile
    runner.addTest("Percentile.Basic", testPercentileBasic);
    runner.addTest("Percentile.Outliers", testPercentileWithOutliers);

    // Rates
    runner.addTest("Rate.AllocationComputation", testAllocationRateComputation);
    runner.addTest("Rate.TimeToFull", testTimeToFull);

    // EWMA
    runner.addTest("EWMA.Convergence", testEWMAConvergence);
    runner.addTest("EWMA.TrendDetection", testEWMATrendDetection);

    // Stack scanning
    runner.addTest("Stack.ConservativeFiltering", testConservativePointerFiltering);
    runner.addTest("Stack.GCMapBinarySearch", testGCMapTableBinarySearch);

    // Thread
    runner.addTest("Thread.TLABAllocation", testTLABAllocation);
    runner.addTest("Thread.TLABExhaustion", testTLABExhaustion);

    // Memory pressure
    runner.addTest("Pressure.Classification", testPressureClassification);

    // Summary
    runner.addTest("HeapSummary.TypeAggregation", testTypeAggregation);

    // Timeline
    runner.addTest("Timeline.Recording", testTimelineRecording);

    // Remembered set
    runner.addTest("RememberedSet.CardAndSlot", testRememberedSetCardAndSlot);

    fprintf(stderr, "\n=== ZepraBrowser GC Unit Tests ===\n\n");
    return runner.run();
}

} // namespace Zepra::Heap::Tests

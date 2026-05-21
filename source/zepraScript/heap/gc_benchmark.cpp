// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file gc_benchmark.cpp
 * @brief GC benchmark suite
 *
 * Measures throughput and latency of GC subsystems:
 * 1. Allocation throughput (bytes/sec, objects/sec)
 * 2. TLAB allocation fast path latency (ns/op)
 * 3. Card table mark/scan throughput
 * 4. SATB buffer push throughput
 * 5. Scavenger pause time vs nursery size
 * 6. Forwarding table lookup latency
 * 7. Mark bitmap iteration throughput
 * 8. Conservative stack scan throughput
 * 9. Object copy throughput (memcpy at various sizes)
 * 10. Percentile tracker insert throughput
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

namespace Zepra::Heap::Benchmark {

// =============================================================================
// Benchmark Framework
// =============================================================================

struct BenchResult {
    const char* name;
    uint64_t iterations;
    double totalMs;
    double opsPerSec;
    double nsPerOp;
    double bytesPerSec;
    size_t bytesProcessed;
};

class BenchRunner {
public:
    using BenchFn = std::function<BenchResult()>;

    void add(const char* name, BenchFn fn) {
        benches_.push_back({name, std::move(fn)});
    }

    struct Summary {
        std::vector<BenchResult> results;
        double totalMs;
    };

    Summary run() {
        Summary summary{};
        auto start = std::chrono::steady_clock::now();

        fprintf(stderr, "\n=== ZepraBrowser GC Benchmarks ===\n\n");
        fprintf(stderr, "%-35s %12s %12s %14s\n",
            "Benchmark", "ops/sec", "ns/op", "throughput");
        fprintf(stderr, "%-35s %12s %12s %14s\n",
            "-----------------------------------",
            "------------", "------------", "--------------");

        for (auto& bench : benches_) {
            auto result = bench.fn();
            result.name = bench.name;
            summary.results.push_back(result);

            char throughput[32] = "";
            if (result.bytesPerSec > 0) {
                if (result.bytesPerSec > 1e9) {
                    snprintf(throughput, sizeof(throughput), "%.2f GB/s",
                        result.bytesPerSec / 1e9);
                } else {
                    snprintf(throughput, sizeof(throughput), "%.2f MB/s",
                        result.bytesPerSec / 1e6);
                }
            }

            fprintf(stderr, "%-35s %12.0f %12.1f %14s\n",
                result.name, result.opsPerSec, result.nsPerOp, throughput);
        }

        summary.totalMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();

        fprintf(stderr, "\nTotal: %.2f ms\n", summary.totalMs);
        return summary;
    }

private:
    struct BenchEntry {
        const char* name;
        BenchFn fn;
    };
    std::vector<BenchEntry> benches_;
};

// Helper: compute benchmark stats
static BenchResult makeResult(uint64_t iters, double ms,
                               size_t bytes = 0) {
    BenchResult r{};
    r.iterations = iters;
    r.totalMs = ms;
    r.opsPerSec = ms > 0 ? static_cast<double>(iters) / (ms / 1000.0) : 0;
    r.nsPerOp = iters > 0 ? (ms * 1e6) / static_cast<double>(iters) : 0;
    r.bytesProcessed = bytes;
    r.bytesPerSec = ms > 0 ? static_cast<double>(bytes) / (ms / 1000.0) : 0;
    return r;
}

// =============================================================================
// Benchmark: Bump Allocation (TLAB Fast Path)
// =============================================================================

static BenchResult benchTLABAllocation() {
    constexpr size_t SIZE = 64 * 1024 * 1024;  // 64MB
    auto heap = static_cast<char*>(zepra_aligned_alloc(4096, SIZE));
    if (!heap) return makeResult(0, 0);

    char* cursor = heap;
    char* end = heap + SIZE;
    constexpr size_t OBJ_SIZE = 64;

    auto start = std::chrono::steady_clock::now();

    uint64_t count = 0;
    while (cursor + OBJ_SIZE <= end) {
        // Bump pointer allocation (what TLAB does)
        cursor += OBJ_SIZE;
        count++;
    }

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    std::free(heap);
    return makeResult(count, ms, count * OBJ_SIZE);
}

// =============================================================================
// Benchmark: Card Table Mark
// =============================================================================

static BenchResult benchCardTableMark() {
    constexpr size_t CARDS = 1024 * 1024;  // 1M cards = 512MB heap
    auto cards = std::make_unique<uint8_t[]>(CARDS);
    std::memset(cards.get(), 0, CARDS);

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist(0, CARDS - 1);

    constexpr uint64_t ITERS = 10000000;
    auto start = std::chrono::steady_clock::now();

    for (uint64_t i = 0; i < ITERS; i++) {
        size_t idx = dist(rng);
        cards[idx] = 1;  // Single byte write (what the barrier does)
    }

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    return makeResult(ITERS, ms);
}

// =============================================================================
// Benchmark: Card Table Scan (Dirty Card Iteration)
// =============================================================================

static BenchResult benchCardTableScan() {
    constexpr size_t CARDS = 1024 * 1024;
    auto cards = std::make_unique<uint8_t[]>(CARDS);
    std::memset(cards.get(), 0, CARDS);

    // Dirty 1% of cards
    std::mt19937 rng(42);
    for (size_t i = 0; i < CARDS / 100; i++) {
        cards[rng() % CARDS] = 1;
    }

    constexpr uint64_t ITERS = 100;
    auto start = std::chrono::steady_clock::now();

    uint64_t dirtyFound = 0;
    for (uint64_t round = 0; round < ITERS; round++) {
        for (size_t i = 0; i < CARDS; i++) {
            if (cards[i] != 0) dirtyFound++;
        }
    }

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    return makeResult(ITERS, ms, CARDS * ITERS);
}

// =============================================================================
// Benchmark: SATB Buffer Push
// =============================================================================

static BenchResult benchSATBPush() {
    constexpr size_t BUF_SIZE = 1024;
    auto buf = std::make_unique<void*[]>(BUF_SIZE);
    size_t count = 0;
    uint64_t flushes = 0;

    constexpr uint64_t ITERS = 10000000;
    auto start = std::chrono::steady_clock::now();

    for (uint64_t i = 0; i < ITERS; i++) {
        if (count >= BUF_SIZE) {
            count = 0;
            flushes++;
        }
        buf[count++] = reinterpret_cast<void*>(i);
    }

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    return makeResult(ITERS, ms);
}

// =============================================================================
// Benchmark: Object Copy (memcpy throughput at different sizes)
// =============================================================================

static BenchResult benchObjectCopy() {
    constexpr size_t BUF_SIZE = 64 * 1024 * 1024;  // 64MB
    auto src = static_cast<char*>(zepra_aligned_alloc(4096, BUF_SIZE));
    auto dst = static_cast<char*>(zepra_aligned_alloc(4096, BUF_SIZE));
    if (!src || !dst) {
        std::free(src);
        std::free(dst);
        return makeResult(0, 0);
    }

    std::memset(src, 0xAB, BUF_SIZE);

    // Copy objects of varying sizes (simulates scavenger copy loop)
    size_t sizes[] = {32, 64, 128, 256, 512, 1024};
    size_t totalBytes = 0;
    uint64_t totalOps = 0;

    auto start = std::chrono::steady_clock::now();

    for (size_t sizeIdx = 0; sizeIdx < 6; sizeIdx++) {
        size_t objSize = sizes[sizeIdx];
        size_t opsPerSize = BUF_SIZE / objSize;

        for (size_t i = 0; i < opsPerSize; i++) {
            size_t offset = (i * objSize) % (BUF_SIZE - objSize);
            std::memcpy(dst + offset, src + offset, objSize);
            totalBytes += objSize;
            totalOps++;
        }
    }

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    std::free(src);
    std::free(dst);
    return makeResult(totalOps, ms, totalBytes);
}

// =============================================================================
// Benchmark: Forwarding Table Lookup
// =============================================================================

static BenchResult benchForwardingLookup() {
    std::unordered_map<uintptr_t, uintptr_t> fwd;

    // Populate with 100K entries
    constexpr size_t ENTRIES = 100000;
    for (size_t i = 0; i < ENTRIES; i++) {
        fwd[0x1000 + i * 64] = 0x80000000 + i * 64;
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist(0, ENTRIES - 1);

    constexpr uint64_t ITERS = 5000000;
    auto start = std::chrono::steady_clock::now();

    uint64_t found = 0;
    for (uint64_t i = 0; i < ITERS; i++) {
        size_t idx = dist(rng);
        auto it = fwd.find(0x1000 + idx * 64);
        if (it != fwd.end()) found++;
    }

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    (void)found;
    return makeResult(ITERS, ms);
}

// =============================================================================
// Benchmark: Mark Bitmap Iteration
// =============================================================================

static BenchResult benchBitmapIteration() {
    // 256KB page → 32768 cells → 4096 bitmap bytes
    constexpr size_t BITMAP_BYTES = 4096;
    auto bitmap = std::make_unique<uint8_t[]>(BITMAP_BYTES);

    // Set ~10% of bits
    std::mt19937 rng(42);
    for (size_t i = 0; i < BITMAP_BYTES; i++) {
        bitmap[i] = (rng() % 10 == 0) ? static_cast<uint8_t>(rng() & 0xFF) : 0;
    }

    constexpr uint64_t ITERS = 10000;
    auto start = std::chrono::steady_clock::now();

    uint64_t liveCells = 0;
    for (uint64_t round = 0; round < ITERS; round++) {
        for (size_t b = 0; b < BITMAP_BYTES; b++) {
            uint8_t byte = bitmap[b];
            if (byte == 0) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (byte & (1 << bit)) liveCells++;
            }
        }
    }

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    (void)liveCells;
    return makeResult(ITERS, ms, BITMAP_BYTES * ITERS);
}

// =============================================================================
// Benchmark: Conservative Stack Scan
// =============================================================================

static BenchResult benchConservativeScan() {
    // Simulate a 1MB stack
    constexpr size_t STACK_SIZE = 1024 * 1024;
    auto stack = std::make_unique<uintptr_t[]>(STACK_SIZE / sizeof(uintptr_t));

    // Fill with mix of valid-looking and invalid values
    std::mt19937 rng(42);
    uintptr_t heapLow = 0x7F0000000000ULL;
    uintptr_t heapHigh = 0x7F0010000000ULL;

    for (size_t i = 0; i < STACK_SIZE / sizeof(uintptr_t); i++) {
        if (rng() % 10 == 0) {
            // Heap-looking pointer
            stack[i] = heapLow + (rng() % (heapHigh - heapLow));
            stack[i] &= ~uintptr_t(7);  // Align to 8
        } else {
            stack[i] = rng();  // Random junk
        }
    }

    constexpr uint64_t ITERS = 100;
    auto start = std::chrono::steady_clock::now();

    uint64_t candidates = 0;
    size_t slotCount = STACK_SIZE / sizeof(uintptr_t);

    for (uint64_t round = 0; round < ITERS; round++) {
        for (size_t i = 0; i < slotCount; i++) {
            uintptr_t val = stack[i];
            if (val >= heapLow && val < heapHigh &&
                (val & 7) == 0 && val > 0x1000) {
                candidates++;
            }
        }
    }

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    (void)candidates;
    return makeResult(ITERS, ms, STACK_SIZE * ITERS);
}

// =============================================================================
// Benchmark: Percentile Tracker Insert
// =============================================================================

static BenchResult benchPercentileInsert() {
    std::vector<double> samples;
    samples.reserve(100000);

    constexpr uint64_t ITERS = 100000;
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(0.1, 50.0);

    auto start = std::chrono::steady_clock::now();

    for (uint64_t i = 0; i < ITERS; i++) {
        samples.push_back(dist(rng));
    }

    // Compute percentiles (sort required)
    std::sort(samples.begin(), samples.end());
    double p50 = samples[ITERS / 2];
    double p99 = samples[ITERS * 99 / 100];

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    (void)p50;
    (void)p99;
    return makeResult(ITERS, ms);
}

// =============================================================================
// Run All Benchmarks
// =============================================================================

static BenchRunner::Summary runAllBenchmarks() {
    BenchRunner runner;

    runner.add("TLAB BumpAllocation (64B)", benchTLABAllocation);
    runner.add("CardTable Mark (random)", benchCardTableMark);
    runner.add("CardTable Scan (1% dirty)", benchCardTableScan);
    runner.add("SATB Buffer Push", benchSATBPush);
    runner.add("Object Copy (32-1024B)", benchObjectCopy);
    runner.add("ForwardingTable Lookup (100K)", benchForwardingLookup);
    runner.add("MarkBitmap Iteration", benchBitmapIteration);
    runner.add("Conservative Stack Scan (1MB)", benchConservativeScan);
    runner.add("Percentile Insert+Sort (100K)", benchPercentileInsert);

    return runner.run();
}

} // namespace Zepra::Heap::Benchmark

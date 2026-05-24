// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file thread_manager.cpp
 * @brief Thread registry, lifecycle management, and GC coordination
 *
 * Every thread that allocates JS objects must register here.
 * The thread manager provides:
 * - Thread registration/deregistration (with TLS linkage)
 * - Per-thread GC state (TLAB, SATB buffer, safe-point flag)
 * - Thread enumeration for root scanning
 * - Stack bounds tracking for conservative scanning
 * - Thread suspension via safe-point polling
 * - Mutator/GC role tracking
 */

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cassert>
#include <memory>
#include <algorithm>

#ifdef __linux__
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#include <sys/syscall.h>
#include <pthread.h>
#include <signal.h>
#endif

namespace Zepra::Heap {

// =============================================================================
// Thread State
// =============================================================================

enum class ThreadState : uint8_t {
    Running,        // Executing JS / mutator code
    InNative,       // Executing native (C++) code — GC-safe
    Blocked,        // Blocked on I/O, lock, etc. — GC-safe
    AtSafePoint,    // Stopped at GC safe-point
    Suspended,      // Suspended by GC
    Terminated,     // Thread finished
};

static const char* threadStateName(ThreadState s) {
    switch (s) {
        case ThreadState::Running: return "Running";
        case ThreadState::InNative: return "InNative";
        case ThreadState::Blocked: return "Blocked";
        case ThreadState::AtSafePoint: return "AtSafePoint";
        case ThreadState::Suspended: return "Suspended";
        case ThreadState::Terminated: return "Terminated";
        default: return "Unknown";
    }
}

// =============================================================================
// Per-Thread GC Data
// =============================================================================

/**
 * @brief Per-thread allocation and GC state
 *
 * Stored in TLS. Each thread has one of these.
 * The GC reads this during root scanning and safe-point sync.
 */
struct ThreadGCData {
    // Thread identity
    uint64_t threadId;
    std::thread::id stdThreadId;
    const char* name;

    // State
    std::atomic<ThreadState> state{ThreadState::Running};

    // Safe-point polling flag
    // The GC sets this to true; mutator checks at allocation / back-edge
    std::atomic<bool> safePointRequested{false};

    // Acknowledged safe-point (mutator sets this when parked)
    std::atomic<bool> atSafePoint{false};

    // Stack bounds — needed for conservative root scanning
    void* stackBase;        // Top of stack (highest address on x86)
    void* stackLimit;       // Bottom of stack (lowest address)

    // TLAB (Thread-Local Allocation Buffer) pointers
    char* tlabStart;
    char* tlabCursor;
    char* tlabEnd;

    // SATB buffer for concurrent marking
    static constexpr size_t SATB_BUFFER_SIZE = 512;
    void* satbBuffer[SATB_BUFFER_SIZE];
    size_t satbCount;

    // Handle scope chain (linked list of HandleScope frames)
    void* handleScopeHead;

    // Per-thread GC statistics
    uint64_t bytesAllocated;
    uint64_t objectsAllocated;
    uint64_t safePointsHit;
    uint64_t tlabRefills;

    // Timestamps
    uint64_t registeredAtUs;
    uint64_t lastSafePointUs;

    ThreadGCData()
        : threadId(0)
        , name("unnamed")
        , stackBase(nullptr)
        , stackLimit(nullptr)
        , tlabStart(nullptr)
        , tlabCursor(nullptr)
        , tlabEnd(nullptr)
        , satbCount(0)
        , handleScopeHead(nullptr)
        , bytesAllocated(0)
        , objectsAllocated(0)
        , safePointsHit(0)
        , tlabRefills(0)
        , registeredAtUs(0)
        , lastSafePointUs(0) {}

    // TLAB allocation fast path
    void* tlabAllocate(size_t size) {
        size = (size + 7) & ~size_t(7);
        if (tlabCursor + size <= tlabEnd) {
            void* result = tlabCursor;
            tlabCursor += size;
            bytesAllocated += size;
            objectsAllocated++;
            return result;
        }
        return nullptr;
    }

    size_t tlabRemaining() const {
        if (!tlabCursor || !tlabEnd) return 0;
        return static_cast<size_t>(tlabEnd - tlabCursor);
    }

    size_t tlabUsed() const {
        if (!tlabStart || !tlabCursor) return 0;
        return static_cast<size_t>(tlabCursor - tlabStart);
    }

    // SATB buffer operations
    bool satbPush(void* oldRef) {
        if (satbCount >= SATB_BUFFER_SIZE) return false;
        satbBuffer[satbCount++] = oldRef;
        return true;
    }

    void satbClear() { satbCount = 0; }
    bool satbFull() const { return satbCount >= SATB_BUFFER_SIZE; }

    // Stack scanning range
    size_t stackSize() const {
        if (!stackBase || !stackLimit) return 0;
        auto base = reinterpret_cast<uintptr_t>(stackBase);
        auto limit = reinterpret_cast<uintptr_t>(stackLimit);
        return base > limit ? base - limit : limit - base;
    }

    bool isGCSafe() const {
        ThreadState s = state.load(std::memory_order_acquire);
        return s == ThreadState::InNative ||
               s == ThreadState::Blocked ||
               s == ThreadState::AtSafePoint ||
               s == ThreadState::Suspended;
    }
};

// =============================================================================
// Native Scope RAII
// =============================================================================

/**
 * @brief RAII guard for native code execution
 *
 * When entering native code (e.g. system calls, FFI), the thread
 * transitions to InNative state so the GC doesn't wait for it.
 */
class NativeScope {
public:
    explicit NativeScope(ThreadGCData& data) : data_(data) {
        prevState_ = data_.state.exchange(ThreadState::InNative,
                                           std::memory_order_release);
    }

    ~NativeScope() {
        data_.state.store(prevState_, std::memory_order_release);
        // Check if GC requested safe-point while we were in native
        if (data_.safePointRequested.load(std::memory_order_acquire)) {
            parkAtSafePoint();
        }
    }

    NativeScope(const NativeScope&) = delete;
    NativeScope& operator=(const NativeScope&) = delete;

private:
    void parkAtSafePoint() {
        data_.state.store(ThreadState::AtSafePoint, std::memory_order_release);
        data_.atSafePoint.store(true, std::memory_order_release);
        data_.safePointsHit++;
        data_.lastSafePointUs = nowUs();

        // Spin until GC clears the request
        while (data_.safePointRequested.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        data_.atSafePoint.store(false, std::memory_order_release);
        data_.state.store(prevState_, std::memory_order_release);
    }

    static uint64_t nowUs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    ThreadGCData& data_;
    ThreadState prevState_;
};

// =============================================================================
// Blocked Scope RAII
// =============================================================================

class BlockedScope {
public:
    explicit BlockedScope(ThreadGCData& data) : data_(data) {
        prevState_ = data_.state.exchange(ThreadState::Blocked,
                                           std::memory_order_release);
    }

    ~BlockedScope() {
        data_.state.store(prevState_, std::memory_order_release);
        if (data_.safePointRequested.load(std::memory_order_acquire)) {
            data_.state.store(ThreadState::AtSafePoint, std::memory_order_release);
            data_.atSafePoint.store(true, std::memory_order_release);
            while (data_.safePointRequested.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            data_.atSafePoint.store(false, std::memory_order_release);
            data_.state.store(prevState_, std::memory_order_release);
        }
    }

    BlockedScope(const BlockedScope&) = delete;
    BlockedScope& operator=(const BlockedScope&) = delete;

private:
    ThreadGCData& data_;
    ThreadState prevState_;
};

// =============================================================================
// Thread Manager
// =============================================================================

class ThreadManager {
public:
    struct Config {
        size_t maxThreads;
        size_t defaultTlabSize;
        uint64_t safePointTimeoutUs;

        Config()
            : maxThreads(256)
            , defaultTlabSize(32 * 1024)
            , safePointTimeoutUs(5000000) {}  // 5 seconds
    };

    explicit ThreadManager(const Config& config = Config{});
    ~ThreadManager();

    ThreadManager(const ThreadManager&) = delete;
    ThreadManager& operator=(const ThreadManager&) = delete;

    // -------------------------------------------------------------------------
    // Registration
    // -------------------------------------------------------------------------

    /**
     * @brief Register the current thread for GC participation
     * @return Pointer to this thread's GC data (stored in TLS)
     */
    ThreadGCData* registerThread(const char* name = "worker");

    /**
     * @brief Deregister the current thread
     */
    void deregisterThread(ThreadGCData* data);

    /**
     * @brief Get current thread's GC data
     */
    ThreadGCData* currentThread() const;

    // -------------------------------------------------------------------------
    // Enumeration
    // -------------------------------------------------------------------------

    /**
     * @brief Iterate all registered threads
     */
    void forEachThread(std::function<void(ThreadGCData&)> callback);

    /**
     * @brief Count of active threads
     */
    size_t threadCount() const;

    /**
     * @brief Get specific thread by ID
     */
    ThreadGCData* findThread(uint64_t threadId);

    // -------------------------------------------------------------------------
    // Safe-point coordination
    // -------------------------------------------------------------------------

    /**
     * @brief Request all threads to reach safe-points
     * @return true if all threads parked within timeout
     */
    bool requestSafePoints();

    /**
     * @brief Release all threads from safe-points
     */
    void releaseSafePoints();

    /**
     * @brief Check if all threads are at safe-points
     */
    bool allAtSafePoint() const;

    /**
     * @brief Poll safe-point from mutator thread
     * Called at allocation sites, back-edges, function entries.
     */
    void pollSafePoint(ThreadGCData& data);

    // -------------------------------------------------------------------------
    // TLAB management
    // -------------------------------------------------------------------------

    using TlabRefillFn = std::function<bool(char** start, char** end, size_t requestedSize)>;

    void setTlabRefillCallback(TlabRefillFn callback) {
        tlabRefill_ = std::move(callback);
    }

    /**
     * @brief Refill a thread's TLAB
     */
    bool refillTlab(ThreadGCData& data, size_t minSize);

    /**
     * @brief Retire all TLABs (before GC)
     */
    void retireAllTlabs();

    // -------------------------------------------------------------------------
    // SATB buffer management
    // -------------------------------------------------------------------------

    using SatbFlushFn = std::function<void(void** buffer, size_t count)>;

    void setSatbFlushCallback(SatbFlushFn callback) {
        satbFlush_ = std::move(callback);
    }

    /**
     * @brief Flush all threads' SATB buffers
     */
    void flushAllSatbBuffers();

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    struct Stats {
        size_t activeThreads;
        uint64_t totalRegistered;
        uint64_t totalDeregistered;
        uint64_t safePointRequests;
        uint64_t safePointTimeouts;
        uint64_t totalBytesAllocated;
        uint64_t totalObjectsAllocated;
    };

    Stats computeStats() const;

private:
    static uint64_t generateThreadId();
    static void* getStackBase();

    Config config_;

    // Thread storage
    mutable std::shared_mutex threadsMutex_;
    std::vector<std::unique_ptr<ThreadGCData>> threads_;

    // TLS key for current thread lookup
    static thread_local ThreadGCData* tlsCurrentThread_;

    // Safe-point state
    std::atomic<bool> safePointActive_{false};
    std::mutex safePointMutex_;
    std::condition_variable safePointCv_;

    // Callbacks
    TlabRefillFn tlabRefill_;
    SatbFlushFn satbFlush_;

    // Counters
    std::atomic<uint64_t> nextThreadId_{1};
    std::atomic<uint64_t> totalRegistered_{0};
    std::atomic<uint64_t> totalDeregistered_{0};
    std::atomic<uint64_t> safePointRequests_{0};
    std::atomic<uint64_t> safePointTimeouts_{0};
};

thread_local ThreadGCData* ThreadManager::tlsCurrentThread_ = nullptr;

// =============================================================================
// Implementation
// =============================================================================

inline ThreadManager::ThreadManager(const Config& config)
    : config_(config) {
    threads_.reserve(config.maxThreads);
}

inline ThreadManager::~ThreadManager() {
    // All threads should be deregistered by now
    std::unique_lock<std::shared_mutex> lock(threadsMutex_);
    threads_.clear();
}

inline ThreadGCData* ThreadManager::registerThread(const char* name) {
    auto data = std::make_unique<ThreadGCData>();
    data->threadId = nextThreadId_.fetch_add(1, std::memory_order_relaxed);
    data->stdThreadId = std::this_thread::get_id();
    data->name = name;
    data->state.store(ThreadState::Running, std::memory_order_relaxed);
    data->stackBase = getStackBase();
    data->registeredAtUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    ThreadGCData* raw = data.get();

    {
        std::unique_lock<std::shared_mutex> lock(threadsMutex_);
        threads_.push_back(std::move(data));
    }

    tlsCurrentThread_ = raw;
    totalRegistered_.fetch_add(1, std::memory_order_relaxed);

    // If safe-point is currently active, park immediately
    if (safePointActive_.load(std::memory_order_acquire)) {
        raw->safePointRequested.store(true, std::memory_order_release);
        raw->state.store(ThreadState::AtSafePoint, std::memory_order_release);
        raw->atSafePoint.store(true, std::memory_order_release);

        while (safePointActive_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        raw->atSafePoint.store(false, std::memory_order_release);
        raw->safePointRequested.store(false, std::memory_order_release);
        raw->state.store(ThreadState::Running, std::memory_order_release);
    }

    return raw;
}

inline void ThreadManager::deregisterThread(ThreadGCData* data) {
    if (!data) return;

    data->state.store(ThreadState::Terminated, std::memory_order_release);
    data->atSafePoint.store(true, std::memory_order_release);

    // Flush SATB buffer
    if (data->satbCount > 0 && satbFlush_) {
        satbFlush_(data->satbBuffer, data->satbCount);
        data->satbClear();
    }

    {
        std::unique_lock<std::shared_mutex> lock(threadsMutex_);
        threads_.erase(
            std::remove_if(threads_.begin(), threads_.end(),
                [data](const auto& p) { return p.get() == data; }),
            threads_.end());
    }

    if (tlsCurrentThread_ == data) {
        tlsCurrentThread_ = nullptr;
    }

    totalDeregistered_.fetch_add(1, std::memory_order_relaxed);
}

inline ThreadGCData* ThreadManager::currentThread() const {
    return tlsCurrentThread_;
}

inline void ThreadManager::forEachThread(
    std::function<void(ThreadGCData&)> callback
) {
    std::shared_lock<std::shared_mutex> lock(threadsMutex_);
    for (auto& t : threads_) {
        if (t->state.load(std::memory_order_acquire) != ThreadState::Terminated) {
            callback(*t);
        }
    }
}

inline size_t ThreadManager::threadCount() const {
    std::shared_lock<std::shared_mutex> lock(threadsMutex_);
    size_t count = 0;
    for (const auto& t : threads_) {
        if (t->state.load(std::memory_order_acquire) != ThreadState::Terminated) {
            count++;
        }
    }
    return count;
}

inline ThreadGCData* ThreadManager::findThread(uint64_t threadId) {
    std::shared_lock<std::shared_mutex> lock(threadsMutex_);
    for (auto& t : threads_) {
        if (t->threadId == threadId) return t.get();
    }
    return nullptr;
}

inline bool ThreadManager::requestSafePoints() {
    safePointRequests_.fetch_add(1, std::memory_order_relaxed);
    safePointActive_.store(true, std::memory_order_release);

    // Set flag on all threads
    {
        std::shared_lock<std::shared_mutex> lock(threadsMutex_);
        for (auto& t : threads_) {
            ThreadState s = t->state.load(std::memory_order_acquire);
            if (s != ThreadState::Terminated) {
                t->safePointRequested.store(true, std::memory_order_release);
            }
        }
    }

    // Wait for all threads to reach safe-point or be in GC-safe state
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::microseconds(config_.safePointTimeoutUs);

    while (std::chrono::steady_clock::now() < deadline) {
        if (allAtSafePoint()) return true;
        std::this_thread::yield();
    }

    safePointTimeouts_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

inline void ThreadManager::releaseSafePoints() {
    {
        std::shared_lock<std::shared_mutex> lock(threadsMutex_);
        for (auto& t : threads_) {
            t->safePointRequested.store(false, std::memory_order_release);
        }
    }

    safePointActive_.store(false, std::memory_order_release);
}

inline bool ThreadManager::allAtSafePoint() const {
    std::shared_lock<std::shared_mutex> lock(threadsMutex_);
    for (const auto& t : threads_) {
        ThreadState s = t->state.load(std::memory_order_acquire);
        if (s == ThreadState::Terminated) continue;

        // Thread must be GC-safe (native, blocked, at safe-point, or suspended)
        if (s == ThreadState::Running) {
            // Running thread hasn't reached safe-point yet
            if (!t->atSafePoint.load(std::memory_order_acquire)) {
                return false;
            }
        }
    }
    return true;
}

inline void ThreadManager::pollSafePoint(ThreadGCData& data) {
    if (!data.safePointRequested.load(std::memory_order_acquire)) return;

    // Park at safe-point
    data.state.store(ThreadState::AtSafePoint, std::memory_order_release);
    data.atSafePoint.store(true, std::memory_order_release);
    data.safePointsHit++;
    data.lastSafePointUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // Wait until GC clears the request
    while (data.safePointRequested.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    data.atSafePoint.store(false, std::memory_order_release);
    data.state.store(ThreadState::Running, std::memory_order_release);
}

inline bool ThreadManager::refillTlab(ThreadGCData& data, size_t minSize) {
    if (!tlabRefill_) return false;

    // Retire current TLAB — the remaining space is wasted
    // (or we could track it as a free chunk)
    char* newStart = nullptr;
    char* newEnd = nullptr;
    size_t requestSize = std::max(minSize, config_.defaultTlabSize);

    if (!tlabRefill_(&newStart, &newEnd, requestSize)) return false;

    data.tlabStart = newStart;
    data.tlabCursor = newStart;
    data.tlabEnd = newEnd;
    data.tlabRefills++;

    return true;
}

inline void ThreadManager::retireAllTlabs() {
    std::shared_lock<std::shared_mutex> lock(threadsMutex_);
    for (auto& t : threads_) {
        // Record wasted space if needed
        t->tlabStart = nullptr;
        t->tlabCursor = nullptr;
        t->tlabEnd = nullptr;
    }
}

inline void ThreadManager::flushAllSatbBuffers() {
    if (!satbFlush_) return;

    std::shared_lock<std::shared_mutex> lock(threadsMutex_);
    for (auto& t : threads_) {
        if (t->satbCount > 0) {
            satbFlush_(t->satbBuffer, t->satbCount);
            t->satbClear();
        }
    }
}

inline ThreadManager::Stats ThreadManager::computeStats() const {
    Stats stats{};
    stats.activeThreads = threadCount();
    stats.totalRegistered = totalRegistered_.load(std::memory_order_relaxed);
    stats.totalDeregistered = totalDeregistered_.load(std::memory_order_relaxed);
    stats.safePointRequests = safePointRequests_.load(std::memory_order_relaxed);
    stats.safePointTimeouts = safePointTimeouts_.load(std::memory_order_relaxed);

    std::shared_lock<std::shared_mutex> lock(threadsMutex_);
    for (const auto& t : threads_) {
        stats.totalBytesAllocated += t->bytesAllocated;
        stats.totalObjectsAllocated += t->objectsAllocated;
    }

    return stats;
}

inline uint64_t ThreadManager::generateThreadId() {
#ifdef __linux__
    return static_cast<uint64_t>(syscall(SYS_gettid));
#else
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
#endif
}

inline void* ThreadManager::getStackBase() {
#ifdef __linux__
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return nullptr;

    // Get attributes of the current thread
    void* stackAddr = nullptr;
    size_t stackSize = 0;

    // pthread_getattr_np is Linux-specific
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &stackAddr, &stackSize);
    }
    pthread_attr_destroy(&attr);

    // Stack base = addr + size (stack grows downward on x86)
    if (stackAddr && stackSize > 0) {
        return static_cast<char*>(stackAddr) + stackSize;
    }
#endif
    return nullptr;
}

} // namespace Zepra::Heap

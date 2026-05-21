// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file CrashHandler.h
 * @brief Engine-level crash recovery and signal handling
 *
 * Provides:
 * - POSIX signal handlers (SIGSEGV, SIGBUS, SIGABRT, SIGFPE)
 * - Crash context capture (registers, stack trace, VM state)
 * - Crash dump generation for post-mortem analysis
 * - Clean abort with resource cleanup
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <vector>
#include <atomic>
#include <mutex>
#include <csignal>

namespace Zepra::Runtime {

// =============================================================================
// Crash Context
// =============================================================================

/**
 * @brief Captured state at time of crash
 */
struct CrashContext {
    int signal = 0;
    int signalCode = 0;
    void* faultAddress = nullptr;

    // VM state snapshot
    size_t instructionPointer = 0;
    size_t stackDepth = 0;
    size_t heapUsedBytes = 0;

    // System info
#ifdef _WIN32
    int processId = 0;
    int threadId = 0;
#else
    pid_t processId = 0;
    pid_t threadId = 0;
#endif
    uint64_t timestampMs = 0;

    // Stack frames (best-effort, signal-safe)
    static constexpr size_t MAX_FRAMES = 64;
    void* stackFrames[MAX_FRAMES] = {};
    size_t frameCount = 0;

    // Human-readable description
    const char* signalName() const;
    std::string summary() const;
};

// =============================================================================
// Crash Dump Writer
// =============================================================================

/**
 * @brief Writes crash dumps to disk for post-mortem analysis
 */
class CrashDumpWriter {
public:
    explicit CrashDumpWriter(const std::string& dumpDir = "/tmp/zepra-crashes");

    /**
     * @brief Write crash dump (signal-safe path uses fd directly)
     * @return Path to dump file, empty on failure
     */
    std::string writeDump(const CrashContext& ctx);

    void setDumpDirectory(const std::string& dir) { dumpDir_ = dir; }
    const std::string& dumpDirectory() const { return dumpDir_; }

private:
    std::string dumpDir_;

    // Async-signal-safe write (no malloc, no locks)
    static void writeRaw(int fd, const char* data, size_t len);
    static void writeInt(int fd, uint64_t value);
    static void writeHex(int fd, uint64_t value);
};

// =============================================================================
// Crash Handler
// =============================================================================

/**
 * @brief Installs signal handlers and manages crash recovery
 *
 * Usage:
 *   CrashHandler::install();
 *   // ... engine runs ...
 *   // On crash: handler captures context, writes dump, calls callbacks
 */
class CrashHandler {
public:
    using CrashCallback = std::function<void(const CrashContext&)>;

    /**
     * @brief Install signal handlers (call once at startup)
     */
    static void install();

    /**
     * @brief Uninstall signal handlers
     */
    static void uninstall();

    /**
     * @brief Check if handlers are installed
     */
    static bool isInstalled() { return installed_.load(); }

    /**
     * @brief Register callback invoked on crash (before abort)
     * Callbacks must be async-signal-safe if called from signal context.
     */
    static void onCrash(CrashCallback callback);

    /**
     * @brief Set crash dump directory
     */
    static void setDumpDirectory(const std::string& dir);

    /**
     * @brief Set the active VM state for crash context capture
     * Called by VM on each dispatch cycle.
     */
    static void setActiveVMState(size_t ip, size_t stackDepth, size_t heapUsed);

private:
#ifndef _WIN32
    static void signalHandler(int sig, siginfo_t* info, void* ucontext);
    static CrashContext captureContext(int sig, siginfo_t* info);
#endif

    static std::atomic<bool> installed_;
    static std::vector<CrashCallback> callbacks_;
    static std::mutex callbackMutex_;
    static CrashDumpWriter dumpWriter_;

    // Thread-local VM state for crash context
    static thread_local size_t activeIP_;
    static thread_local size_t activeStackDepth_;
    static thread_local size_t activeHeapUsed_;

#ifndef _WIN32
    // Saved previous handlers for chaining
    static struct sigaction prevSIGSEGV_;
    static struct sigaction prevSIGBUS_;
    static struct sigaction prevSIGABRT_;
    static struct sigaction prevSIGFPE_;
#endif
};

// =============================================================================
// Execution Watchdog
// =============================================================================

/**
 * @brief Watchdog thread that detects infinite loops and deadlocks
 *
 * Monitors a counter incremented by the VM's dispatch loop.
 * If the counter doesn't advance within the timeout, the watchdog
 * requests termination via the VM's termination flag.
 */
class ExecutionWatchdog {
public:
    /**
     * @param timeoutMs Maximum time (ms) between progress updates
     * @param onTimeout Called when timeout fires (from watchdog thread)
     */
    explicit ExecutionWatchdog(uint32_t timeoutMs = 30000,
                               std::function<void()> onTimeout = nullptr);
    ~ExecutionWatchdog();

    // Non-copyable
    ExecutionWatchdog(const ExecutionWatchdog&) = delete;
    ExecutionWatchdog& operator=(const ExecutionWatchdog&) = delete;

    /**
     * @brief Start monitoring
     */
    void start();

    /**
     * @brief Stop monitoring
     */
    void stop();

    /**
     * @brief Report progress (called by VM at safe-points)
     */
    void kick() { progressCounter_.fetch_add(1, std::memory_order_relaxed); }

    /**
     * @brief Check if timeout has fired
     */
    bool hasTimedOut() const { return timedOut_.load(); }

    /**
     * @brief Set the termination flag pointer (VM's flag)
     */
    void setTerminationFlag(std::atomic<bool>* flag) { terminationFlag_ = flag; }

private:
    void watchdogLoop();

    uint32_t timeoutMs_;
    std::function<void()> onTimeout_;
    std::atomic<uint64_t> progressCounter_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> timedOut_{false};
    std::atomic<bool>* terminationFlag_ = nullptr;
};

} // namespace Zepra::Runtime

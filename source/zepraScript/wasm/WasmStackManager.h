// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmStackManager.h
 * @brief WebAssembly Stack Overflow Protection
 *
 * Manages per-thread stack limits and guard pages for safe WASM execution.
 * Call StackManager::instance().initThreadStack() once per thread before
 * entering WASM execution, then check isStackSafe(sp) at each call frame.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <thread>

namespace Zepra::Wasm {

class StackManager {
public:
    static StackManager& instance() {
        static StackManager manager;
        return manager;
    }

    /**
     * @brief Probe the current thread's stack bounds and set the thread-local
     *        limit. Must be called once per thread before WASM execution.
     * @param guardReserve Bytes to reserve above the platform stack bottom
     *                     as an additional guard zone. Default: 64 KB.
     */
    void initThreadStack(size_t guardReserve = 64u * 1024u);

    /**
     * @brief Returns true if currentSP is safely above the guard limit.
     *        Call this before allocating a new WASM stack frame.
     */
    bool isStackSafe(uintptr_t currentSP) const;

    /** @brief Raw accessor — 0 means not yet initialized. */
    uintptr_t currentLimit() const { return threadLimit_; }

    /** @brief Explicit override (used by tests or embedders). */
    void setLimit(uintptr_t limit) { threadLimit_ = limit; }

private:
    static thread_local uintptr_t threadLimit_;
};

} // namespace Zepra::Wasm


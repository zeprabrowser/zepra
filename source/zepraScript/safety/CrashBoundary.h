// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file CrashBoundary.h
 * @brief Crash containment umbrella — safe execution boundary
 *
 * Individual components:
 *   UncatchableException.h — Non-catchable engine exceptions
 *   OOMHandler.h           — Out-of-memory detection and handling
 *   WasmTrapHandler.h      — WASM trap signal containment
 *   StackGuard.h           — Stack overflow prevention
 *   JITFallback.h          — JIT tier degradation
 *   EngineRecovery.h       — Engine reset without restart
 *   WorkerIsolation.h      — Worker crash isolation
 */

#pragma once

#include "UncatchableException.h"
#include "OOMHandler.h"
#include "WasmTrapHandler.h"
#include "StackGuard.h"
#include "JITFallback.h"
#include "EngineRecovery.h"
#include "WorkerIsolation.h"

#include <string>
#include <atomic>
#include <thread>
#include <chrono>

namespace Zepra::Safety {

class CrashBoundary {
public:
    enum class Result {
        Success,
        JSException,
        OOM,
        StackOverflow,
        WasmTrap,
        InternalError,
        Timeout
    };
    
    struct ExecutionResult {
        Result result = Result::Success;
        std::string errorMessage;
        bool recoverable = true;
    };
    
    template<typename F>
    static ExecutionResult execute(F&& func) {
        ExecutionResult res;
        
        if (!WasmTrapHandler::instance().setRecoveryPoint()) {
            auto trap = WasmTrapHandler::instance().lastTrap();
            res.result = Result::WasmTrap;
            res.errorMessage = WasmTrapHandler::instance().trapToMessage(trap.kind);
            return res;
        }
        
        try {
            func();
            res.result = Result::Success;
        } catch (const UncatchableException& e) {
            switch (e.kind()) {
                case UncatchableException::Kind::OutOfMemory:
                    res.result = Result::OOM;
                    break;
                case UncatchableException::Kind::StackOverflow:
                    res.result = Result::StackOverflow;
                    break;
                default:
                    res.result = Result::InternalError;
            }
            res.errorMessage = e.what();
            res.recoverable = (e.kind() != UncatchableException::Kind::SecurityViolation);
        } catch (const std::exception& e) {
            res.result = Result::JSException;
            res.errorMessage = e.what();
        } catch (...) {
            res.result = Result::InternalError;
            res.errorMessage = "Unknown exception";
            res.recoverable = false;
        }
        
        return res;
    }
    
    template<typename F>
    static ExecutionResult executeWithTimeout(F&& func, uint32_t timeoutMs) {
        if (timeoutMs == 0) {
            return execute(std::forward<F>(func));
        }

        std::atomic<bool> timedOut{false};
        std::atomic<bool> done{false};

        std::thread watchdog([&timedOut, &done, timeoutMs]() {
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(timeoutMs);
            while (!done.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    timedOut.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        ExecutionResult res = execute([&]() {
            func();
            if (timedOut.load(std::memory_order_acquire)) {
                throw UncatchableException(UncatchableException::Kind::Terminated,
                                           "Execution timeout");
            }
        });

        done.store(true, std::memory_order_release);
        watchdog.join();

        if (timedOut.load(std::memory_order_relaxed) && res.result == Result::Success) {
            res.result = Result::Timeout;
            res.errorMessage = "Execution exceeded " + std::to_string(timeoutMs) + "ms timeout";
            res.recoverable = true;
        }

        return res;
    }
};

} // namespace Zepra::Safety

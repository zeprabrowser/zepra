// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmTrapHandler.h
 * @brief WASM trap containment — POSIX signal-based on Linux/NeolyxOS,
 *        C++ exception fallback on Windows (SEH not available in all contexts).
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <atomic>

// Platform-specific trap recovery headers
#ifndef _WIN32
#  include <signal.h>
#  include <setjmp.h>
#else
#  include <setjmp.h>   // jmp_buf / setjmp / longjmp available on Windows
#endif

namespace Zepra::Safety {

class WasmTrapHandler {
public:
    enum class TrapKind {
        None,
        Unreachable,
        IntegerOverflow,
        IntegerDivideByZero,
        OutOfBoundsMemory,
        OutOfBoundsTable,
        IndirectCallTypeMismatch,
        StackOverflow,
        UndefinedElement,
        UnalignedAccess
    };

    struct TrapContext {
        TrapKind kind = TrapKind::None;
        uint32_t pc = 0;
        uint32_t stackDepth = 0;
        std::string moduleName;
        std::string functionName;
    };

    static WasmTrapHandler& instance() {
        static WasmTrapHandler inst;
        return inst;
    }

    void installHandlers() {
#ifndef _WIN32
        // POSIX: install SA_SIGINFO signal handlers for SIGSEGV, SIGFPE, SIGBUS
        struct sigaction sa;
        sa.sa_sigaction = signalHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);

        sigaction(SIGFPE,  &sa, nullptr);
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS,  &sa, nullptr);
#endif
        // Windows: WASM traps are caught via C++ exceptions / explicit checks
        // (hardware exceptions via SEH would require /EHa, avoided by design)
        handlersInstalled_ = true;
    }

    bool setRecoveryPoint() {
#ifndef _WIN32
        return sigsetjmp(recoveryPoint_, 1) == 0;
#else
        // Windows: use plain setjmp (no signal mask save needed)
        return setjmp(recoveryPoint_) == 0;
#endif
    }

    [[noreturn]] void trap(TrapKind kind, const char* message = nullptr) {
        (void)message;
        currentTrap_.kind = kind;
        stats_.trapCount++;
#ifndef _WIN32
        siglongjmp(recoveryPoint_, static_cast<int>(kind));
#else
        longjmp(recoveryPoint_, static_cast<int>(kind));
#endif
    }

    const TrapContext& lastTrap() const { return currentTrap_; }

    std::string trapToMessage(TrapKind kind) const {
        switch (kind) {
            case TrapKind::Unreachable:               return "unreachable executed";
            case TrapKind::IntegerOverflow:           return "integer overflow";
            case TrapKind::IntegerDivideByZero:       return "integer divide by zero";
            case TrapKind::OutOfBoundsMemory:         return "out of bounds memory access";
            case TrapKind::OutOfBoundsTable:          return "out of bounds table access";
            case TrapKind::IndirectCallTypeMismatch:  return "indirect call type mismatch";
            case TrapKind::StackOverflow:             return "call stack exhausted";
            default:                                  return "wasm trap";
        }
    }

    struct Stats {
        size_t trapCount = 0;
        size_t recoveredCount = 0;
    };

    Stats stats() const { return stats_; }

private:
    WasmTrapHandler() = default;

#ifndef _WIN32
    // POSIX: SA_SIGINFO handler receives siginfo_t — POSIX-only type
    static void signalHandler(int sig, siginfo_t* info, void* context) {
        (void)info; (void)context;
        TrapKind kind = TrapKind::None;
        switch (sig) {
            case SIGFPE:  kind = TrapKind::IntegerDivideByZero; break;
            case SIGSEGV: kind = TrapKind::OutOfBoundsMemory;   break;
            case SIGBUS:  kind = TrapKind::UnalignedAccess;      break;
        }
        instance().trap(kind);
    }

    sigjmp_buf   recoveryPoint_;
#else
    // Windows: plain jmp_buf (no signal mask)
    jmp_buf      recoveryPoint_;
#endif

    TrapContext  currentTrap_;
    Stats        stats_;
    bool         handlersInstalled_ = false;
};

} // namespace Zepra::Safety

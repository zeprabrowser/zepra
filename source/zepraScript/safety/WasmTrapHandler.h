// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmTrapHandler.h
 * @brief WASM trap containment via signal handlers
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <atomic>
#include <signal.h>
#include <setjmp.h>

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
        struct sigaction sa;
        sa.sa_sigaction = signalHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        
        sigaction(SIGFPE, &sa, nullptr);
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
#endif
        handlersInstalled_ = true;
    }
    
    bool setRecoveryPoint() {
        return sigsetjmp(recoveryPoint_, 1) == 0;
    }
    
    [[noreturn]] void trap(TrapKind kind, const char* message = nullptr) {
        (void)message;
        currentTrap_.kind = kind;
        stats_.trapCount++;
        siglongjmp(recoveryPoint_, static_cast<int>(kind));
    }
    
    const TrapContext& lastTrap() const { return currentTrap_; }
    
    std::string trapToMessage(TrapKind kind) const {
        switch (kind) {
            case TrapKind::Unreachable: return "unreachable executed";
            case TrapKind::IntegerOverflow: return "integer overflow";
            case TrapKind::IntegerDivideByZero: return "integer divide by zero";
            case TrapKind::OutOfBoundsMemory: return "out of bounds memory access";
            case TrapKind::OutOfBoundsTable: return "out of bounds table access";
            case TrapKind::IndirectCallTypeMismatch: return "indirect call type mismatch";
            case TrapKind::StackOverflow: return "call stack exhausted";
            default: return "wasm trap";
        }
    }
    
    struct Stats {
        size_t trapCount = 0;
        size_t recoveredCount = 0;
    };
    
    Stats stats() const { return stats_; }
    
private:
    WasmTrapHandler() = default;
    
    static void signalHandler(int sig, siginfo_t* info, void* context) {
        (void)info; (void)context;
        
        TrapKind kind = TrapKind::None;
        switch (sig) {
            case SIGFPE: kind = TrapKind::IntegerDivideByZero; break;
            case SIGSEGV: kind = TrapKind::OutOfBoundsMemory; break;
            case SIGBUS: kind = TrapKind::UnalignedAccess; break;
        }
        
        instance().trap(kind);
    }
    
    sigjmp_buf recoveryPoint_;
    TrapContext currentTrap_;
    Stats stats_;
    bool handlersInstalled_ = false;
};

} // namespace Zepra::Safety

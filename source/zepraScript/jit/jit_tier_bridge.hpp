// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include <cstdint>

namespace Zepra::Bytecode { class BytecodeChunk; }
namespace Zepra::Runtime { class VM; }

namespace Zepra::JIT {

class TierUpBridge;

void initTierUpBridge(Runtime::VM* vm);
void shutdownTierUpBridge();
TierUpBridge* getTierUpBridge();

class TierUpBridge {
public:
    explicit TierUpBridge(Runtime::VM* vm);

    void* onFunctionEntry(uintptr_t functionId, const Bytecode::BytecodeChunk* chunk);
    void* onLoopBackEdge(uintptr_t functionId, const Bytecode::BytecodeChunk* chunk,
                          uint32_t bytecodeOffset);
    void onDeoptimization(uintptr_t functionId);
    bool isCompiled(uintptr_t functionId) const;
    void invalidateAll();

    struct Stats {
        uint64_t compilationAttempts = 0;
        uint64_t compilationSuccesses = 0;
        uint64_t compilationFailures = 0;
        uint64_t deoptimizations = 0;
        double totalCompileTimeMs = 0.0;
    };

    const Stats& stats() const;
};

} // namespace Zepra::JIT

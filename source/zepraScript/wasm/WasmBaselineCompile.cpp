// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmBaselineCompile.cpp
 * @brief BaselineCompiler core: register allocator, compile dispatch, emitOp
 *
 * Split structure:
 *   WasmBaselineInternal.h    — Shared internal header (CodeBuffer, macros, constants)
 *   WasmCodeBuffer.cpp        — Platform support check
 *   WasmX86Assembler.cpp      — X86Assembler class
 *   WasmARM64Assembler.cpp    — ARM64Assembler class
 *   WasmBaselineCompiler.cpp  — This file (core + dispatch)
 *   WasmBaselineOpcodes.cpp   — All emitXxx opcode handlers
 *   WasmBaselineBulkGC.cpp    — Exception, atomics, GC, bulk, reference ops
 */

#include "wasm/WasmBaselineInternal.h"
#include <algorithm>

namespace Zepra::Wasm {
// =============================================================================
// Simple Register Allocator
// =============================================================================

SimpleRegAllocator::SimpleRegAllocator() {
#if defined(__x86_64__) || defined(_M_X64)
    // Available GPRs: rax, rcx, rdx, rbx, rsi, rdi, r8-r15 (minus rsp, rbp)
    gprMask_ = 0b1111111111001111;  // Exclude rsp (4), rbp (5)
    gprScratch_ = 11;  // r11 as scratch
    fprMask_ = 0xFFFF;  // xmm0-xmm15
    fprScratch_ = 15;   // xmm15 as scratch
#elif defined(__aarch64__) || defined(_M_ARM64)
    // Available GPRs: x0-x28 (exclude x29=fp, x30=lr, x31=sp)
    gprMask_ = 0x1FFFFFFF;
    gprScratch_ = 16;
    fprMask_ = 0xFFFFFFFF;  // v0-v31
    fprScratch_ = 31;
#else
    gprMask_ = 0xFF;
    gprScratch_ = 0;
    fprMask_ = 0xFF;
    fprScratch_ = 0;
#endif
}

Reg SimpleRegAllocator::allocGPR() {
    for (int i = 0; i < 32; i++) {
        if (gprMask_ & (1u << i)) {
            gprMask_ &= ~(1u << i);
            return Reg::gpr(i);
        }
    }
    // Spill needed
    return Reg::gpr(255);
}

Reg SimpleRegAllocator::allocFPR() {
    for (int i = 0; i < 32; i++) {
        if (fprMask_ & (1u << i)) {
            fprMask_ &= ~(1u << i);
            return Reg::fpr(i);
        }
    }
    return Reg::fpr(255);
}

Reg SimpleRegAllocator::allocV128() {
    // V128 uses FPR registers on most platforms
    return allocFPR();
}

void SimpleRegAllocator::freeReg(Reg reg) {
    if (reg.regClass == RegClass::GPR && reg.code < 32) {
        gprMask_ |= (1u << reg.code);
    } else if ((reg.regClass == RegClass::FPR || reg.regClass == RegClass::V128) && reg.code < 32) {
        fprMask_ |= (1u << reg.code);
    }
}

bool SimpleRegAllocator::hasAvailableGPR() const {
    return gprMask_ != 0;
}

bool SimpleRegAllocator::hasAvailableFPR() const {
    return fprMask_ != 0;
}

void SimpleRegAllocator::spillAll() {
    // Reset to initial state
    *this = SimpleRegAllocator();
}

Reg SimpleRegAllocator::scratchGPR() const {
    return Reg::gpr(gprScratch_);
}

Reg SimpleRegAllocator::scratchFPR() const {
    return Reg::fpr(fprScratch_);
}

// =============================================================================
// Baseline Compiler Implementation
// =============================================================================

BaselineCompiler::BaselineCompiler(const WasmModule* module,
                                   const BaselineCompilerOptions& options)
    : module_(module)
    , options_(options)
    , codeBuffer_(std::make_unique<CodeBuffer>()) {
}

BaselineCompiler::~BaselineCompiler() = default;

bool BaselineCompiler::compileFunction(uint32_t funcIndex, CompiledFunction* result) {
    if (funcIndex >= module_->numFunctions()) {
        error_ = "function index out of bounds";
        return false;
    }
    
    const auto& funcDecl = module_->functions()[funcIndex];
    return compileFunctionBody(funcDecl, result);
}

BaselineCompilationResult BaselineCompiler::compileAll() {
    BaselineCompilationResult result;
    result.success = true;
    
    for (uint32_t i = module_->numImportedFunctions(); i < module_->numFunctions(); i++) {
        CompiledFunction func;
        if (!compileFunction(i, &func)) {
            result.error = error_;
            result.success = false;
            return result;
        }
        result.functions.push_back(std::move(func));
    }
    
    return result;
}

bool BaselineCompiler::compileFunctionBody(const FunctionDecl& func, CompiledFunction* result) {
    // Reset state
    valueStack_.clear();
    controlStack_.clear();
    locals_.clear();
    frameSize_ = 0;
    maxFrameSize_ = 0;
    codeBuffer_ = std::make_unique<CodeBuffer>();
    regAlloc_ = SimpleRegAllocator();
    pcMapping_.clear();
    
    funcIndex_ = func.typeIndex;
    funcType_ = module_->funcType(func.typeIndex);
    if (!funcType_) {
        error_ = "invalid function type index";
        return false;
    }
    
    // Set up locals (params + local declarations)
    uint32_t numLocals = funcType_->params().size();
    
    // Calculate frame size
    for (const auto& param : funcType_->params()) {
        StackSlot slot;
        slot.type = param;
        slot.offset = frameSize_;
        frameSize_ += param.size();
        locals_.push_back(slot);
    }
    
    // Emit function prologue
    emitPrologue(funcType_, numLocals);
    
    // Initial control frame for the function body (results = function return types)
    BlockType bodyType = BlockType::typeIndex(func.typeIndex);
    controlStack_.push_back(BaselineControlFrame(
        BaselineControlFrame::Kind::Block,
        bodyType,
        0  // Stack height
    ));
    controlStack_.back().endLabel = newLabel();
    
    // Decode and compile the function body
    const uint8_t* bodyStart = module_->bytecode() + func.codeOffset;
    const uint8_t* bodyEnd = bodyStart + func.codeSize;
    Decoder bodyDecoder(bodyStart, bodyEnd);
    decoder_ = &bodyDecoder;
    
    // Skip local declarations (already processed by validation)
    uint32_t numLocalDecls = 0;
    if (!bodyDecoder.readVarU32(&numLocalDecls)) {
        error_ = "failed to read local count";
        return false;
    }
    
    for (uint32_t i = 0; i < numLocalDecls; i++) {
        uint32_t count;
        uint8_t typeCode;
        if (!bodyDecoder.readVarU32(&count) || !bodyDecoder.readFixedU8(&typeCode)) {
            error_ = "failed to read local declaration";
            return false;
        }
        
        auto localTypeOpt = ValType::fromTypeCode(static_cast<TypeCode>(typeCode));
        if (!localTypeOpt) {
            error_ = "invalid local type";
            return false;
        }
        ValType localType = *localTypeOpt;
        for (uint32_t j = 0; j < count; j++) {
            StackSlot slot;
            slot.type = localType;
            slot.offset = frameSize_;
            frameSize_ += localType.size();
            locals_.push_back(slot);
        }
    }
    
    maxFrameSize_ = frameSize_;
    
    // Compile instructions
    while (!bodyDecoder.done()) {
        size_t instrOffset = bodyDecoder.currentOffset();
        pcMapping_.push_back({codeBuffer_->offset(), static_cast<uint32_t>(instrOffset)});
        
        Opcode op;
        if (!bodyDecoder.readOp(&op)) {
            error_ = "failed to read opcode";
            return false;
        }
        
        if (!emitOp(op)) {
            return false;
        }
    }
    
    // Bind end label
    if (!controlStack_.empty()) {
        bindLabel(controlStack_.back().endLabel);
    }
    
    // Emit epilogue
    emitEpilogue();
    
    // Finalize result
    result->code = codeBuffer_->takeCode();
    result->funcIndex = funcIndex_;
    result->stackSlots = maxFrameSize_ / 8;
    result->pcToOffset = std::move(pcMapping_);
    
    decoder_ = nullptr;
    return true;
}

bool BaselineCompiler::emitOp(Opcode op) {
    // Handle primary opcodes (single byte)
    if (op.isOp()) {
        Op primaryOp = op.asOp();
        
        switch (primaryOp) {
            case Op::Unreachable:
                return emitUnreachable();
            case Op::Nop:
                return true;  // Nothing to emit
            case Op::Block: {
                BlockType bt;
                if (!decoder_->readBlockType(&bt)) {
                    error_ = "failed to read block type";
                    return false;
                }
                return emitBlock(bt);
            }
            case Op::Loop: {
                BlockType bt;
                if (!decoder_->readBlockType(&bt)) {
                    error_ = "failed to read block type";
                    return false;
                }
                return emitLoop(bt);
            }
            case Op::If: {
                BlockType bt;
                if (!decoder_->readBlockType(&bt)) {
                    error_ = "failed to read block type";
                    return false;
                }
                return emitIf(bt);
            }
            case Op::Else:
                return emitElse();
            case Op::End:
                return emitEnd();
            case Op::Br: {
                uint32_t depth;
                if (!decoder_->readVarU32(&depth)) {
                    error_ = "failed to read br depth";
                    return false;
                }
                return emitBr(depth);
            }
            case Op::BrIf: {
                uint32_t depth;
                if (!decoder_->readVarU32(&depth)) {
                    error_ = "failed to read br_if depth";
                    return false;
                }
                return emitBrIf(depth);
            }
            case Op::Return:
                return emitReturn();
            case Op::Call: {
                uint32_t funcIndex;
                if (!decoder_->readVarU32(&funcIndex)) {
                    error_ = "failed to read call func index";
                    return false;
                }
                return emitCall(funcIndex);
            }
            case Op::CallIndirect: {
                uint32_t typeIndex, tableIndex;
                if (!decoder_->readVarU32(&typeIndex) || !decoder_->readVarU32(&tableIndex)) {
                    error_ = "failed to read call_indirect indices";
                    return false;
                }
                return emitCallIndirect(typeIndex, tableIndex);
            }
            case Op::Drop:
                return emitDrop();
            case Op::SelectNumeric:
            case Op::SelectTyped:
                return emitSelect();
            case Op::LocalGet: {
                uint32_t localIndex;
                if (!decoder_->readVarU32(&localIndex)) {
                    error_ = "failed to read local index";
                    return false;
                }
                return emitLocalGet(localIndex);
            }
            case Op::LocalSet: {
                uint32_t localIndex;
                if (!decoder_->readVarU32(&localIndex)) {
                    error_ = "failed to read local index";
                    return false;
                }
                return emitLocalSet(localIndex);
            }
            case Op::LocalTee: {
                uint32_t localIndex;
                if (!decoder_->readVarU32(&localIndex)) {
                    error_ = "failed to read local index";
                    return false;
                }
                return emitLocalTee(localIndex);
            }
            case Op::GlobalGet: {
                uint32_t globalIndex;
                if (!decoder_->readVarU32(&globalIndex)) {
                    error_ = "failed to read global index";
                    return false;
                }
                return emitGlobalGet(globalIndex);
            }
            case Op::GlobalSet: {
                uint32_t globalIndex;
                if (!decoder_->readVarU32(&globalIndex)) {
                    error_ = "failed to read global index";
                    return false;
                }
                return emitGlobalSet(globalIndex);
            }
            case Op::I32Const: {
                int32_t value;
                if (!decoder_->readVarS32(&value)) {
                    error_ = "failed to read i32.const";
                    return false;
                }
                return emitI32Const(value);
            }
            case Op::I64Const: {
                int64_t value;
                if (!decoder_->readVarS64(&value)) {
                    error_ = "failed to read i64.const";
                    return false;
                }
                return emitI64Const(value);
            }
            case Op::F32Const: {
                float value;
                if (!decoder_->readFixedF32(&value)) {
                    error_ = "failed to read f32.const";
                    return false;
                }
                return emitF32Const(value);
            }
            case Op::F64Const: {
                double value;
                if (!decoder_->readFixedF64(&value)) {
                    error_ = "failed to read f64.const";
                    return false;
                }
                return emitF64Const(value);
            }
            
            // I32 arithmetic
            case Op::I32Add:    return emitI32Add();
            case Op::I32Sub:    return emitI32Sub();
            case Op::I32Mul:    return emitI32Mul();
            case Op::I32DivS:   return emitI32DivS();
            case Op::I32DivU:   return emitI32DivU();
            case Op::I32RemS:   return emitI32RemS();
            case Op::I32RemU:   return emitI32RemU();
            case Op::I32And:    return emitI32And();
            case Op::I32Or:     return emitI32Or();
            case Op::I32Xor:    return emitI32Xor();
            case Op::I32Shl:    return emitI32Shl();
            case Op::I32ShrS:   return emitI32ShrS();
            case Op::I32ShrU:   return emitI32ShrU();
            case Op::I32Rotl:   return emitI32Rotl();
            case Op::I32Rotr:   return emitI32Rotr();
            case Op::I32Clz:    return emitI32Clz();
            case Op::I32Ctz:    return emitI32Ctz();
            case Op::I32Popcnt: return emitI32Popcnt();
            case Op::I32Eqz:    return emitI32Eqz();
            
            // I32 comparisons
            case Op::I32Eq:     return emitI32Eq();
            case Op::I32Ne:     return emitI32Ne();
            case Op::I32LtS:    return emitI32LtS();
            case Op::I32LtU:    return emitI32LtU();
            case Op::I32GtS:    return emitI32GtS();
            case Op::I32GtU:    return emitI32GtU();
            case Op::I32LeS:    return emitI32LeS();
            case Op::I32LeU:    return emitI32LeU();
            case Op::I32GeS:    return emitI32GeS();
            case Op::I32GeU:    return emitI32GeU();
            
            // I64 arithmetic
            case Op::I64Add:    return emitI64Add();
            case Op::I64Sub:    return emitI64Sub();
            case Op::I64Mul:    return emitI64Mul();
            
            // F32 operations
            case Op::F32Add:    return emitF32Add();
            case Op::F32Sub:    return emitF32Sub();
            case Op::F32Mul:    return emitF32Mul();
            case Op::F32Div:    return emitF32Div();
            case Op::F32Sqrt:   return emitF32Sqrt();
            case Op::F32Abs:    return emitF32Abs();
            case Op::F32Neg:    return emitF32Neg();
            
            // F64 operations
            case Op::F64Add:    return emitF64Add();
            case Op::F64Sub:    return emitF64Sub();
            case Op::F64Mul:    return emitF64Mul();
            case Op::F64Div:    return emitF64Div();
            case Op::F64Sqrt:   return emitF64Sqrt();
            
            // Conversions
            case Op::I32WrapI64:       return emitI32WrapI64();
            case Op::I64ExtendI32S:    return emitI64ExtendI32S();
            case Op::I64ExtendI32U:    return emitI64ExtendI32U();
            case Op::I32TruncF32S:     return emitI32TruncF32S();
            case Op::I32TruncF32U:     return emitI32TruncF32U();
            case Op::I32TruncF64S:     return emitI32TruncF64S();
            case Op::I32TruncF64U:     return emitI32TruncF64U();
            case Op::F32ConvertI32S:   return emitF32ConvertI32S();
            case Op::F32ConvertI32U:   return emitF32ConvertI32U();
            case Op::F64ConvertI32S:   return emitF64ConvertI32S();
            case Op::F64ConvertI32U:   return emitF64ConvertI32U();
            case Op::F32DemoteF64:     return emitF32DemoteF64();
            case Op::F64PromoteF32:    return emitF64PromoteF32();
            case Op::I32ReinterpretF32: return emitI32ReinterpretF32();
            case Op::I64ReinterpretF64: return emitI64ReinterpretF64();
            case Op::F32ReinterpretI32: return emitF32ReinterpretI32();
            case Op::F64ReinterpretI64: return emitF64ReinterpretI64();
            
            // Memory operations - read memarg
            case Op::I32Load:
            case Op::I64Load:
            case Op::F32Load:
            case Op::F64Load:
            case Op::I32Load8S:
            case Op::I32Load8U:
            case Op::I32Load16S:
            case Op::I32Load16U:
            case Op::I64Load8S:
            case Op::I64Load8U:
            case Op::I64Load16S:
            case Op::I64Load16U:
            case Op::I64Load32S:
            case Op::I64Load32U: {
                uint32_t align, offset;
                if (!decoder_->readVarU32(&align) || !decoder_->readVarU32(&offset)) {
                    error_ = "failed to read memarg";
                    return false;
                }
                return emitLoad(primaryOp, align, offset);
            }
            
            case Op::I32Store:
            case Op::I64Store:
            case Op::F32Store:
            case Op::F64Store:
            case Op::I32Store8:
            case Op::I32Store16:
            case Op::I64Store8:
            case Op::I64Store16:
            case Op::I64Store32: {
                uint32_t align, offset;
                if (!decoder_->readVarU32(&align) || !decoder_->readVarU32(&offset)) {
                    error_ = "failed to read memarg";
                    return false;
                }
                return emitStore(primaryOp, align, offset);
            }
            
            case Op::MemorySize: {
                uint32_t memIndex;
                if (!decoder_->readVarU32(&memIndex)) {
                    error_ = "failed to read memory index";
                    return false;
                }
                return emitMemorySize(memIndex);
            }
            
            case Op::MemoryGrow: {
                uint32_t memIndex;
                if (!decoder_->readVarU32(&memIndex)) {
                    error_ = "failed to read memory index";
                    return false;
                }
                return emitMemoryGrow(memIndex);
            }
            
            // SIMD operations (0xFD prefix) - handled by checking if we're in the SIMD range
            // The SIMD prefix byte is handled at the top level by matching the raw opcode value
            default: {
                // Check if this might be a SIMD operation (0xFD prefix byte)
                uint8_t primaryByte = static_cast<uint8_t>(primaryOp);
                if (primaryByte == 0xFD) {
                    uint32_t simdOp;
                    if (!decoder_->readVarU32(&simdOp)) {
                        error_ = "failed to read SIMD opcode";
                        return false;
                    }
                    return emitSimdOp(simdOp);
                }
                error_ = "unsupported opcode: " + std::to_string(static_cast<int>(primaryOp));
                return false;
            }
        }
    }
    
    error_ = "invalid opcode";
    return false;
}

} // namespace Zepra::Wasm

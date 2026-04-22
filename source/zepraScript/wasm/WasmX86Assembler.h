// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmX86Assembler.h
 * @brief x86-64 assembler for baseline JIT
 */

#pragma once

#include "wasm/WasmBaselineInternal.h"

namespace Zepra::Wasm {
// =============================================================================

class X86Assembler {
public:
    explicit X86Assembler(CodeBuffer* buffer) : buf_(buffer) {}
    
    // x86-64 register codes
    enum GPR : uint8_t {
        RAX = 0, RCX = 1, RDX = 2, RBX = 3,
        RSP = 4, RBP = 5, RSI = 6, RDI = 7,
        R8 = 8, R9 = 9, R10 = 10, R11 = 11,
        R12 = 12, R13 = 13, R14 = 14, R15 = 15
    };
    
    enum XMM : uint8_t {
        XMM0 = 0, XMM1 = 1, XMM2 = 2, XMM3 = 3,
        XMM4 = 4, XMM5 = 5, XMM6 = 6, XMM7 = 7,
        XMM8 = 8, XMM9 = 9, XMM10 = 10, XMM11 = 11,
        XMM12 = 12, XMM13 = 13, XMM14 = 14, XMM15 = 15
    };
    
    enum Condition : uint8_t {
        O = 0, NO = 1, B = 2, AE = 3, E = 4, NE = 5, BE = 6, A = 7,
        S = 8, NS = 9, P = 10, NP = 11, L = 12, GE = 13, LE = 14, G = 15
    };
    
    // REX prefix helpers
    void rexW() { buf_->emit8(0x48); }  // 64-bit operand
    void rex(bool w, uint8_t r, uint8_t x, uint8_t b) {
        uint8_t rex = 0x40 | (w ? 8 : 0) | ((r >> 3) << 2) | ((x >> 3) << 1) | (b >> 3);
        if (rex != 0x40) buf_->emit8(rex);
    }
    void rexRB(bool w, uint8_t r, uint8_t b) { rex(w, r, 0, b); }
    
    // ModR/M byte
    void modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
        buf_->emit8((mod << 6) | ((reg & 7) << 3) | (rm & 7));
    }
    
    // Stack operations
    void push(uint8_t reg) {
        if (reg >= 8) buf_->emit8(0x41);
        buf_->emit8(0x50 + (reg & 7));
    }
    
    void pop(uint8_t reg) {
        if (reg >= 8) buf_->emit8(0x41);
        buf_->emit8(0x58 + (reg & 7));
    }
    
    // Move register to register (32-bit)
    void mov32(uint8_t dst, uint8_t src) {
        rexRB(false, src, dst);
        buf_->emit8(0x89);
        modrm(3, src, dst);
    }
    
    // Move register to register (64-bit)
    void mov64(uint8_t dst, uint8_t src) {
        rexRB(true, src, dst);
        buf_->emit8(0x89);
        modrm(3, src, dst);
    }
    
    // Move immediate to register (32-bit)
    void mov32Imm(uint8_t dst, int32_t imm) {
        if (dst >= 8) buf_->emit8(0x41);
        buf_->emit8(0xB8 + (dst & 7));
        buf_->emit32(static_cast<uint32_t>(imm));
    }
    
    // Move immediate to register (64-bit)
    void mov64Imm(uint8_t dst, int64_t imm) {
        rexRB(true, 0, dst);
        buf_->emit8(0xB8 + (dst & 7));
        buf_->emit64(static_cast<uint64_t>(imm));
    }
    
    // ALU operations (32-bit): add/or/adc/sbb/and/sub/xor/cmp
    void alu32(uint8_t op, uint8_t dst, uint8_t src) {
        rexRB(false, src, dst);
        buf_->emit8(0x01 + (op << 3));
        modrm(3, src, dst);
    }
    
    void add32(uint8_t dst, uint8_t src) { alu32(0, dst, src); }
    void or32(uint8_t dst, uint8_t src)  { alu32(1, dst, src); }
    void and32(uint8_t dst, uint8_t src) { alu32(4, dst, src); }
    void sub32(uint8_t dst, uint8_t src) { alu32(5, dst, src); }
    void xor32(uint8_t dst, uint8_t src) { alu32(6, dst, src); }
    void cmp32(uint8_t dst, uint8_t src) { alu32(7, dst, src); }
    
    // TEST: logical AND but only sets flags, doesn't store result
    void test32(uint8_t dst, uint8_t src) {
        rexRB(false, src, dst);
        buf_->emit8(0x85);  // test r/m32, r32
        modrm(3, src, dst);
    }
    
    void test64(uint8_t dst, uint8_t src) {
        rexRB(true, src, dst);
        buf_->emit8(0x85);  // test r/m64, r64
        modrm(3, src, dst);
    }
    
    // ALU operations (64-bit)
    void alu64(uint8_t op, uint8_t dst, uint8_t src) {
        rexRB(true, src, dst);
        buf_->emit8(0x01 + (op << 3));
        modrm(3, src, dst);
    }
    
    void add64(uint8_t dst, uint8_t src) { alu64(0, dst, src); }
    void sub64(uint8_t dst, uint8_t src) { alu64(5, dst, src); }
    void and64(uint8_t dst, uint8_t src) { alu64(4, dst, src); }
    void or64(uint8_t dst, uint8_t src)  { alu64(1, dst, src); }
    void xor64(uint8_t dst, uint8_t src) { alu64(6, dst, src); }
    void cmp64(uint8_t dst, uint8_t src) { alu64(7, dst, src); }
    
    // CMP with immediate (32-bit)
    void cmp32Imm(uint8_t reg, int32_t imm) {
        if (reg >= 8) buf_->emit8(0x41);
        if (imm >= -128 && imm <= 127) {
            buf_->emit8(0x83);
            modrm(3, 7, reg);
            buf_->emit8(static_cast<uint8_t>(imm));
        } else {
            buf_->emit8(0x81);
            modrm(3, 7, reg);
            buf_->emit32(static_cast<uint32_t>(imm));
        }
    }
    
    // CMP with immediate (64-bit)
    void cmp64Imm(uint8_t reg, int32_t imm) {
        if (reg >= 8) buf_->emit8(0x49); else buf_->emit8(0x48);
        if (imm >= -128 && imm <= 127) {
            buf_->emit8(0x83);
            modrm(3, 7, reg);
            buf_->emit8(static_cast<uint8_t>(imm));
        } else {
            buf_->emit8(0x81);
            modrm(3, 7, reg);
            buf_->emit32(static_cast<uint32_t>(imm));
        }
    }
    
    // Multiply (32-bit): imul dst, src
    void imul32(uint8_t dst, uint8_t src) {
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0xAF);
        modrm(3, dst, src);
    }
    
    // Multiply (64-bit)
    void imul64(uint8_t dst, uint8_t src) {
        rexRB(true, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0xAF);
        modrm(3, dst, src);
    }
    
    // Shift operations (32-bit)
    void shl32(uint8_t dst, uint8_t count) {
        if (dst >= 8) buf_->emit8(0x41);
        buf_->emit8(0xC1);
        modrm(3, 4, dst);
        buf_->emit8(count & 0x1F);
    }
    
    void shr32(uint8_t dst, uint8_t count) {
        if (dst >= 8) buf_->emit8(0x41);
        buf_->emit8(0xC1);
        modrm(3, 5, dst);
        buf_->emit8(count & 0x1F);
    }
    
    void sar32(uint8_t dst, uint8_t count) {
        if (dst >= 8) buf_->emit8(0x41);
        buf_->emit8(0xC1);
        modrm(3, 7, dst);
        buf_->emit8(count & 0x1F);
    }
    
    // Shift by CL (32-bit)
    void shl32_cl(uint8_t dst) {
        if (dst >= 8) buf_->emit8(0x41);
        buf_->emit8(0xD3);
        modrm(3, 4, dst);
    }
    
    void shr32_cl(uint8_t dst) {
        if (dst >= 8) buf_->emit8(0x41);
        buf_->emit8(0xD3);
        modrm(3, 5, dst);
    }
    
    void sar32_cl(uint8_t dst) {
        if (dst >= 8) buf_->emit8(0x41);
        buf_->emit8(0xD3);
        modrm(3, 7, dst);
    }
    
    // Set byte on condition
    void setcc(Condition cc, uint8_t dst) {
        if (dst >= 8) buf_->emit8(0x41);
        buf_->emit8(0x0F);
        buf_->emit8(0x90 + cc);
        modrm(3, 0, dst);
    }
    
    // Movzx (8 to 32)
    void movzx8to32(uint8_t dst, uint8_t src) {
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0xB6);
        modrm(3, dst, src);
    }
    
    // Memory load (32-bit): mov dst, [base + offset]
    void load32(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(false, dst, base);
        buf_->emit8(0x8B);
        if (offset == 0 && (base & 7) != RBP) {
            modrm(0, dst, base);
        } else if (offset >= -128 && offset <= 127) {
            modrm(1, dst, base);
            buf_->emit8(static_cast<uint8_t>(offset));
        } else {
            modrm(2, dst, base);
            buf_->emit32(static_cast<uint32_t>(offset));
        }
        if ((base & 7) == RSP) buf_->emit8(0x24);  // SIB byte
    }
    
    // Memory store (32-bit): mov [base + offset], src
    void store32(uint8_t base, int32_t offset, uint8_t src) {
        rexRB(false, src, base);
        buf_->emit8(0x89);
        if (offset == 0 && (base & 7) != RBP) {
            modrm(0, src, base);
        } else if (offset >= -128 && offset <= 127) {
            modrm(1, src, base);
            buf_->emit8(static_cast<uint8_t>(offset));
        } else {
            modrm(2, src, base);
            buf_->emit32(static_cast<uint32_t>(offset));
        }
        if ((base & 7) == RSP) buf_->emit8(0x24);
    }
    
    // Jump to relative offset
    size_t jmp32() {
        buf_->emit8(0xE9);
        size_t patchOffset = buf_->offset();
        buf_->emit32(0);  // Placeholder
        return patchOffset;
    }
    
    // Conditional jump
    size_t jcc32(Condition cc) {
        buf_->emit8(0x0F);
        buf_->emit8(0x80 + cc);
        size_t patchOffset = buf_->offset();
        buf_->emit32(0);
        return patchOffset;
    }
    
    // Patch jump target
    void patchJump(size_t patchOffset, size_t target) {
        int32_t rel = static_cast<int32_t>(target - (patchOffset + 4));
        buf_->patch32(patchOffset, static_cast<uint32_t>(rel));
    }
    
    // Return
    void ret() { buf_->emit8(0xC3); }
    
    // Int3 (debug break / trap)
    void int3() { buf_->emit8(0xCC); }
    
    // CDQ: sign-extend EAX to EDX:EAX
    void cdq() { buf_->emit8(0x99); }
    
    // CQO: sign-extend RAX to RDX:RAX (64-bit)
    void cqo() {
        buf_->emit8(0x48);
        buf_->emit8(0x99);
    }
    
    // IDIV: signed divide EDX:EAX by r32, result in EAX, remainder in EDX
    void idiv32(uint8_t src) {
        if (src >= 8) buf_->emit8(0x41);
        buf_->emit8(0xF7);
        modrm(3, 7, src);
    }
    
    // DIV: unsigned divide EDX:EAX by r32
    void div32(uint8_t src) {
        if (src >= 8) buf_->emit8(0x41);
        buf_->emit8(0xF7);
        modrm(3, 6, src);
    }
    
    // IDIV64: signed divide RDX:RAX by r64
    void idiv64(uint8_t src) {
        rexRB(true, 0, src);
        buf_->emit8(0xF7);
        modrm(3, 7, src);
    }
    
    // DIV64: unsigned divide RDX:RAX by r64
    void div64(uint8_t src) {
        rexRB(true, 0, src);
        buf_->emit8(0xF7);
        modrm(3, 6, src);
    }
    
    // NEG32: negate register
    void neg32(uint8_t dst) {
        if (dst >= 8) buf_->emit8(0x41);
        buf_->emit8(0xF7);
        modrm(3, 3, dst);
    }
    
    // NEG64: negate register 64-bit
    void neg64(uint8_t dst) {
        rexRB(true, 0, dst);
        buf_->emit8(0xF7);
        modrm(3, 3, dst);
    }
    
    // ROL32 by CL
    void rol32_cl(uint8_t dst) {
        if (dst >= 8) buf_->emit8(0x41);
        buf_->emit8(0xD3);
        modrm(3, 0, dst);
    }
    
    // ROR32 by CL
    void ror32_cl(uint8_t dst) {
        if (dst >= 8) buf_->emit8(0x41);
        buf_->emit8(0xD3);
        modrm(3, 1, dst);
    }
    
    // ROL64 by CL
    void rol64_cl(uint8_t dst) {
        rexRB(true, 0, dst);
        buf_->emit8(0xD3);
        modrm(3, 0, dst);
    }
    
    // ROR64 by CL
    void ror64_cl(uint8_t dst) {
        rexRB(true, 0, dst);
        buf_->emit8(0xD3);
        modrm(3, 1, dst);
    }
    
    // BSR: bit scan reverse (find MSB)
    void bsr32(uint8_t dst, uint8_t src) {
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0xBD);
        modrm(3, dst, src);
    }
    
    // BSF: bit scan forward (find LSB)
    void bsf32(uint8_t dst, uint8_t src) {
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0xBC);
        modrm(3, dst, src);
    }
    
    // LZCNT: count leading zeros (requires BMI1)
    void lzcnt32(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0xBD);
        modrm(3, dst, src);
    }
    
    // TZCNT: count trailing zeros (requires BMI1)
    void tzcnt32(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0xBC);
        modrm(3, dst, src);
    }
    
    // POPCNT: population count
    void popcnt32(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0xB8);
        modrm(3, dst, src);
    }
    
    // POPCNT64
    void popcnt64(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(true, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0xB8);
        modrm(3, dst, src);
    }
    
    // XOR32 with immediate 0 (clear register)
    void xor32Self(uint8_t dst) {
        rexRB(false, dst, dst);
        buf_->emit8(0x31);
        modrm(3, dst, dst);
    }
    
    // SSE: sqrtss
    void sqrtss(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x51);
        modrm(3, dst, src);
    }
    
    // SSE: sqrtsd
    void sqrtsd(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x51);
        modrm(3, dst, src);
    }
    
    // CVTSI2SS: convert i32 to f32
    void cvtsi2ss(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x2A);
        modrm(3, dst, src);
    }
    
    // CVTSI2SD: convert i32 to f64
    void cvtsi2sd(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x2A);
        modrm(3, dst, src);
    }
    
    // CVTTSS2SI: truncate f32 to i32
    void cvttss2si(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x2C);
        modrm(3, dst, src);
    }
    
    // CVTTSD2SI: truncate f64 to i32
    void cvttsd2si(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x2C);
        modrm(3, dst, src);
    }
    
    // CVTSS2SD: f32 to f64
    void cvtss2sd(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x5A);
        modrm(3, dst, src);
    }
    
    // CVTSD2SS: f64 to f32
    void cvtsd2ss(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x5A);
        modrm(3, dst, src);
    }
    
    // MOVSXD: sign-extend i32 to i64
    void movsxd(uint8_t dst, uint8_t src) {
        rexRB(true, dst, src);
        buf_->emit8(0x63);
        modrm(3, dst, src);
    }
    
    // SSE2: movss (32-bit float)
    void movss(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x10);
        modrm(3, dst, src);
    }
    
    // SSE2: movsd (64-bit float)
    void movsd(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x10);
        modrm(3, dst, src);
    }
    
    // SSE2: addss
    void addss(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x58);
        modrm(3, dst, src);
    }
    
    // SSE2: subss
    void subss(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x5C);
        modrm(3, dst, src);
    }
    
    // SSE2: mulss
    void mulss(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x59);
        modrm(3, dst, src);
    }
    
    // SSE2: divss  
    void divss(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x5E);
        modrm(3, dst, src);
    }
    
    // SSE2: addsd
    void addsd(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x58);
        modrm(3, dst, src);
    }
    
    // SSE2: subsd
    void subsd(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x5C);
        modrm(3, dst, src);
    }
    
    // SSE2: mulsd
    void mulsd(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x59);
        modrm(3, dst, src);
    }
    
    // SSE2: divsd
    void divsd(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x5E);
        modrm(3, dst, src);
    }
    
    // SSE: minss
    void minss(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x5D);
        modrm(3, dst, src);
    }
    
    // SSE: maxss
    void maxss(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x5F);
        modrm(3, dst, src);
    }
    
    // SSE2: minsd
    void minsd(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x5D);
        modrm(3, dst, src);
    }
    
    // SSE2: maxsd
    void maxsd(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x5F);
        modrm(3, dst, src);
    }
    
    // SSE: andps (bitwise AND for f32 sign manipulation)
    void andps(uint8_t dst, uint8_t src) {
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x54);
        modrm(3, dst, src);
    }
    
    // SSE: orps (bitwise OR for f32)
    void orps(uint8_t dst, uint8_t src) {
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x56);
        modrm(3, dst, src);
    }
    
    // SSE: xorps (bitwise XOR for f32 neg)
    void xorps(uint8_t dst, uint8_t src) {
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x57);
        modrm(3, dst, src);
    }
    
    // SSE2: andpd (bitwise AND for f64)
    void andpd(uint8_t dst, uint8_t src) {
        buf_->emit8(0x66);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x54);
        modrm(3, dst, src);
    }
    
    // SSE2: xorpd (bitwise XOR for f64 neg)
    void xorpd(uint8_t dst, uint8_t src) {
        buf_->emit8(0x66);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x57);
        modrm(3, dst, src);
    }
    
    // SSE4.1: roundss (round f32)
    // mode: 0=nearest, 1=floor, 2=ceil, 3=trunc
    void roundss(uint8_t dst, uint8_t src, uint8_t mode) {
        buf_->emit8(0x66);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x3A);
        buf_->emit8(0x0A);
        modrm(3, dst, src);
        buf_->emit8(mode);
    }
    
    // SSE4.1: roundsd (round f64)
    void roundsd(uint8_t dst, uint8_t src, uint8_t mode) {
        buf_->emit8(0x66);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x3A);
        buf_->emit8(0x0B);
        modrm(3, dst, src);
        buf_->emit8(mode);
    }
    
    // UCOMISS: unordered compare f32 (sets flags)
    void ucomiss(uint8_t dst, uint8_t src) {
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x2E);
        modrm(3, dst, src);
    }
    
    // UCOMISD: unordered compare f64 (sets flags)
    void ucomisd(uint8_t dst, uint8_t src) {
        buf_->emit8(0x66);
        rexRB(false, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x2E);
        modrm(3, dst, src);
    }
    
    // 64-bit shift by CL
    void shl64_cl(uint8_t dst) {
        rexRB(true, 0, dst);
        buf_->emit8(0xD3);
        modrm(3, 4, dst);
    }
    
    void shr64_cl(uint8_t dst) {
        rexRB(true, 0, dst);
        buf_->emit8(0xD3);
        modrm(3, 5, dst);
    }
    
    void sar64_cl(uint8_t dst) {
        rexRB(true, 0, dst);
        buf_->emit8(0xD3);
        modrm(3, 7, dst);
    }
    
    // 64-bit lzcnt/tzcnt
    void lzcnt64(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(true, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0xBD);
        modrm(3, dst, src);
    }
    
    void tzcnt64(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(true, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0xBC);
        modrm(3, dst, src);
    }
    
    // 64-bit conversions
    void cvttss2si64(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(true, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x2C);
        modrm(3, dst, src);
    }
    
    void cvttsd2si64(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(true, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x2C);
        modrm(3, dst, src);
    }
    
    void cvtsi2ss64(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(true, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x2A);
        modrm(3, dst, src);
    }
    
    void cvtsi2sd64(uint8_t dst, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(true, dst, src);
        buf_->emit8(0x0F);
        buf_->emit8(0x2A);
        modrm(3, dst, src);
    }
    
    // MOVD: move 32 bits between GPR and XMM
    void movd_gpr2xmm(uint8_t xmm, uint8_t gpr) {
        buf_->emit8(0x66);
        rexRB(false, xmm, gpr);
        buf_->emit8(0x0F);
        buf_->emit8(0x6E);
        modrm(3, xmm, gpr);
    }
    
    void movd_xmm2gpr(uint8_t gpr, uint8_t xmm) {
        buf_->emit8(0x66);
        rexRB(false, xmm, gpr);
        buf_->emit8(0x0F);
        buf_->emit8(0x7E);
        modrm(3, xmm, gpr);
    }
    
    // MOVQ: move 64 bits between GPR and XMM
    void movq_gpr2xmm(uint8_t xmm, uint8_t gpr) {
        buf_->emit8(0x66);
        rexRB(true, xmm, gpr);
        buf_->emit8(0x0F);
        buf_->emit8(0x6E);
        modrm(3, xmm, gpr);
    }
    
    void movq_xmm2gpr(uint8_t gpr, uint8_t xmm) {
        buf_->emit8(0x66);
        rexRB(true, xmm, gpr);
        buf_->emit8(0x0F);
        buf_->emit8(0x7E);
        modrm(3, xmm, gpr);
    }
    
    // 64-bit memory load
    void load64(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(true, dst, base);
        buf_->emit8(0x8B);
        if (offset == 0 && (base & 7) != RBP) {
            modrm(0, dst, base);
        } else if (offset >= -128 && offset <= 127) {
            modrm(1, dst, base);
            buf_->emit8(static_cast<uint8_t>(offset));
        } else {
            modrm(2, dst, base);
            buf_->emit32(static_cast<uint32_t>(offset));
        }
        if ((base & 7) == RSP) buf_->emit8(0x24);
    }
    
    // 64-bit memory store
    void store64(uint8_t base, int32_t offset, uint8_t src) {
        rexRB(true, src, base);
        buf_->emit8(0x89);
        if (offset == 0 && (base & 7) != RBP) {
            modrm(0, src, base);
        } else if (offset >= -128 && offset <= 127) {
            modrm(1, src, base);
            buf_->emit8(static_cast<uint8_t>(offset));
        } else {
            modrm(2, src, base);
            buf_->emit32(static_cast<uint32_t>(offset));
        }
        if ((base & 7) == RSP) buf_->emit8(0x24);
    }
    
    // LEA64: Load effective address with base + index*scale + disp
    void lea64(uint8_t dst, uint8_t base, uint8_t index, uint8_t scale, int32_t disp) {
        // REX.W prefix for 64-bit operand
        uint8_t rex = 0x48;
        if (dst >= 8) rex |= 0x04;  // REX.R
        if (index >= 8) rex |= 0x02; // REX.X
        if (base >= 8) rex |= 0x01;  // REX.B
        buf_->emit8(rex);
        buf_->emit8(0x8D);  // LEA
        
        // SIB addressing mode (base + index*scale + disp)
        uint8_t ss = 0;
        switch (scale) {
            case 1: ss = 0; break;
            case 2: ss = 1; break;
            case 4: ss = 2; break;
            case 8: ss = 3; break;
        }
        
        if (disp == 0 && (base & 7) != RBP) {
            modrm(0, dst, 4);  // SIB follows
            buf_->emit8((ss << 6) | ((index & 7) << 3) | (base & 7));
        } else if (disp >= -128 && disp <= 127) {
            modrm(1, dst, 4);  // SIB + disp8
            buf_->emit8((ss << 6) | ((index & 7) << 3) | (base & 7));
            buf_->emit8(static_cast<uint8_t>(disp));
        } else {
            modrm(2, dst, 4);  // SIB + disp32
            buf_->emit8((ss << 6) | ((index & 7) << 3) | (base & 7));
            buf_->emit32(static_cast<uint32_t>(disp));
        }
    }
    
    // CALL indirect through register
    void call(uint8_t reg) {
        if (reg >= 8) buf_->emit8(0x41);  // REX.B for extended registers
        buf_->emit8(0xFF);
        modrm(3, 2, reg);  // ModR/M: mod=11, reg=2 (opcode ext), r/m=reg
    }
    
    // CALL relative
    size_t callRel32() {
        buf_->emit8(0xE8);
        size_t patchOffset = buf_->offset();
        buf_->emit32(0);  // Placeholder
        return patchOffset;
    }
    
    // Load with sign/zero extension
    void load8s(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(false, dst, base);
        buf_->emit8(0x0F);
        buf_->emit8(0xBE);
        modrm(0, dst, base);
    }
    
    void load8u(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(false, dst, base);
        buf_->emit8(0x0F);
        buf_->emit8(0xB6);
        modrm(0, dst, base);
    }
    
    void load16s(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(false, dst, base);
        buf_->emit8(0x0F);
        buf_->emit8(0xBF);
        modrm(0, dst, base);
    }
    
    void load16u(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(false, dst, base);
        buf_->emit8(0x0F);
        buf_->emit8(0xB7);
        modrm(0, dst, base);
    }
    
    // 64-bit loads with extension
    void load8s64(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(true, dst, base);
        buf_->emit8(0x0F);
        buf_->emit8(0xBE);
        modrm(0, dst, base);
    }
    
    void load8u64(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(true, dst, base);
        buf_->emit8(0x0F);
        buf_->emit8(0xB6);
        modrm(0, dst, base);
    }
    
    void load16s64(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(true, dst, base);
        buf_->emit8(0x0F);
        buf_->emit8(0xBF);
        modrm(0, dst, base);
    }
    
    void load16u64(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(true, dst, base);
        buf_->emit8(0x0F);
        buf_->emit8(0xB7);
        modrm(0, dst, base);
    }
    
    void load32s64(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(true, dst, base);
        buf_->emit8(0x63);
        modrm(0, dst, base);
    }
    
    void load32u64(uint8_t dst, uint8_t base, int32_t offset) {
        rexRB(false, dst, base);
        buf_->emit8(0x8B);
        modrm(0, dst, base);
    }
    
    // Byte/word stores
    void store8(uint8_t base, int32_t offset, uint8_t src) {
        if (src >= 4) rexRB(false, src, base);
        buf_->emit8(0x88);
        modrm(0, src, base);
    }
    
    void store16(uint8_t base, int32_t offset, uint8_t src) {
        buf_->emit8(0x66);
        rexRB(false, src, base);
        buf_->emit8(0x89);
        modrm(0, src, base);
    }
    
    // SSE memory loads/stores
    void loadss(uint8_t dst, uint8_t base, int32_t offset) {
        buf_->emit8(0xF3);
        rexRB(false, dst, base);
        buf_->emit8(0x0F);
        buf_->emit8(0x10);
        modrm(0, dst, base);
    }
    
    void loadsd(uint8_t dst, uint8_t base, int32_t offset) {
        buf_->emit8(0xF2);
        rexRB(false, dst, base);
        buf_->emit8(0x0F);
        buf_->emit8(0x10);
        modrm(0, dst, base);
    }
    
    void storess(uint8_t base, int32_t offset, uint8_t src) {
        buf_->emit8(0xF3);
        rexRB(false, src, base);
        buf_->emit8(0x0F);
        buf_->emit8(0x11);
        modrm(0, src, base);
    }
    
    void storesd(uint8_t base, int32_t offset, uint8_t src) {
        buf_->emit8(0xF2);
        rexRB(false, src, base);
        buf_->emit8(0x0F);
        buf_->emit8(0x11);
        modrm(0, src, base);
    }
    
    // Add immediate to 64-bit register
    void add64Imm(uint8_t dst, int32_t imm) {
        rexRB(true, 0, dst);
        if (imm >= -128 && imm <= 127) {
            buf_->emit8(0x83);
            modrm(3, 0, dst);
            buf_->emit8(static_cast<uint8_t>(imm));
        } else {
            buf_->emit8(0x81);
            modrm(3, 0, dst);
            buf_->emit32(static_cast<uint32_t>(imm));
        }
    }
    
    // String operations for memory.copy / memory.fill
    void cld() {
        buf_->emit8(0xFC);  // CLD - Clear Direction Flag
    }
    
    void repMovsb() {
        buf_->emit8(0xF3);  // REP prefix
        buf_->emit8(0xA4);  // MOVSB
    }
    
    void repStosb() {
        buf_->emit8(0xF3);  // REP prefix
        buf_->emit8(0xAA);  // STOSB
    }
    
    // OR immediate to 32-bit register
    void or32Imm(uint8_t dst, int32_t imm) {
        rexRB(false, 0, dst);
        if (imm >= -128 && imm <= 127) {
            buf_->emit8(0x83);
            modrm(3, 1, dst);  // /1 = OR
            buf_->emit8(static_cast<uint8_t>(imm));
        } else {
            buf_->emit8(0x81);
            modrm(3, 1, dst);
            buf_->emit32(static_cast<uint32_t>(imm));
        }
    }
    
    // AND immediate to 32-bit register
    void and32Imm(uint8_t dst, int32_t imm) {
        rexRB(false, 0, dst);
        if (imm >= -128 && imm <= 127) {
            buf_->emit8(0x83);
            modrm(3, 4, dst);  // /4 = AND
            buf_->emit8(static_cast<uint8_t>(imm));
        } else {
            buf_->emit8(0x81);
            modrm(3, 4, dst);
        }
    }
    
    // JMP relative 32-bit
    void jmp(int32_t offset) {
        buf_->emit8(0xE9);  // JMP rel32
        buf_->emit32(static_cast<uint32_t>(offset - 5));  // Offset from end of instruction
    }
    
    // JMP to register (indirect)
    void jmpReg(uint8_t reg) {
        if (reg >= 8) {
            buf_->emit8(0x41);  // REX.B prefix for extended registers
        }
        buf_->emit8(0xFF);
        modrm(3, 4, reg & 7);  // /4 = JMP
    }
    
    // ADD to 32-bit register with immediate
    void add32Imm(uint8_t dst, int32_t imm) {
        rexRB(false, 0, dst);
        if (imm >= -128 && imm <= 127) {
            buf_->emit8(0x83);
            modrm(3, 0, dst);  // /0 = ADD
            buf_->emit8(static_cast<uint8_t>(imm));
        } else {
            buf_->emit8(0x81);
            modrm(3, 0, dst);
            buf_->emit32(static_cast<uint32_t>(imm));
        }
    }
    
    // MFENCE memory fence
    void mfence() {
        buf_->emit8(0x0F);
        buf_->emit8(0xAE);
        buf_->emit8(0xF0);
    }
    
    // LOCK prefix for atomic operations
    void lockPrefix() {
        buf_->emit8(0xF0);
    }
    
    // CMPXCHG
    void cmpxchg32(uint8_t dst, uint8_t src) {
        buf_->emit8(0x0F);
        buf_->emit8(0xB1);
        modrm(3, src, dst);
    }
    
    void cmpxchg64(uint8_t dst, uint8_t src) {
        rexRB(true, src, dst);
        buf_->emit8(0x0F);
        buf_->emit8(0xB1);
        modrm(3, src, dst);
    }
    
    // XADD - exchange and add
    void xadd32(uint8_t dst, uint8_t src) {
        buf_->emit8(0x0F);
        buf_->emit8(0xC1);
        modrm(3, src, dst);
    }
    
    void xadd64(uint8_t dst, uint8_t src) {
        rexRB(true, src, dst);
        buf_->emit8(0x0F);
        buf_->emit8(0xC1);
        modrm(3, src, dst);
    }
    
    // XCHG - exchange
    void xchg32(uint8_t dst, uint8_t src) {
        buf_->emit8(0x87);
        modrm(3, src, dst);
    }
    
    void xchg64(uint8_t dst, uint8_t src) {
        rexRB(true, src, dst);
        buf_->emit8(0x87);
        modrm(3, src, dst);
    }
    
    // Push immediate 32-bit value
    void pushImm32(int32_t imm) {
        if (imm >= -128 && imm <= 127) {
            buf_->emit8(0x6A);  // push imm8
            buf_->emit8(static_cast<uint8_t>(imm));
        } else {
            buf_->emit8(0x68);  // push imm32
            buf_->emit32(static_cast<uint32_t>(imm));
        }
    }
    
    // Pop to register
    void popReg(uint8_t reg) {
        if (reg >= 8) {
            buf_->emit8(0x41);  // REX.B
        }
        buf_->emit8(0x58 + (reg & 7));  // pop r64
    }
    
    // ==========================================================================
    // Relaxed SIMD Operations (WASM Relaxed SIMD Proposal)
    // ==========================================================================
    
    // VEX-encoded FMA3 vfmadd132ps xmm, xmm, xmm (fused multiply-add)
    // vd = vd * vm + vn
    void vfmadd132ps(uint8_t vd, uint8_t vn, uint8_t vm) {
        // VEX.128.66.0F38.W0 98 /r VFMADD132PS
        vex3(0x66, 0x0F38, false, vd, vn);
        buf_->emit8(0x98);
        modrm(3, vd, vm);
    }
    
    // VEX-encoded FMA3 vfmadd213ps xmm, xmm, xmm
    // vd = vn * vd + vm
    void vfmadd213ps(uint8_t vd, uint8_t vn, uint8_t vm) {
        vex3(0x66, 0x0F38, false, vd, vn);
        buf_->emit8(0xA8);
        modrm(3, vd, vm);
    }
    
    // VEX-encoded FMA3 vfmsub132ps (fused multiply-subtract)
    void vfmsub132ps(uint8_t vd, uint8_t vn, uint8_t vm) {
        vex3(0x66, 0x0F38, false, vd, vn);
        buf_->emit8(0x9A);
        modrm(3, vd, vm);
    }
    
    // PSHUFB - packed shuffle bytes (swizzle)
    void pshufb(uint8_t vd, uint8_t vm) {
        buf_->emit8(0x66);
        buf_->emit8(0x0F);
        buf_->emit8(0x38);
        buf_->emit8(0x00);
        modrm(3, vd, vm);
    }
    
    // MINPS - relaxed minimum (may differ in NaN handling)
    void minps(uint8_t vd, uint8_t vm) {
        buf_->emit8(0x0F);
        buf_->emit8(0x5D);
        modrm(3, vd, vm);
    }
    
    // MAXPS - relaxed maximum
    void maxps(uint8_t vd, uint8_t vm) {
        buf_->emit8(0x0F);
        buf_->emit8(0x5F);
        modrm(3, vd, vm);
    }
    
    // Helper to emit 3-byte VEX prefix
    void vex3(uint8_t pp, uint16_t mmmmm, bool W, uint8_t vd, uint8_t vvvv) {
        buf_->emit8(0xC4);  // 3-byte VEX
        uint8_t b1 = 0;
        if (mmmmm == 0x0F) b1 = 0x01;
        else if (mmmmm == 0x0F38) b1 = 0x02;
        else if (mmmmm == 0x0F3A) b1 = 0x03;
        b1 |= ((~vd & 0x8) << 4);  // R bit
        buf_->emit8(b1);
        uint8_t b2 = ((~vvvv & 0xF) << 3);
        if (pp == 0x66) b2 |= 0x01;
        if (W) b2 |= 0x80;
        buf_->emit8(b2);
    }
    
    size_t offset() const { return buf_->offset(); }
    
private:
    CodeBuffer* buf_;
};

} // namespace Zepra::Wasm

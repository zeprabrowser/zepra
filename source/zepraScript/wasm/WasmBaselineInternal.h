// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmBaselineInternal.h
 * @brief Internal shared header for baseline compiler split files
 *
 * Included by WasmBaselineCompiler.cpp, WasmBaselineOpcodes.cpp,
 * and WasmBaselineBulkGC.cpp. Not for external consumption.
 */

#pragma once

#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmCpuFeatures.h"
#include "wasm/WasmTypeDef.h"
#include <cstring>
#include <algorithm>

namespace Zepra::Wasm {

// Bulk Memory opcodes (0xFC prefix)
namespace BulkOp {
    constexpr uint8_t MEMORY_INIT = 0x08;
    constexpr uint8_t DATA_DROP = 0x09;
    constexpr uint8_t MEMORY_COPY = 0x0A;
    constexpr uint8_t MEMORY_FILL = 0x0B;
    constexpr uint8_t TABLE_INIT = 0x0C;
    constexpr uint8_t ELEM_DROP = 0x0D;
    constexpr uint8_t TABLE_COPY = 0x0E;
    constexpr uint8_t TABLE_GROW = 0x0F;
    constexpr uint8_t TABLE_SIZE = 0x10;
    constexpr uint8_t TABLE_FILL = 0x11;
}

// GC opcodes (0xFB prefix)
namespace GCOp {
    constexpr uint8_t STRUCT_NEW = 0x00;
    constexpr uint8_t STRUCT_NEW_DEFAULT = 0x01;
    constexpr uint8_t STRUCT_GET = 0x02;
    constexpr uint8_t STRUCT_GET_S = 0x03;
    constexpr uint8_t STRUCT_GET_U = 0x04;
    constexpr uint8_t STRUCT_SET = 0x05;
    constexpr uint8_t ARRAY_NEW = 0x06;
    constexpr uint8_t ARRAY_NEW_DEFAULT = 0x07;
    constexpr uint8_t ARRAY_NEW_FIXED = 0x08;
    constexpr uint8_t ARRAY_NEW_DATA = 0x09;
    constexpr uint8_t ARRAY_NEW_ELEM = 0x0A;
    constexpr uint8_t ARRAY_GET = 0x0B;
    constexpr uint8_t ARRAY_GET_S = 0x0C;
    constexpr uint8_t ARRAY_GET_U = 0x0D;
    constexpr uint8_t ARRAY_SET = 0x0E;
    constexpr uint8_t ARRAY_LEN = 0x0F;
    constexpr uint8_t ARRAY_FILL = 0x10;
    constexpr uint8_t ARRAY_COPY = 0x11;
    constexpr uint8_t REF_TEST = 0x14;
    constexpr uint8_t REF_TEST_NULL = 0x15;
    constexpr uint8_t REF_CAST = 0x16;
    constexpr uint8_t REF_CAST_NULL = 0x17;
    constexpr uint8_t BR_ON_CAST = 0x18;
    constexpr uint8_t BR_ON_CAST_FAIL = 0x19;
    constexpr uint8_t I31_NEW = 0x1C;
    constexpr uint8_t I31_GET_S = 0x1D;
    constexpr uint8_t I31_GET_U = 0x1E;
    constexpr uint8_t REF_NULL = 0xD0;
    constexpr uint8_t REF_IS_NULL = 0xD1;
    constexpr uint8_t REF_AS_NON_NULL = 0xD3;
    constexpr uint8_t REF_EQ = 0xD4;
}

// CodeBuffer — inline byte buffer for JIT emission
class CodeBuffer {
public:
    CodeBuffer() { buffer_.reserve(4096); }

    void emit8(uint8_t byte) { buffer_.push_back(byte); }
    void emit16(uint16_t val) {
        emit8(val & 0xFF);
        emit8((val >> 8) & 0xFF);
    }
    void emit32(uint32_t val) {
        emit8(val & 0xFF);
        emit8((val >> 8) & 0xFF);
        emit8((val >> 16) & 0xFF);
        emit8((val >> 24) & 0xFF);
    }
    void emit64(uint64_t val) {
        emit32(val & 0xFFFFFFFF);
        emit32((val >> 32) & 0xFFFFFFFF);
    }

    size_t offset() const { return buffer_.size(); }
    uint8_t* data() { return buffer_.data(); }
    const uint8_t* data() const { return buffer_.data(); }
    size_t size() const { return buffer_.size(); }

    std::vector<uint8_t> takeCode() { return std::move(buffer_); }

    void patch32(size_t offset, uint32_t val) {
        if (offset + 4 <= buffer_.size()) {
            buffer_[offset] = val & 0xFF;
            buffer_[offset + 1] = (val >> 8) & 0xFF;
            buffer_[offset + 2] = (val >> 16) & 0xFF;
            buffer_[offset + 3] = (val >> 24) & 0xFF;
        }
    }

    void patchByte(size_t offset, uint8_t val) {
        if (offset < buffer_.size()) {
            buffer_[offset] = val;
        }
    }

private:
    std::vector<uint8_t> buffer_;
};

} // namespace Zepra::Wasm

// Assembler class definitions (all-inline)
#include "wasm/WasmX86Assembler.h"
#include "wasm/WasmARM64Assembler.h"

namespace Zepra::Wasm {

// Platform selection
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    using PlatformAssembler = X86Assembler;
    constexpr bool IsX64Platform = true;
    constexpr bool IsARM64Platform = false;
#elif defined(__aarch64__) || defined(_M_ARM64)
    using PlatformAssembler = ARM64Assembler;
    constexpr bool IsX64Platform = false;
    constexpr bool IsARM64Platform = true;
#else
    using PlatformAssembler = X86Assembler;
    constexpr bool IsX64Platform = true;
    constexpr bool IsARM64Platform = false;
#endif

#define EMIT_X64(...) do { if constexpr (IsX64Platform) { __VA_ARGS__ } } while(0)
#define EMIT_ARM64(...) do { if constexpr (IsARM64Platform) { __VA_ARGS__ } } while(0)

} // namespace Zepra::Wasm

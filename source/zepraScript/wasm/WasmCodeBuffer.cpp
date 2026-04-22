// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmCodeBuffer.cpp
 * @brief Platform support check
 */

#include "wasm/WasmBaselineInternal.h"

namespace Zepra::Wasm {

bool baselinePlatformSupport() {
#if defined(__x86_64__) || defined(_M_X64)
    return true;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return true;
#elif defined(__i386__) || defined(_M_IX86)
    return true;
#elif defined(__arm__) || defined(_M_ARM)
    return true;
#else
    return false;
#endif
}

} // namespace Zepra::Wasm

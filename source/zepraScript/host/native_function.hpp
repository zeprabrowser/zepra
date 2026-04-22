// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file native_function.hpp
 * @brief C++ ↔ JS native function binding declarations
 */

#pragma once

#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include <string>

namespace Zepra::Host {

void registerGlobalFunction(Runtime::Object* global, const std::string& name,
                            Runtime::NativeFn callback, size_t arity);

void registerMethod(Runtime::Object* prototype, const std::string& name,
                    Runtime::NativeFn callback, size_t arity);

} // namespace Zepra::Host

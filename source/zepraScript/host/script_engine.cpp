// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file script_engine.cpp
 * @brief Embedding API entry point
 *
 * Top-level interface for embedding ZebraScript in ZepraBrowser
 * or other host applications. Manages the full pipeline:
 *   source → lex → parse → compile → execute → result
 */

#include "config.hpp"
#include "runtime/objects/value.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/object.hpp"
#include "frontend/source_code.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/syntax_checker.hpp"
#include "bytecode/bytecode_generator.hpp"
#include "safety/CrashBoundary.h"
#include <string>
#include <memory>
#include <stdexcept>

namespace Zepra::Host {

using Runtime::Value;
using Runtime::VM;

/**
 * Evaluate a JavaScript source string and return the result.
 * Wrapped with CrashBoundary for OOM/trap containment.
 */
Value evaluate(VM* vm, const std::string& source, const std::string& filename) {
    if (!vm) {
        throw std::runtime_error("VM is null");
    }

    Value returnValue;

    auto boundary = Safety::CrashBoundary::execute([&]() {
        // 1. Create SourceCode object
        auto sourceCode = Frontend::SourceCode::fromString(source, filename);

        // 2. Parse
        Frontend::Parser parser(sourceCode.get());
        auto ast = parser.parseProgram();

        if (parser.hasErrors()) {
            std::string errors;
            for (const auto& err : parser.errors()) {
                if (!errors.empty()) errors += "\n";
                errors += err;
            }
            throw std::runtime_error("SyntaxError: " + errors);
        }

        // 3. Syntax check
        Frontend::SyntaxChecker checker;
        if (!checker.check(ast.get())) {
            std::string errors;
            for (const auto& err : checker.errors()) {
                if (!errors.empty()) errors += "\n";
                errors += err;
            }
            throw std::runtime_error("SyntaxError: " + errors);
        }

        // 4. Compile to bytecode
        Bytecode::BytecodeGenerator generator;
        auto chunk = generator.compile(ast.get());

        if (generator.hasErrors()) {
            std::string errors;
            for (const auto& err : generator.errors()) {
                if (!errors.empty()) errors += "\n";
                errors += err;
            }
            throw std::runtime_error("CompileError: " + errors);
        }

        // 5. Execute
        auto result = vm->execute(chunk.get());

        if (result.status == Runtime::ExecutionResult::Status::Exception) {
            throw std::runtime_error(result.error);
        }

        returnValue = result.value;
    });

    // Convert CrashBoundary failures to runtime_error
    if (boundary.result != Safety::CrashBoundary::Result::Success &&
        boundary.result != Safety::CrashBoundary::Result::JSException) {
        throw std::runtime_error(boundary.errorMessage);
    }

    return returnValue;
}

} // namespace Zepra::Host


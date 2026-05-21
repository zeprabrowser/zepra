// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — eval_impl.cpp — Direct/indirect eval, strict mode, scope injection

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <string>
#include <functional>
#include <vector>
#include <unordered_set>

namespace Zepra::Runtime {

enum class EvalKind : uint8_t {
    DirectEval,      // eval("...") — uses caller's scope
    IndirectEval,    // (0, eval)("...") — global scope
    NewFunction,     // new Function("...") — global scope
};

struct EvalRequest {
    std::string source;
    EvalKind kind;
    bool strictMode;
    void* callerScope;        // For direct eval
    void* globalScope;
    uint32_t callerScriptId;
    uint32_t callerLine;

    EvalRequest() : kind(EvalKind::DirectEval), strictMode(false)
        , callerScope(nullptr), globalScope(nullptr)
        , callerScriptId(0), callerLine(0) {}
};

struct EvalResult {
    uint64_t valueBits;
    bool success;
    std::string errorMessage;
    std::string errorType;     // SyntaxError, ReferenceError, etc.

    EvalResult() : valueBits(0), success(false) {}
};

class EvalImpl {
public:
    struct Callbacks {
        // Parse source → bytecode.
        std::function<bool(const std::string& source, bool strict,
                          std::vector<uint8_t>& bytecodeOut, std::string& errorOut)> parse;
        // Execute bytecode in scope.
        std::function<uint64_t(const std::vector<uint8_t>& bytecode,
                              void* scope, bool strict)> execute;
        // Sandbox permission check.
        std::function<bool(uint32_t tabId)> checkEvalPermission;
    };

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    EvalResult eval(const EvalRequest& request) {
        EvalResult result;

        // Permission check.
        if (cb_.checkEvalPermission && !cb_.checkEvalPermission(0)) {
            result.errorType = "EvalError";
            result.errorMessage = "eval() is not allowed in this context";
            return result;
        }

        if (request.source.empty()) {
            result.success = true;
            result.valueBits = 0x7FF8000000000001ULL;  // undefined
            return result;
        }

        // Check for "use strict" directive.
        bool strict = request.strictMode || hasUseStrict(request.source);

        // Parse.
        std::vector<uint8_t> bytecode;
        std::string parseError;

        if (cb_.parse) {
            if (!cb_.parse(request.source, strict, bytecode, parseError)) {
                result.errorType = "SyntaxError";
                result.errorMessage = parseError;
                return result;
            }
        }

        // Determine scope.
        void* scope = nullptr;
        switch (request.kind) {
            case EvalKind::DirectEval:
                scope = request.callerScope;
                break;
            case EvalKind::IndirectEval:
            case EvalKind::NewFunction:
                scope = request.globalScope;
                break;
        }

        // Execute.
        if (cb_.execute) {
            result.valueBits = cb_.execute(bytecode, scope, strict);
            result.success = true;
        }

        stats_.evalCount++;
        if (request.kind == EvalKind::DirectEval) stats_.directEvals++;
        else stats_.indirectEvals++;

        return result;
    }

    // new Function(params, body) — construct function from strings.
    EvalResult createFunction(const std::string& params, const std::string& body,
                              void* globalScope) {
        std::string source = "(function(" + params + ") {\n" + body + "\n})";
        EvalRequest req;
        req.source = source;
        req.kind = EvalKind::NewFunction;
        req.globalScope = globalScope;
        req.strictMode = hasUseStrict(body);
        return eval(req);
    }

    struct Stats {
        uint64_t evalCount = 0;
        uint64_t directEvals = 0;
        uint64_t indirectEvals = 0;
    };

    const Stats& stats() const { return stats_; }

private:
    bool hasUseStrict(const std::string& source) const {
        size_t pos = 0;
        // Skip whitespace and comments.
        while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\n'
               || source[pos] == '\r' || source[pos] == '\t')) {
            pos++;
        }
        if (source.compare(pos, 12, "\"use strict\"") == 0 ||
            source.compare(pos, 12, "'use strict'") == 0) {
            return true;
        }
        return false;
    }

    Callbacks cb_;
    Stats stats_;
};

} // namespace Zepra::Runtime

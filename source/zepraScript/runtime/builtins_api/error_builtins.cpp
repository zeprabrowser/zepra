// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — error_builtins.cpp — Error subclasses, stack traces, AggregateError

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace Zepra::Runtime {

enum class ErrorKind : uint8_t {
    Error,
    TypeError,
    RangeError,
    ReferenceError,
    SyntaxError,
    URIError,
    EvalError,
    AggregateError,
    SuppressedError,
    InternalError,
};

struct StackTraceEntry {
    std::string functionName;
    std::string fileName;
    uint32_t lineNumber;
    uint32_t columnNumber;
    bool isNative;

    StackTraceEntry() : lineNumber(0), columnNumber(0), isNative(false) {}
};

struct ErrorObject {
    ErrorKind kind;
    std::string message;
    std::string name;
    std::vector<StackTraceEntry> stackTrace;
    ErrorObject* cause;          // ES2022 Error.cause
    std::vector<std::unique_ptr<ErrorObject>> errors;  // AggregateError

    // SuppressedError (ES2024 — Explicit Resource Management)
    ErrorObject* error;
    ErrorObject* suppressed;

    ErrorObject() : kind(ErrorKind::Error), cause(nullptr)
        , error(nullptr), suppressed(nullptr) {}
};

class ErrorBuiltins {
public:
    using CaptureStackFn = std::function<std::vector<StackTraceEntry>()>;

    void setCaptureStack(CaptureStackFn fn) { captureStack_ = std::move(fn); }

    // Create an error with message and automatic stack capture.
    std::unique_ptr<ErrorObject> createError(ErrorKind kind, const std::string& message,
                                              ErrorObject* cause = nullptr) {
        auto err = std::make_unique<ErrorObject>();
        err->kind = kind;
        err->message = message;
        err->name = errorKindName(kind);
        err->cause = cause;
        if (captureStack_) err->stackTrace = captureStack_();
        return err;
    }

    // AggregateError (ES2021)
    std::unique_ptr<ErrorObject> createAggregateError(
            const std::string& message,
            std::vector<std::unique_ptr<ErrorObject>>& errors) {
        auto err = createError(ErrorKind::AggregateError, message);
        err->errors = std::move(errors);
        return err;
    }

    // SuppressedError (ES2024)
    std::unique_ptr<ErrorObject> createSuppressedError(
            ErrorObject* error, ErrorObject* suppressed, const std::string& message) {
        auto err = createError(ErrorKind::SuppressedError, message);
        err->error = error;
        err->suppressed = suppressed;
        return err;
    }

    // Format stack trace string.
    static std::string formatStackTrace(const ErrorObject& err) {
        std::string result = err.name + ": " + err.message + "\n";
        for (auto& entry : err.stackTrace) {
            result += "    at ";
            result += entry.functionName.empty() ? "<anonymous>" : entry.functionName;
            if (!entry.fileName.empty()) {
                result += " (" + entry.fileName + ":" +
                          std::to_string(entry.lineNumber) + ":" +
                          std::to_string(entry.columnNumber) + ")";
            }
            if (entry.isNative) result += " [native]";
            result += "\n";
        }
        if (err.cause) {
            result += "[cause]: " + formatStackTrace(*err.cause);
        }
        return result;
    }

    static const char* errorKindName(ErrorKind kind) {
        switch (kind) {
            case ErrorKind::Error: return "Error";
            case ErrorKind::TypeError: return "TypeError";
            case ErrorKind::RangeError: return "RangeError";
            case ErrorKind::ReferenceError: return "ReferenceError";
            case ErrorKind::SyntaxError: return "SyntaxError";
            case ErrorKind::URIError: return "URIError";
            case ErrorKind::EvalError: return "EvalError";
            case ErrorKind::AggregateError: return "AggregateError";
            case ErrorKind::SuppressedError: return "SuppressedError";
            case ErrorKind::InternalError: return "InternalError";
        }
        return "Error";
    }

private:
    CaptureStackFn captureStack_;
};

} // namespace Zepra::Runtime

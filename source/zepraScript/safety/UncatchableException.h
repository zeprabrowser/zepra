// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file UncatchableException.h
 * @brief Exceptions that bypass JS try/catch and propagate to the embedder
 */

#pragma once

#include <exception>
#include <string>

namespace Zepra::Safety {

class UncatchableException : public std::exception {
public:
    enum class Kind {
        OutOfMemory,
        StackOverflow,
        InternalError,
        SecurityViolation,
        Terminated
    };
    
    explicit UncatchableException(Kind k, const std::string& msg = "")
        : kind_(k), message_(msg) {}
    
    Kind kind() const { return kind_; }
    const char* what() const noexcept override { return message_.c_str(); }
    
    static UncatchableException OOM() {
        return UncatchableException(Kind::OutOfMemory, "Out of memory");
    }
    
    static UncatchableException StackOverflow() {
        return UncatchableException(Kind::StackOverflow, "Stack overflow");
    }
    
    static UncatchableException Internal(const std::string& msg) {
        return UncatchableException(Kind::InternalError, msg);
    }
    
    bool isUncatchable() const { return true; }
    
private:
    Kind kind_;
    std::string message_;
};

} // namespace Zepra::Safety

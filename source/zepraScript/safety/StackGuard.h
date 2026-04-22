// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file StackGuard.h
 * @brief Stack depth monitoring and overflow prevention
 */

#pragma once

#include "UncatchableException.h"
#include <cstddef>

namespace Zepra::Safety {

class StackGuard {
public:
    explicit StackGuard(size_t maxDepth = 1024) : maxDepth_(maxDepth) {}
    
    bool canPush() const { return currentDepth_ < maxDepth_; }
    
    void push() {
        if (!canPush()) {
            throw UncatchableException::StackOverflow();
        }
        currentDepth_++;
        if (currentDepth_ > peakDepth_) peakDepth_ = currentDepth_;
    }
    
    void pop() {
        if (currentDepth_ > 0) currentDepth_--;
    }
    
    size_t depth() const { return currentDepth_; }
    size_t peak() const { return peakDepth_; }
    size_t remaining() const { return maxDepth_ - currentDepth_; }
    
    class Frame {
    public:
        explicit Frame(StackGuard& guard) : guard_(guard) { guard_.push(); }
        ~Frame() { guard_.pop(); }
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
    private:
        StackGuard& guard_;
    };
    
private:
    size_t maxDepth_;
    size_t currentDepth_ = 0;
    size_t peakDepth_ = 0;
};

} // namespace Zepra::Safety

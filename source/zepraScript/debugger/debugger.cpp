// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file debugger.cpp
 * @brief ZepraScript Debugger implementation
 */

#include "debugger/debugger.hpp"
#include <algorithm>
#include "runtime/execution/vm.hpp"
#include <sstream>

namespace Zepra::Debug {

Debugger::Debugger(Runtime::VM* vm) : vm_(vm) {}

// =============================================================================
// Breakpoint Management
// =============================================================================

uint32_t Debugger::setBreakpoint(const std::string& file, uint32_t line) {
    return setBreakpoint(file, line, "");
}

uint32_t Debugger::setBreakpoint(const std::string& file, uint32_t line,
                                  const std::string& condition) {
    Breakpoint bp;
    bp.id = nextBreakpointId_++;
    bp.location.sourceFile = file;
    bp.location.line = line;
    bp.condition = condition;
    bp.enabled = true;
    
    breakpoints_[bp.id] = bp;
    
    // Add to fast lookup set
    std::ostringstream key;
    key << file << ":" << line;
    breakpointLocations_.insert(key.str());
    
    return bp.id;
}

bool Debugger::removeBreakpoint(uint32_t id) {
    auto it = breakpoints_.find(id);
    if (it == breakpoints_.end()) return false;
    
    // Remove from fast lookup
    std::ostringstream key;
    key << it->second.location.sourceFile << ":" << it->second.location.line;
    breakpointLocations_.erase(key.str());
    
    breakpoints_.erase(it);
    return true;
}

void Debugger::setBreakpointEnabled(uint32_t id, bool enabled) {
    auto it = breakpoints_.find(id);
    if (it != breakpoints_.end()) {
        it->second.enabled = enabled;
    }
}

std::vector<Breakpoint> Debugger::getBreakpoints() const {
    std::vector<Breakpoint> result;
    result.reserve(breakpoints_.size());
    for (const auto& [id, bp] : breakpoints_) {
        result.push_back(bp);
    }
    return result;
}

void Debugger::clearAllBreakpoints() {
    breakpoints_.clear();
    breakpointLocations_.clear();
}

// =============================================================================
// Execution Control
// =============================================================================

void Debugger::pause() {
    paused_ = true;
    notifyEvent(DebugEvent::ExecutionPaused);
}

void Debugger::resume() {
    paused_ = false;
    stepMode_ = StepMode::Continue;
    notifyEvent(DebugEvent::ExecutionResumed);
}

void Debugger::stepInto() {
    stepMode_ = StepMode::StepInto;
    paused_ = false;
}

void Debugger::stepOver() {
    stepMode_ = StepMode::StepOver;
    stepStartDepth_ = getCallStack().size();
    paused_ = false;
}

void Debugger::stepOut() {
    stepMode_ = StepMode::StepOut;
    stepStartDepth_ = getCallStack().size();
    paused_ = false;
}

// =============================================================================
// Call Stack
// =============================================================================

std::vector<DebugCallFrame> Debugger::getCallStack() const {
    std::vector<DebugCallFrame> frames;
    
    size_t depth = vm_->getCallDepth();
    
    for (size_t i = 0; i < depth; ++i) {
        DebugCallFrame frame;
        frame.functionName = vm_->getFrameFunctionName(i);
        frame.sourceFile = vm_->getFrameSourceFile(i);
        frame.line = vm_->getFrameLine(i);
        frame.column = vm_->getFrameColumn(i);
        frame.thisValue = vm_->getFrameThisValue(i);

        auto localNames = vm_->getFrameLocalNames(i);
        for (const auto& name : localNames) {
            frame.locals[name] = vm_->getFrameLocal(i, name);
        }

        frames.push_back(frame);
    }
    
    return frames;
}

std::unordered_map<std::string, Value> Debugger::getScopeVariables(size_t frameIndex) const {
    std::unordered_map<std::string, Value> vars;

    auto localNames = vm_->getFrameLocalNames(frameIndex);
    for (const auto& name : localNames) {
        vars[name] = vm_->getFrameLocal(frameIndex, name);
    }

    auto closureNames = vm_->getFrameClosureNames(frameIndex);
    for (const auto& name : closureNames) {
        vars[name] = vm_->getFrameClosureValue(frameIndex, name);
    }

    return vars;
}

// =============================================================================
// Variable Inspection
// =============================================================================

Value Debugger::evaluate(const std::string& expression) {
    if (expression.empty()) return Value::undefined();
    return vm_->evaluateInFrame(0, expression);
}

void Debugger::addWatch(const std::string& expression) {
    watches_.push_back(expression);
}

std::vector<std::pair<std::string, Value>> Debugger::getWatches() const {
    std::vector<std::pair<std::string, Value>> result;
    for (const auto& expr : watches_) {
        Value val = vm_->evaluateInFrame(0, expr);
        result.push_back({expr, val});
    }
    return result;
}

// =============================================================================
// Callbacks
// =============================================================================

void Debugger::setCallback(DebugCallback callback) {
    callback_ = std::move(callback);
}

void Debugger::notifyEvent(DebugEvent event, const Breakpoint* bp) {
    if (callback_) {
        callback_(event, bp);
    }
}

// =============================================================================
// VM Integration
// =============================================================================

bool Debugger::onInstruction(const std::string& file, uint32_t line) {
    // Check for breakpoints
    if (checkBreakpoint(file, line)) {
        paused_ = true;
        return false; // Pause execution
    }
    
    // Handle stepping
    switch (stepMode_) {
        case StepMode::StepInto:
            // Pause on any line change
            paused_ = true;
            stepMode_ = StepMode::None;
            notifyEvent(DebugEvent::StepComplete);
            return false;
            
        case StepMode::StepOver: {
            size_t currentDepth = getCallStack().size();
            if (currentDepth <= stepStartDepth_) {
                paused_ = true;
                stepMode_ = StepMode::None;
                notifyEvent(DebugEvent::StepComplete);
                return false;
            }
            break;
        }
        
        case StepMode::StepOut: {
            size_t currentDepth = getCallStack().size();
            if (currentDepth < stepStartDepth_) {
                paused_ = true;
                stepMode_ = StepMode::None;
                notifyEvent(DebugEvent::StepComplete);
                return false;
            }
            break;
        }
        
        default:
            break;
    }
    
    return !paused_; // Continue if not paused
}

bool Debugger::checkBreakpoint(const std::string& file, uint32_t line) {
    // Fast path: check if any breakpoint at this location
    std::ostringstream key;
    key << file << ":" << line;
    if (breakpointLocations_.find(key.str()) == breakpointLocations_.end()) {
        return false;
    }
    
    // Find and check the breakpoint
    for (auto& [id, bp] : breakpoints_) {
        if (bp.location.sourceFile == file && 
            bp.location.line == line && 
            bp.enabled) {
            
            bp.hitCount++;
            
            if (!bp.condition.empty()) {
                Value result = vm_->evaluateInFrame(0, bp.condition);
                if (!result.toBoolean()) continue;
            }
            
            notifyEvent(DebugEvent::BreakpointHit, &bp);
            return true;
        }
    }
    
    return false;
}

} // namespace Zepra::Debug

// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — interpreter_debug.cpp — Debug hooks, breakpoints, single-step

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <string>
#include <mutex>

namespace Zepra::Interpreter {

enum class DebugAction : uint8_t {
    Continue,
    StepInto,
    StepOver,
    StepOut,
    Pause,
};

struct BreakpointInfo {
    uint32_t id;
    uint32_t scriptId;
    uint32_t lineNumber;
    uint32_t columnNumber;
    std::string condition;     // Conditional breakpoint expression
    uint32_t hitCount;
    uint32_t ignoreCount;      // Skip this many hits
    bool enabled;
    bool isLogPoint;           // Log instead of break
    std::string logMessage;

    BreakpointInfo() : id(0), scriptId(0), lineNumber(0), columnNumber(0)
        , hitCount(0), ignoreCount(0), enabled(true), isLogPoint(false) {}
};

struct DebugEvent {
    enum class Kind : uint8_t {
        BreakpointHit,
        StepComplete,
        ExceptionThrown,
        ScriptLoaded,
        ScriptParsed,
        ContextCreated,
        ContextDestroyed,
    };

    Kind kind;
    uint32_t scriptId;
    uint32_t lineNumber;
    uint32_t breakpointId;

    DebugEvent() : kind(Kind::BreakpointHit), scriptId(0), lineNumber(0)
        , breakpointId(0) {}
};

class DebugHooks {
public:
    using DebugCallback = std::function<DebugAction(const DebugEvent& event)>;
    using EvalCondition = std::function<bool(const std::string& expr)>;

    DebugHooks() : enabled_(false), nextBpId_(1), stepping_(DebugAction::Continue)
        , stepDepth_(0), breakOnException_(false), breakOnUncaught_(true) {}

    void setCallback(DebugCallback cb) { callback_ = std::move(cb); }
    void setConditionEvaluator(EvalCondition eval) { evalCondition_ = std::move(eval); }

    // Enable/disable debugging globally.
    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }

    // Breakpoint management.
    uint32_t addBreakpoint(uint32_t scriptId, uint32_t line, uint32_t column = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t id = nextBpId_++;
        BreakpointInfo bp;
        bp.id = id;
        bp.scriptId = scriptId;
        bp.lineNumber = line;
        bp.columnNumber = column;
        breakpoints_[id] = bp;
        // Index by script:line for fast lookup.
        uint64_t key = (static_cast<uint64_t>(scriptId) << 32) | line;
        bpIndex_[key].insert(id);
        return id;
    }

    bool removeBreakpoint(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = breakpoints_.find(id);
        if (it == breakpoints_.end()) return false;
        uint64_t key = (static_cast<uint64_t>(it->second.scriptId) << 32)
                       | it->second.lineNumber;
        bpIndex_[key].erase(id);
        breakpoints_.erase(it);
        return true;
    }

    void enableBreakpoint(uint32_t id, bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = breakpoints_.find(id);
        if (it != breakpoints_.end()) it->second.enabled = enabled;
    }

    void setCondition(uint32_t id, const std::string& condition) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = breakpoints_.find(id);
        if (it != breakpoints_.end()) it->second.condition = condition;
    }

    void setLogPoint(uint32_t id, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = breakpoints_.find(id);
        if (it != breakpoints_.end()) {
            it->second.isLogPoint = true;
            it->second.logMessage = message;
        }
    }

    // Called at each instruction.
    bool shouldPause(uint32_t scriptId, uint32_t lineNumber, size_t callDepth) {
        if (!enabled_) return false;

        // Check stepping.
        switch (stepping_) {
            case DebugAction::StepInto:
                return true;
            case DebugAction::StepOver:
                if (callDepth <= stepDepth_) return true;
                break;
            case DebugAction::StepOut:
                if (callDepth < stepDepth_) return true;
                break;
            default:
                break;
        }

        // Check breakpoints.
        uint64_t key = (static_cast<uint64_t>(scriptId) << 32) | lineNumber;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = bpIndex_.find(key);
        if (it == bpIndex_.end()) return false;

        for (uint32_t bpId : it->second) {
            auto bpIt = breakpoints_.find(bpId);
            if (bpIt == breakpoints_.end() || !bpIt->second.enabled) continue;

            auto& bp = bpIt->second;
            bp.hitCount++;

            // Ignore count.
            if (bp.hitCount <= bp.ignoreCount) continue;

            // Conditional breakpoint.
            if (!bp.condition.empty() && evalCondition_) {
                if (!evalCondition_(bp.condition)) continue;
            }

            // Log point — log but don't pause.
            if (bp.isLogPoint) {
                if (!bp.logMessage.empty()) {
                    fprintf(stderr, "[LogPoint] %s\n", bp.logMessage.c_str());
                }
                continue;
            }

            return true;
        }
        return false;
    }

    // Handle a debug pause.
    DebugAction onPause(uint32_t scriptId, uint32_t lineNumber, size_t callDepth) {
        DebugEvent event;
        event.kind = DebugEvent::Kind::BreakpointHit;
        event.scriptId = scriptId;
        event.lineNumber = lineNumber;

        DebugAction action = DebugAction::Continue;
        if (callback_) action = callback_(event);

        stepping_ = action;
        stepDepth_ = callDepth;
        return action;
    }

    // Exception handling.
    void setBreakOnException(bool all, bool uncaught) {
        breakOnException_ = all;
        breakOnUncaught_ = uncaught;
    }

    bool shouldBreakOnException(bool isCaught) const {
        if (breakOnException_) return true;
        if (!isCaught && breakOnUncaught_) return true;
        return false;
    }

    // Stats.
    size_t breakpointCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return breakpoints_.size();
    }

private:
    bool enabled_;
    DebugCallback callback_;
    EvalCondition evalCondition_;

    mutable std::mutex mutex_;
    uint32_t nextBpId_;
    std::unordered_map<uint32_t, BreakpointInfo> breakpoints_;
    std::unordered_map<uint64_t, std::unordered_set<uint32_t>> bpIndex_;

    DebugAction stepping_;
    size_t stepDepth_;
    bool breakOnException_;
    bool breakOnUncaught_;
};

} // namespace Zepra::Interpreter

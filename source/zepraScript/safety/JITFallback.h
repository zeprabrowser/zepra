// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file JITFallback.h
 * @brief Graceful JIT tier degradation on compilation failure
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Zepra::Safety {

class JITFallback {
public:
    enum class Tier {
        Interpreter,
        Baseline,
        Optimizing
    };
    
    static JITFallback& instance() {
        static JITFallback inst;
        return inst;
    }
    
    void recordFailure(Tier tier, const std::string& reason) {
        failures_.push_back({tier, reason});
        
        if (failures_.size() > failureThreshold_) {
            if (tier == Tier::Optimizing) {
                optimizingEnabled_ = false;
            } else if (tier == Tier::Baseline) {
                baselineEnabled_ = false;
            }
        }
    }
    
    Tier bestAvailableTier() const {
        if (optimizingEnabled_) return Tier::Optimizing;
        if (baselineEnabled_) return Tier::Baseline;
        return Tier::Interpreter;
    }
    
    bool isTierAvailable(Tier tier) const {
        switch (tier) {
            case Tier::Optimizing: return optimizingEnabled_;
            case Tier::Baseline: return baselineEnabled_;
            case Tier::Interpreter: return true;
        }
        return false;
    }
    
    void reset() {
        failures_.clear();
        optimizingEnabled_ = true;
        baselineEnabled_ = true;
    }
    
    void setFailureThreshold(size_t n) { failureThreshold_ = n; }
    
private:
    JITFallback() = default;
    
    struct Failure {
        Tier tier;
        std::string reason;
    };
    std::vector<Failure> failures_;
    bool optimizingEnabled_ = true;
    bool baselineEnabled_ = true;
    size_t failureThreshold_ = 3;
};

} // namespace Zepra::Safety

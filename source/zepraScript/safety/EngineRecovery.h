// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file EngineRecovery.h
 * @brief Engine reset without process restart
 */

#pragma once

#include <string>
#include <functional>
#include <unordered_map>

namespace Zepra::Safety {

class EngineRecovery {
public:
    using ResetCallback = std::function<void()>;
    
    static EngineRecovery& instance() {
        static EngineRecovery inst;
        return inst;
    }
    
    void registerResetHandler(const std::string& name, ResetCallback cb) {
        handlers_[name] = std::move(cb);
    }
    
    void resetAll() {
        resetCount_++;
        for (auto& [name, handler] : handlers_) {
            try {
                handler();
            } catch (...) {}
        }
    }
    
    void reset(const std::string& subsystem) {
        auto it = handlers_.find(subsystem);
        if (it != handlers_.end()) {
            try {
                it->second();
            } catch (...) {}
        }
    }
    
    size_t resetCount() const { return resetCount_; }
    
private:
    EngineRecovery() = default;
    
    std::unordered_map<std::string, ResetCallback> handlers_;
    size_t resetCount_ = 0;
};

} // namespace Zepra::Safety

// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file OOMHandler.h
 * @brief Out-of-memory handling with graceful degradation
 */

#pragma once

#include "UncatchableException.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>

namespace Zepra::Safety {

class OOMHandler {
public:
    using OOMCallback = std::function<void(size_t requestedBytes)>;
    
    static OOMHandler& instance() {
        static OOMHandler inst;
        return inst;
    }
    
    void* allocateOrThrow(size_t bytes) {
        void* ptr = tryAllocate(bytes);
        if (!ptr) {
            if (oomCallback_) {
                oomCallback_(bytes);
                ptr = tryAllocate(bytes);
                if (ptr) return ptr;
            }
            stats_.oomCount++;
            throw UncatchableException::OOM();
        }
        return ptr;
    }
    
    bool isLowMemory() const {
        return getAvailableMemory() < lowMemoryThreshold_;
    }
    
    void setOOMCallback(OOMCallback cb) { oomCallback_ = std::move(cb); }
    void setLowMemoryThreshold(size_t bytes) { lowMemoryThreshold_ = bytes; }
    
    struct Stats {
        size_t oomCount = 0;
        size_t recoveryCount = 0;
        size_t lastRequestedBytes = 0;
    };
    
    Stats stats() const { return stats_; }
    
private:
    OOMHandler() = default;
    
    void* tryAllocate(size_t bytes) {
        stats_.lastRequestedBytes = bytes;
        return std::malloc(bytes);
    }
    
    size_t getAvailableMemory() const {
#ifdef __linux__
        FILE* f = std::fopen("/proc/meminfo", "r");
        if (!f) return 1024ULL * 1024 * 1024;
        char line[256];
        size_t avail = 0;
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "MemAvailable:", 13) == 0) {
                unsigned long long kb = 0;
                std::sscanf(line + 13, " %llu", &kb);
                avail = static_cast<size_t>(kb) * 1024;
                break;
            }
        }
        std::fclose(f);
        return avail > 0 ? avail : 1024ULL * 1024 * 1024;
#else
        return 1024ULL * 1024 * 1024;
#endif
    }
    
    OOMCallback oomCallback_;
    size_t lowMemoryThreshold_ = 64 * 1024 * 1024;
    Stats stats_;
};

} // namespace Zepra::Safety

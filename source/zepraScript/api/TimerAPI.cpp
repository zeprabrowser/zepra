// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "api/TimerAPI.h"

namespace Zepra::API {

TimerManager& getTimerManager() {
    static TimerManager instance;
    return instance;
}

} // namespace Zepra::API

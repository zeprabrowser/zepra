// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "api/PerformanceAPI.h"

namespace Zepra::API {

Performance& performance() {
    static Performance instance;
    return instance;
}

} // namespace Zepra::API

// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "api/CryptoAPI.h"

namespace Zepra::API {

Crypto& getCrypto() {
    static Crypto instance;
    return instance;
}

} // namespace Zepra::API

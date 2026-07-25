/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <optional>

namespace lora::ports {

using UnixSeconds = std::int64_t;

class IWallClock {
public:
    virtual ~IWallClock() = default;
    virtual std::optional<UnixSeconds> now_unix_seconds() noexcept = 0;
};

} // namespace lora::ports

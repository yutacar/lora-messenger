/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace lora::ports {

class IRandomBytes {
public:
    virtual ~IRandomBytes() = default;
    virtual bool fill(std::uint8_t* destination, std::size_t size) noexcept = 0;
};

} // namespace lora::ports

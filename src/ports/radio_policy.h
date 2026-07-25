/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace lora::ports {

using RadioTick = std::uint64_t;

enum class RadioTransmitStatus {
    Allowed,
    Deferred,
    Locked,
    Invalid,
};

struct RadioTransmitDecision {
    RadioTransmitStatus status{RadioTransmitStatus::Locked};
    RadioTick not_before_tick{0U};
};

// The scheduler asks before each physical datagram attempt and records airtime
// only after the transport returns Accepted. Implementations own regional and
// airtime policy; the protocol and simulated transport do not.
class IRadioPolicy {
public:
    virtual ~IRadioPolicy() = default;

    virtual RadioTransmitDecision evaluate_transmit(
        RadioTick now_tick, std::size_t datagram_size) noexcept = 0;

    virtual void record_transmit(
        RadioTick at_tick, std::size_t datagram_size) noexcept = 0;
};

} // namespace lora::ports

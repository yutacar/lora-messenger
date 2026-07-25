/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ports/radio_policy.h"

#include <cstddef>
#include <cstdint>

namespace lora::adapters::network {

struct LanBroadcastPolicyConfig {
    ports::RadioTick minimum_gap_ms{10U};
    std::size_t bucket_capacity_bytes{4U * 1024U};
    std::size_t refill_bytes_per_ms{64U};
};

// A local congestion guard, not a regulatory radio policy. It prevents a
// damaged queue or rapid retry loop from flooding the attached LAN.
class LanBroadcastPolicy final : public ports::IRadioPolicy {
public:
    explicit LanBroadcastPolicy(
        LanBroadcastPolicyConfig config = {}) noexcept;

    ports::RadioTransmitDecision evaluate_transmit(
        ports::RadioTick now_tick,
        std::size_t datagram_size) noexcept override;
    void record_transmit(
        ports::RadioTick at_tick,
        std::size_t datagram_size) noexcept override;

    bool valid() const noexcept;
    std::size_t available_bytes() const noexcept;

private:
    void refill(ports::RadioTick now_tick) noexcept;

    LanBroadcastPolicyConfig config_;
    std::size_t tokens_bytes_{0U};
    ports::RadioTick last_refill_tick_{0U};
    ports::RadioTick next_transmit_tick_{0U};
    bool clock_started_{false};
    bool valid_{false};
};

} // namespace lora::adapters::network

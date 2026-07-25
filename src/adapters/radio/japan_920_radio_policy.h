/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ports/radio_policy.h"

#include <cstddef>
#include <cstdint>

namespace lora::adapters::radio {

struct Japan920Profile {
    std::uint32_t frequency_hz{920'800'000U};
    std::uint32_t bandwidth_hz{125'000U};
    std::uint8_t spreading_factor{9U};
    std::uint8_t coding_rate_denominator{7U};
    std::uint8_t transmit_power_dbm{13U};
    std::uint16_t preamble_symbols{12U};
    std::uint8_t sync_word{0x12U};

    bool valid() const noexcept;
};

struct Japan920PolicyConfig {
    Japan920Profile profile;
    ports::RadioTick minimum_gap_ms{100U};
    ports::RadioTick bucket_capacity_ms{6'000U};
    std::uint32_t refill_denominator{10U};
};

// Conservative congestion budget for the approved JP profile. The token bucket
// permits at most 10% long-term calculated airtime by default even though the
// selected profile also requires the transport to perform listen-before-talk.
class Japan920RadioPolicy final : public ports::IRadioPolicy {
public:
    explicit Japan920RadioPolicy(
        Japan920PolicyConfig config = {}) noexcept;

    ports::RadioTransmitDecision evaluate_transmit(
        ports::RadioTick now_tick,
        std::size_t datagram_size) noexcept override;
    void record_transmit(
        ports::RadioTick at_tick,
        std::size_t datagram_size) noexcept override;

    bool valid() const noexcept;
    ports::RadioTick estimated_airtime_ms(
        std::size_t datagram_size) const noexcept;
    ports::RadioTick available_airtime_ms() const noexcept;

private:
    void refill(ports::RadioTick now_tick) noexcept;

    Japan920PolicyConfig config_;
    ports::RadioTick tokens_ms_{0U};
    ports::RadioTick last_refill_tick_{0U};
    ports::RadioTick next_transmit_tick_{0U};
    bool clock_started_{false};
    bool valid_{false};
};

} // namespace lora::adapters::radio

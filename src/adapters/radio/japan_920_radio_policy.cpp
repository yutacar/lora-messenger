/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/radio/japan_920_radio_policy.h"

#include "ports/datagram_transport.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lora::adapters::radio {
namespace {

ports::RadioTick saturating_add(
    ports::RadioTick left, ports::RadioTick right) noexcept {
    const auto maximum = std::numeric_limits<ports::RadioTick>::max();
    return left > maximum - right ? maximum : left + right;
}

} // namespace

bool Japan920Profile::valid() const noexcept {
    constexpr std::uint32_t kCertifiedLowerHz = 920'500'000U;
    // The published Cap LoRa-1262 range ends at 923 MHz. Keep the entire
    // occupied 125 kHz channel inside that documented hardware range.
    constexpr std::uint32_t kCapUpperHz = 923'000'000U;
    if (bandwidth_hz != 125'000U ||
        spreading_factor < 7U || spreading_factor > 12U ||
        coding_rate_denominator < 5U ||
        coding_rate_denominator > 8U ||
        transmit_power_dbm > 13U ||
        preamble_symbols < 8U ||
        sync_word != 0x12U) {
        return false;
    }
    const auto half_bandwidth = bandwidth_hz / 2U;
    return frequency_hz >= kCertifiedLowerHz + half_bandwidth &&
           frequency_hz <= kCapUpperHz - half_bandwidth;
}

Japan920RadioPolicy::Japan920RadioPolicy(
    Japan920PolicyConfig config) noexcept
    : config_(config),
      tokens_ms_(config.bucket_capacity_ms),
      valid_(config.profile.valid() &&
             config.minimum_gap_ms > 0U &&
             config.bucket_capacity_ms > 0U &&
             config.refill_denominator > 0U) {}

ports::RadioTransmitDecision Japan920RadioPolicy::evaluate_transmit(
    ports::RadioTick now_tick, std::size_t datagram_size) noexcept {
    if (!valid_ || datagram_size == 0U ||
        datagram_size > ports::kMaximumDatagramBytes) {
        return {ports::RadioTransmitStatus::Invalid, 0U};
    }
    if (clock_started_ && now_tick < last_refill_tick_) {
        valid_ = false;
        return {ports::RadioTransmitStatus::Locked, 0U};
    }

    refill(now_tick);
    const auto airtime = estimated_airtime_ms(datagram_size);
    if (airtime == 0U || airtime > config_.bucket_capacity_ms) {
        return {ports::RadioTransmitStatus::Invalid, 0U};
    }
    if (now_tick < next_transmit_tick_) {
        return {
            ports::RadioTransmitStatus::Deferred,
            next_transmit_tick_};
    }
    if (tokens_ms_ < airtime) {
        const auto deficit = airtime - tokens_ms_;
        return {
            ports::RadioTransmitStatus::Deferred,
            saturating_add(
                now_tick,
                deficit *
                    static_cast<ports::RadioTick>(
                        config_.refill_denominator))};
    }
    return {ports::RadioTransmitStatus::Allowed, now_tick};
}

void Japan920RadioPolicy::record_transmit(
    ports::RadioTick at_tick, std::size_t datagram_size) noexcept {
    if (!valid_) {
        return;
    }
    const auto decision = evaluate_transmit(at_tick, datagram_size);
    if (decision.status != ports::RadioTransmitStatus::Allowed) {
        valid_ = false;
        return;
    }
    const auto airtime = estimated_airtime_ms(datagram_size);
    tokens_ms_ -= airtime;
    next_transmit_tick_ =
        saturating_add(at_tick, config_.minimum_gap_ms);
}

bool Japan920RadioPolicy::valid() const noexcept {
    return valid_;
}

ports::RadioTick Japan920RadioPolicy::estimated_airtime_ms(
    std::size_t datagram_size) const noexcept {
    if (!config_.profile.valid() || datagram_size == 0U ||
        datagram_size > ports::kMaximumDatagramBytes) {
        return 0U;
    }

    const double sf =
        static_cast<double>(config_.profile.spreading_factor);
    const double symbol_seconds =
        std::ldexp(1.0, config_.profile.spreading_factor) /
        static_cast<double>(config_.profile.bandwidth_hz);
    const double low_data_rate_optimization =
        symbol_seconds >= 0.016 ? 1.0 : 0.0;
    const double numerator =
        8.0 * static_cast<double>(datagram_size) -
        4.0 * sf + 28.0 + 16.0;
    const double denominator =
        4.0 * (sf - 2.0 * low_data_rate_optimization);
    const double coded_groups =
        std::max(0.0, std::ceil(numerator / denominator));
    const double payload_symbols =
        8.0 + coded_groups *
            static_cast<double>(
                config_.profile.coding_rate_denominator);
    const double total_symbols =
        static_cast<double>(config_.profile.preamble_symbols) +
        4.25 + payload_symbols;
    const double milliseconds =
        total_symbols * symbol_seconds * 1'000.0;
    if (milliseconds <= 0.0 ||
        milliseconds >
            static_cast<double>(
                std::numeric_limits<ports::RadioTick>::max())) {
        return 0U;
    }
    return static_cast<ports::RadioTick>(std::ceil(milliseconds));
}

ports::RadioTick Japan920RadioPolicy::available_airtime_ms() const noexcept {
    return tokens_ms_;
}

void Japan920RadioPolicy::refill(ports::RadioTick now_tick) noexcept {
    if (!clock_started_) {
        clock_started_ = true;
        last_refill_tick_ = now_tick;
        return;
    }
    const auto elapsed = now_tick - last_refill_tick_;
    const auto added =
        elapsed /
        static_cast<ports::RadioTick>(config_.refill_denominator);
    if (added > 0U) {
        tokens_ms_ = std::min(
            config_.bucket_capacity_ms,
            saturating_add(tokens_ms_, added));
        last_refill_tick_ = now_tick;
    }
}

} // namespace lora::adapters::radio

/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/network/lan_broadcast_policy.h"

#include "ports/datagram_transport.h"

#include <algorithm>
#include <limits>

namespace lora::adapters::network {
namespace {

ports::RadioTick saturating_add(
    ports::RadioTick left, ports::RadioTick right) noexcept {
    const auto maximum = std::numeric_limits<ports::RadioTick>::max();
    return left > maximum - right ? maximum : left + right;
}

} // namespace

LanBroadcastPolicy::LanBroadcastPolicy(
    LanBroadcastPolicyConfig config) noexcept
    : config_(config),
      tokens_bytes_(config.bucket_capacity_bytes),
      valid_(config.minimum_gap_ms > 0U &&
             config.bucket_capacity_bytes >=
                 ports::kMaximumDatagramBytes &&
             config.refill_bytes_per_ms > 0U) {}

ports::RadioTransmitDecision
LanBroadcastPolicy::evaluate_transmit(
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
    if (now_tick < next_transmit_tick_) {
        return {
            ports::RadioTransmitStatus::Deferred,
            next_transmit_tick_};
    }
    if (tokens_bytes_ < datagram_size) {
        const auto deficit = datagram_size - tokens_bytes_;
        const auto wait =
            (deficit + config_.refill_bytes_per_ms - 1U) /
            config_.refill_bytes_per_ms;
        return {
            ports::RadioTransmitStatus::Deferred,
            saturating_add(
                now_tick,
                static_cast<ports::RadioTick>(wait))};
    }
    return {ports::RadioTransmitStatus::Allowed, now_tick};
}

void LanBroadcastPolicy::record_transmit(
    ports::RadioTick at_tick, std::size_t datagram_size) noexcept {
    if (!valid_) {
        return;
    }
    const auto decision =
        evaluate_transmit(at_tick, datagram_size);
    if (decision.status != ports::RadioTransmitStatus::Allowed) {
        valid_ = false;
        return;
    }
    tokens_bytes_ -= datagram_size;
    next_transmit_tick_ =
        saturating_add(at_tick, config_.minimum_gap_ms);
}

bool LanBroadcastPolicy::valid() const noexcept {
    return valid_;
}

std::size_t LanBroadcastPolicy::available_bytes() const noexcept {
    return tokens_bytes_;
}

void LanBroadcastPolicy::refill(
    ports::RadioTick now_tick) noexcept {
    if (!clock_started_) {
        clock_started_ = true;
        last_refill_tick_ = now_tick;
        return;
    }
    const auto elapsed = now_tick - last_refill_tick_;
    if (elapsed == 0U) {
        return;
    }
    const auto maximum = std::numeric_limits<std::size_t>::max();
    const auto elapsed_size =
        elapsed > static_cast<ports::RadioTick>(maximum)
            ? maximum
            : static_cast<std::size_t>(elapsed);
    const auto added =
        elapsed_size >
                maximum / config_.refill_bytes_per_ms
            ? maximum
            : elapsed_size * config_.refill_bytes_per_ms;
    tokens_bytes_ = std::min(
        config_.bucket_capacity_bytes,
        added > maximum - tokens_bytes_
            ? maximum
            : tokens_bytes_ + added);
    last_refill_tick_ = now_tick;
}

} // namespace lora::adapters::network

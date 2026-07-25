/*
 * SPDX-License-Identifier: MIT
 */

#include "application/transmission_scheduler.h"

#include "protocol/fragmenter.h"
#include "protocol/limits.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace lora::application {
namespace {

ports::RadioTick saturating_add(
    ports::RadioTick value, ports::RadioTick increment) noexcept {
    const auto maximum = std::numeric_limits<ports::RadioTick>::max();
    return value > maximum - increment ? maximum : value + increment;
}

ports::RadioTick scaled_jitter(std::uint8_t value,
                               ports::RadioTick inclusive_maximum) noexcept {
    return (static_cast<ports::RadioTick>(value) *
            (inclusive_maximum + 1U)) /
           256U;
}

} // namespace

TransmissionScheduler::TransmissionScheduler(ports::IRandomBytes& random)
    : random_(random) {
    pending_.reserve(kMaximumOutboundMessages);
}

SchedulerError TransmissionScheduler::enqueue(
    const model::PostPayload& post, std::size_t mtu,
    ports::RadioTick now) {
    if (mtu < protocol::kMinimumTransportMtu ||
        mtu > protocol::kMaximumTransportMtu) {
        return SchedulerError::InvalidMtu;
    }
    if (pending_.size() >= kMaximumOutboundMessages) {
        return SchedulerError::QueueFull;
    }
    if (std::any_of(
            pending_.begin(), pending_.end(),
            [&post](const PendingMessage& pending) {
                return pending.message_id == post.message_id();
            })) {
        return SchedulerError::DuplicateMessage;
    }

    auto fragmented = protocol::fragment_post(post, mtu);
    if (!fragmented) {
        return SchedulerError::InvalidPost;
    }

    const std::size_t jitter_count =
        2U * fragmented.value().size() + 2U;
    std::vector<std::uint8_t> jitter(jitter_count);
    if (!random_.fill(jitter.data(), jitter.size())) {
        return SchedulerError::RandomUnavailable;
    }
    const auto initial_due =
        saturating_add(now, scaled_jitter(jitter[0], 250U));
    std::vector<std::uint8_t> offer_counts(
        fragmented.value().size(), 0U);

    PendingMessage pending{
        post.message_id(),
        mtu,
        std::move(fragmented).value(),
        std::move(jitter),
        std::move(offer_counts),
        0U,
        1U,
        initial_due,
        saturating_add(now, kOutboundDeadlineTicks),
        false,
    };
    pending_.push_back(std::move(pending));
    update_pending_metric();
    return SchedulerError::None;
}

std::optional<OutboundEvent> TransmissionScheduler::fail_front() noexcept {
    const core::MessageId message_id = pending_.front().message_id;
    pending_.erase(pending_.begin());
    ++metrics_.failed_messages;
    update_pending_metric();
    return OutboundEvent{message_id, OutboundTerminalState::Failed};
}

std::optional<OutboundEvent> TransmissionScheduler::pump(
    ports::RadioTick now, ports::IDatagramTransport& transport,
    ports::IRadioPolicy& radio_policy) {
    if (pending_.empty()) {
        return std::nullopt;
    }

    PendingMessage& pending = pending_.front();
    if (now >= pending.deadline) {
        if (!pending.repeat_pass) {
            return fail_front();
        }
        pending_.erase(pending_.begin());
        update_pending_metric();
        return std::nullopt;
    }
    if (now < pending.due) {
        return std::nullopt;
    }
    const auto& frame = pending.frames[pending.fragment_index];
    if (transport.maximum_datagram_size() != pending.mtu ||
        transport.closed()) {
        if (!pending.repeat_pass) {
            return fail_front();
        }
        pending_.erase(pending_.begin());
        update_pending_metric();
        return std::nullopt;
    }

    if (pending.repeat_pass &&
        pending.offer_counts[pending.fragment_index] >= 2U) {
        ++pending.fragment_index;
        if (pending.fragment_index == pending.frames.size()) {
            pending_.erase(pending_.begin());
            update_pending_metric();
        } else {
            pending.due = now;
        }
        return std::nullopt;
    }

    const auto decision =
        radio_policy.evaluate_transmit(now, frame.size());
    switch (decision.status) {
        case ports::RadioTransmitStatus::Deferred:
            ++metrics_.policy_deferrals;
            pending.due = decision.not_before_tick > now
                              ? decision.not_before_tick
                              : saturating_add(now, 100U);
            return std::nullopt;
        case ports::RadioTransmitStatus::Locked:
        case ports::RadioTransmitStatus::Invalid:
            if (!pending.repeat_pass) {
                return fail_front();
            }
            pending_.erase(pending_.begin());
            update_pending_metric();
            return std::nullopt;
        case ports::RadioTransmitStatus::Allowed:
            break;
    }

    ++metrics_.transport_offers;
    const auto send_status =
        transport.try_send(frame.data(), frame.size());
    ++pending.offer_counts[pending.fragment_index];
    if (send_status == ports::DatagramSendStatus::WouldBlock ||
        send_status == ports::DatagramSendStatus::Disconnected) {
        ++metrics_.backpressure_events;
        if (pending.offer_counts[pending.fragment_index] >= 2U) {
            if (!pending.repeat_pass) {
                return fail_front();
            }
            ++pending.fragment_index;
            if (pending.fragment_index == pending.frames.size()) {
                pending_.erase(pending_.begin());
                update_pending_metric();
            } else {
                pending.due = now;
            }
            return std::nullopt;
        }
        const auto retry_jitter =
            scaled_jitter(
                pending.jitter[pending.jitter_index++], 100U);
        pending.due =
            saturating_add(now, 100U + retry_jitter);
        return std::nullopt;
    }
    if (send_status != ports::DatagramSendStatus::Accepted) {
        if (!pending.repeat_pass) {
            return fail_front();
        }
        pending_.erase(pending_.begin());
        update_pending_metric();
        return std::nullopt;
    }

    radio_policy.record_transmit(now, frame.size());
    ++metrics_.accepted_datagrams;
    ++pending.fragment_index;
    if (pending.fragment_index < pending.frames.size()) {
        const auto gap =
            scaled_jitter(pending.jitter[pending.jitter_index++], 50U);
        pending.due = saturating_add(now, gap);
        return std::nullopt;
    }

    if (!pending.repeat_pass) {
        pending.repeat_pass = true;
        pending.fragment_index = 0;
        const auto repeat_jitter =
            scaled_jitter(pending.jitter[pending.jitter_index++], 250U);
        pending.due =
            saturating_add(now, 1'000U + repeat_jitter);
        ++metrics_.broadcast_messages;
        return OutboundEvent{
            pending.message_id, OutboundTerminalState::Broadcast};
    }

    pending_.erase(pending_.begin());
    update_pending_metric();
    return std::nullopt;
}

void TransmissionScheduler::cancel_all() noexcept {
    pending_.clear();
    update_pending_metric();
}

std::size_t TransmissionScheduler::pending_count() const noexcept {
    return pending_.size();
}

bool TransmissionScheduler::empty() const noexcept {
    return pending_.empty();
}

const SchedulerMetrics& TransmissionScheduler::metrics() const noexcept {
    return metrics_;
}

void TransmissionScheduler::update_pending_metric() noexcept {
    metrics_.pending_messages = pending_.size();
    metrics_.high_water_messages =
        std::max(metrics_.high_water_messages, pending_.size());
}

} // namespace lora::application

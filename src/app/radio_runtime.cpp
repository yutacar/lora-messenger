/*
 * SPDX-License-Identifier: MIT
 */

#include "radio_runtime.h"

#include "model/timeline.h"

#include <array>
#include <variant>

namespace app {
namespace {

constexpr std::size_t kMaximumRadioWorkPerPump = 4U;

bool is_queued(const lora::model::TimelineEntry& entry) noexcept {
    const auto* local =
        std::get_if<lora::model::LocalDelivery>(&entry.origin);
    return local &&
           local->state ==
               lora::model::LocalDeliveryState::Queued;
}

} // namespace

RadioRuntime::RadioRuntime(
    lora::ports::IDatagramTransport& transport,
    lora::ports::IRadioPolicy& radio_policy,
    lora::ports::IRandomBytes& random) noexcept
    : transport_(transport),
      radio_policy_(radio_policy),
      scheduler_(random),
      inbound_(transport.maximum_datagram_size()),
      valid_(!transport.closed() &&
             transport.maximum_datagram_size() >=
                 lora::ports::kMinimumDatagramMtu &&
             transport.maximum_datagram_size() <=
                 lora::ports::kMaximumDatagramBytes) {
    if (!valid_) {
        inbound_.stop();
        transport_.close();
        stopped_ = true;
    }
}

RadioRuntime::~RadioRuntime() {
    stop();
}

bool RadioRuntime::ready() const noexcept {
    return valid_ && !stopped_ && transport_.connected();
}

bool RadioRuntime::pump(
    lora::ports::RadioTick now,
    PersistentSession& session) noexcept {
    if (!valid_ || stopped_ || !session.ready()) {
        return false;
    }
    // A reversible link loss (for example, Wi-Fi roaming) keeps durable
    // outbound work queued. A permanently closed transport must still reach
    // the scheduler so its ordinary failure path can terminate queued work.
    if (!transport_.connected() && !transport_.closed()) {
        return false;
    }
    bool changed = enqueue_queued_posts(now, session);
    changed = pump_outbound(now, session) || changed;
    changed = pump_inbound(now, session) || changed;
    return changed;
}

void RadioRuntime::stop() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    scheduler_.cancel_all();
    inbound_.stop();
    transport_.close();
}

const RadioRuntimeMetrics& RadioRuntime::metrics() const noexcept {
    return metrics_;
}

bool RadioRuntime::enqueue_queued_posts(
    lora::ports::RadioTick now,
    PersistentSession& session) noexcept {
    for (const auto& entry :
         session.state().timeline().entries()) {
        if (!is_queued(entry)) {
            continue;
        }
        const auto error = scheduler_.enqueue(
            entry.post, transport_.maximum_datagram_size(), now);
        switch (error) {
            case lora::application::SchedulerError::None:
            case lora::application::SchedulerError::DuplicateMessage:
            case lora::application::SchedulerError::QueueFull:
                break;
            case lora::application::SchedulerError::InvalidMtu:
            case lora::application::SchedulerError::InvalidPost:
            case lora::application::SchedulerError::RandomUnavailable:
                if (session.state().mark_failed(
                        entry.post.message_id()).ok()) {
                    ++metrics_.failed_posts;
                    // The durable transition replaces the timeline value and
                    // invalidates the current borrowed entry range.
                    return true;
                }
                break;
        }
    }
    return false;
}

bool RadioRuntime::pump_outbound(
    lora::ports::RadioTick now,
    PersistentSession& session) noexcept {
    const auto event =
        scheduler_.pump(now, transport_, radio_policy_);
    if (!event) {
        return false;
    }
    if (event->state ==
        lora::application::OutboundTerminalState::Broadcast) {
        if (session.state().mark_broadcast(
                event->message_id).ok()) {
            ++metrics_.broadcast_posts;
            return true;
        }
        return false;
    }
    if (session.state().mark_failed(event->message_id).ok()) {
        ++metrics_.failed_posts;
        return true;
    }
    return false;
}

bool RadioRuntime::pump_inbound(
    lora::ports::RadioTick now,
    PersistentSession& session) noexcept {
    std::array<
        std::uint8_t, lora::ports::kMaximumDatagramBytes> bytes{};
    for (std::size_t work = 0U;
         work < kMaximumRadioWorkPerPump; ++work) {
        const auto received =
            transport_.try_receive(bytes.data(), bytes.size());
        if (received.status ==
            lora::ports::DatagramReceiveStatus::WouldBlock) {
            break;
        }
        if (received.status !=
            lora::ports::DatagramReceiveStatus::Received) {
            break;
        }
        ++metrics_.received_datagrams;
        const auto enqueued = inbound_.enqueue(
            bytes.data(), received.size, now);
        if (enqueued ==
            lora::protocol::IngressEnqueueStatus::QueueFull) {
            ++metrics_.receive_overflow;
        }
    }

    bool changed = false;
    for (std::size_t work = 0U;
         work < kMaximumRadioWorkPerPump; ++work) {
        auto processed = inbound_.process_next(now);
        if (processed.status ==
            lora::protocol::IngressProcessStatus::Empty) {
            break;
        }
        if (processed.status !=
                lora::protocol::IngressProcessStatus::Complete ||
            !processed.completed) {
            continue;
        }

        const auto status = session.accept_received_post(
            processed.completed->canonical_post);
        if (status == InboundPostStatus::Accepted ||
            status == InboundPostStatus::Duplicate) {
            inbound_.mark_committed(
                processed.completed->canonical_frames, now);
        }
        if (status == InboundPostStatus::Accepted) {
            ++metrics_.completed_posts;
            changed = true;
        } else if (status == InboundPostStatus::Duplicate) {
            ++metrics_.duplicate_posts;
        } else {
            ++metrics_.rejected_posts;
        }
    }
    return changed;
}

} // namespace app

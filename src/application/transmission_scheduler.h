/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/uuid.h"
#include "model/post.h"
#include "ports/datagram_transport.h"
#include "ports/radio_policy.h"
#include "ports/random.h"
#include "protocol/post_codec.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lora::application {

inline constexpr std::size_t kMaximumOutboundMessages = 16;
inline constexpr ports::RadioTick kOutboundDeadlineTicks = 60'000;

enum class SchedulerError {
    None,
    InvalidMtu,
    InvalidPost,
    DuplicateMessage,
    QueueFull,
    RandomUnavailable,
};

enum class OutboundTerminalState {
    Broadcast,
    Failed,
};

struct OutboundEvent {
    core::MessageId message_id;
    OutboundTerminalState state{OutboundTerminalState::Failed};
};

struct SchedulerMetrics {
    std::size_t pending_messages{0};
    std::size_t high_water_messages{0};
    std::uint64_t transport_offers{0};
    std::uint64_t accepted_datagrams{0};
    std::uint64_t backpressure_events{0};
    std::uint64_t policy_deferrals{0};
    std::uint64_t failed_messages{0};
    std::uint64_t broadcast_messages{0};
};

class TransmissionScheduler {
public:
    explicit TransmissionScheduler(ports::IRandomBytes& random);

    SchedulerError enqueue(const model::PostPayload& post,
                           std::size_t mtu,
                           ports::RadioTick now);

    // Performs at most one transport offer. Broadcast means every primary
    // fragment was accepted locally once; it is never a receiver ACK.
    std::optional<OutboundEvent> pump(
        ports::RadioTick now,
        ports::IDatagramTransport& transport,
        ports::IRadioPolicy& radio_policy);

    void cancel_all() noexcept;
    std::size_t pending_count() const noexcept;
    bool empty() const noexcept;
    const SchedulerMetrics& metrics() const noexcept;

private:
    struct PendingMessage {
        core::MessageId message_id;
        std::size_t mtu{0};
        std::vector<protocol::Bytes> frames;
        std::vector<std::uint8_t> jitter;
        std::vector<std::uint8_t> offer_counts;
        std::size_t fragment_index{0};
        std::size_t jitter_index{0};
        ports::RadioTick due{0};
        ports::RadioTick deadline{0};
        bool repeat_pass{false};
    };

    std::optional<OutboundEvent> fail_front() noexcept;
    void update_pending_metric() noexcept;

    ports::IRandomBytes& random_;
    std::vector<PendingMessage> pending_;
    SchedulerMetrics metrics_;
};

} // namespace lora::application

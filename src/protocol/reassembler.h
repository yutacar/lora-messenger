/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/post.h"
#include "protocol/frame_codec.h"
#include "protocol/post_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace lora::protocol {

inline constexpr std::size_t kMaximumInboundFrames = 64;
inline constexpr std::size_t kMaximumReassemblies = 8;
inline constexpr std::size_t kMaximumRecentFrames = 128;
inline constexpr std::size_t kMaximumQuarantinedKeys = 32;
inline constexpr std::uint64_t kReassemblyTimeoutTicks = 60'000;

enum class ReassemblyStatus {
    FragmentAccepted,
    DuplicateFragment,
    Complete,
    CapacityExceeded,
    Conflict,
    Quarantined,
    InvalidPost,
};

struct CompletedPost {
    model::PostPayload post;
    Bytes canonical_post;
    std::vector<Bytes> canonical_frames;
};

struct ReassemblyResult {
    ReassemblyStatus status{ReassemblyStatus::FragmentAccepted};
    std::optional<CompletedPost> completed;
};

class Reassembler {
public:
    explicit Reassembler(std::size_t mtu);
    ~Reassembler();

    ReassemblyResult accept(const DataFrame& frame,
                            std::uint64_t received_tick);
    std::size_t expire(std::uint64_t now) noexcept;
    void clear() noexcept;
    std::size_t active_count() const noexcept;
    std::size_t high_water_count() const noexcept;

private:
    struct Slot;
    struct QuarantinedKey {
        MessageTag tag{};
        std::uint64_t sender_sequence{0};
        std::uint64_t deadline{0};
    };

    void quarantine(const MessageTag& tag, std::uint64_t sender_sequence,
                    std::uint64_t deadline);
    bool is_quarantined(const MessageTag& tag,
                        std::uint64_t sender_sequence,
                        std::uint64_t now);

    std::size_t mtu_;
    std::size_t fragment_capacity_;
    std::vector<Slot> slots_;
    std::deque<QuarantinedKey> quarantine_;
    std::size_t high_water_count_{0};
};

enum class IngressEnqueueStatus {
    Accepted,
    QueueFull,
    Invalid,
    Closed,
};

enum class IngressProcessStatus {
    Empty,
    Stale,
    Malformed,
    RecentDuplicate,
    FragmentAccepted,
    DuplicateFragment,
    Complete,
    CapacityExceeded,
    Conflict,
    Quarantined,
    InvalidPost,
    Closed,
};

struct IngressProcessResult {
    IngressProcessStatus status{IngressProcessStatus::Empty};
    std::optional<CompletedPost> completed;
};

struct IngressMetrics {
    std::size_t queued_frames{0};
    std::size_t queue_high_water{0};
    std::size_t active_reassemblies{0};
    std::size_t reassembly_high_water{0};
    std::uint64_t malformed_frames{0};
    std::uint64_t stale_frames{0};
    std::uint64_t duplicate_frames{0};
    std::uint64_t capacity_rejections{0};
    std::uint64_t expired_reassemblies{0};
    std::uint64_t completed_posts{0};
};

class InboundFrameQueue {
public:
    explicit InboundFrameQueue(std::size_t mtu);

    IngressEnqueueStatus enqueue(const std::uint8_t* data,
                                 std::size_t size,
                                 std::uint64_t received_tick);
    IngressProcessResult process_next(std::uint64_t now);

    // Call only after the completed post is durably stored or durably classified
    // as an identical duplicate. The fixed expiry is never extended by repeats.
    void mark_committed(const std::vector<Bytes>& canonical_frames,
                        std::uint64_t committed_tick);

    void stop() noexcept;
    bool stopped() const noexcept;
    std::size_t queued_count() const noexcept;
    const IngressMetrics& metrics() const noexcept;

private:
    struct QueuedFrame {
        Bytes datagram;
        std::uint64_t received_tick{0};
    };
    struct RecentFrame {
        Bytes datagram;
        std::uint64_t deadline{0};
    };

    bool recently_committed(const Bytes& datagram,
                            std::uint64_t now);
    void expire_recent(std::uint64_t now) noexcept;
    void update_metrics() noexcept;

    std::size_t mtu_;
    std::deque<QueuedFrame> queue_;
    std::deque<RecentFrame> recent_;
    Reassembler reassembler_;
    IngressMetrics metrics_;
    bool stopped_{false};
};

} // namespace lora::protocol

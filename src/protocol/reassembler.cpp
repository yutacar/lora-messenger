/*
 * SPDX-License-Identifier: MIT
 */

#include "protocol/reassembler.h"

#include "protocol/limits.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace lora::protocol {
namespace {

std::uint64_t saturating_deadline(
    std::uint64_t start, std::uint64_t duration) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return start > maximum - duration ? maximum : start + duration;
}

bool deadline_reached(std::uint64_t now,
                      std::uint64_t deadline) noexcept {
    return now >= deadline;
}

bool same_key(const MessageTag& left_tag, std::uint64_t left_sequence,
              const MessageTag& right_tag,
              std::uint64_t right_sequence) noexcept {
    return left_tag == right_tag && left_sequence == right_sequence;
}

} // namespace

struct Reassembler::Slot {
    bool used{false};
    MessageTag tag{};
    std::uint64_t sender_sequence{0};
    std::uint16_t total_length{0};
    std::uint8_t fragment_count{0};
    std::uint16_t received_bitmap{0};
    std::uint64_t deadline{0};
    std::array<std::uint8_t, kMaximumEncodedPostBytes> bytes{};
    std::array<std::uint16_t, kMaximumFragments> fragment_lengths{};
};

Reassembler::Reassembler(std::size_t mtu)
    : mtu_(mtu),
      fragment_capacity_(
          mtu >= kFrameHeaderBytes ? mtu - kFrameHeaderBytes : 0U),
      slots_(kMaximumReassemblies) {}

Reassembler::~Reassembler() = default;

void Reassembler::quarantine(
    const MessageTag& tag, std::uint64_t sender_sequence,
    std::uint64_t deadline) {
    const auto found = std::find_if(
        quarantine_.begin(), quarantine_.end(),
        [&tag, sender_sequence](const QuarantinedKey& key) {
            return same_key(key.tag, key.sender_sequence,
                            tag, sender_sequence);
        });
    if (found != quarantine_.end()) {
        return;
    }
    if (quarantine_.size() >= kMaximumQuarantinedKeys) {
        quarantine_.pop_front();
    }
    quarantine_.push_back(
        QuarantinedKey{tag, sender_sequence, deadline});
}

bool Reassembler::is_quarantined(
    const MessageTag& tag, std::uint64_t sender_sequence,
    std::uint64_t now) {
    quarantine_.erase(
        std::remove_if(
            quarantine_.begin(), quarantine_.end(),
            [now](const QuarantinedKey& key) {
                return deadline_reached(now, key.deadline);
            }),
        quarantine_.end());
    return std::any_of(
        quarantine_.begin(), quarantine_.end(),
        [&tag, sender_sequence](const QuarantinedKey& key) {
            return same_key(key.tag, key.sender_sequence,
                            tag, sender_sequence);
        });
}

std::size_t Reassembler::expire(std::uint64_t now) noexcept {
    std::size_t expired = 0;
    for (auto& slot : slots_) {
        if (slot.used && deadline_reached(now, slot.deadline)) {
            slot = Slot{};
            ++expired;
        }
    }
    quarantine_.erase(
        std::remove_if(
            quarantine_.begin(), quarantine_.end(),
            [now](const QuarantinedKey& key) {
                return deadline_reached(now, key.deadline);
            }),
        quarantine_.end());
    return expired;
}

ReassemblyResult Reassembler::accept(
    const DataFrame& frame, std::uint64_t received_tick) {
    static_cast<void>(expire(received_tick));
    if (!is_supported_mtu(mtu_) || frame.sender_sequence == 0 ||
        frame.fragment_count == 0 ||
        frame.fragment_count > kMaximumFragments ||
        frame.fragment_index >= frame.fragment_count ||
        frame.total_length < kMinimumEncodedPostBytes ||
        frame.total_length > kMaximumEncodedPostBytes ||
        fragment_capacity_ == 0) {
        return {ReassemblyStatus::InvalidPost, std::nullopt};
    }
    const std::size_t expected_count =
        (static_cast<std::size_t>(frame.total_length) +
         fragment_capacity_ - 1U) /
        fragment_capacity_;
    const std::size_t expected_payload =
        frame.fragment_index + 1U < frame.fragment_count
            ? fragment_capacity_
            : static_cast<std::size_t>(frame.total_length) -
                  fragment_capacity_ *
                      (frame.fragment_count - 1U);
    if (expected_count != frame.fragment_count ||
        frame.payload.size() != expected_payload) {
        return {ReassemblyStatus::InvalidPost, std::nullopt};
    }
    if (is_quarantined(frame.tag, frame.sender_sequence,
                       received_tick)) {
        return {ReassemblyStatus::Quarantined, std::nullopt};
    }

    auto slot = std::find_if(
        slots_.begin(), slots_.end(),
        [&frame](const Slot& candidate) {
            return candidate.used &&
                   same_key(candidate.tag, candidate.sender_sequence,
                            frame.tag, frame.sender_sequence);
        });
    if (slot == slots_.end()) {
        slot = std::find_if(
            slots_.begin(), slots_.end(),
            [](const Slot& candidate) { return !candidate.used; });
        if (slot == slots_.end()) {
            return {ReassemblyStatus::CapacityExceeded, std::nullopt};
        }
        slot->used = true;
        slot->tag = frame.tag;
        slot->sender_sequence = frame.sender_sequence;
        slot->total_length = frame.total_length;
        slot->fragment_count = frame.fragment_count;
        slot->deadline =
            saturating_deadline(received_tick,
                                kReassemblyTimeoutTicks);
        high_water_count_ =
            std::max(high_water_count_, active_count());
    } else if (slot->total_length != frame.total_length ||
               slot->fragment_count != frame.fragment_count) {
        const auto deadline = slot->deadline;
        *slot = Slot{};
        quarantine(frame.tag, frame.sender_sequence, deadline);
        return {ReassemblyStatus::Conflict, std::nullopt};
    }

    const std::size_t offset =
        static_cast<std::size_t>(frame.fragment_index) *
        fragment_capacity_;
    const auto bit = static_cast<std::uint16_t>(
        1U << frame.fragment_index);
    if ((slot->received_bitmap & bit) != 0U) {
        const bool identical =
            slot->fragment_lengths[frame.fragment_index] ==
                frame.payload.size() &&
            std::equal(
                frame.payload.begin(), frame.payload.end(),
                slot->bytes.begin() +
                    static_cast<std::ptrdiff_t>(offset));
        if (identical) {
            return {ReassemblyStatus::DuplicateFragment,
                    std::nullopt};
        }
        const auto deadline = slot->deadline;
        *slot = Slot{};
        quarantine(frame.tag, frame.sender_sequence, deadline);
        return {ReassemblyStatus::Conflict, std::nullopt};
    }

    std::copy(frame.payload.begin(), frame.payload.end(),
              slot->bytes.begin() +
                  static_cast<std::ptrdiff_t>(offset));
    slot->fragment_lengths[frame.fragment_index] =
        static_cast<std::uint16_t>(frame.payload.size());
    slot->received_bitmap =
        static_cast<std::uint16_t>(slot->received_bitmap | bit);

    const std::uint16_t complete_mask =
        frame.fragment_count == kMaximumFragments
            ? std::numeric_limits<std::uint16_t>::max()
            : static_cast<std::uint16_t>(
                  (1U << frame.fragment_count) - 1U);
    if (slot->received_bitmap != complete_mask) {
        return {ReassemblyStatus::FragmentAccepted, std::nullopt};
    }

    Bytes canonical(
        slot->bytes.begin(),
        slot->bytes.begin() +
            static_cast<std::ptrdiff_t>(slot->total_length));
    auto decoded = decode_post(canonical.data(), canonical.size());
    if (!decoded ||
        message_tag(decoded.value().message_id()) != slot->tag ||
        decoded.value().sender_sequence() != slot->sender_sequence) {
        const auto deadline = slot->deadline;
        const auto tag = slot->tag;
        const auto sequence = slot->sender_sequence;
        *slot = Slot{};
        quarantine(tag, sequence, deadline);
        return {ReassemblyStatus::InvalidPost, std::nullopt};
    }

    std::vector<Bytes> frames;
    frames.reserve(slot->fragment_count);
    for (std::size_t index = 0; index < slot->fragment_count;
         ++index) {
        const std::size_t fragment_offset =
            index * fragment_capacity_;
        const std::size_t fragment_length =
            slot->fragment_lengths[index];
        DataFrame canonical_frame;
        canonical_frame.tag = slot->tag;
        canonical_frame.sender_sequence = slot->sender_sequence;
        canonical_frame.fragment_index =
            static_cast<std::uint8_t>(index);
        canonical_frame.fragment_count = slot->fragment_count;
        canonical_frame.total_length = slot->total_length;
        canonical_frame.payload.assign(
            slot->bytes.begin() +
                static_cast<std::ptrdiff_t>(fragment_offset),
            slot->bytes.begin() +
                static_cast<std::ptrdiff_t>(
                    fragment_offset + fragment_length));
        auto encoded = encode_frame(canonical_frame, mtu_);
        if (!encoded) {
            const auto deadline = slot->deadline;
            const auto tag = slot->tag;
            const auto sequence = slot->sender_sequence;
            *slot = Slot{};
            quarantine(tag, sequence, deadline);
            return {ReassemblyStatus::InvalidPost, std::nullopt};
        }
        frames.push_back(std::move(encoded).value());
    }

    CompletedPost completed{
        std::move(decoded).value(), std::move(canonical),
        std::move(frames)};
    *slot = Slot{};
    return {ReassemblyStatus::Complete, std::move(completed)};
}

void Reassembler::clear() noexcept {
    for (auto& slot : slots_) {
        slot = Slot{};
    }
    quarantine_.clear();
}

std::size_t Reassembler::active_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        slots_.begin(), slots_.end(),
        [](const Slot& slot) { return slot.used; }));
}

std::size_t Reassembler::high_water_count() const noexcept {
    return high_water_count_;
}

InboundFrameQueue::InboundFrameQueue(std::size_t mtu)
    : mtu_(mtu), reassembler_(mtu) {}

IngressEnqueueStatus InboundFrameQueue::enqueue(
    const std::uint8_t* data, std::size_t size,
    std::uint64_t received_tick) {
    if (stopped_) {
        return IngressEnqueueStatus::Closed;
    }
    if (data == nullptr || size < kFrameHeaderBytes ||
        size > mtu_ || size > kMaximumTransportMtu) {
        return IngressEnqueueStatus::Invalid;
    }
    if (queue_.size() >= kMaximumInboundFrames) {
        return IngressEnqueueStatus::QueueFull;
    }
    queue_.push_back(
        QueuedFrame{Bytes(data, data + size), received_tick});
    update_metrics();
    return IngressEnqueueStatus::Accepted;
}

void InboundFrameQueue::expire_recent(std::uint64_t now) noexcept {
    recent_.erase(
        std::remove_if(
            recent_.begin(), recent_.end(),
            [now](const RecentFrame& frame) {
                return deadline_reached(now, frame.deadline);
            }),
        recent_.end());
}

bool InboundFrameQueue::recently_committed(
    const Bytes& datagram, std::uint64_t now) {
    expire_recent(now);
    const auto found = std::find_if(
        recent_.begin(), recent_.end(),
        [&datagram](const RecentFrame& frame) {
            return frame.datagram == datagram;
        });
    if (found == recent_.end()) {
        return false;
    }
    RecentFrame retained = std::move(*found);
    recent_.erase(found);
    recent_.push_back(std::move(retained));
    return true;
}

IngressProcessResult InboundFrameQueue::process_next(
    std::uint64_t now) {
    if (stopped_) {
        return {IngressProcessStatus::Closed, std::nullopt};
    }
    metrics_.expired_reassemblies += reassembler_.expire(now);
    if (queue_.empty()) {
        update_metrics();
        return {IngressProcessStatus::Empty, std::nullopt};
    }

    QueuedFrame queued = std::move(queue_.front());
    queue_.pop_front();
    update_metrics();
    if (queued.received_tick > now ||
        now - queued.received_tick >=
            kReassemblyTimeoutTicks) {
        ++metrics_.stale_frames;
        return {IngressProcessStatus::Stale, std::nullopt};
    }
    if (recently_committed(queued.datagram, now)) {
        ++metrics_.duplicate_frames;
        return {IngressProcessStatus::RecentDuplicate, std::nullopt};
    }

    auto frame = decode_frame(
        queued.datagram.data(), queued.datagram.size(), mtu_);
    if (!frame) {
        ++metrics_.malformed_frames;
        return {IngressProcessStatus::Malformed, std::nullopt};
    }

    auto reassembled =
        reassembler_.accept(frame.value(), queued.received_tick);
    update_metrics();
    switch (reassembled.status) {
        case ReassemblyStatus::FragmentAccepted:
            return {IngressProcessStatus::FragmentAccepted,
                    std::nullopt};
        case ReassemblyStatus::DuplicateFragment:
            ++metrics_.duplicate_frames;
            return {IngressProcessStatus::DuplicateFragment,
                    std::nullopt};
        case ReassemblyStatus::CapacityExceeded:
            ++metrics_.capacity_rejections;
            return {IngressProcessStatus::CapacityExceeded,
                    std::nullopt};
        case ReassemblyStatus::Conflict:
            return {IngressProcessStatus::Conflict, std::nullopt};
        case ReassemblyStatus::Quarantined:
            return {IngressProcessStatus::Quarantined,
                    std::nullopt};
        case ReassemblyStatus::InvalidPost:
            return {IngressProcessStatus::InvalidPost,
                    std::nullopt};
        case ReassemblyStatus::Complete:
            ++metrics_.completed_posts;
            return {IngressProcessStatus::Complete,
                    std::move(reassembled.completed)};
    }
    return {IngressProcessStatus::InvalidPost, std::nullopt};
}

void InboundFrameQueue::mark_committed(
    const std::vector<Bytes>& canonical_frames,
    std::uint64_t committed_tick) {
    expire_recent(committed_tick);
    const auto deadline =
        saturating_deadline(committed_tick,
                            kReassemblyTimeoutTicks);
    for (const auto& datagram : canonical_frames) {
        if (datagram.size() < kFrameHeaderBytes ||
            datagram.size() > mtu_) {
            continue;
        }
        const auto existing = std::find_if(
            recent_.begin(), recent_.end(),
            [&datagram](const RecentFrame& frame) {
                return frame.datagram == datagram;
            });
        if (existing != recent_.end()) {
            continue;
        }
        if (recent_.size() >= kMaximumRecentFrames) {
            recent_.pop_front();
        }
        recent_.push_back(RecentFrame{datagram, deadline});
    }
}

void InboundFrameQueue::stop() noexcept {
    stopped_ = true;
    queue_.clear();
    recent_.clear();
    reassembler_.clear();
    update_metrics();
}

bool InboundFrameQueue::stopped() const noexcept {
    return stopped_;
}

std::size_t InboundFrameQueue::queued_count() const noexcept {
    return queue_.size();
}

const IngressMetrics& InboundFrameQueue::metrics() const noexcept {
    return metrics_;
}

void InboundFrameQueue::update_metrics() noexcept {
    metrics_.queued_frames = queue_.size();
    metrics_.queue_high_water =
        std::max(metrics_.queue_high_water, queue_.size());
    metrics_.active_reassemblies = reassembler_.active_count();
    metrics_.reassembly_high_water =
        std::max(metrics_.reassembly_high_water,
                 reassembler_.high_water_count());
}

} // namespace lora::protocol

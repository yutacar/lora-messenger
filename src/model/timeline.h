/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/limits.h"
#include "core/result.h"
#include "model/post.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace lora::model {

struct ReceivedOrigin {
    friend constexpr bool operator==(ReceivedOrigin, ReceivedOrigin) noexcept {
        return true;
    }
};

enum class LocalDeliveryState {
    Queued,
    Broadcast,
    Failed,
    Unknown,
};

struct LocalDelivery {
    LocalDeliveryState state = LocalDeliveryState::Queued;

    friend constexpr bool operator==(LocalDelivery left, LocalDelivery right) noexcept {
        return left.state == right.state;
    }
};

using EntryOrigin = std::variant<ReceivedOrigin, LocalDelivery>;

struct TimelineEntry {
    PostPayload post;
    std::uint64_t received_order;
    EntryOrigin origin;
};

enum class TimelineInsertError {
    None,
    Duplicate,
    Conflict,
    QueueFull,
    Full,
    OrderExhausted,
};

struct TimelineInsertResult {
    TimelineInsertError error = TimelineInsertError::None;
    std::optional<core::MessageId> evicted_message_id;
    std::optional<std::uint64_t> received_order;

    bool ok() const noexcept { return error == TimelineInsertError::None; }
};

enum class TransitionError {
    None,
    NotFound,
    NotLocal,
    InvalidState,
};

enum class ReplyState {
    NotReply,
    ParentAvailable,
    ParentUnavailable,
};

enum class TimelineRestoreError {
    InvalidCapacity,
    TooManyEntries,
    InvalidOrder,
    DuplicateMessageId,
    TooManyQueuedEntries,
};

class Timeline {
public:
    explicit Timeline(std::size_t capacity = core::kMaxTimelineEntries,
                      std::uint64_t last_assigned_order = 0);
    static core::Result<Timeline, TimelineRestoreError> restore(
        std::size_t capacity, std::uint64_t last_assigned_order,
        std::vector<TimelineEntry> entries);

    std::size_t capacity() const noexcept;
    std::size_t size() const noexcept;
    bool empty() const noexcept;
    std::uint64_t last_assigned_order() const noexcept;
    std::size_t queued_capacity() const noexcept;
    std::size_t queued_count() const noexcept;
    // Borrowed views are invalid after any non-const Timeline operation. UI and
    // application callers must retain MessageId handles and re-query instead.
    const std::vector<TimelineEntry>& entries() const noexcept;

    const TimelineEntry* find(const core::MessageId& message_id) const noexcept;
    const TimelineEntry* newest_at(std::size_t index) const noexcept;
    ReplyState reply_state(const TimelineEntry& entry) const noexcept;
    bool mentions(const TimelineEntry& entry,
                  const core::InstallId& install_id) const noexcept;

    TimelineInsertError insertion_availability(
        std::optional<core::MessageId> protected_message_id = std::nullopt) const noexcept;
    TimelineInsertResult insert_received(PostPayload post);
    TimelineInsertResult insert_local(
        PostPayload post,
        LocalDeliveryState initial_state = LocalDeliveryState::Queued);

    TransitionError mark_broadcast(const core::MessageId& message_id) noexcept;
    TransitionError mark_failed(const core::MessageId& message_id) noexcept;

private:
    TimelineInsertResult insert(
        PostPayload post, EntryOrigin origin,
        std::optional<core::MessageId> protected_message_id = std::nullopt);
    TransitionError transition(const core::MessageId& message_id,
                               LocalDeliveryState target) noexcept;
    TimelineEntry* find_mutable(const core::MessageId& message_id) noexcept;
    std::optional<std::size_t> eviction_index(
        const std::optional<core::MessageId>& protected_message_id) const noexcept;

    std::size_t capacity_;
    std::uint64_t last_assigned_order_;
    std::vector<TimelineEntry> entries_;
};

} // namespace lora::model

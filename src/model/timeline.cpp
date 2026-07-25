/*
 * SPDX-License-Identifier: MIT
 */

#include "model/timeline.h"

#include "core/limits.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace lora::model {

static_assert(std::is_nothrow_move_constructible_v<TimelineEntry>);
static_assert(std::is_nothrow_move_assignable_v<TimelineEntry>);

Timeline::Timeline(std::size_t capacity, std::uint64_t last_assigned_order)
    : capacity_(capacity), last_assigned_order_(last_assigned_order) {
    if (capacity == 0 || capacity > core::kMaxTimelineEntries) {
        throw std::invalid_argument("timeline capacity is outside the supported range");
    }
    entries_.reserve(capacity_);
}

core::Result<Timeline, TimelineRestoreError> Timeline::restore(
    std::size_t capacity, std::uint64_t last_assigned_order,
    std::vector<TimelineEntry> entries) {
    if (capacity == 0 || capacity > core::kMaxTimelineEntries) {
        return core::Result<Timeline, TimelineRestoreError>::failure(
            TimelineRestoreError::InvalidCapacity);
    }
    if (entries.size() > capacity) {
        return core::Result<Timeline, TimelineRestoreError>::failure(
            TimelineRestoreError::TooManyEntries);
    }

    std::uint64_t previous_order = 0;
    std::size_t queued_count = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        if (entry.received_order == 0 ||
            entry.received_order > last_assigned_order ||
            (index != 0 && entry.received_order <= previous_order)) {
            return core::Result<Timeline, TimelineRestoreError>::failure(
                TimelineRestoreError::InvalidOrder);
        }
        previous_order = entry.received_order;

        if (std::any_of(entries.begin(),
                        entries.begin() + static_cast<std::ptrdiff_t>(index),
                        [&](const TimelineEntry& existing) {
                            return existing.post.message_id() ==
                                   entry.post.message_id();
                        })) {
            return core::Result<Timeline, TimelineRestoreError>::failure(
                TimelineRestoreError::DuplicateMessageId);
        }

        const auto* local = std::get_if<LocalDelivery>(&entry.origin);
        if (local && local->state == LocalDeliveryState::Queued) {
            ++queued_count;
        }
    }

    const auto queued_capacity =
        std::min(core::kMaxQueuedLocalPosts, capacity - 1);
    if (queued_count > queued_capacity) {
        return core::Result<Timeline, TimelineRestoreError>::failure(
            TimelineRestoreError::TooManyQueuedEntries);
    }

    Timeline restored(capacity, last_assigned_order);
    restored.entries_ = std::move(entries);
    return core::Result<Timeline, TimelineRestoreError>::success(
        std::move(restored));
}

std::size_t Timeline::capacity() const noexcept { return capacity_; }
std::size_t Timeline::size() const noexcept { return entries_.size(); }
bool Timeline::empty() const noexcept { return entries_.empty(); }
std::uint64_t Timeline::last_assigned_order() const noexcept { return last_assigned_order_; }
const std::vector<TimelineEntry>& Timeline::entries() const noexcept { return entries_; }

std::size_t Timeline::queued_capacity() const noexcept {
    return std::min(core::kMaxQueuedLocalPosts, capacity_ - 1);
}

std::size_t Timeline::queued_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        entries_.begin(), entries_.end(), [](const TimelineEntry& entry) {
            const auto* local = std::get_if<LocalDelivery>(&entry.origin);
            return local && local->state == LocalDeliveryState::Queued;
        }));
}

const TimelineEntry* Timeline::find(const core::MessageId& message_id) const noexcept {
    const auto iterator = std::find_if(entries_.begin(), entries_.end(),
        [&](const TimelineEntry& entry) { return entry.post.message_id() == message_id; });
    return iterator == entries_.end() ? nullptr : &*iterator;
}

TimelineEntry* Timeline::find_mutable(const core::MessageId& message_id) noexcept {
    const auto iterator = std::find_if(entries_.begin(), entries_.end(),
        [&](const TimelineEntry& entry) { return entry.post.message_id() == message_id; });
    return iterator == entries_.end() ? nullptr : &*iterator;
}

const TimelineEntry* Timeline::newest_at(std::size_t index) const noexcept {
    if (index >= entries_.size()) {
        return nullptr;
    }
    return &entries_[entries_.size() - 1 - index];
}

ReplyState Timeline::reply_state(const TimelineEntry& entry) const noexcept {
    if (!entry.post.reply_to()) {
        return ReplyState::NotReply;
    }
    return find(*entry.post.reply_to()) ? ReplyState::ParentAvailable
                                        : ReplyState::ParentUnavailable;
}

bool Timeline::mentions(const TimelineEntry& entry,
                        const core::InstallId& install_id) const noexcept {
    const auto& values = entry.post.mentions();
    return std::find(values.begin(), values.end(), install_id) != values.end();
}

std::optional<std::size_t> Timeline::eviction_index(
    const std::optional<core::MessageId>& protected_message_id) const noexcept {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (protected_message_id &&
            entries_[index].post.message_id() == *protected_message_id) {
            continue;
        }
        const auto* local = std::get_if<LocalDelivery>(&entries_[index].origin);
        if (!local || local->state != LocalDeliveryState::Queued) {
            return index;
        }
    }
    return std::nullopt;
}

TimelineInsertError Timeline::insertion_availability(
    std::optional<core::MessageId> protected_message_id) const noexcept {
    if (last_assigned_order_ == std::numeric_limits<std::uint64_t>::max()) {
        return TimelineInsertError::OrderExhausted;
    }
    if (entries_.size() < capacity_ || eviction_index(protected_message_id)) {
        return TimelineInsertError::None;
    }
    return TimelineInsertError::Full;
}

TimelineInsertResult Timeline::insert_received(PostPayload post) {
    return insert(std::move(post), ReceivedOrigin{});
}

TimelineInsertResult Timeline::insert_local(PostPayload post,
                                            LocalDeliveryState initial_state) {
    const auto protected_message_id = post.reply_to();
    return insert(std::move(post), LocalDelivery{initial_state},
                  protected_message_id);
}

TimelineInsertResult Timeline::insert(
    PostPayload post, EntryOrigin origin,
    std::optional<core::MessageId> protected_message_id) {
    if (const auto* existing = find(post.message_id())) {
        return {existing->post == post ? TimelineInsertError::Duplicate
                                      : TimelineInsertError::Conflict,
                std::nullopt, std::nullopt};
    }

    const auto* local = std::get_if<LocalDelivery>(&origin);
    if (local && local->state == LocalDeliveryState::Queued &&
        queued_count() >= queued_capacity()) {
        return {TimelineInsertError::QueueFull, std::nullopt, std::nullopt};
    }

    const auto availability = insertion_availability(protected_message_id);
    if (availability != TimelineInsertError::None) {
        return {availability, std::nullopt, std::nullopt};
    }

    const std::uint64_t next_order = last_assigned_order_ + 1;
    TimelineEntry candidate{std::move(post), next_order, std::move(origin)};
    std::optional<core::MessageId> evicted;
    if (entries_.size() == capacity_) {
        const auto index = *eviction_index(protected_message_id);
        evicted = entries_[index].post.message_id();
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    entries_.push_back(std::move(candidate));
    last_assigned_order_ = next_order;
    return {TimelineInsertError::None, std::move(evicted), next_order};
}

TransitionError Timeline::mark_broadcast(const core::MessageId& message_id) noexcept {
    return transition(message_id, LocalDeliveryState::Broadcast);
}

TransitionError Timeline::mark_failed(const core::MessageId& message_id) noexcept {
    return transition(message_id, LocalDeliveryState::Failed);
}

TransitionError Timeline::transition(const core::MessageId& message_id,
                                     LocalDeliveryState target) noexcept {
    auto* entry = find_mutable(message_id);
    if (!entry) {
        return TransitionError::NotFound;
    }
    auto* local = std::get_if<LocalDelivery>(&entry->origin);
    if (!local) {
        return TransitionError::NotLocal;
    }
    if (local->state == target) {
        return TransitionError::None;
    }
    if (local->state != LocalDeliveryState::Queued) {
        return TransitionError::InvalidState;
    }
    local->state = target;
    return TransitionError::None;
}

} // namespace lora::model

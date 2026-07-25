/*
 * SPDX-License-Identifier: MIT
 */

#include "application/messenger_state.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace lora::application {
namespace {

static_assert(std::is_nothrow_move_assignable_v<model::Identity>);

CommandResult map_insert_result(model::TimelineInsertResult result) {
    CommandResult command;
    command.evicted_message_id = std::move(result.evicted_message_id);
    switch (result.error) {
        case model::TimelineInsertError::None:
            break;
        case model::TimelineInsertError::Duplicate:
            command.error = CommandError::DuplicatePost;
            break;
        case model::TimelineInsertError::Conflict:
            command.error = CommandError::ConflictingPost;
            break;
        case model::TimelineInsertError::QueueFull:
            command.error = CommandError::QueueFull;
            break;
        case model::TimelineInsertError::Full:
            command.error = CommandError::TimelineFull;
            break;
        case model::TimelineInsertError::OrderExhausted:
            command.error = CommandError::OrderExhausted;
            break;
    }
    return command;
}

CommandResult map_availability(model::TimelineInsertError error) {
    CommandResult result;
    if (error == model::TimelineInsertError::Full) {
        result.error = CommandError::TimelineFull;
    } else if (error == model::TimelineInsertError::OrderExhausted) {
        result.error = CommandError::OrderExhausted;
    }
    return result;
}

} // namespace

MessengerState::MessengerState(ports::IRandomBytes& random, ports::IWallClock& clock,
                               std::size_t timeline_capacity,
                               std::uint64_t last_assigned_order,
                               ports::IStateCommit* commit)
    : random_(random),
      clock_(clock),
      timeline_(timeline_capacity, last_assigned_order),
      commit_(commit) {}

MessengerStatus MessengerState::status() const noexcept {
    return identity_ ? MessengerStatus::Ready : MessengerStatus::Uninitialized;
}

const std::optional<model::Identity>& MessengerState::identity() const noexcept {
    return identity_;
}

const model::Timeline& MessengerState::timeline() const noexcept {
    return timeline_;
}

CommandResult MessengerState::initialize_new(std::string_view user_id) {
    if (identity_) {
        return {CommandError::AlreadyInitialized};
    }
    auto validated_user_id = core::UserId::create(user_id);
    if (!validated_user_id) {
        return {CommandError::InvalidUserId, validated_user_id.error()};
    }
    auto install_id = core::InstallId::generate(random_);
    if (!install_id) {
        return {CommandError::RandomUnavailable};
    }
    auto candidate = model::Identity::restore(std::move(install_id).value(),
                                              std::move(validated_user_id).value());
    if (commit_ && !commit_->persist_identity(candidate)) {
        return {CommandError::PersistenceUnavailable};
    }
    identity_ = std::move(candidate);
    return {};
}

CommandResult MessengerState::restore_identity(model::Identity identity) {
    if (identity_) {
        return {CommandError::AlreadyInitialized};
    }
    identity_ = std::move(identity);
    return {};
}

CommandResult MessengerState::restore_state(model::Identity identity,
                                            model::Timeline timeline) {
    if (identity_) {
        return {CommandError::AlreadyInitialized};
    }
    identity_ = std::move(identity);
    timeline_ = std::move(timeline);
    return {};
}

CommandResult MessengerState::rename_user(std::string_view user_id) {
    if (!identity_) {
        return {CommandError::NotInitialized};
    }
    auto validated = core::UserId::create(user_id);
    if (!validated) {
        return {CommandError::InvalidUserId, validated.error()};
    }
    auto candidate = *identity_;
    candidate.rename(std::move(validated).value());
    if (commit_ && !commit_->persist_identity(candidate)) {
        return {CommandError::PersistenceUnavailable};
    }
    *identity_ = std::move(candidate);
    return {};
}

CommandResult MessengerState::compose(model::PostDraft draft) {
    if (!identity_) {
        return {CommandError::NotInitialized};
    }

    auto body = core::PostBody::create(draft.body);
    if (!body) {
        return {CommandError::InvalidBody, body.error()};
    }
    if (draft.mentions.size() > core::kMaxMentions) {
        return {CommandError::TooManyMentions};
    }
    for (std::size_t index = 0; index < draft.mentions.size(); ++index) {
        if (std::find(draft.mentions.begin() + static_cast<std::ptrdiff_t>(index + 1),
                      draft.mentions.end(), draft.mentions[index]) != draft.mentions.end()) {
            return {CommandError::DuplicateMention};
        }
    }
    if (draft.reply_to && !timeline_.find(*draft.reply_to)) {
        return {CommandError::ReplyParentUnavailable};
    }
    if (timeline_.queued_count() >= timeline_.queued_capacity()) {
        return {CommandError::QueueFull};
    }

    const auto sender_sequence = identity_->next_sender_sequence();
    if (!sender_sequence) {
        return {CommandError::SequenceExhausted};
    }
    auto updated_identity = *identity_;
    if (!updated_identity.commit_sender_sequence(*sender_sequence)) {
        return {CommandError::SequenceExhausted};
    }
    if (const auto unavailable = timeline_.insertion_availability(draft.reply_to);
        unavailable != model::TimelineInsertError::None) {
        return map_availability(unavailable);
    }
    auto message_id = core::MessageId::generate(random_);
    if (!message_id) {
        return {CommandError::RandomUnavailable};
    }
    if (timeline_.find(message_id.value())) {
        return {CommandError::MessageIdCollision};
    }

    model::PostPayloadInput input;
    input.message_id = message_id.value().uuid();
    input.sender_id = identity_->install_id().uuid();
    input.sender_sequence = *sender_sequence;
    input.sender_user_id = identity_->user_id().value();
    input.body = std::move(draft.body);
    input.mentions.reserve(draft.mentions.size());
    for (const auto& mention : draft.mentions) {
        input.mentions.push_back(mention.uuid());
    }
    if (draft.reply_to) {
        input.reply_to = draft.reply_to->uuid();
    }
    input.sender_time = clock_.now_unix_seconds();

    auto post = model::PostPayload::create(std::move(input));
    if (!post) {
        return {CommandError::InvalidPost, core::TextError::None, post.error()};
    }
    const core::MessageId created_id = post.value().message_id();
    auto candidate_timeline = timeline_;
    auto inserted = candidate_timeline.insert_local(std::move(post).value());
    if (!inserted.ok()) {
        return map_insert_result(std::move(inserted));
    }

    // Reserve the sender sequence durably before committing the history. If
    // history persistence then fails, adopting only this high-water mark creates
    // a safe gap and prevents reuse after this process or the next launch.
    if (commit_ && !commit_->persist_identity(updated_identity)) {
        return {CommandError::PersistenceUnavailable};
    }
    *identity_ = std::move(updated_identity);
    if (commit_ && !commit_->persist_timeline(candidate_timeline)) {
        return {CommandError::PersistenceUnavailable};
    }
    timeline_ = std::move(candidate_timeline);

    CommandResult result = map_insert_result(std::move(inserted));
    result.message_id = created_id;
    return result;
}

CommandResult MessengerState::accept_received(model::PostPayload post) {
    if (!identity_) {
        return {CommandError::NotInitialized};
    }
    auto candidate = timeline_;
    auto result = map_insert_result(candidate.insert_received(std::move(post)));
    if (!result.ok()) {
        return result;
    }
    if (commit_ && !commit_->persist_timeline(candidate)) {
        return {CommandError::PersistenceUnavailable};
    }
    timeline_ = std::move(candidate);
    return result;
}

CommandResult MessengerState::mark_broadcast(const core::MessageId& message_id) {
    return transition(message_id, true);
}

CommandResult MessengerState::mark_failed(const core::MessageId& message_id) {
    return transition(message_id, false);
}

CommandResult MessengerState::transition(const core::MessageId& message_id,
                                         bool broadcast) {
    if (!identity_) {
        return {CommandError::NotInitialized};
    }
    auto candidate = timeline_;
    const auto error = broadcast ? candidate.mark_broadcast(message_id)
                                 : candidate.mark_failed(message_id);
    switch (error) {
        case model::TransitionError::None:
            if (commit_ && !commit_->persist_timeline(candidate)) {
                return {CommandError::PersistenceUnavailable};
            }
            timeline_ = std::move(candidate);
            return {};
        case model::TransitionError::NotFound:
            return {CommandError::MessageNotFound};
        case model::TransitionError::NotLocal:
            return {CommandError::NotLocalPost};
        case model::TransitionError::InvalidState:
            return {CommandError::InvalidTransition};
    }
    return {CommandError::InvalidTransition};
}

} // namespace lora::application

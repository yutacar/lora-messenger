/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/limits.h"
#include "core/text.h"
#include "model/identity.h"
#include "model/post.h"
#include "model/timeline.h"
#include "ports/clock.h"
#include "ports/random.h"
#include "ports/state_commit.h"

#include <cstddef>
#include <optional>
#include <string_view>

namespace lora::application {

enum class MessengerStatus {
    Uninitialized,
    Ready,
};

enum class CommandError {
    None,
    NotInitialized,
    AlreadyInitialized,
    InvalidUserId,
    InvalidBody,
    TooManyMentions,
    DuplicateMention,
    ReplyParentUnavailable,
    QueueFull,
    TimelineFull,
    SequenceExhausted,
    OrderExhausted,
    RandomUnavailable,
    MessageIdCollision,
    InvalidPost,
    DuplicatePost,
    ConflictingPost,
    MessageNotFound,
    NotLocalPost,
    InvalidTransition,
    PersistenceUnavailable,
};

struct CommandResult {
    CommandError error = CommandError::None;
    core::TextError text_error = core::TextError::None;
    model::PostError post_error = model::PostError::None;
    std::optional<core::MessageId> message_id;
    std::optional<core::MessageId> evicted_message_id;

    CommandResult() = default;
    CommandResult(CommandError command_error) : error(command_error) {}
    CommandResult(CommandError command_error, core::TextError validation_error)
        : error(command_error), text_error(validation_error) {}
    CommandResult(CommandError command_error, core::TextError validation_error,
                  model::PostError validation_post_error)
        : error(command_error), text_error(validation_error),
          post_error(validation_post_error) {}

    bool ok() const noexcept { return error == CommandError::None; }
};

class MessengerState {
public:
    MessengerState(ports::IRandomBytes& random, ports::IWallClock& clock,
                   std::size_t timeline_capacity = core::kMaxTimelineEntries,
                   std::uint64_t last_assigned_order = 0,
                   ports::IStateCommit* commit = nullptr);

    MessengerStatus status() const noexcept;
    const std::optional<model::Identity>& identity() const noexcept;
    const model::Timeline& timeline() const noexcept;

    CommandResult initialize_new(std::string_view user_id);
    CommandResult restore_identity(model::Identity identity);
    CommandResult restore_state(model::Identity identity,
                                model::Timeline timeline);
    CommandResult rename_user(std::string_view user_id);
    CommandResult compose(model::PostDraft draft);
    CommandResult accept_received(model::PostPayload post);
    CommandResult mark_broadcast(const core::MessageId& message_id);
    CommandResult mark_failed(const core::MessageId& message_id);

private:
    CommandResult transition(const core::MessageId& message_id,
                             bool broadcast);

    ports::IRandomBytes& random_;
    ports::IWallClock& clock_;
    std::optional<model::Identity> identity_;
    model::Timeline timeline_;
    ports::IStateCommit* commit_;
};

} // namespace lora::application

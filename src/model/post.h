/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/result.h"
#include "core/text.h"
#include "core/uuid.h"
#include "ports/clock.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lora::model {

enum class PostError {
    None,
    InvalidMessageId,
    InvalidSenderId,
    InvalidSenderSequence,
    InvalidSenderUserId,
    InvalidBody,
    TooManyMentions,
    InvalidMentionId,
    DuplicateMention,
    InvalidReplyId,
    SelfReply,
};

struct PostPayloadInput {
    core::Uuid message_id;
    core::Uuid sender_id;
    std::uint64_t sender_sequence = 0;
    std::string sender_user_id;
    std::string body;
    std::vector<core::Uuid> mentions;
    std::optional<core::Uuid> reply_to;
    std::optional<ports::UnixSeconds> sender_time;
};

class PostPayload {
public:
    static core::Result<PostPayload, PostError> create(PostPayloadInput input);

    const core::MessageId& message_id() const noexcept;
    const core::InstallId& sender_id() const noexcept;
    std::uint64_t sender_sequence() const noexcept;
    const core::UserId& sender_user_id() const noexcept;
    const core::PostBody& body() const noexcept;
    const std::vector<core::InstallId>& mentions() const noexcept;
    const std::optional<core::MessageId>& reply_to() const noexcept;
    const std::optional<ports::UnixSeconds>& sender_time() const noexcept;

    friend bool operator==(const PostPayload& left, const PostPayload& right) noexcept;
    friend bool operator!=(const PostPayload& left, const PostPayload& right) noexcept;

private:
    PostPayload(core::MessageId message_id, core::InstallId sender_id,
                std::uint64_t sender_sequence, core::UserId sender_user_id,
                core::PostBody body, std::vector<core::InstallId> mentions,
                std::optional<core::MessageId> reply_to,
                std::optional<ports::UnixSeconds> sender_time);

    core::MessageId message_id_;
    core::InstallId sender_id_;
    std::uint64_t sender_sequence_;
    core::UserId sender_user_id_;
    core::PostBody body_;
    std::vector<core::InstallId> mentions_;
    std::optional<core::MessageId> reply_to_;
    std::optional<ports::UnixSeconds> sender_time_;
};

struct PostDraft {
    std::string body;
    std::vector<core::InstallId> mentions;
    std::optional<core::MessageId> reply_to;
};

} // namespace lora::model

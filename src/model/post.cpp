/*
 * SPDX-License-Identifier: MIT
 */

#include "model/post.h"

#include "core/limits.h"

#include <algorithm>
#include <utility>

namespace lora::model {

PostPayload::PostPayload(core::MessageId message_id, core::InstallId sender_id,
                         std::uint64_t sender_sequence, core::UserId sender_user_id,
                         core::PostBody body, std::vector<core::InstallId> mentions,
                         std::optional<core::MessageId> reply_to,
                         std::optional<ports::UnixSeconds> sender_time)
    : message_id_(std::move(message_id)),
      sender_id_(std::move(sender_id)),
      sender_sequence_(sender_sequence),
      sender_user_id_(std::move(sender_user_id)),
      body_(std::move(body)),
      mentions_(std::move(mentions)),
      reply_to_(std::move(reply_to)),
      sender_time_(sender_time) {}

core::Result<PostPayload, PostError> PostPayload::create(PostPayloadInput input) {
    auto message_id = core::MessageId::from_uuid(std::move(input.message_id));
    if (!message_id) {
        return core::Result<PostPayload, PostError>::failure(PostError::InvalidMessageId);
    }

    auto sender_id = core::InstallId::from_uuid(std::move(input.sender_id));
    if (!sender_id) {
        return core::Result<PostPayload, PostError>::failure(PostError::InvalidSenderId);
    }
    if (input.sender_sequence == 0) {
        return core::Result<PostPayload, PostError>::failure(PostError::InvalidSenderSequence);
    }

    auto sender_user_id = core::UserId::create(input.sender_user_id);
    if (!sender_user_id) {
        return core::Result<PostPayload, PostError>::failure(PostError::InvalidSenderUserId);
    }
    auto body = core::PostBody::create(input.body);
    if (!body) {
        return core::Result<PostPayload, PostError>::failure(PostError::InvalidBody);
    }
    if (input.mentions.size() > core::kMaxMentions) {
        return core::Result<PostPayload, PostError>::failure(PostError::TooManyMentions);
    }

    std::vector<core::InstallId> mentions;
    mentions.reserve(input.mentions.size());
    for (auto& raw_mention : input.mentions) {
        auto mention = core::InstallId::from_uuid(std::move(raw_mention));
        if (!mention) {
            return core::Result<PostPayload, PostError>::failure(PostError::InvalidMentionId);
        }
        if (std::find(mentions.begin(), mentions.end(), mention.value()) != mentions.end()) {
            return core::Result<PostPayload, PostError>::failure(PostError::DuplicateMention);
        }
        mentions.push_back(std::move(mention).value());
    }

    std::optional<core::MessageId> reply_to;
    if (input.reply_to) {
        auto reply = core::MessageId::from_uuid(std::move(*input.reply_to));
        if (!reply) {
            return core::Result<PostPayload, PostError>::failure(PostError::InvalidReplyId);
        }
        if (reply.value() == message_id.value()) {
            return core::Result<PostPayload, PostError>::failure(PostError::SelfReply);
        }
        reply_to = std::move(reply).value();
    }

    return core::Result<PostPayload, PostError>::success(PostPayload(
        std::move(message_id).value(), std::move(sender_id).value(),
        input.sender_sequence, std::move(sender_user_id).value(),
        std::move(body).value(), std::move(mentions), std::move(reply_to),
        input.sender_time));
}

const core::MessageId& PostPayload::message_id() const noexcept { return message_id_; }
const core::InstallId& PostPayload::sender_id() const noexcept { return sender_id_; }
std::uint64_t PostPayload::sender_sequence() const noexcept { return sender_sequence_; }
const core::UserId& PostPayload::sender_user_id() const noexcept { return sender_user_id_; }
const core::PostBody& PostPayload::body() const noexcept { return body_; }
const std::vector<core::InstallId>& PostPayload::mentions() const noexcept { return mentions_; }
const std::optional<core::MessageId>& PostPayload::reply_to() const noexcept { return reply_to_; }
const std::optional<ports::UnixSeconds>& PostPayload::sender_time() const noexcept { return sender_time_; }

bool operator==(const PostPayload& left, const PostPayload& right) noexcept {
    return left.message_id_ == right.message_id_ &&
           left.sender_id_ == right.sender_id_ &&
           left.sender_sequence_ == right.sender_sequence_ &&
           left.sender_user_id_ == right.sender_user_id_ &&
           left.body_ == right.body_ &&
           left.mentions_ == right.mentions_ &&
           left.reply_to_ == right.reply_to_ &&
           left.sender_time_ == right.sender_time_;
}

bool operator!=(const PostPayload& left, const PostPayload& right) noexcept {
    return !(left == right);
}

} // namespace lora::model

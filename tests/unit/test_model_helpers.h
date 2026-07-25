/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/post.h"

#include "test_support.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lora::test {

inline model::PostPayloadInput make_post_input(
    std::uint8_t message_discriminator,
    std::uint8_t sender_discriminator,
    std::uint64_t sender_sequence = 1,
    std::string body = "hello",
    std::string sender_user_id = "alice") {
    model::PostPayloadInput input;
    input.message_id = make_uuid(message_discriminator);
    input.sender_id = make_uuid(sender_discriminator);
    input.sender_sequence = sender_sequence;
    input.sender_user_id = std::move(sender_user_id);
    input.body = std::move(body);
    return input;
}

inline model::PostPayload make_post(
    std::uint8_t message_discriminator,
    std::uint8_t sender_discriminator,
    std::uint64_t sender_sequence = 1,
    std::string body = "hello",
    std::vector<core::InstallId> mentions = {},
    std::optional<core::MessageId> reply_to = std::nullopt,
    std::optional<ports::UnixSeconds> sender_time = std::nullopt,
    std::string sender_user_id = "alice") {
    auto input = make_post_input(message_discriminator, sender_discriminator,
                                 sender_sequence, std::move(body),
                                 std::move(sender_user_id));
    input.mentions.reserve(mentions.size());
    for (const auto& mention : mentions) {
        input.mentions.push_back(mention.uuid());
    }
    if (reply_to) {
        input.reply_to = reply_to->uuid();
    }
    input.sender_time = sender_time;
    auto result = model::PostPayload::create(std::move(input));
    return std::move(result).value();
}

} // namespace lora::test

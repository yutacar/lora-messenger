/*
 * SPDX-License-Identifier: MIT
 */

#include "model/post.h"

#include "test_model_helpers.h"
#include "test_support.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

int main() {
    using lora::model::PostError;
    using lora::model::PostPayload;
    lora::test::Runner runner;

    runner.run("a valid post preserves every validated wire field", [&] {
        auto input = lora::test::make_post_input(20, 40, 77U, u8"hello 世界", u8"アリス");
        input.mentions = {
            lora::test::make_uuid(50),
            lora::test::make_uuid(51),
            lora::test::make_uuid(52),
            lora::test::make_uuid(53),
        };
        input.reply_to = lora::test::make_uuid(19);
        input.sender_time = 1'725'000'123;

        const auto result = PostPayload::create(std::move(input));
        REQUIRE(result.has_value());
        CHECK_EQ(result.value().message_id(), lora::test::make_message_id(20));
        CHECK_EQ(result.value().sender_id(), lora::test::make_install_id(40));
        CHECK_EQ(result.value().sender_sequence(), 77U);
        CHECK_EQ(result.value().sender_user_id().value(), std::string(u8"アリス"));
        CHECK_EQ(result.value().body().value(), std::string(u8"hello 世界"));
        REQUIRE(result.value().mentions().size() == 4U);
        CHECK_EQ(result.value().mentions()[0], lora::test::make_install_id(50));
        CHECK_EQ(result.value().mentions()[1], lora::test::make_install_id(51));
        CHECK_EQ(result.value().mentions()[2], lora::test::make_install_id(52));
        CHECK_EQ(result.value().mentions()[3], lora::test::make_install_id(53));
        REQUIRE(result.value().reply_to().has_value());
        CHECK_EQ(*result.value().reply_to(), lora::test::make_message_id(19));
        REQUIRE(result.value().sender_time().has_value());
        CHECK_EQ(*result.value().sender_time(), 1'725'000'123);
    });

    runner.run("required UUID and sequence fields reject invalid values", [&] {
        auto invalid_message = lora::test::make_post_input(20, 40);
        invalid_message.message_id = lora::core::Uuid{};
        const auto message_result = PostPayload::create(std::move(invalid_message));
        CHECK(!message_result.has_value());
        CHECK_EQ(message_result.error(), PostError::InvalidMessageId);

        auto invalid_sender = lora::test::make_post_input(20, 40);
        invalid_sender.sender_id = lora::core::Uuid{};
        const auto sender_result = PostPayload::create(std::move(invalid_sender));
        CHECK(!sender_result.has_value());
        CHECK_EQ(sender_result.error(), PostError::InvalidSenderId);

        auto invalid_sequence = lora::test::make_post_input(20, 40);
        invalid_sequence.sender_sequence = 0;
        const auto sequence_result = PostPayload::create(std::move(invalid_sequence));
        CHECK(!sequence_result.has_value());
        CHECK_EQ(sequence_result.error(), PostError::InvalidSenderSequence);
    });

    runner.run("invalid user IDs and bodies are rejected at the post boundary", [&] {
        auto invalid_user = lora::test::make_post_input(20, 40);
        invalid_user.sender_user_id = " alice";
        const auto user_result = PostPayload::create(std::move(invalid_user));
        CHECK(!user_result.has_value());
        CHECK_EQ(user_result.error(), PostError::InvalidSenderUserId);

        auto invalid_body = lora::test::make_post_input(20, 40);
        invalid_body.body = "\t";
        const auto body_result = PostPayload::create(std::move(invalid_body));
        CHECK(!body_result.has_value());
        CHECK_EQ(body_result.error(), PostError::InvalidBody);
    });

    runner.run("mention count, validity, and uniqueness are enforced", [&] {
        auto too_many = lora::test::make_post_input(20, 40);
        too_many.mentions = {
            lora::test::make_uuid(1), lora::test::make_uuid(2),
            lora::test::make_uuid(3), lora::test::make_uuid(4),
            lora::test::make_uuid(5),
        };
        const auto count_result = PostPayload::create(std::move(too_many));
        CHECK(!count_result.has_value());
        CHECK_EQ(count_result.error(), PostError::TooManyMentions);

        auto invalid = lora::test::make_post_input(20, 40);
        invalid.mentions = {lora::core::Uuid{}};
        const auto invalid_result = PostPayload::create(std::move(invalid));
        CHECK(!invalid_result.has_value());
        CHECK_EQ(invalid_result.error(), PostError::InvalidMentionId);

        auto duplicate = lora::test::make_post_input(20, 40);
        duplicate.mentions = {
            lora::test::make_uuid(8),
            lora::test::make_uuid(9),
            lora::test::make_uuid(8),
        };
        const auto duplicate_result = PostPayload::create(std::move(duplicate));
        CHECK(!duplicate_result.has_value());
        CHECK_EQ(duplicate_result.error(), PostError::DuplicateMention);
    });

    runner.run("reply IDs must be valid and cannot point to the post itself", [&] {
        auto invalid = lora::test::make_post_input(20, 40);
        invalid.reply_to = lora::core::Uuid{};
        const auto invalid_result = PostPayload::create(std::move(invalid));
        CHECK(!invalid_result.has_value());
        CHECK_EQ(invalid_result.error(), PostError::InvalidReplyId);

        auto self_reply = lora::test::make_post_input(20, 40);
        self_reply.reply_to = lora::test::make_uuid(20);
        const auto self_result = PostPayload::create(std::move(self_reply));
        CHECK(!self_result.has_value());
        CHECK_EQ(self_result.error(), PostError::SelfReply);
    });

    runner.run("optional fields can be absent and byte limits are inclusive", [&] {
        auto input = lora::test::make_post_input(
            21, 41, 1U, std::string(160, 'x'), std::string(24, 'u'));
        const auto result = PostPayload::create(std::move(input));
        REQUIRE(result.has_value());
        CHECK(result.value().mentions().empty());
        CHECK(!result.value().reply_to().has_value());
        CHECK(!result.value().sender_time().has_value());
        CHECK_EQ(result.value().body().value().size(), 160U);
        CHECK_EQ(result.value().sender_user_id().value().size(), 24U);

        auto maximum_sequence = lora::test::make_post_input(
            22, 42, std::numeric_limits<std::uint64_t>::max(), "x", "u");
        const auto maximum_result = PostPayload::create(std::move(maximum_sequence));
        REQUIRE(maximum_result.has_value());
        CHECK_EQ(maximum_result.value().sender_sequence(),
                 std::numeric_limits<std::uint64_t>::max());
    });

    runner.run("post equality covers all payload fields", [&] {
        const auto mention = lora::test::make_install_id(70);
        const auto reply = lora::test::make_message_id(22);
        const auto first = lora::test::make_post(
            23, 42, 9U, "same", {mention}, reply, 123, "alice");
        const auto same = lora::test::make_post(
            23, 42, 9U, "same", {mention}, reply, 123, "alice");
        const auto different_body = lora::test::make_post(
            23, 42, 9U, "different", {mention}, reply, 123, "alice");
        const auto different_time = lora::test::make_post(
            23, 42, 9U, "same", {mention}, reply, 124, "alice");
        CHECK_EQ(first, same);
        CHECK_NE(first, different_body);
        CHECK_NE(first, different_time);
    });

    return runner.finish();
}

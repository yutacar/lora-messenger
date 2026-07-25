/*
 * SPDX-License-Identifier: MIT
 */

#include "protocol/post_codec.h"

#include "test_model_helpers.h"
#include "test_support.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

lora::protocol::Bytes bytes_from_hex(std::string_view hex) {
    auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint8_t>(value - '0');
        }
        return static_cast<std::uint8_t>(value - 'a' + 10);
    };

    lora::protocol::Bytes output;
    output.reserve(hex.size() / 2U);
    for (std::size_t index = 0; index < hex.size(); index += 2U) {
        output.push_back(static_cast<std::uint8_t>(
            (nibble(hex[index]) << 4U) | nibble(hex[index + 1U])));
    }
    return output;
}

lora::model::PostPayload golden_post() {
    lora::model::PostPayloadInput input;
    input.message_id =
        *lora::core::Uuid::parse(
            "00112233-4455-4677-8899-aabbccddeeff");
    input.sender_id =
        *lora::core::Uuid::parse(
            "10213243-5465-4767-98a9-bacbdcedfe0f");
    input.sender_sequence = 1U;
    input.sender_user_id = "A";
    input.body = "B";
    auto result =
        lora::model::PostPayload::create(std::move(input));
    return std::move(result).value();
}

} // namespace

int main() {
    using lora::protocol::PostCodecError;
    lora::test::Runner runner;

    runner.run("Post v1 golden bytes are stable and canonical", [&] {
        const auto post = golden_post();
        const auto expected = bytes_from_hex(
            "0000112233445546778899aabbccddeeff"
            "102132435465476798a9bacbdcedfe0f"
            "0000000000000001"
            "0101004142");

        const auto encoded = lora::protocol::encode_post(post);
        REQUIRE(encoded.has_value());
        CHECK_EQ(encoded.value(), expected);
        CHECK_EQ(encoded.value().size(),
                 lora::protocol::kMinimumEncodedPostBytes);

        const auto decoded =
            lora::protocol::decode_post(encoded.value());
        REQUIRE(decoded.has_value());
        CHECK_EQ(decoded.value(), post);
        const auto tag =
            lora::protocol::message_tag(post.message_id());
        CHECK_EQ(tag, (lora::protocol::MessageTag{
                          0x00U, 0x11U, 0x22U, 0x33U,
                          0x44U, 0x55U, 0x46U, 0x77U}));
    });

    runner.run("all fields fill the exact 316-byte upper bound", [&] {
        auto input = lora::test::make_post_input(
            20U, 40U, std::numeric_limits<std::uint64_t>::max(),
            std::string(160U, 'b'), std::string(24U, 'u'));
        input.mentions = {
            lora::test::make_uuid(50U),
            lora::test::make_uuid(51U),
            lora::test::make_uuid(52U),
            lora::test::make_uuid(53U),
        };
        input.reply_to = lora::test::make_uuid(19U);
        input.sender_time =
            std::numeric_limits<std::int64_t>::min();
        const auto post =
            lora::model::PostPayload::create(std::move(input));
        REQUIRE(post.has_value());

        const auto encoded =
            lora::protocol::encode_post(post.value());
        REQUIRE(encoded.has_value());
        CHECK_EQ(encoded.value().size(),
                 lora::protocol::kMaximumEncodedPostBytes);
        const auto decoded =
            lora::protocol::decode_post(encoded.value());
        REQUIRE(decoded.has_value());
        CHECK_EQ(decoded.value(), post.value());
        REQUIRE(decoded.value().sender_time().has_value());
        CHECK_EQ(*decoded.value().sender_time(),
                 std::numeric_limits<std::int64_t>::min());
    });

    runner.run("signed sender-time endpoints round trip exactly", [&] {
        for (const auto timestamp : {
                 std::numeric_limits<std::int64_t>::min(),
                 static_cast<std::int64_t>(-1),
                 static_cast<std::int64_t>(0),
                 std::numeric_limits<std::int64_t>::max()}) {
            auto input =
                lora::test::make_post_input(21U, 41U);
            input.sender_time = timestamp;
            const auto post =
                lora::model::PostPayload::create(std::move(input));
            REQUIRE(post.has_value());
            const auto encoded =
                lora::protocol::encode_post(post.value());
            REQUIRE(encoded.has_value());
            const auto decoded =
                lora::protocol::decode_post(encoded.value());
            REQUIRE(decoded.has_value());
            REQUIRE(decoded.value().sender_time().has_value());
            CHECK_EQ(*decoded.value().sender_time(), timestamp);
        }
    });

    runner.run("bounds and reserved flags are rejected before fields", [&] {
        CHECK_EQ(
            lora::protocol::decode_post(nullptr, 0U).error(),
            PostCodecError::NullData);

        lora::protocol::Bytes short_post(
            lora::protocol::kMinimumEncodedPostBytes - 1U, 0U);
        CHECK_EQ(
            lora::protocol::decode_post(short_post).error(),
            PostCodecError::SizeOutOfRange);

        lora::protocol::Bytes long_post(
            lora::protocol::kMaximumEncodedPostBytes + 1U, 0U);
        CHECK_EQ(
            lora::protocol::decode_post(long_post).error(),
            PostCodecError::SizeOutOfRange);

        auto reserved =
            std::move(lora::protocol::encode_post(golden_post())).value();
        reserved[0] = 0x04U;
        CHECK_EQ(
            lora::protocol::decode_post(reserved).error(),
            PostCodecError::ReservedFlags);
    });

    runner.run("declared lengths must consume the datagram exactly", [&] {
        auto too_long =
            std::move(lora::protocol::encode_post(golden_post())).value();
        too_long[42] = 2U;
        CHECK_EQ(
            lora::protocol::decode_post(too_long).error(),
            PostCodecError::LengthMismatch);

        auto too_many_mentions =
            std::move(lora::protocol::encode_post(golden_post())).value();
        too_many_mentions[43] = 5U;
        CHECK_EQ(
            lora::protocol::decode_post(too_many_mentions).error(),
            PostCodecError::LengthMismatch);

        auto unexpected_reply =
            std::move(lora::protocol::encode_post(golden_post())).value();
        unexpected_reply[0] = 0x01U;
        CHECK_EQ(
            lora::protocol::decode_post(unexpected_reply).error(),
            PostCodecError::LengthMismatch);
    });

    runner.run("domain-invalid text and identifiers cannot cross the codec", [&] {
        auto whitespace =
            std::move(lora::protocol::encode_post(golden_post())).value();
        whitespace.back() = static_cast<std::uint8_t>(' ');
        CHECK_EQ(
            lora::protocol::decode_post(whitespace).error(),
            PostCodecError::InvalidPost);

        auto zero_sequence =
            std::move(lora::protocol::encode_post(golden_post())).value();
        for (std::size_t index = 33U; index < 41U; ++index) {
            zero_sequence[index] = 0U;
        }
        CHECK_EQ(
            lora::protocol::decode_post(zero_sequence).error(),
            PostCodecError::InvalidPost);

        auto invalid_uuid =
            std::move(lora::protocol::encode_post(golden_post())).value();
        invalid_uuid[7] &= 0x0fU;
        CHECK_EQ(
            lora::protocol::decode_post(invalid_uuid).error(),
            PostCodecError::InvalidPost);
    });

    return runner.finish();
}

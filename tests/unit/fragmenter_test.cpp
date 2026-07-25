/*
 * SPDX-License-Identifier: MIT
 */

#include "protocol/fragmenter.h"

#include "protocol/frame_codec.h"
#include "protocol/post_codec.h"
#include "test_model_helpers.h"
#include "test_support.h"

#include <array>
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

lora::model::PostPayload maximum_post() {
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
    input.sender_time = std::numeric_limits<std::int64_t>::max();
    auto result =
        lora::model::PostPayload::create(std::move(input));
    return std::move(result).value();
}

} // namespace

int main() {
    using lora::protocol::FragmentError;
    lora::test::Runner runner;

    runner.run("the golden post fragments to the published MTU64 frames", [&] {
        const auto frames =
            lora::protocol::fragment_post(golden_post(), 64U);
        REQUIRE(frames.has_value());
        REQUIRE(frames.value().size() == 2U);
        CHECK_EQ(
            frames.value()[0],
            bytes_from_hex(
                "4c4d1000112233445546770000000000000001"
                "0002002e24"
                "9f544865"
                "0000112233445546778899aabbccddeeff"
                "102132435465476798a9bacbdcedfe0f"
                "000000"));
        CHECK_EQ(
            frames.value()[1],
            bytes_from_hex(
                "4c4d1000112233445546770000000000000001"
                "0102002e0a"
                "78da9132"
                "00000000010101004142"));
    });

    runner.run("the 316-byte maximum fits every required MTU", [&] {
        struct Case {
            std::size_t mtu;
            std::size_t fragments;
        };
        constexpr std::array<Case, 5> cases{{
            {48U, 16U},
            {51U, 14U},
            {64U, 9U},
            {128U, 4U},
            {255U, 2U},
        }};
        const auto post = maximum_post();
        const auto logical = lora::protocol::encode_post(post);
        REQUIRE(logical.has_value());
        CHECK_EQ(logical.value().size(), 316U);

        for (const auto& test_case : cases) {
            const auto frames =
                lora::protocol::fragment_post(
                    post, test_case.mtu);
            REQUIRE(frames.has_value());
            CHECK_EQ(frames.value().size(),
                     test_case.fragments);

            lora::protocol::Bytes rebuilt;
            for (std::size_t index = 0;
                 index < frames.value().size(); ++index) {
                const auto decoded =
                    lora::protocol::decode_frame(
                        frames.value()[index], test_case.mtu);
                REQUIRE(decoded.has_value());
                CHECK_EQ(decoded.value().fragment_index, index);
                CHECK_EQ(decoded.value().fragment_count,
                         test_case.fragments);
                CHECK_EQ(decoded.value().total_length, 316U);
                CHECK_EQ(decoded.value().sender_sequence,
                         post.sender_sequence());
                CHECK_EQ(decoded.value().tag,
                         lora::protocol::message_tag(
                             post.message_id()));
                if (index + 1U < frames.value().size()) {
                    CHECK_EQ(frames.value()[index].size(),
                             test_case.mtu);
                }
                rebuilt.insert(
                    rebuilt.end(),
                    decoded.value().payload.begin(),
                    decoded.value().payload.end());
            }
            CHECK_EQ(rebuilt, logical.value());
            const auto decoded_post =
                lora::protocol::decode_post(rebuilt);
            REQUIRE(decoded_post.has_value());
            CHECK_EQ(decoded_post.value(), post);
        }
    });

    runner.run("the 46-byte minimum uses the canonical fragment counts", [&] {
        struct Case {
            std::size_t mtu;
            std::size_t fragments;
        };
        constexpr std::array<Case, 5> cases{{
            {48U, 3U},
            {51U, 2U},
            {64U, 2U},
            {128U, 1U},
            {255U, 1U},
        }};
        const auto post = golden_post();
        for (const auto& test_case : cases) {
            const auto frames =
                lora::protocol::fragment_post(
                    post, test_case.mtu);
            REQUIRE(frames.has_value());
            CHECK_EQ(frames.value().size(),
                     test_case.fragments);
        }
    });

    runner.run("MTU bounds are inclusive and fail outside the contract", [&] {
        const auto post = golden_post();
        CHECK_EQ(
            lora::protocol::fragment_post(post, 47U).error(),
            FragmentError::InvalidMtu);
        CHECK(
            lora::protocol::fragment_post(post, 48U).has_value());
        CHECK(
            lora::protocol::fragment_post(post, 255U).has_value());
        CHECK_EQ(
            lora::protocol::fragment_post(post, 256U).error(),
            FragmentError::InvalidMtu);
    });

    return runner.finish();
}

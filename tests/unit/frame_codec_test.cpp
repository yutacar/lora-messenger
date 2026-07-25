/*
 * SPDX-License-Identifier: MIT
 */

#include "protocol/frame_codec.h"

#include "test_support.h"

#include <algorithm>
#include <cstdint>
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

lora::protocol::Bytes golden_logical_post() {
    return bytes_from_hex(
        "0000112233445546778899aabbccddeeff"
        "102132435465476798a9bacbdcedfe0f"
        "0000000000000001"
        "0101004142");
}

lora::protocol::DataFrame golden_frame(std::uint8_t index) {
    const auto logical = golden_logical_post();
    lora::protocol::DataFrame frame;
    frame.tag = {
        0x00U, 0x11U, 0x22U, 0x33U,
        0x44U, 0x55U, 0x46U, 0x77U,
    };
    frame.sender_sequence = 1U;
    frame.fragment_index = index;
    frame.fragment_count = 2U;
    frame.total_length =
        static_cast<std::uint16_t>(logical.size());
    const std::size_t offset = index == 0U ? 0U : 36U;
    frame.payload.assign(
        logical.begin() + static_cast<std::ptrdiff_t>(offset),
        logical.end());
    if (index == 0U) {
        frame.payload.resize(36U);
    }
    return frame;
}

void rewrite_crc(lora::protocol::Bytes& frame) {
    const std::uint32_t checksum =
        lora::protocol::crc32_iso_hdlc(
            frame.data(), 24U, frame.data() + 28U,
            frame.size() - 28U);
    frame[24] = static_cast<std::uint8_t>(
        (checksum >> 24U) & 0xffU);
    frame[25] = static_cast<std::uint8_t>(
        (checksum >> 16U) & 0xffU);
    frame[26] = static_cast<std::uint8_t>(
        (checksum >> 8U) & 0xffU);
    frame[27] =
        static_cast<std::uint8_t>(checksum & 0xffU);
}

} // namespace

int main() {
    using lora::protocol::FrameCodecError;
    lora::test::Runner runner;

    runner.run("CRC-32/ISO-HDLC uses the published check value", [&] {
        const auto* check =
            reinterpret_cast<const std::uint8_t*>("123456789");
        CHECK_EQ(
            lora::protocol::crc32_iso_hdlc(check, 9U),
            0xcbf43926U);
        CHECK_EQ(
            lora::protocol::crc32_iso_hdlc(
                check, 4U, check + 4U, 5U),
            0xcbf43926U);
    });

    runner.run("both MTU64 golden frames are byte exact", [&] {
        const auto expected_first = bytes_from_hex(
            "4c4d1000112233445546770000000000000001"
            "0002002e24"
            "9f544865"
            "0000112233445546778899aabbccddeeff"
            "102132435465476798a9bacbdcedfe0f"
            "000000");
        const auto expected_second = bytes_from_hex(
            "4c4d1000112233445546770000000000000001"
            "0102002e0a"
            "78da9132"
            "00000000010101004142");

        const auto first =
            lora::protocol::encode_frame(golden_frame(0U), 64U);
        const auto second =
            lora::protocol::encode_frame(golden_frame(1U), 64U);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK_EQ(first.value(), expected_first);
        CHECK_EQ(second.value(), expected_second);
        CHECK_EQ(first.value().size(), 64U);
        CHECK_EQ(second.value().size(), 38U);
    });

    runner.run("golden frames decode every header field and payload", [&] {
        for (std::uint8_t index = 0U; index < 2U; ++index) {
            const auto source = golden_frame(index);
            const auto encoded =
                lora::protocol::encode_frame(source, 64U);
            REQUIRE(encoded.has_value());
            const auto decoded =
                lora::protocol::decode_frame(encoded.value(), 64U);
            REQUIRE(decoded.has_value());
            CHECK_EQ(decoded.value().tag, source.tag);
            CHECK_EQ(decoded.value().sender_sequence,
                     source.sender_sequence);
            CHECK_EQ(decoded.value().fragment_index,
                     source.fragment_index);
            CHECK_EQ(decoded.value().fragment_count,
                     source.fragment_count);
            CHECK_EQ(decoded.value().total_length,
                     source.total_length);
            CHECK_EQ(decoded.value().payload, source.payload);
        }
    });

    runner.run("invalid MTUs and datagram lengths fail closed", [&] {
        const auto encoded =
            lora::protocol::encode_frame(golden_frame(1U), 64U);
        REQUIRE(encoded.has_value());
        CHECK_EQ(
            lora::protocol::encode_frame(
                golden_frame(0U), 47U).error(),
            FrameCodecError::InvalidMtu);
        CHECK_EQ(
            lora::protocol::decode_frame(
                encoded.value(), 256U).error(),
            FrameCodecError::InvalidMtu);
        CHECK_EQ(
            lora::protocol::decode_frame(
                nullptr, encoded.value().size(), 64U).error(),
            FrameCodecError::NullData);

        lora::protocol::Bytes short_frame(28U, 0U);
        CHECK_EQ(
            lora::protocol::decode_frame(short_frame, 64U).error(),
            FrameCodecError::DatagramTooShort);

        auto trailing = encoded.value();
        trailing.push_back(0U);
        CHECK_EQ(
            lora::protocol::decode_frame(trailing, 64U).error(),
            FrameCodecError::LengthMismatch);
    });

    runner.run("magic version type length and checksum are independent gates", [&] {
        auto encoded =
            std::move(lora::protocol::encode_frame(
                golden_frame(1U), 64U)).value();

        auto bad_magic = encoded;
        bad_magic[0] ^= 1U;
        CHECK_EQ(
            lora::protocol::decode_frame(bad_magic, 64U).error(),
            FrameCodecError::BadMagic);

        auto bad_version = encoded;
        bad_version[2] = 0x20U;
        CHECK_EQ(
            lora::protocol::decode_frame(bad_version, 64U).error(),
            FrameCodecError::UnsupportedVersion);

        auto bad_type = encoded;
        bad_type[2] = 0x11U;
        CHECK_EQ(
            lora::protocol::decode_frame(bad_type, 64U).error(),
            FrameCodecError::UnsupportedType);

        auto bad_length = encoded;
        bad_length[23] =
            static_cast<std::uint8_t>(bad_length[23] + 1U);
        CHECK_EQ(
            lora::protocol::decode_frame(bad_length, 64U).error(),
            FrameCodecError::LengthMismatch);

        auto bad_checksum = encoded;
        bad_checksum.back() ^= 1U;
        CHECK_EQ(
            lora::protocol::decode_frame(bad_checksum, 64U).error(),
            FrameCodecError::ChecksumMismatch);
    });

    runner.run("CRC-valid semantic malformations are rejected", [&] {
        auto encoded =
            std::move(lora::protocol::encode_frame(
                golden_frame(0U), 64U)).value();

        auto zero_sequence = encoded;
        std::fill(zero_sequence.begin() + 11,
                  zero_sequence.begin() + 19, 0U);
        rewrite_crc(zero_sequence);
        CHECK_EQ(
            lora::protocol::decode_frame(
                zero_sequence, 64U).error(),
            FrameCodecError::InvalidSenderSequence);

        auto bad_total = encoded;
        bad_total[21] = 0U;
        bad_total[22] = 45U;
        rewrite_crc(bad_total);
        CHECK_EQ(
            lora::protocol::decode_frame(bad_total, 64U).error(),
            FrameCodecError::InvalidTotalLength);

        auto bad_index = encoded;
        bad_index[19] = 2U;
        rewrite_crc(bad_index);
        CHECK_EQ(
            lora::protocol::decode_frame(bad_index, 64U).error(),
            FrameCodecError::InvalidFragment);

        auto bad_count = encoded;
        bad_count[20] = 1U;
        rewrite_crc(bad_count);
        CHECK_EQ(
            lora::protocol::decode_frame(bad_count, 64U).error(),
            FrameCodecError::NonCanonicalFragment);

        auto short_nonfinal = encoded;
        short_nonfinal.erase(short_nonfinal.end() - 1);
        short_nonfinal[23] = 35U;
        rewrite_crc(short_nonfinal);
        CHECK_EQ(
            lora::protocol::decode_frame(
                short_nonfinal, 64U).error(),
            FrameCodecError::NonCanonicalFragment);
    });

    return runner.finish();
}

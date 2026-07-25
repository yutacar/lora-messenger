/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/result.h"
#include "protocol/limits.h"

#include <cstddef>
#include <cstdint>

namespace lora::protocol {

struct DataFrame {
    MessageTag tag{};
    std::uint64_t sender_sequence = 0;
    std::uint8_t fragment_index = 0;
    std::uint8_t fragment_count = 0;
    std::uint16_t total_length = 0;
    Bytes payload;
};

enum class FrameCodecError {
    None,
    InvalidMtu,
    NullData,
    DatagramTooShort,
    DatagramTooLarge,
    BadMagic,
    UnsupportedVersion,
    UnsupportedType,
    LengthMismatch,
    ChecksumMismatch,
    InvalidSenderSequence,
    InvalidTotalLength,
    InvalidFragment,
    NonCanonicalFragment,
};

std::uint32_t crc32_iso_hdlc(
    const std::uint8_t* first, std::size_t first_size,
    const std::uint8_t* second = nullptr,
    std::size_t second_size = 0) noexcept;

core::Result<Bytes, FrameCodecError>
encode_frame(const DataFrame& frame, std::size_t mtu);

core::Result<DataFrame, FrameCodecError>
decode_frame(const std::uint8_t* data, std::size_t size,
             std::size_t mtu);

inline core::Result<DataFrame, FrameCodecError>
decode_frame(const Bytes& bytes, std::size_t mtu) {
    return decode_frame(bytes.data(), bytes.size(), mtu);
}

} // namespace lora::protocol

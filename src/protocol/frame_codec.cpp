/*
 * SPDX-License-Identifier: MIT
 */

#include "protocol/frame_codec.h"

#include <algorithm>
#include <utility>

namespace lora::protocol {
namespace {

constexpr std::size_t kControlOffset = 2U;
constexpr std::size_t kTagOffset = 3U;
constexpr std::size_t kSequenceOffset = 11U;
constexpr std::size_t kFragmentIndexOffset = 19U;
constexpr std::size_t kFragmentCountOffset = 20U;
constexpr std::size_t kTotalLengthOffset = 21U;
constexpr std::size_t kPayloadLengthOffset = 23U;
constexpr std::size_t kChecksumOffset = 24U;

void write_uint16(std::uint8_t* destination,
                  std::uint16_t value) noexcept {
    destination[0] =
        static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    destination[1] =
        static_cast<std::uint8_t>(value & 0xffU);
}

void write_uint32(std::uint8_t* destination,
                  std::uint32_t value) noexcept {
    for (unsigned index = 0; index < 4U; ++index) {
        const unsigned shift = 24U - index * 8U;
        destination[index] =
            static_cast<std::uint8_t>((value >> shift) & 0xffU);
    }
}

void write_uint64(std::uint8_t* destination,
                  std::uint64_t value) noexcept {
    for (unsigned index = 0; index < 8U; ++index) {
        const unsigned shift = 56U - index * 8U;
        destination[index] =
            static_cast<std::uint8_t>((value >> shift) & 0xffU);
    }
}

std::uint16_t read_uint16(const std::uint8_t* source) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(source[0]) << 8U) |
        static_cast<std::uint16_t>(source[1]));
}

std::uint32_t read_uint32(const std::uint8_t* source) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        value = (value << 8U) |
                static_cast<std::uint32_t>(source[index]);
    }
    return value;
}

std::uint64_t read_uint64(const std::uint8_t* source) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) |
                static_cast<std::uint64_t>(source[index]);
    }
    return value;
}

std::uint32_t update_crc(std::uint32_t crc,
                         const std::uint8_t* data,
                         std::size_t size) noexcept {
    if (data == nullptr && size != 0U) {
        return crc;
    }
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= static_cast<std::uint32_t>(data[index]);
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask =
                0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

FrameCodecError validate_frame_fields(
    std::uint64_t sender_sequence,
    std::uint8_t fragment_index,
    std::uint8_t fragment_count,
    std::uint16_t total_length,
    std::size_t payload_size,
    std::size_t mtu) noexcept {
    if (sender_sequence == 0U) {
        return FrameCodecError::InvalidSenderSequence;
    }
    if (total_length < kMinimumEncodedPostBytes ||
        total_length > kMaximumEncodedPostBytes) {
        return FrameCodecError::InvalidTotalLength;
    }
    if (fragment_count == 0U ||
        fragment_count > kMaximumFragments ||
        fragment_index >= fragment_count ||
        payload_size == 0U ||
        payload_size > kMaximumFramePayloadBytes) {
        return FrameCodecError::InvalidFragment;
    }

    const std::size_t capacity = frame_payload_capacity(mtu);
    const std::size_t expected_count =
        (static_cast<std::size_t>(total_length) + capacity - 1U) /
        capacity;
    if (expected_count == 0U ||
        expected_count > kMaximumFragments ||
        fragment_count != expected_count) {
        return FrameCodecError::NonCanonicalFragment;
    }
    const std::size_t expected_payload_size =
        fragment_index + 1U < fragment_count
            ? capacity
            : static_cast<std::size_t>(total_length) -
                  capacity *
                      (static_cast<std::size_t>(fragment_count) - 1U);
    if (payload_size != expected_payload_size) {
        return FrameCodecError::NonCanonicalFragment;
    }
    return FrameCodecError::None;
}

} // namespace

std::uint32_t crc32_iso_hdlc(
    const std::uint8_t* first, std::size_t first_size,
    const std::uint8_t* second, std::size_t second_size) noexcept {
    std::uint32_t crc = 0xffffffffU;
    crc = update_crc(crc, first, first_size);
    crc = update_crc(crc, second, second_size);
    return crc ^ 0xffffffffU;
}

core::Result<Bytes, FrameCodecError>
encode_frame(const DataFrame& frame, std::size_t mtu) {
    if (!is_supported_mtu(mtu)) {
        return core::Result<Bytes, FrameCodecError>::failure(
            FrameCodecError::InvalidMtu);
    }
    const FrameCodecError validation = validate_frame_fields(
        frame.sender_sequence, frame.fragment_index,
        frame.fragment_count, frame.total_length,
        frame.payload.size(), mtu);
    if (validation != FrameCodecError::None) {
        return core::Result<Bytes, FrameCodecError>::failure(
            validation);
    }

    Bytes encoded(kFrameHeaderBytes + frame.payload.size(), 0U);
    encoded[0] = kFrameMagicFirst;
    encoded[1] = kFrameMagicSecond;
    encoded[kControlOffset] = static_cast<std::uint8_t>(
        (kProtocolVersion << 4U) | kDataFrameType);
    std::copy(frame.tag.begin(), frame.tag.end(),
              encoded.begin() +
                  static_cast<std::ptrdiff_t>(kTagOffset));
    write_uint64(encoded.data() + kSequenceOffset,
                 frame.sender_sequence);
    encoded[kFragmentIndexOffset] = frame.fragment_index;
    encoded[kFragmentCountOffset] = frame.fragment_count;
    write_uint16(encoded.data() + kTotalLengthOffset,
                 frame.total_length);
    encoded[kPayloadLengthOffset] =
        static_cast<std::uint8_t>(frame.payload.size());
    std::copy(frame.payload.begin(), frame.payload.end(),
              encoded.begin() +
                  static_cast<std::ptrdiff_t>(kFrameHeaderBytes));

    const std::uint32_t checksum = crc32_iso_hdlc(
        encoded.data(), kChecksumOffset,
        encoded.data() + kFrameHeaderBytes,
        frame.payload.size());
    write_uint32(encoded.data() + kChecksumOffset, checksum);
    return core::Result<Bytes, FrameCodecError>::success(
        std::move(encoded));
}

core::Result<DataFrame, FrameCodecError>
decode_frame(const std::uint8_t* data, std::size_t size,
             std::size_t mtu) {
    if (!is_supported_mtu(mtu)) {
        return core::Result<DataFrame, FrameCodecError>::failure(
            FrameCodecError::InvalidMtu);
    }
    if (data == nullptr) {
        return core::Result<DataFrame, FrameCodecError>::failure(
            FrameCodecError::NullData);
    }
    if (size < kFrameHeaderBytes + 1U) {
        return core::Result<DataFrame, FrameCodecError>::failure(
            FrameCodecError::DatagramTooShort);
    }
    if (size > mtu || size > kMaximumTransportMtu) {
        return core::Result<DataFrame, FrameCodecError>::failure(
            FrameCodecError::DatagramTooLarge);
    }
    if (data[0] != kFrameMagicFirst ||
        data[1] != kFrameMagicSecond) {
        return core::Result<DataFrame, FrameCodecError>::failure(
            FrameCodecError::BadMagic);
    }

    const std::uint8_t control = data[kControlOffset];
    if ((control >> 4U) != kProtocolVersion) {
        return core::Result<DataFrame, FrameCodecError>::failure(
            FrameCodecError::UnsupportedVersion);
    }
    if ((control & 0x0fU) != kDataFrameType) {
        return core::Result<DataFrame, FrameCodecError>::failure(
            FrameCodecError::UnsupportedType);
    }

    const std::size_t payload_size = data[kPayloadLengthOffset];
    if (payload_size == 0U ||
        size != kFrameHeaderBytes + payload_size) {
        return core::Result<DataFrame, FrameCodecError>::failure(
            FrameCodecError::LengthMismatch);
    }
    const std::uint32_t expected_checksum =
        read_uint32(data + kChecksumOffset);
    const std::uint32_t actual_checksum = crc32_iso_hdlc(
        data, kChecksumOffset, data + kFrameHeaderBytes,
        payload_size);
    if (expected_checksum != actual_checksum) {
        return core::Result<DataFrame, FrameCodecError>::failure(
            FrameCodecError::ChecksumMismatch);
    }

    DataFrame frame;
    std::copy_n(data + kTagOffset, frame.tag.size(),
                frame.tag.begin());
    frame.sender_sequence = read_uint64(data + kSequenceOffset);
    frame.fragment_index = data[kFragmentIndexOffset];
    frame.fragment_count = data[kFragmentCountOffset];
    frame.total_length = read_uint16(data + kTotalLengthOffset);
    const FrameCodecError validation = validate_frame_fields(
        frame.sender_sequence, frame.fragment_index,
        frame.fragment_count, frame.total_length,
        payload_size, mtu);
    if (validation != FrameCodecError::None) {
        return core::Result<DataFrame, FrameCodecError>::failure(
            validation);
    }

    frame.payload.assign(data + kFrameHeaderBytes,
                         data + size);
    return core::Result<DataFrame, FrameCodecError>::success(
        std::move(frame));
}

} // namespace lora::protocol

/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ports/datagram_transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lora::protocol {

using Bytes = std::vector<std::uint8_t>;
using MessageTag = std::array<std::uint8_t, 8>;

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::uint8_t kDataFrameType = 0;
inline constexpr std::uint8_t kFrameMagicFirst = 0x4cU;
inline constexpr std::uint8_t kFrameMagicSecond = 0x4dU;

inline constexpr std::size_t kFrameHeaderBytes = 28;
inline constexpr std::size_t kMinimumTransportMtu =
    ports::kMinimumDatagramMtu;
inline constexpr std::size_t kMaximumTransportMtu =
    ports::kMaximumDatagramBytes;
inline constexpr std::size_t kMaximumFramePayloadBytes =
    kMaximumTransportMtu - kFrameHeaderBytes;

inline constexpr std::size_t kMinimumEncodedPostBytes = 46;
inline constexpr std::size_t kMaximumEncodedPostBytes = 316;
inline constexpr std::size_t kMaximumFragments = 16;

constexpr bool is_supported_mtu(std::size_t mtu) noexcept {
    return mtu >= kMinimumTransportMtu &&
           mtu <= kMaximumTransportMtu;
}

constexpr std::size_t frame_payload_capacity(std::size_t mtu) noexcept {
    return is_supported_mtu(mtu) ? mtu - kFrameHeaderBytes : 0U;
}

static_assert(kMaximumEncodedPostBytes <=
              kMaximumFragments *
                  (kMinimumTransportMtu - kFrameHeaderBytes),
              "Post v1 must fit in sixteen frames at MTU 48");
static_assert(kMaximumFramePayloadBytes <= 255U,
              "the one-byte payload length must represent every frame");

} // namespace lora::protocol

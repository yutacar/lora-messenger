/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace lora::test::phase4_pipe {

// The two-process harness deliberately uses a tiny, bounded record protocol:
// a two-byte big-endian length followed by exactly one canonical datagram.
// A zero length requests a clean shutdown. Every input record produces a
// three-byte response header (status plus a two-byte payload length), followed
// by the completed canonical post only when a post is committed or recognized
// as an already committed duplicate.
inline constexpr std::size_t kLengthPrefixBytes = 2U;
inline constexpr std::size_t kResponseHeaderBytes = 3U;
inline constexpr std::size_t kMaximumRecordBytes = 255U;

enum class Response : std::uint8_t {
    FragmentAccepted = 1U,
    DuplicateFrame = 2U,
    MalformedFrame = 3U,
    Committed = 4U,
    DuplicatePost = 5U,
    Conflict = 6U,
    QueueRejected = 7U,
    Shutdown = 0x7fU,
};

} // namespace lora::test::phase4_pipe

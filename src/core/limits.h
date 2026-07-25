/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>

namespace lora::core {

inline constexpr std::size_t kMaxUserIdBytes = 24;
inline constexpr std::size_t kMaxPostBodyBytes = 160;
inline constexpr std::size_t kMaxMentions = 4;
inline constexpr std::size_t kMaxQueuedLocalPosts = 16;
inline constexpr std::size_t kMaxTimelineEntries = 256;

} // namespace lora::core

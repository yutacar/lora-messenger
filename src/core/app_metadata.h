/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace lora::core {

inline constexpr char kPackageName[] = "lora-messenger";
inline constexpr char kDisplayName[] = "LoRa Messenger";
inline constexpr char kExecutableName[] = "lora-messenger";
inline constexpr char kVersion[] = "0.1.0";
inline constexpr std::int32_t kDisplayWidth = 320;
inline constexpr std::int32_t kDisplayHeight = 170;

enum class RadioState {
    Disabled,
};

struct AppMetadata {
    std::string_view package_name;
    std::string_view display_name;
    std::string_view executable_name;
    std::string_view version;
    std::int32_t display_width;
    std::int32_t display_height;
    RadioState radio_state;
};

const AppMetadata& app_metadata() noexcept;
bool metadata_is_valid() noexcept;

} // namespace lora::core

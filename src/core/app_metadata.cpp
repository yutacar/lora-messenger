/*
 * SPDX-License-Identifier: MIT
 */

#include "core/app_metadata.h"

#include <algorithm>

namespace lora::core {
namespace {

constexpr AppMetadata kMetadata{
    kPackageName,
    kDisplayName,
    kExecutableName,
    kVersion,
    kDisplayWidth,
    kDisplayHeight,
    RadioState::Disabled,
};

bool is_package_character(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
           value == '+' || value == '.' || value == '-';
}

} // namespace

const AppMetadata& app_metadata() noexcept {
    return kMetadata;
}

bool metadata_is_valid() noexcept {
    const auto& metadata = app_metadata();
    return !metadata.package_name.empty() &&
           std::all_of(metadata.package_name.begin(), metadata.package_name.end(),
                       [](char value) { return is_package_character(static_cast<unsigned char>(value)); }) &&
           metadata.display_name == kDisplayName &&
           metadata.executable_name == metadata.package_name &&
           metadata.display_width == 320 &&
           metadata.display_height == 170 &&
           metadata.radio_state == RadioState::Disabled;
}

} // namespace lora::core

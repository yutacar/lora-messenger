/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/text.h"
#include "core/uuid.h"

#include <cstdint>
#include <string_view>

namespace lora::persistence {

inline constexpr std::uint64_t kInitialSettingsGeneration = 1U;

enum class StoredLocale {
    English,
    Japanese,
    SimplifiedChinese,
};

inline constexpr std::string_view stored_locale_code(
    StoredLocale locale) noexcept {
    switch (locale) {
        case StoredLocale::English: return "en";
        case StoredLocale::Japanese: return "ja";
        case StoredLocale::SimplifiedChinese: return "zh-Hans";
    }
    return {};
}

struct SettingsRecord {
    std::uint64_t generation;
    core::InstallId install_id;
    core::UserId user_id;
    std::uint64_t sender_sequence_high_watermark;
    StoredLocale locale;
    bool history_initialized;
    bool skip_title{false};
};

inline bool operator==(const SettingsRecord& left,
                       const SettingsRecord& right) noexcept {
    return left.generation == right.generation &&
           left.install_id == right.install_id &&
           left.user_id == right.user_id &&
           left.sender_sequence_high_watermark ==
               right.sender_sequence_high_watermark &&
           left.locale == right.locale &&
           left.history_initialized == right.history_initialized &&
           left.skip_title == right.skip_title;
}

inline bool operator!=(const SettingsRecord& left,
                       const SettingsRecord& right) noexcept {
    return !(left == right);
}

} // namespace lora::persistence

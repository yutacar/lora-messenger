/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/result.h"
#include "persistence/settings_record.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace lora::adapters::storage {

inline constexpr std::size_t kMaxSettingsJsonBytes = 16U * 1024U;
inline constexpr std::string_view kSettingsFormat =
    "lora-messenger.settings";
inline constexpr unsigned int kSettingsSchemaVersion = 2U;

enum class SettingsJsonError {
    TooLarge,
    InvalidUtf8,
    MalformedJson,
    ExcessiveNesting,
    DuplicateKey,
    UnknownKey,
    MissingKey,
    WrongType,
    UnsupportedFormat,
    UnsupportedSchemaVersion,
    InvalidGeneration,
    InvalidInstallUuid,
    InvalidUserId,
    InvalidSenderSequence,
    InvalidLocale,
};

core::Result<persistence::SettingsRecord, SettingsJsonError>
parse_settings_json(std::string_view json);

core::Result<std::string, SettingsJsonError>
serialize_settings_json(const persistence::SettingsRecord& settings);

} // namespace lora::adapters::storage

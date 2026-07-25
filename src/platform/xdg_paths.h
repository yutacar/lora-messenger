/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/result.h"

#include <filesystem>
#include <optional>
#include <string>

namespace lora::platform {

struct XdgEnvironment {
    std::optional<std::string> home;
    std::optional<std::string> config_home;
    std::optional<std::string> data_home;
};

struct XdgPaths {
    std::filesystem::path config_directory;
    std::filesystem::path data_directory;
    std::filesystem::path settings_file;
    std::filesystem::path history_database;
};

enum class XdgPathError {
    HomeMissing,
    HomeNotAbsolute,
};

core::Result<XdgPaths, XdgPathError>
resolve_xdg_paths(const XdgEnvironment& environment);

core::Result<XdgPaths, XdgPathError> resolve_xdg_paths_from_environment();

} // namespace lora::platform

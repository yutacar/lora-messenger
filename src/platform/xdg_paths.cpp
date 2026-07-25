/*
 * SPDX-License-Identifier: MIT
 */

#include "platform/xdg_paths.h"

#include <cstdlib>
#include <utility>

namespace lora::platform {
namespace {

std::optional<std::string> environment_value(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

std::filesystem::path absolute_or_fallback(
    const std::optional<std::string>& candidate,
    const std::filesystem::path& fallback) {
    if (candidate && !candidate->empty()) {
        const std::filesystem::path path(*candidate);
        if (path.is_absolute()) {
            return path.lexically_normal();
        }
    }
    return fallback.lexically_normal();
}

} // namespace

core::Result<XdgPaths, XdgPathError>
resolve_xdg_paths(const XdgEnvironment& environment) {
    if (!environment.home || environment.home->empty()) {
        return core::Result<XdgPaths, XdgPathError>::failure(
            XdgPathError::HomeMissing);
    }

    const std::filesystem::path home(*environment.home);
    if (!home.is_absolute()) {
        return core::Result<XdgPaths, XdgPathError>::failure(
            XdgPathError::HomeNotAbsolute);
    }

    const auto normalized_home = home.lexically_normal();
    const auto config_root = absolute_or_fallback(
        environment.config_home, normalized_home / ".config");
    const auto data_root = absolute_or_fallback(
        environment.data_home, normalized_home / ".local" / "share");

    XdgPaths paths{
        (config_root / "lora-messenger").lexically_normal(),
        (data_root / "lora-messenger").lexically_normal(),
        {},
        {},
    };
    paths.settings_file = paths.config_directory / "settings.json";
    paths.history_database = paths.data_directory / "history.sqlite3";
    return core::Result<XdgPaths, XdgPathError>::success(std::move(paths));
}

core::Result<XdgPaths, XdgPathError> resolve_xdg_paths_from_environment() {
    return resolve_xdg_paths(XdgEnvironment{
        environment_value("HOME"),
        environment_value("XDG_CONFIG_HOME"),
        environment_value("XDG_DATA_HOME"),
    });
}

} // namespace lora::platform

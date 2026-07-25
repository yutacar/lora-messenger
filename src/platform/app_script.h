/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace platform {

using AppScriptProbe =
    std::function<std::optional<std::string>(std::string_view field)>;

// Desktop-only deterministic sequence. Legacy WAIT, HOME, ESC, CLOSE, and
// SHOT=<stem> tokens remain supported. The parser also accepts named navigation
// keys, percent-encoded TEXT, and UI-independent EXPECT/AWAIT probes.
void start_app_script();
void start_app_script(AppScriptProbe probe);
void stop_app_script();
bool app_script_failed();
bool app_script_pending();

} // namespace platform

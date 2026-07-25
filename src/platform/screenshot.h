/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

namespace platform {

inline constexpr char kScreenshotDirectory[] = "screenshot";

bool capture_screenshot(const std::string& stem);

} // namespace platform

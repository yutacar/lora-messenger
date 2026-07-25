/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl.h"

#include <cstdint>
#include <functional>

namespace platform {

using KeyHandler = std::function<void(std::uint32_t)>;

void init_key_input(lv_display_t* display);
void attach_key_router(lv_indev_t* input);
void set_key_handler(KeyHandler handler);
void route_key(std::uint32_t key);

} // namespace platform

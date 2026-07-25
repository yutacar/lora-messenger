/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl.h"

namespace view {

inline constexpr int kScreenWidth = 320;
inline constexpr int kScreenHeight = 170;

struct Palette {
    lv_color_t background;
    lv_color_t panel;
    lv_color_t panel_border;
    lv_color_t text;
    lv_color_t muted_text;
    lv_color_t accent;
    lv_color_t warning;
    lv_color_t footer;
};

Palette app_palette();
void apply_lvgl_theme(lv_display_t* display);

} // namespace view
